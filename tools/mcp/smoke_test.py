#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Audacity: A Digital Audio Editor
"""End-to-end smoke test for the MCP bridge against a real Audacity build.

Launches the given Audacity executable with an isolated profile (XDG dirs and
bridge dir inside --workdir), opens a new project, and drives it through the
bridge and through the MCP stdio server:

  * imports a generated 48 kHz stereo WAV whose channels differ (440 Hz left,
    1 kHz right, different levels) plus a mono WAV,
  * checks project/track/clip inspection,
  * exports the project and a time selection as float WAV and compares samples
    with the source (rate stays 48 kHz, stereo stays stereo, L/R not swapped),
  * applies clip gain, verifies it in the export, undoes it, verifies again,
  * adds a label and undoes it,
  * checks error paths (overwrite refusal, missing file, unknown method,
    unauthenticated connection).

Usage:
  python3 tools/mcp/smoke_test.py --audacity /path/to/build/bin/audacity \
      --workdir ~/.t3/scratch/mcp-smoke [--offscreen] [--keep]

Or against an Audacity you started yourself (with AUDACITY_MCP_BRIDGE=1 and a
new empty project open):
  AUDACITY_MCP_DIR=... python3 tools/mcp/smoke_test.py --attach --workdir DIR
"""

from __future__ import annotations

import argparse
import array
import json
import math
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from audacity_mcp import BridgeClient, BridgeError  # noqa: E402

RATE = 48000
SECONDS = 2.0
FRAMES = int(RATE * SECONDS)
LEFT = (440.0, 0.5, 0.0)     # Hz, amplitude, phase
RIGHT = (1000.0, 0.25, 0.3)
MONO = (220.0, 0.2, 0.0)


# ---------------------------------------------------------------------------
# WAV helpers (stdlib only)
# ---------------------------------------------------------------------------

def q24(x: float) -> int:
    return max(-(1 << 23), min((1 << 23) - 1, int(round(x * (1 << 23)))))


def tone(spec, n):
    f, a, ph = spec
    return [q24(a * math.sin(2 * math.pi * f * i / RATE + ph)) for i in range(n)]


def write_wav24(path, channels):
    n = len(channels[0])
    frames = bytearray()
    for i in range(n):
        for ch in channels:
            frames += struct.pack("<i", ch[i])[:3]
    with wave.open(path, "wb") as w:
        w.setnchannels(len(channels))
        w.setsampwidth(3)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))


def read_wav(path):
    """Returns (rate, channels, list of per-channel float lists). Handles PCM 16/24/32 and float32."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise AssertionError(f"{path} is not a RIFF/WAVE file")
    pos, fmt, payload = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = body
        elif cid == b"data":
            payload = body
        pos += 8 + size + (size & 1)
    tag, nch, rate, _, _, bits = struct.unpack("<HHIIHH", fmt[:16])
    if tag == 0xFFFE:
        tag = struct.unpack("<H", fmt[24:26])[0]
    width = bits // 8
    count = len(payload) // width
    if tag == 3 and bits == 32:
        samples = array.array("f")
        samples.frombytes(payload[:count * 4])
        if sys.byteorder != "little":
            samples.byteswap()
        values = list(samples)
    elif tag == 1:
        scale = float(1 << (bits - 1))
        values = [int.from_bytes(payload[i:i + width], "little", signed=True) / scale
                  for i in range(0, count * width, width)]
    else:
        raise AssertionError(f"unsupported WAV format tag {tag} / {bits} bits")
    chans = [values[c::nch] for c in range(nch)]
    return rate, nch, chans


def max_diff(a, b):
    return max(abs(x - y) for x, y in zip(a, b))


# ---------------------------------------------------------------------------
# Harness
# ---------------------------------------------------------------------------

class Smoke:
    def __init__(self, args):
        self.args = args
        self.work = os.path.abspath(os.path.expanduser(args.workdir))
        self.bridge_dir = os.environ.get("AUDACITY_MCP_DIR") if args.attach else os.path.join(self.work, "bridge")
        self.proc = None
        self.client = None
        self.results = []

    def check(self, name, cond, detail=""):
        self.results.append({"check": name, "ok": bool(cond), "detail": detail})
        print(("PASS " if cond else "FAIL ") + name + (f" — {detail}" if detail else ""), flush=True)
        if not cond and self.args.fail_fast:
            raise SystemExit(1)

    def launch(self):
        os.makedirs(self.work, exist_ok=True)
        profile = os.path.join(self.work, "profile")
        env = dict(os.environ)
        for var, sub in (("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                         ("XDG_CACHE_HOME", "cache"), ("XDG_STATE_HOME", "state")):
            env[var] = os.path.join(profile, sub)
            os.makedirs(env[var], exist_ok=True)
        # Audacity keeps the configured project rate when importing a track.
        config_dir = os.path.join(env["XDG_CONFIG_HOME"], "Audacity")
        os.makedirs(config_dir, exist_ok=True)
        for app_name in ("Audacity4", "Audacity4Development"):
            with open(os.path.join(config_dir, f"{app_name}.ini"), "w", encoding="utf-8") as config:
                config.write(f"[SamplingRate]\nDefaultProjectSampleRate={RATE}\n")
        env["AUDACITY_MCP_BRIDGE"] = "1"
        env["AUDACITY_MCP_DIR"] = self.bridge_dir
        if self.args.offscreen:
            env["AU_QT_QPA_PLATFORM"] = "offscreen"
            env["QT_QPA_PLATFORM"] = "offscreen"
        log = open(os.path.join(self.work, "audacity.log"), "wb")
        self.proc = subprocess.Popen([self.args.audacity, "--session-type", "start-with-new"],
                                     env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)

    def connect(self):
        deadline = time.time() + self.args.startup_timeout
        token = os.path.join(self.bridge_dir, "token")
        while time.time() < deadline:
            if self.proc is not None and self.proc.poll() is not None:
                raise SystemExit(f"Audacity exited early with code {self.proc.returncode}; see {self.work}/audacity.log")
            if os.path.exists(token):
                break
            time.sleep(0.5)
        self.client = BridgeClient(self.bridge_dir, timeout=120)
        while time.time() < deadline:
            try:
                if self.client.call("ping").get("project_open"):
                    return
            except BridgeError:
                self.client.close()
            time.sleep(0.5)
        raise SystemExit("bridge did not report an open project in time")

    def stop(self):
        if self.client:
            self.client.close()
        if self.proc and self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait()

    def call(self, method, **params):
        return self.client.call(method, params)

    def expect_error(self, name, code, method, **params):
        try:
            self.call(method, **params)
        except BridgeError as e:
            self.check(name, e.code == code, f"{e.code}: {e.message}")
            return
        self.check(name, False, "call unexpectedly succeeded")

    def export(self, name, **params):
        path = os.path.join(self.work, name)
        if os.path.exists(path):
            os.unlink(path)
        info = self.call("export_wav", path=path, **params)
        return info, read_wav(path)

    def run(self):
        os.makedirs(self.work, exist_ok=True)
        src = os.path.join(self.work, "stereo-48k.wav")
        mono_src = os.path.join(self.work, "mono-48k.wav")
        left, right = tone(LEFT, FRAMES), tone(RIGHT, FRAMES)
        write_wav24(src, [left, right])
        write_wav24(mono_src, [tone(MONO, FRAMES)])
        left_f = [v / (1 << 23) for v in left]
        right_f = [v / (1 << 23) for v in right]

        if not self.args.attach:
            self.launch()
        self.connect()

        # --- auth boundary -------------------------------------------------------
        raw = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        raw.settimeout(5)
        raw.connect(os.path.join(self.bridge_dir, "bridge.sock"))
        raw.sendall(b'{"id":1,"method":"get_project","params":{}}\n')
        reply = json.loads(raw.recv(65536).split(b"\n")[0])
        self.check("unauthenticated request rejected", reply.get("error", {}).get("code") == "unauthenticated")
        raw.close()
        self.expect_error("unknown method rejected", "unknown_method", "run_action")

        project = self.call("get_project")
        self.check("project is open and empty", project["track_count"] == 0, json.dumps(project))

        # --- import ------------------------------------------------------------
        imported = self.call("import_wav", path=src)
        self.check("import created one track", len(imported["new_track_ids"]) == 1, json.dumps(imported))
        stereo_id = imported["new_track_ids"][0]
        self.expect_error("missing import file rejected", "not_found", "import_wav", path=os.path.join(self.work, "nope.wav"))

        project = self.call("get_project")
        self.check("project kept its configured 48 kHz", project["sample_rate"] == RATE, str(project["sample_rate"]))
        self.check("duration is 2 s", abs(project["duration"] - SECONDS) < 1e-3, str(project["duration"]))

        tracks = self.call("list_tracks")["tracks"]
        track = next(t for t in tracks if t["id"] == stereo_id)
        self.check("track is stereo 48 kHz", track["type"] == "stereo" and track["channels"] == 2
                   and track["sample_rate"] == RATE, json.dumps(track))
        self.check("track has one clip", len(track["clips"]) == 1, json.dumps(track["clips"]))
        clip = track["clips"][0]
        self.check("clip spans 0..2 s", abs(clip["start"]) < 1e-6 and abs(clip["end"] - SECONDS) < 1e-3, json.dumps(clip))
        self.check("clip starts at unity gain", clip["gain_db"] == 0, json.dumps(clip))

        # --- full export: fidelity and channel intent --------------------------------
        info, (rate, nch, chans) = self.export("project.wav")
        self.check("project export keeps 48 kHz", rate == RATE and info["sample_rate"] == RATE, f"{rate}")
        self.check("project export keeps stereo", nch == 2 and info["channels"] == 2, f"{nch}")
        self.check("project export length", len(chans[0]) == FRAMES, f"{len(chans[0])}")
        dl, dr = max_diff(chans[0], left_f), max_diff(chans[1], right_f)
        self.check("left channel matches source left", dl < 1e-5, f"max diff {dl:.3g}")
        self.check("right channel matches source right", dr < 1e-5, f"max diff {dr:.3g}")
        self.check("channels are not identical", max_diff(chans[0], chans[1]) > 0.1)

        self.expect_error("existing export not overwritten", "failed", "export_wav",
                          path=os.path.join(self.work, "project.wav"))
        self.expect_error("relative export path rejected", "invalid_params", "export_wav", path="relative.wav")

        # --- selection export ---------------------------------------------------------
        sel = self.call("set_selection", start=0.5, end=1.5, track_ids=[stereo_id])["selection"]
        self.check("selection set", abs(sel["start"] - 0.5) < 1e-9 and abs(sel["end"] - 1.5) < 1e-9
                   and sel["track_ids"] == [stereo_id], json.dumps(sel))
        info, (rate, nch, chans) = self.export("selection.wav", scope="selection")
        n = len(chans[0])
        self.check("selection export keeps 48 kHz stereo", rate == RATE and nch == 2, f"{rate} Hz, {nch} ch")
        self.check("selection export is 1 s", abs(n - RATE) <= 1, str(n))
        off = RATE // 2
        dl = max_diff(chans[0], left_f[off:off + n])
        dr = max_diff(chans[1], right_f[off:off + n])
        self.check("selection matches source slice", dl < 1e-5 and dr < 1e-5, f"{dl:.3g} / {dr:.3g}")

        # --- clip gain + undo ------------------------------------------------------------
        gain = self.call("set_clip_gain", track_id=stereo_id, clip_id=clip["id"], gain_db=-6.0206)
        self.check("clip gain set", abs(gain["gain_linear"] - 0.5) < 1e-4, json.dumps(gain))
        _, (_, _, chans) = self.export("project.wav", overwrite=True)
        dl = max_diff(chans[0], [0.5 * v for v in left_f])
        dr = max_diff(chans[1], [0.5 * v for v in right_f])
        self.check("explicit overwrite reflects -6 dB clip gain", dl < 1e-4 and dr < 1e-4, f"{dl:.3g} / {dr:.3g}")
        undone = self.call("undo")
        self.check("undo reverted clip gain", "undone" in undone, json.dumps(undone))
        _, (_, _, chans) = self.export("undo.wav")
        self.check("export after undo matches source", max_diff(chans[0], left_f) < 1e-5
                   and max_diff(chans[1], right_f) < 1e-5)

        # --- labels ------------------------------------------------------------------------
        label = self.call("add_label", start=0.25, end=0.75, text="intro")
        self.check("label added", abs(label["start"] - 0.25) < 1e-9 and abs(label["end"] - 0.75) < 1e-9
                   and label["text"] == "intro", json.dumps(label))
        labels = [l for t in self.call("list_tracks")["tracks"] if t["type"] == "label" for l in t.get("labels", [])]
        self.check("label listed", any(l["text"] == "intro" for l in labels), json.dumps(labels))
        self.call("undo")
        labels = [l for t in self.call("list_tracks")["tracks"] if t["type"] == "label" for l in t.get("labels", [])]
        self.check("undo removed label", not any(l["text"] == "intro" for l in labels), json.dumps(labels))

        # --- explicit options --------------------------------------------------------------
        _, (rate, nch, chans) = self.export("pcm24-44k.wav", encoding="pcm24", sample_rate=44100)
        self.check("explicit 44.1 kHz honoured", rate == 44100 and nch == 2 and abs(len(chans[0]) - 88200) <= 2,
                   f"{rate} Hz, {nch} ch, {len(chans[0])} frames")
        _, (rate, nch, _) = self.export("forced-mono.wav", channels="mono")
        self.check("explicit mono honoured", nch == 1 and rate == RATE, f"{nch} ch")

        # --- mixed mono + stereo: auto must stay stereo ---------------------------------------
        mono_ids = self.call("import_wav", path=mono_src)["new_track_ids"]
        mono_track = next(t for t in self.call("list_tracks")["tracks"] if t["id"] in mono_ids)
        self.check("mono file imported as mono", mono_track["type"] == "mono", json.dumps(mono_track))
        _, (rate, nch, _) = self.export("mixed.wav")
        self.check("auto keeps stereo with a mono track present", nch == 2 and rate == RATE, f"{nch} ch, {rate} Hz")
        self.call("set_selection", start=0.0, end=1.0, track_ids=mono_ids)
        info, (rate, nch, _) = self.export("mono-selection.wav", scope="selection")
        self.check("auto exports a mono-only selection as mono", nch == 1 and rate == RATE, f"{nch} ch")

        # --- through the MCP stdio server -----------------------------------------------------
        msgs = [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
                "protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "smoke", "version": "0"}}},
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
            {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "get_project", "arguments": {}}},
        ]
        proc = subprocess.run([sys.executable, "-I", os.path.join(HERE, "audacity_mcp.py")],
                              input="".join(json.dumps(x) + "\n" for x in msgs), capture_output=True, text=True,
                              env=dict(os.environ, AUDACITY_MCP_DIR=self.bridge_dir), timeout=60)
        out = [json.loads(l) for l in proc.stdout.splitlines() if l.strip()]
        ok = len(out) == 3 and not out[2]["result"]["isError"] \
            and out[2]["result"]["structuredContent"]["sample_rate"] == RATE
        self.check("MCP stdio get_project", ok, proc.stdout[-500:] + proc.stderr[-500:])

    def summary(self):
        failed = [r for r in self.results if not r["ok"]]
        report = {"passed": len(self.results) - len(failed), "failed": len(failed), "results": self.results}
        with open(os.path.join(self.work, "smoke-report.json"), "w") as f:
            json.dump(report, f, indent=2)
        print(f"\n{report['passed']} passed, {report['failed']} failed; report: {self.work}/smoke-report.json")
        return 0 if not failed and self.results else 1


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--audacity", help="path to the Audacity executable to launch")
    p.add_argument("--attach", action="store_true", help="use a running Audacity (AUDACITY_MCP_DIR must be set)")
    p.add_argument("--workdir", default=None, help="scratch directory (default: a new temp dir)")
    p.add_argument("--offscreen", action="store_true", help="run Qt with the offscreen platform")
    p.add_argument("--startup-timeout", type=float, default=180)
    p.add_argument("--fail-fast", action="store_true")
    p.add_argument("--keep", action="store_true", help="keep the work directory")
    args = p.parse_args()
    if not args.attach and not args.audacity:
        p.error("--audacity is required unless --attach is given")
    if args.attach and not os.environ.get("AUDACITY_MCP_DIR"):
        p.error("--attach needs AUDACITY_MCP_DIR")
    created = args.workdir is None
    if created:
        args.workdir = tempfile.mkdtemp(prefix="audacity-mcp-smoke-")

    smoke = Smoke(args)
    try:
        smoke.run()
    except (BridgeError, AssertionError, OSError, KeyError, StopIteration) as e:
        smoke.check("smoke run completed", False, f"{type(e).__name__}: {e}")
    finally:
        smoke.stop()
    code = smoke.summary()
    if created and not args.keep and code == 0:
        shutil.rmtree(args.workdir, ignore_errors=True)
    return code


if __name__ == "__main__":
    sys.exit(main())
