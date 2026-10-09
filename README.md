# The Darkness Recomp

A native Windows x64 port of **The Darkness (Xbox 360)**. The 360 executable is
translated to C++ ahead of time and built into `DarkRecomp.exe` with a D3D11
renderer — no emulation at runtime.

> **Bring your own game dump.** This repo contains no game code, assets, or
> binaries. Dump your Xbox 360 copy and drop the files into `Darkness/`
> (step 1 below).

Playable today: full gameplay, mouse look + controller input, video settings
(resolution, FOV, original Xbox gamma calibration, bloom, frame cap, VSync,
16x texture filtering), keyboard remapping, cursor menu selection, XMA audio, and saves.

## Download and play

**[Download the Windows release](https://github.com/portingpete/The-Darkness-Recomp/releases).**
Choose `The-Darkness-Recomp-v...-windows-x64.zip` under **Assets**.
The GitHub **Source code** downloads are for building the project yourself.

1. Right-click the Windows ZIP and choose **Extract All**.
2. Copy your own extracted Xbox 360 game dump into the included **Darkness**
   folder: the original `default.xex`, `Content`, `System`, and all the other
   files and folders from your dump. No XexTool preparation is required.
3. Double-click **Launch.cmd** to play with sound.

Use **LaunchWithSettings.cmd** to choose settings and language before playing.
Use **LaunchWithUpdates.cmd** to check GitHub for a newer release before playing.
It asks before installing an update and keeps your game files, saves, achievements,
and settings. Windows PowerShell is included with Windows; no extra tools are needed.

No compiler, Python, or separate audio setup is needed for the Windows release.
Use 64-bit Windows 10/11 with a Direct3D 11-capable graphics device.
See [START_HERE.txt](START_HERE.txt) for the folder layout and troubleshooting.
An ISO alone is not enough; use an extracted dump of the supported game revision.
The port decodes `default.xex` in memory at startup; `_uncrypted.xex` and
`basefile.exe` are no longer required.

The original supported disc executable and the Russian localization identified
in [issue #37](https://github.com/portingpete/The-Darkness-Recomp/issues/37) are
recognized automatically. Copy all files from the matching dump, including its
own `default.xex`. The Russian localization uses the game's English content
slot: choose **English**, or **System default** on Russian Windows. Its translated
text, fonts, and audio come from that dump.

The mouse captures automatically when keyboard/mouse gameplay starts or resumes
after a menu. Click the game window to recapture after a manual release or Alt-Tab.
**F1** shows controls, **F2** toggles
capture, and **Esc** releases it. Graphics options are in **Options > Video Settings**
and in the video settings launcher. Both use the same saved settings.
Click menu choices to select them; the mouse wheel also changes choices and **E** confirms.
Choose **Extra Content > Achievements** in the main menu to see the original
achievement list. Unlocks from gameplay are stored in
**saves/achievements.dat** and persist between launches. The list uses your game
dump's achievement text and selected language; multiplayer achievements require
their original gameplay conditions. Xbox Live synchronization is unavailable.
Use **Options > Controls > Keyboard bindings** to remap gameplay keys and mouse buttons.
The four-page game menu supports primary/secondary keys, clear, defaults, and Save/Cancel.
**Exit Game** in the pause or main menu closes the game after confirmation.
Press **F5** to open developer tools for mission selection, player speed,
invincibility, noclip, all Darkness abilities and maximum Darkness level.
Enable the F6 resolution shortcut there to allow live 720p/1440p switching;
the shortcut starts off. Mission loading and Darkness grants can autosave;
see [CONTROLS.md](CONTROLS.md#developer-tools) before changing progression.
Choose **Language** in **Options > Video Settings** or the settings launcher:
System default, English, German, French, Spanish, or Italian. In-game changes
take effect after restarting. See [CONTROLS.md](CONTROLS.md) for language overrides
and the full input guide.

## Steam Deck and Linux

The Windows build can run through Proton on Linux. `Launch.sh` and the file I/O
compatibility fixes were tested with GE-Proton11-7 in Ubuntu 24.04 under WSL2.
The opening level rendered with audio and created checkpoint saves; memory,
file I/O and save-storage tests passed. Visible camera movement and completion
of the opening tutorial were not confirmed. Steam Deck gameplay and performance
remain unverified; a native Linux build is not available.

For automatic setup from Windows with Ubuntu in WSL2, double-click
**SetupLinux.cmd**, then **PlayLinux.cmd**. The installer copies the game to
Linux storage and preserves that copy's saves and settings on later updates.
See [STEAM_DECK.md](STEAM_DECK.md) for prerequisites, options, Steam shortcuts
and the verified test scope.

## Other games and title updates

This executable ports **The Darkness**, using translated code and engine
integration for its supported Xbox 360 revision. It cannot load another Xbox
game by replacing the files or changing a hash. Supporting a different title
requires its own translation configuration, runtime integration, renderer,
and testing; the upstream XenonRecomp toolchain is a starting point for that
work. The automatic revision check also applies to The Darkness title updates: an
updated executable needs a separate verified translation before it can be used.

## Preparing the game files

Copy the original extracted files into **Darkness**, then launch the game.
The runtime uses Windows' built-in AES provider to decode `default.xex` and
expand its zero-filled blocks in memory. It checks both the original file and
decoded image against an approved pair of SHA-256 hashes before executing game code.
The Russian executable shares the same translated code, imports and TLS layout;
its localization data remains intact. Startup menu overrides also preserve the
dump's cached strings and fonts.
The source generator also reads the original `default.xex` directly.
Any approved executable can be used to build the shared translation.
Existing prepared files may be left in the folder; they are not read.

The game checks the supported revision automatically at launch; no manual hash
check is needed. If it reports `default.xex differs from the supported AOT revision`,
your dump uses a different revision or a modified executable. Include your disc
revision and the newest `build_native/run/desktop-*/runtime.log` when reporting
the problem. Do not post game files.

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

**3. Play** — double-click a launcher:

| Command | What it does |
|---|---|
| `Launch.cmd` | Play with sound |
| `LaunchWithSettings.cmd` | Choose settings and language, then play with sound |
| `LaunchWithUpdates.cmd` | Check for updates, then play with sound |
| `LaunchStallProfiler.cmd` | Play with sound and record runtime stalls in the runtime log |
| `Launch.cmd mute` | Play muted |
| `Launch.cmd preview` | Engine preview build (muted by default) |
| `Launch.cmd stutter` | Play with sound while recording slow-frame timings |
| `Launch.cmd performance` / `render-profile` / `steady-60` | Diagnostic recording runs |
| `Launch.cmd help` | Full usage |

Extra arguments reach the game untouched, e.g. `Launch.cmd play --fps 120`.

For runtime hitch diagnostics, use `LaunchStallProfiler.cmd`, reproduce the hitch,
then close the game. The newest `build_native/run/desktop-*/runtime.log` includes
`[STALL]` reports for frames over 16.67 ms and runtime/HLE calls over 2 ms,
`[WAIT]` details, and the largest slow-frame contributors. The launcher uses
your saved game settings. Profiling is disabled by default; set
`DARKRECOMP_STALL_PROFILE=1` to enable it for another launch, or use
`tools/run_native.py --stall-profile` for a bounded development run. Build with
`-DDARK_STALL_PROFILER=OFF` to exclude the profiler completely.

The mouse captures automatically when keyboard/mouse gameplay starts or resumes
after a menu. Click to recapture after a manual release or Alt-Tab (**F1** controls
guide, **F2** toggle capture, **Esc** release).

## Docs

- [CONTROLS.md](CONTROLS.md) — every binding, launcher option, and setting
- [RENDERING.md](RENDERING.md) — how the renderer and frame pacing work
- [STEAM_DECK.md](STEAM_DECK.md) — Proton setup, validation scope and diagnostics

## Updating a downloaded release

Run **LaunchWithUpdates.cmd** to check for a newer GitHub release, including
prereleases. Approve the update when prompted, or decline to play your installed
version. Downloads and installed files are checked against SHA-256 hashes.
Close the game first. Recovery copies are kept in the folder printed by the
updater. Network failures still let you play the installed version.
The updater works with extracted releases; update source checkouts with Git.

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

Development includes assistance from OpenAI Codex.
