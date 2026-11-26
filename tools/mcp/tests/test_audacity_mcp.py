# SPDX-License-Identifier: GPL-3.0-only
# Audacity: A Digital Audio Editor
"""Unit tests for tools/mcp/audacity_mcp.py against a fake bridge.

Run: python3 -m unittest discover -s tools/mcp/tests -v
"""

import io
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(os.path.dirname(HERE), "audacity_mcp.py")
sys.path.insert(0, os.path.dirname(HERE))

import audacity_mcp as m  # noqa: E402


class FakeBridge:
    """Minimal stand-in for Audacity's BridgeServer (same framing and auth rule)."""

    def __init__(self, directory, token="t" * 64):
        self.directory = directory
        self.token = token
        self.calls = []
        os.makedirs(directory, mode=0o700, exist_ok=True)
        os.chmod(directory, 0o700)
        self.write_token()
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        path = os.path.join(directory, "bridge.sock")
        if os.path.exists(path):
            os.unlink(path)
        self.sock.bind(path)
        self.sock.listen(4)
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def write_token(self):
        path = os.path.join(self.directory, "token")
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(self.token + "\n")
        os.chmod(path, 0o600)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def serve(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self.handle, args=(conn,), daemon=True).start()

    def handle(self, conn):
        buf = b""
        authed = False
        with conn:
            while True:
                try:
                    chunk = conn.recv(65536)
                except OSError:
                    return
                if not chunk:
                    return
                buf += chunk
                while b"\n" in buf:
                    line, _, buf = buf.partition(b"\n")
                    req = json.loads(line)
                    rid, method, params = req["id"], req["method"], req.get("params", {})
                    if not authed:
                        if method == "auth" and params.get("token") == self.token:
                            authed = True
                            self.send(conn, {"id": rid, "result": {"protocol": 1}})
                            continue
                        self.send(conn, {"id": rid, "error": {"code": "unauthenticated", "message": "bad token"}})
                        return
                    self.calls.append((method, params))
                    if params.get("disconnect"):
                        return
                    if method == "ping":
                        self.send(conn, {"id": rid, "result": {"pong": True, "project_open": True}})
                    elif method == "export_wav" and params.get("path", "").endswith(".mp3"):
                        self.send(conn, {"id": rid, "error": {"code": "invalid_params", "message": "must be .wav"}})
                    else:
                        self.send(conn, {"id": rid, "result": {"method": method, "params": params}})

    @staticmethod
    def send(conn, obj):
        conn.sendall((json.dumps(obj) + "\n").encode())


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=os.environ.get("XDG_RUNTIME_DIR"))
        self.dir = os.path.join(self.tmp.name, "b")
        self.bridge = FakeBridge(self.dir)
        self.client = m.BridgeClient(self.dir, timeout=5)

    def tearDown(self):
        self.client.close()
        self.bridge.close()
        self.tmp.cleanup()


class BridgeClientTests(Base):
    def test_auth_and_call(self):
        self.assertEqual(self.client.call("ping")["pong"], True)
        self.assertEqual(self.bridge.calls, [("ping", {})])

    def test_bridge_error_is_raised(self):
        with self.assertRaises(m.BridgeError) as cm:
            self.client.call("export_wav", {"path": "/x.mp3"})
        self.assertEqual(cm.exception.code, "invalid_params")

    def test_wrong_token(self):
        with open(os.path.join(self.dir, "token"), "w") as f:
            f.write("nope\n")
        with self.assertRaises(m.BridgeError) as cm:
            self.client.call("ping")
        self.assertEqual(cm.exception.code, "unauthenticated")

    def test_refuses_group_readable_dir(self):
        os.chmod(self.dir, 0o750)
        with self.assertRaises(m.BridgeError) as cm:
            self.client.call("ping")
        self.assertEqual(cm.exception.code, "insecure_bridge")

    def test_refuses_group_readable_token(self):
        os.chmod(os.path.join(self.dir, "token"), 0o640)
        with self.assertRaises(m.BridgeError) as cm:
            self.client.call("ping")
        self.assertEqual(cm.exception.code, "insecure_bridge")

    def test_refuses_symlinked_token(self):
        real = os.path.join(self.dir, "real-token")
        os.rename(os.path.join(self.dir, "token"), real)
        os.symlink(real, os.path.join(self.dir, "token"))
        with self.assertRaises(m.BridgeError) as cm:
            self.client.call("ping")
        self.assertEqual(cm.exception.code, "insecure_bridge")

    def test_not_running(self):
        client = m.BridgeClient(os.path.join(self.tmp.name, "missing"), timeout=1)
        with self.assertRaises(m.BridgeError) as cm:
            client.call("ping")
        self.assertEqual(cm.exception.code, "not_running")

    def test_reconnects_after_drop(self):
        self.client.call("ping")
        with self.assertRaises(m.BridgeError) as cm:
            self.client.call("ping", {"disconnect": True})
        self.assertEqual(cm.exception.code, "not_running")
        self.assertEqual(self.bridge.calls.count(("ping", {"disconnect": True})), 2)
        self.assertEqual(self.client.call("ping")["pong"], True)

    def test_disconnect_does_not_replay_edit(self):
        request = {"start": 0, "text": "One label", "disconnect": True}
        with self.assertRaises(m.BridgeError) as cm:
            self.client.call("add_label", request)
        self.assertEqual(cm.exception.code, "outcome_unknown")
        self.assertEqual(self.bridge.calls, [("add_label", request)])
        self.assertEqual(self.client.call("ping")["pong"], True)

    def test_default_dir_honours_env(self):
        old = os.environ.get("AUDACITY_MCP_DIR")
        os.environ["AUDACITY_MCP_DIR"] = "/x/y/../z"
        try:
            self.assertEqual(m.default_bridge_dir(), "/x/z")
        finally:
            if old is None:
                del os.environ["AUDACITY_MCP_DIR"]
            else:
                os.environ["AUDACITY_MCP_DIR"] = old


class McpServerTests(Base):
    def setUp(self):
        super().setUp()
        self.out = io.StringIO()
        self.server = m.McpServer(self.client, out=self.out)

    def roundtrip(self, *messages):
        for msg in messages:
            self.server.handle_line(json.dumps(msg))
        lines = [json.loads(l) for l in self.out.getvalue().splitlines() if l]
        self.out.seek(0)
        self.out.truncate()
        return lines

    def test_initialize_and_list(self):
        init, = self.roundtrip({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                                "params": {"protocolVersion": "2025-03-26", "capabilities": {},
                                           "clientInfo": {"name": "t", "version": "0"}}})
        self.assertEqual(init["result"]["protocolVersion"], "2025-03-26")
        self.assertIn("tools", init["result"]["capabilities"])
        self.assertEqual(self.roundtrip({"jsonrpc": "2.0", "method": "notifications/initialized"}), [])
        listed, = self.roundtrip({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        names = [t["name"] for t in listed["result"]["tools"]]
        self.assertEqual(names, ["bridge_status", "get_project", "list_tracks", "import_wav", "set_time_selection",
                                 "set_clip_gain", "add_label", "undo", "export_wav"])
        for t in listed["result"]["tools"]:
            self.assertNotIn("method", t)
            self.assertFalse(t["inputSchema"]["additionalProperties"])

    def test_unknown_protocol_version_gets_latest(self):
        init, = self.roundtrip({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "1999"}})
        self.assertEqual(init["result"]["protocolVersion"], m.SUPPORTED_PROTOCOL_VERSIONS[0])

    def test_tool_call_maps_to_bridge_method(self):
        self.roundtrip({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18"}})
        res, = self.roundtrip({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                               "params": {"name": "set_time_selection", "arguments": {"start": 1, "end": 2}}})
        self.assertFalse(res["result"]["isError"])
        self.assertEqual(self.bridge.calls[-1], ("set_selection", {"start": 1, "end": 2}))
        self.assertEqual(res["result"]["structuredContent"]["method"], "set_selection")

    def test_old_protocol_has_no_structured_content(self):
        self.roundtrip({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2024-11-05"}})
        res, = self.roundtrip({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "bridge_status"}})
        self.assertNotIn("structuredContent", res["result"])

    def test_bridge_error_becomes_tool_error(self):
        res, = self.roundtrip({"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                               "params": {"name": "export_wav", "arguments": {"path": "/a.mp3"}}})
        self.assertTrue(res["result"]["isError"])
        self.assertIn("invalid_params", res["result"]["content"][0]["text"])

    def test_unknown_tool_and_arguments_never_reach_bridge(self):
        a, = self.roundtrip({"jsonrpc": "2.0", "id": 5, "method": "tools/call", "params": {"name": "run_shell"}})
        b, = self.roundtrip({"jsonrpc": "2.0", "id": 6, "method": "tools/call",
                             "params": {"name": "undo", "arguments": {"steps": 99}}})
        self.assertTrue(a["result"]["isError"])
        self.assertTrue(b["result"]["isError"])
        self.assertEqual(self.bridge.calls, [])

    def test_protocol_errors(self):
        self.server.handle_line("{not json")
        out = [json.loads(l) for l in self.out.getvalue().splitlines()]
        self.assertEqual(out[-1]["error"]["code"], -32700)
        self.out.seek(0)
        self.out.truncate()
        unknown, = self.roundtrip({"jsonrpc": "2.0", "id": 7, "method": "resources/list"})
        self.assertEqual(unknown["error"]["code"], -32601)
        ping, = self.roundtrip({"jsonrpc": "2.0", "id": 8, "method": "ping"})
        self.assertEqual(ping["result"], {})


class StdioProcessTests(unittest.TestCase):
    def test_real_stdio_enumeration_without_audacity(self):
        with tempfile.TemporaryDirectory() as d:
            env = dict(os.environ, AUDACITY_MCP_DIR=os.path.join(d, "absent"))
            msgs = [
                {"jsonrpc": "2.0", "id": 1, "method": "initialize",
                 "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "t", "version": "0"}}},
                {"jsonrpc": "2.0", "method": "notifications/initialized"},
                {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
                {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "get_project", "arguments": {}}},
            ]
            proc = subprocess.run([sys.executable, "-I", SCRIPT], input="".join(json.dumps(x) + "\n" for x in msgs),
                                  capture_output=True, text=True, env=env, timeout=30)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            out = [json.loads(l) for l in proc.stdout.splitlines()]
            self.assertEqual([o["id"] for o in out], [1, 2, 3])
            self.assertEqual(len(out[1]["result"]["tools"]), 9)
            self.assertTrue(out[2]["result"]["isError"])
            self.assertIn("not_running", out[2]["result"]["content"][0]["text"])


if __name__ == "__main__":
    unittest.main()
