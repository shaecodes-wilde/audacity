#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Audacity: A Digital Audio Editor
"""MCP stdio server for Audacity's opt-in local bridge.

Speaks the Model Context Protocol (JSON-RPC 2.0, one message per line) on
stdin/stdout and forwards a fixed set of tools to a running Audacity that was
started with AUDACITY_MCP_BRIDGE=1. Standard library only; Python 3.9+.

The bridge directory is $AUDACITY_MCP_DIR, or the platform runtime directory
plus "/audacity-mcp" (same rule Audacity uses). It must be owned by the current
user with mode 0700; the server refuses to talk to it otherwise.

See docs/mcp-bridge.md for setup with Codex and Claude Code.
"""

from __future__ import annotations

import json
import os
import socket
import stat
import sys
import tempfile
from typing import Any, Dict, Optional

SERVER_NAME = "audacity"
SERVER_VERSION = "0.1.0"
BRIDGE_PROTOCOL = 1
SUPPORTED_PROTOCOL_VERSIONS = ("2025-06-18", "2025-03-26", "2024-11-05")
DEFAULT_TIMEOUT = float(os.environ.get("AUDACITY_MCP_TIMEOUT", "300"))
MAX_RESPONSE_BYTES = 16 * 1024 * 1024


# ---------------------------------------------------------------------------
# Bridge client
# ---------------------------------------------------------------------------

class BridgeError(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


def default_bridge_dir() -> str:
    env = os.environ.get("AUDACITY_MCP_DIR")
    if env:
        return os.path.normpath(env)
    if sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Application Support")
    elif sys.platform.startswith("win"):
        base = tempfile.gettempdir()
    else:
        base = os.environ.get("XDG_RUNTIME_DIR") or os.path.join(
            tempfile.gettempdir(), "runtime-" + (os.environ.get("USER") or str(os.getuid())))
    return os.path.normpath(os.path.join(base, "audacity-mcp"))


def _check_private(path: str, want_dir: bool) -> None:
    """Refuse bridge files that another user could have planted or can read."""
    st = os.lstat(path)
    if want_dir and not stat.S_ISDIR(st.st_mode):
        raise BridgeError("insecure_bridge", f"{path} is not a directory")
    if not want_dir and not stat.S_ISREG(st.st_mode):
        raise BridgeError("insecure_bridge", f"{path} is not a regular file")
    if hasattr(os, "getuid"):
        if st.st_uid != os.getuid():
            raise BridgeError("insecure_bridge", f"{path} is owned by another user")
        if st.st_mode & 0o077:
            raise BridgeError("insecure_bridge", f"{path} is accessible by group or others")


class BridgeClient:
    """One authenticated connection to Audacity, reopened on demand."""

    def __init__(self, directory: Optional[str] = None, timeout: float = DEFAULT_TIMEOUT):
        self.directory = directory or default_bridge_dir()
        self.timeout = timeout
        self._sock: Optional[socket.socket] = None
        self._buf = b""
        self._next_id = 1

    @property
    def socket_path(self) -> str:
        return os.path.join(self.directory, "bridge.sock")

    @property
    def token_path(self) -> str:
        return os.path.join(self.directory, "token")

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock = None
                self._buf = b""

    def _connect(self) -> None:
        if not hasattr(socket, "AF_UNIX"):
            raise BridgeError("unsupported", "this platform has no Unix domain sockets")
        try:
            _check_private(self.directory, want_dir=True)
            _check_private(self.token_path, want_dir=False)
            with open(self.token_path, "r", encoding="ascii") as f:
                token = f.read().strip()
        except FileNotFoundError:
            raise BridgeError(
                "not_running",
                f"no Audacity bridge in {self.directory}; start Audacity with AUDACITY_MCP_BRIDGE=1"
                " (and the same AUDACITY_MCP_DIR, if you set one)") from None

        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(self.timeout)
        try:
            sock.connect(self.socket_path)
        except OSError as e:
            sock.close()
            raise BridgeError("not_running", f"cannot connect to {self.socket_path}: {e}") from None
        self._sock = sock
        self._buf = b""
        try:
            result = self._roundtrip("auth", {"token": token})
        except BaseException:
            self.close()
            raise
        if result.get("protocol") != BRIDGE_PROTOCOL:
            self.close()
            raise BridgeError("protocol_mismatch", f"bridge protocol {result.get('protocol')} is not {BRIDGE_PROTOCOL}")

    def _roundtrip(self, method: str, params: Dict[str, Any]) -> Any:
        assert self._sock is not None
        req_id = self._next_id
        self._next_id += 1
        line = json.dumps({"id": req_id, "method": method, "params": params}, separators=(",", ":")) + "\n"
        self._sock.sendall(line.encode("utf-8"))
        while b"\n" not in self._buf:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise ConnectionError("Audacity closed the bridge connection")
            self._buf += chunk
            if len(self._buf) > MAX_RESPONSE_BYTES:
                raise BridgeError("invalid_response", "bridge response too large")
        raw, _, self._buf = self._buf.partition(b"\n")
        response = json.loads(raw.decode("utf-8"))
        if response.get("id") != req_id:
            raise BridgeError("invalid_response", "bridge response id does not match the request")
        if "error" in response:
            err = response["error"] or {}
            raise BridgeError(str(err.get("code", "failed")), str(err.get("message", "unknown error")))
        return response.get("result")

    def call(self, method: str, params: Optional[Dict[str, Any]] = None) -> Any:
        params = params or {}
        read_only = method in ("ping", "get_project", "list_tracks")
        for attempt in (1, 2):
            if self._sock is None:
                self._connect()
            try:
                return self._roundtrip(method, params)
            except ConnectionError:
                self.close()
                if not read_only:
                    raise BridgeError(
                        "outcome_unknown",
                        "lost the connection during the operation. Check the project or output before retrying."
                    ) from None
                if attempt == 2:
                    raise BridgeError("not_running", "lost the connection to Audacity") from None
            except socket.timeout:
                self.close()
                message = f"Audacity did not answer within {self.timeout:g}s"
                if not read_only:
                    message += ". Check the project or output before retrying."
                raise BridgeError("timeout" if read_only else "outcome_unknown", message) from None
        raise AssertionError("unreachable")


# ---------------------------------------------------------------------------
# Tools (names map 1:1 onto bridge methods; Audacity re-validates everything)
# ---------------------------------------------------------------------------

def _schema(props: Dict[str, Any], required=()) -> Dict[str, Any]:
    return {"type": "object", "properties": props, "required": list(required), "additionalProperties": False}


_ID = {"type": "integer", "minimum": 0}
_TIME = {"type": "number", "minimum": 0}

TOOLS = [
    {
        "name": "bridge_status",
        "method": "ping",
        "description": "Check that Audacity's MCP bridge is reachable and whether a project is open.",
        "inputSchema": _schema({}),
        "annotations": {"readOnlyHint": True},
    },
    {
        "name": "get_project",
        "method": "get_project",
        "description": "Describe the open project: title, path, sample rate, duration, track count, "
                       "current time selection and the next undo step.",
        "inputSchema": _schema({}),
        "annotations": {"readOnlyHint": True},
    },
    {
        "name": "list_tracks",
        "method": "list_tracks",
        "description": "List tracks (id, title, mono/stereo/label, rate, sample format, mute/solo). "
                       "With include_items (default true), also each audio track's clips (id, start, end, "
                       "clip gain) and each label track's labels.",
        "inputSchema": _schema({"include_items": {"type": "boolean", "default": True}}),
        "annotations": {"readOnlyHint": True},
    },
    {
        "name": "import_wav",
        "method": "import_wav",
        "description": "Import a .wav file into the open project as new track(s). Keeps the file's channels; "
                       "an empty project adopts the file's sample rate. Returns the new track ids. Undoable.",
        "inputSchema": _schema({"path": {"type": "string", "description": "Absolute path to a .wav file."}}, ["path"]),
    },
    {
        "name": "set_time_selection",
        "method": "set_selection",
        "description": "Select the time range [start, end) in seconds on the given tracks (default: all tracks). "
                       "Use before export_wav with scope=selection.",
        "inputSchema": _schema({
            "start": _TIME,
            "end": _TIME,
            "track_ids": {"type": "array", "items": _ID, "maxItems": 1024},
        }, ["start", "end"]),
    },
    {
        "name": "set_clip_gain",
        "method": "set_clip_gain",
        "description": "Set a clip's gain to a flat value in dB (replaces its gain envelope). One undo step. "
                       "Clip gain cannot exceed about +6 dB.",
        "inputSchema": _schema({
            "track_id": _ID,
            "clip_id": _ID,
            "gain_db": {"type": "number", "minimum": -60, "maximum": 6},
        }, ["track_id", "clip_id", "gain_db"]),
    },
    {
        "name": "add_label",
        "method": "add_label",
        "description": "Add a label from start to end (seconds; end defaults to start) with optional text. Uses "
                       "track_id if given, else the first label track, creating one if needed. One undo step.",
        "inputSchema": _schema({
            "start": _TIME,
            "end": _TIME,
            "text": {"type": "string", "maxLength": 1000},
            "track_id": _ID,
        }, ["start"]),
    },
    {
        "name": "undo",
        "method": "undo",
        "description": "Undo the most recent project change (the same as Edit > Undo).",
        "inputSchema": _schema({}),
        "annotations": {"destructiveHint": True},
    },
    {
        "name": "export_wav",
        "method": "export_wav",
        "description": "Export the whole project or the current time selection to a WAV file. Defaults keep "
                       "fidelity: the project sample rate, 32-bit float, and stereo whenever any exported track is "
                       "stereo or panned (mono only when everything is mono and centred). Refuses to replace an "
                       "existing file unless overwrite is true.",
        "inputSchema": _schema({
            "path": {"type": "string", "description": "Absolute output path ending in .wav."},
            "scope": {"type": "string", "enum": ["project", "selection"], "default": "project"},
            "encoding": {"type": "string", "enum": ["float32", "pcm24", "pcm16"], "default": "float32"},
            "channels": {"type": "string", "enum": ["auto", "mono", "stereo"], "default": "auto"},
            "sample_rate": {"type": "integer", "minimum": 8000, "maximum": 384000,
                            "description": "Output rate in Hz. Default: the project rate."},
            "overwrite": {"type": "boolean", "default": False},
        }, ["path"]),
        "annotations": {"destructiveHint": True},
    },
]

TOOLS_BY_NAME = {t["name"]: t for t in TOOLS}


def public_tool(tool: Dict[str, Any]) -> Dict[str, Any]:
    return {k: v for k, v in tool.items() if k != "method"}


# ---------------------------------------------------------------------------
# MCP stdio server
# ---------------------------------------------------------------------------

class McpServer:
    def __init__(self, bridge: BridgeClient, out=None):
        self.bridge = bridge
        self.out = out or sys.stdout
        self.protocol_version = SUPPORTED_PROTOCOL_VERSIONS[0]

    def send(self, message: Dict[str, Any]) -> None:
        self.out.write(json.dumps(message, separators=(",", ":")) + "\n")
        self.out.flush()

    def reply(self, msg_id: Any, result: Any) -> None:
        self.send({"jsonrpc": "2.0", "id": msg_id, "result": result})

    def reply_error(self, msg_id: Any, code: int, message: str) -> None:
        self.send({"jsonrpc": "2.0", "id": msg_id, "error": {"code": code, "message": message}})

    def handle_line(self, line: str) -> None:
        line = line.strip()
        if not line:
            return
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            self.reply_error(None, -32700, "parse error")
            return
        if isinstance(message, list):
            # JSON-RPC batches are not part of current MCP; answer each entry.
            for entry in message:
                self.handle_message(entry)
            return
        self.handle_message(message)

    def handle_message(self, message: Any) -> None:
        if not isinstance(message, dict) or message.get("jsonrpc") != "2.0":
            self.reply_error(None, -32600, "invalid request")
            return
        method = message.get("method")
        msg_id = message.get("id")
        is_request = "id" in message
        if not isinstance(method, str):
            if is_request:  # a response to something we never sent; ignore
                return
            return
        params = message.get("params") or {}
        if not isinstance(params, dict):
            if is_request:
                self.reply_error(msg_id, -32602, "params must be an object")
            return

        if not is_request:
            return  # notifications (initialized, cancelled, ...) need no answer

        try:
            if method == "initialize":
                requested = params.get("protocolVersion")
                if requested in SUPPORTED_PROTOCOL_VERSIONS:
                    self.protocol_version = requested
                self.reply(msg_id, {
                    "protocolVersion": self.protocol_version,
                    "capabilities": {"tools": {"listChanged": False}},
                    "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
                    "instructions": "Controls a running Audacity 4 project through its local bridge. "
                                    "Times are in seconds; ids come from list_tracks. Edits are undoable.",
                })
            elif method == "ping":
                self.reply(msg_id, {})
            elif method == "tools/list":
                self.reply(msg_id, {"tools": [public_tool(t) for t in TOOLS]})
            elif method == "tools/call":
                self.reply(msg_id, self.call_tool(params))
            else:
                self.reply_error(msg_id, -32601, f"method not found: {method}")
        except Exception as e:  # keep the server alive whatever happens
            self.reply_error(msg_id, -32603, f"internal error: {e}")

    def call_tool(self, params: Dict[str, Any]) -> Dict[str, Any]:
        name = params.get("name")
        tool = TOOLS_BY_NAME.get(name) if isinstance(name, str) else None
        if tool is None:
            return self.tool_error(f"unknown tool: {name}")
        arguments = params.get("arguments") or {}
        if not isinstance(arguments, dict):
            return self.tool_error("arguments must be an object")
        allowed = tool["inputSchema"]["properties"].keys()
        unknown = sorted(set(arguments) - set(allowed))
        if unknown:
            return self.tool_error(f"unknown argument(s): {', '.join(unknown)}")
        try:
            result = self.bridge.call(tool["method"], arguments)
        except BridgeError as e:
            return self.tool_error(str(e))
        except (OSError, ValueError) as e:
            self.bridge.close()
            return self.tool_error(f"bridge failure: {e}")
        out = {"content": [{"type": "text", "text": json.dumps(result, indent=2, sort_keys=True)}], "isError": False}
        if self.protocol_version >= "2025-06-18" and isinstance(result, dict):
            out["structuredContent"] = result
        return out

    @staticmethod
    def tool_error(text: str) -> Dict[str, Any]:
        return {"content": [{"type": "text", "text": text}], "isError": True}

    def serve(self, stream=None) -> None:
        stream = stream or sys.stdin
        for line in stream:
            self.handle_line(line)


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    if argv and argv[0] == "--check":
        # Quick manual check: connect, authenticate and print bridge status.
        client = BridgeClient()
        try:
            print(json.dumps(client.call("ping"), indent=2))
            return 0
        except BridgeError as e:
            print(str(e), file=sys.stderr)
            return 1
        finally:
            client.close()
    server = McpServer(BridgeClient())
    try:
        server.serve()
    finally:
        server.bridge.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
