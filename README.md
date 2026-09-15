# The Darkness Recomp

Native Windows x64 port of **The Darkness (Xbox 360)** via static recompilation (AOT).
The 360 PowerPC executable is translated to C++ by the XenonRecomp toolchain and
built into a native `DarkRecomp.exe` with a D3D11 renderer — no emulation at runtime.

> **You must provide your own game dump.** This repository contains no game code,
> assets, or binaries. Dump your Xbox 360 copy and place the files under
> `Darkness/` as described below.

## Status

Playable native port: gameplay, mouse look + controller input, video settings
(resolution, FOV, gamma, bloom, frame cap, VSync), XMA audio, and saves.
See [CONTROLS.md](CONTROLS.md) and [RENDERING.md](RENDERING.md) for details.

## Requirements

- 64-bit Windows, Visual Studio 2022 (x64) with the **ClangCL** toolset
- CMake 3.24+, Python 3.11+
- ~15 GB free (build tree + dependencies)

## Game files (user-provided)

From your own dumped copy, populate `Darkness/` so it contains at least:

```
Darkness/
  _uncrypted.xex            # decrypted executable
  basefile.exe              # raw extracted image used by analysis/tests
  default.xex
  Content/  Content_Eng/ .../ ExtraContent/
  System/                   # engine shaders and system data
```

`Darkness/darkness_switch_tables.toml` (jump-table analysis) is tracked in this
repo — keep it and add the rest from your dump. Nothing else under `Darkness/`
is committed.

## Reference dependency (build-time)

The AOT generator comes from upstream XenonRecomp. Before building:

```powershell
git clone https://github.com/hedge-dev/UnleashedRecomp.git refs/UnleashedRecomp
git -C refs/UnleashedRecomp checkout 5e8695a157ce9d2a783944d63439cc8c76a38fc2
git -C refs/UnleashedRecomp submodule update --init tools/XenonRecomp
```

`refs/` is intentionally not committed (see `.gitignore`).

## Build

One-step build (generator + AOT regeneration + XMA codec + native compile + tests):

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1
```

Output lands in `build_native/Release/` (`DarkRecomp.exe`, `DarkRecompPreview.exe`).

## Run

`Launch.cmd` plays with audio. Other modes:

- `Launch.cmd mute` — muted
- `Launch.cmd preview` — engine preview / diagnostics
- `Launch.cmd steady-60`, `performance`, `render-profile`, `stutter` — diagnostics
- `Launch.cmd help` — full usage

Extra arguments are forwarded to the game, e.g. `Launch.cmd play --fps 120`.

Click the game window to capture the mouse (**F1** controls guide,
**F2** toggle capture, **Esc** release). Full bindings: [CONTROLS.md](CONTROLS.md).
Graphics internals: [RENDERING.md](RENDERING.md).

## Tests

```powershell
ctest --test-dir build_native -C Release --output-on-failure
```

`tools/build.ps1` already runs the full suite after compiling.

## Layout

| Path | Contents |
|---|---|
| `app/` | Windows entry points, launchers, display settings |
| `assets/` | Prompt artwork sources and bundled assets |
| `cmake/` | CMake helpers (native compilation stamp) |
| `config/darkness.toml` | Canonical recomp configuration |
| `renderer/` | D3D11 backend + engine scene/world reconstruction |
| `runtime/` | Native kernel/dispatcher/filesystem/audio/input/XMA bridge |
| `tools/` | `recompile.py`, `build.ps1`, shader/menu/XMA codegen |
| `tests/` | Native + contract tests run by CTest |

## License

GPLv3 — see [COPYING](COPYING), matching the upstream
[UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) /
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) toolchain this port builds on.
