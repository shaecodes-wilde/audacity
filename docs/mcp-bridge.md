# MCP bridge (opt-in)

Audacity can expose a small, fixed set of editing tools to local AI coding
agents (Codex, Claude Code, or any MCP client) through a local bridge. It is
**off unless you start Audacity with `AUDACITY_MCP_BRIDGE=1`**.

```
MCP client ──stdio──▶ tools/mcp/audacity_mcp.py ──owner-only local socket + token──▶ Audacity (GUI thread)
```

## What it can do

| MCP tool             | Bridge method   | Effect |
|----------------------|-----------------|--------|
| `bridge_status`      | `ping`          | Is the bridge up, is a project open |
| `get_project`        | `get_project`   | Title, path, sample rate, duration, track count, selection, next undo step |
| `list_tracks`        | `list_tracks`   | Tracks (mono/stereo/label, rate, format, mute/solo), clips (id, start, end, clip gain), labels |
| `import_wav`         | `import_wav`    | Import a `.wav` as new track(s); returns new track ids (undoable) |
| `set_time_selection` | `set_selection` | Select `[start, end)` seconds on some or all tracks |
| `set_clip_gain`      | `set_clip_gain` | Flat clip gain in dB, −60…+6 (one undo step) |
| `add_label`          | `add_label`     | Label with optional text and range (one undo step) |
| `undo`               | `undo`          | Edit › Undo |
| `export_wav`         | `export_wav`    | Export project or selection to WAV |

Nothing else is reachable: there is no generic "run action/command" method, no
effects, no saving or closing projects, and no file deletion.

`export_wav` defaults keep what is in the project:

* The **project sample rate** is the default. Pass `sample_rate` to resample.
* `channels: "auto"` = stereo whenever any exported track is stereo or panned, mono only
  when every exported track is mono and centred (the same rule Audacity uses for
  audio.com uploads). Set `"mono"` or `"stereo"` to choose the channel count explicitly.
* The default encoding is `float32`. PCM options are `pcm24` and `pcm16`.
* Use `scope: "project"` for the whole project or `"selection"` after calling `set_time_selection`.
* Set `overwrite: true` to replace an existing file after the render succeeds.

The "Trim blank space" export preference still applies to whole-project exports; the
result reports it as `trim_blank_space`.

## Security model

* **Opt-in:** without `AUDACITY_MCP_BRIDGE=1` nothing listens.
* **Owner-only directory:** `$AUDACITY_MCP_DIR`, else `<runtime dir>/audacity-mcp`
  (`$XDG_RUNTIME_DIR` on Linux). Audacity creates it with mode `0700` and refuses to
  start the bridge if it is a symlink, owned by someone else, or group/world accessible.
  The client applies the same checks.
* **Socket:** `<dir>/bridge.sock`, a local (Unix-domain) socket created with
  `QLocalServer::UserAccessOption`; no TCP port is opened.
* **Token:** a fresh 256-bit random token is written to `<dir>/token` (mode `0600`) on
  every start and removed on exit. The first request on a connection must be `auth`
  with that token; anything else closes the connection. Comparison is constant-time.
* **Validation:** every method has a fixed parameter schema (types, ranges, enums;
  unknown parameters rejected) checked before dispatch. Requests are capped at 64 KiB,
  four concurrent clients, ten seconds to authenticate.
* **GUI thread:** the socket lives on Audacity's main thread, so every edit runs there,
  one request at a time, through the same interfaces the UI uses (and lands in undo
  history).
* A lock allows one Audacity process to own each bridge directory. Use different
  `AUDACITY_MCP_DIR` values for side-by-side instances/profiles.
* If an editing request loses its connection, inspect the project before trying again.
  The client retries read-only requests automatically.

Platform: Linux and macOS. The Python client needs Unix-domain sockets; Windows is
untested.

## Start Audacity with the bridge

```sh
export AUDACITY_MCP_DIR="$XDG_RUNTIME_DIR/audacity-mcp"   # optional, but makes clients' config explicit
AUDACITY_MCP_BRIDGE=1 audacity
```

Open or create a project (most tools need one). Check it from a shell:

```sh
python3 tools/mcp/audacity_mcp.py --check
```

The bridge serves the first project window of the Audacity process.

## Claude Code

```sh
claude mcp add audacity \
  -e AUDACITY_MCP_DIR="$XDG_RUNTIME_DIR/audacity-mcp" \
  -- python3 /absolute/path/to/audacity/tools/mcp/audacity_mcp.py
```

or in a project's `.mcp.json`:

```json
{
  "mcpServers": {
    "audacity": {
      "command": "python3",
      "args": ["/absolute/path/to/audacity/tools/mcp/audacity_mcp.py"],
      "env": { "AUDACITY_MCP_DIR": "/run/user/1000/audacity-mcp" }
    }
  }
}
```

Tools appear as `mcp__audacity__get_project`, `mcp__audacity__export_wav`, and so on.

## Codex

```sh
codex mcp add audacity \
  --env AUDACITY_MCP_DIR="$XDG_RUNTIME_DIR/audacity-mcp" \
  -- python3 /absolute/path/to/audacity/tools/mcp/audacity_mcp.py
```

or in `~/.codex/config.toml`:

```toml
[mcp_servers.audacity]
command = "python3"
args = ["/absolute/path/to/audacity/tools/mcp/audacity_mcp.py"]
env = { AUDACITY_MCP_DIR = "/run/user/1000/audacity-mcp" }
```

Set `AUDACITY_MCP_DIR` explicitly for Codex: MCP servers it launches may not inherit
`XDG_RUNTIME_DIR`. Use the absolute runtime path (shown as `/run/user/1000` above; check
yours with `echo $XDG_RUNTIME_DIR`).

## Example session

```text
get_project                                   → sample_rate 48000, track_count 0
import_wav {"path": "/home/me/take1.wav"}     → new_track_ids [1]
list_tracks                                   → track 1 stereo, clip 3 spans 0–42.5 s
set_clip_gain {"track_id": 1, "clip_id": 3, "gain_db": -3}
add_label {"start": 12.0, "end": 14.5, "text": "breath"}
set_time_selection {"start": 10, "end": 20}
export_wav {"path": "/home/me/out/excerpt.wav", "scope": "selection"}
                                              → 48000 Hz, 2 channels, float32
undo                                          → "Add label" undone
```

## Tests

* C++ (transport, auth, validation): `mcpbridge_tests`, built with
  `-DAU_BUILD_MCPBRIDGE_TESTS=ON` (on by default with unit tests).
* Python client: `python3 -m unittest discover -s tools/mcp/tests -v`
* End-to-end against a built Audacity, with an isolated profile and generated
  48 kHz stereo audio:

  ```sh
  python3 tools/mcp/smoke_test.py --audacity /path/to/build/bin/audacity \
      --workdir ~/scratch/mcp-smoke
  ```

## Build switch

`AU_BUILD_MCPBRIDGE_MODULE` (CMake, default `ON`) compiles the module in. It still does
nothing at runtime without `AUDACITY_MCP_BRIDGE=1`.

If Qt 6.12 reports an ambiguous `Type` in generated QML C++, configure with
`-DMUSE_COMPILE_USE_UNITY=OFF` to compile those sources separately.

## Credits

Scope and tool ideas were informed by the community projects
[xDarkzx/Audacity4-Dev-MCP](https://github.com/xDarkzx/Audacity4-Dev-MCP) and
[xDarkzx/Audacity4-MCP](https://github.com/xDarkzx/Audacity4-MCP). This
implementation is new code for Audacity 4.0.2: it does not modify the Muse
framework's `rcontrol` MCP transport and uses its own authenticated local socket.
