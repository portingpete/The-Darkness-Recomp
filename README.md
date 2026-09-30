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
   folder: the original `default.xex`, `Content`, `System`, and all the other
   files and folders from your dump. No XexTool preparation is required.
3. Double-click **Launch.cmd** to play with sound, or
   **LaunchWithSettings.cmd** to choose video settings before playing.

No compiler, Python, or separate audio setup is needed for the Windows release.
Use 64-bit Windows 10/11 with a Direct3D 11-capable graphics device.
See [START_HERE.txt](START_HERE.txt) for the folder layout and troubleshooting.
An ISO alone is not enough; use an extracted dump of the supported game revision.
The port decodes `default.xex` in memory at startup; `_uncrypted.xex` and
`basefile.exe` are no longer required.

Click the game window to capture the mouse. **F1** shows controls, **F2** toggles
capture, and **Esc** releases it. Graphics options are in **Options > Video Settings**
and in the video settings launcher. Both use the same saved settings.
The mouse wheel selects dialogue and menu choices; **E** confirms.
Press **F5** to open developer tools for mission selection, player speed,
and invincibility. Mission loading can autosave; see
[CONTROLS.md](CONTROLS.md#developer-tools) before selecting a destination.
Choose **Language** in **Options > Video Settings** or the settings launcher:
System default, English, German, French, Spanish, or Italian. In-game changes
take effect after restarting. See [CONTROLS.md](CONTROLS.md) for language overrides
and the full input guide.

## Steam Deck and Linux

The complete Windows release can be used for a Proton test setup. Steam Deck
gameplay and Linux compatibility have not been verified for this release.
See [STEAM_DECK.md](STEAM_DECK.md) for the Steam shortcut, bundled dependencies,
controller setup, and diagnostic logs. A native Linux build is not available.

## Other games and title updates

This executable ports **The Darkness**, using translated code and engine
integration for its supported Xbox 360 revision. It cannot load another Xbox
game by replacing the files or changing a hash. Supporting a different title
requires its own translation configuration, runtime integration, renderer,
and testing; the upstream XenonRecomp toolchain is a starting point for that
work. The pinned hashes below also apply to The Darkness title updates: an
updated executable needs a separate verified translation before it can be used.

## Preparing the game files

Copy the original extracted files into **Darkness**, then launch the game.
The runtime uses Windows' built-in AES provider to decode `default.xex` and
expand its zero-filled blocks in memory. It checks both the original file and
decoded image against the AOT build's SHA-256 hashes before executing game code.
The source generator also reads the original `default.xex` directly.
Existing prepared files may be left in the folder; they are not read.

### Checking the supported game revision

For builds with original-file loading, run this in your **Darkness** folder:

```powershell
Get-FileHash .\default.xex -Algorithm SHA256 | Format-List
```

The supported original file's SHA-256 is (case does not matter):

```text
default.xex  aace35a8f9bcdc7f28aeab9ff8cf3bdf200353f5c83705f6284487347acb3c5f
```

Direct decoding reproduces the previous memory image exactly (SHA-256
`180b7fc8f57462f6bac3404ecab061a8d79914c7a3e9a12c238bd3449e72d049`).
The tested XEX metadata reports Title ID `545407EE`, Media ID
`0F213645`, version `0.0.0.1`, and **All Regions**. That does not establish
compatibility with every USA/Canadian/international disc revision; the generated
hash is the decisive check for this revision.

If the hash differs, report the hash and your disc revision. Do not post game files.
If the game closes after launch, also attach the newest
`build_native/run/desktop-*/runtime.log`. A different result can indicate a
different revision or a modified dump. Previously published releases may still
require prepared files; use a build containing the original-file loader.

## Build from source

**1. Game files** — from your own dumped copy, fill in `Darkness/`:

Use the original extracted dump; no decrypted executable or raw image is needed.

```
Darkness/
  default.xex               # original encrypted executable
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

**3. Play** — double-click either launcher:

| Command | What it does |
|---|---|
| `Launch.cmd` | Play with sound |
| `LaunchWithSettings.cmd` | Choose video settings, then play with sound |
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
- [STEAM_DECK.md](STEAM_DECK.md) — unverified Proton setup and diagnostics

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
