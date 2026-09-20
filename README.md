# The Darkness Recomp

A native Windows x64 port of **The Darkness (Xbox 360)**. The 360 executable is
translated to C++ ahead of time and built into `DarkRecomp.exe` with a D3D11
renderer — no emulation at runtime.

> **Bring your own game dump.** This repo contains no game code, assets, or
> binaries. Dump your Xbox 360 copy and drop the files into `Darkness/`
> (step 1 below).

Playable today: full gameplay, mouse look + controller input, video settings
(resolution, FOV, gamma, bloom, frame cap, VSync), XMA audio, and saves.

## Download and play

**[Download the Windows release](https://github.com/portingpete/The-Darkness-Recomp/releases).**
Choose `The-Darkness-Recomp-v...-windows-x64.zip` under **Assets**.
The GitHub **Source code** downloads are for building the project yourself.

1. Right-click the Windows ZIP and choose **Extract All**.
2. Copy your own extracted Xbox 360 game dump into the included **Darkness**
   folder: `_uncrypted.xex`, `basefile.exe`, `default.xex`, `Content`, `System`,
   and all the other files and folders from your dump.
3. Double-click **Launch.cmd** to play with sound.

No compiler, Python, or separate audio setup is needed for the Windows release.
Use 64-bit Windows 10/11 with a Direct3D 11-capable graphics device.
See [START_HERE.txt](START_HERE.txt) for the folder layout and troubleshooting.
An ISO alone is not enough; the dump must include the decrypted executable
images for the supported game revision.

Click the game window to capture the mouse. **F1** shows controls, **F2** toggles
capture, and **Esc** releases it. Graphics options are in **Options > Video Settings**.

## Build from source

**1. Game files** — from your own dumped copy, fill in `Darkness/`:

```
Darkness/
  _uncrypted.xex            # decrypted executable
  basefile.exe              # raw extracted image used by analysis/tests
  default.xex
  Content/  Content_Eng/ .../ ExtraContent/
  System/                   # engine shaders and system data
```

`darkness_switch_tables.toml` already ships with the repo — keep it and add
the rest from your dump. Nothing else under `Darkness/` is committed.

**2. Build** — one step (dependency setup + generator + translation + XMA codec + compile + tests):

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1
```

The build downloads the pinned XenonRecomp source and applies the bundled
Darkness patches automatically. Existing checkouts from the old setup instructions
are supported too. The first build needs an internet connection.

You'll need 64-bit Windows, Git, Visual Studio 2022 with the **ClangCL** toolset,
CMake 3.24+, Python 3.11+, and ~15 GB free. Output lands in
`build_native/Release/`.

The XMA audio build also requires MSYS2 at `C:\msys64` with MinGW64 GCC and
`make`, plus standalone LLVM at `C:\Program Files\LLVM` (for `llvm-lib.exe`).

**3. Play** — double-click `Launch.cmd`:

| Command | What it does |
|---|---|
| `Launch.cmd` | Play with sound |
| `Launch.cmd mute` | Play muted |
| `Launch.cmd preview` | Engine preview build (muted by default) |
| `Launch.cmd stutter` | Play with sound while recording slow-frame timings |
| `Launch.cmd performance` / `render-profile` / `steady-60` | Diagnostic recording runs |
| `Launch.cmd help` | Full usage |

Extra arguments reach the game untouched, e.g. `Launch.cmd play --fps 120`.

Click the game window to capture the mouse (**F1** controls guide, **F2**
toggle capture, **Esc** release).

## Docs

- [CONTROLS.md](CONTROLS.md) — every binding, launcher option, and setting
- [RENDERING.md](RENDERING.md) — how the renderer and frame pacing work

## Updating a downloaded release

Close the game, extract the new Windows ZIP into your existing game folder,
and replace the included program files. Keep **Darkness/**, **saves/**, and
**DarkRecomp.settings.ini** to preserve your game files, progress, and settings.
You do not need to rebuild.

## Updating a source build

Pull the latest version and run the build again:

```powershell
git pull --ff-only
powershell -ExecutionPolicy Bypass -File tools\build.ps1
```

Keep your game dump and existing build directory. Unchanged generated files
retain their timestamps so the native build can reuse previous compilation work.

If an older setup stopped with `AOT gate: Generator failed (0)`, these same
commands install the missing generator patches and regenerate the output.

## Packaging a release

Maintainers can build, test, and package a Windows ZIP with:

```powershell
python tools/package_release.py --version v0.1.1
```

Run from a clean, committed checkout with the build prerequisites installed.
The ZIP and SHA-256 checksum land in `build_native/releases/`. The packager
includes the launcher, runtime dependencies, notices, and audio-library source;
game dumps, saves, settings, build tools, and diagnostic logs are excluded.

## Tests

The build already runs the full suite. To re-run it later:

```powershell
ctest --test-dir build_native -C Release --output-on-failure
```

## Project layout

| Path | Contents |
|---|---|
| `app/` | Windows entry points and display settings |
| `assets/` | Button-prompt artwork bundled with the game |
| `cmake/` | CMake helpers |
| `config/darkness.toml` | Translation configuration |
| `renderer/` | D3D11 backend and engine scene reconstruction |
| `runtime/` | Native kernel, audio, input, XMA, and filesystem bridges |
| `tools/` | Translator driver, `build.ps1`, shader/menu/XMA codegen |
| `tests/` | Native and contract tests run by CTest |

## License

GPLv3 — see [COPYING](COPYING), matching the upstream
[UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) /
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) toolchain this port builds on.
