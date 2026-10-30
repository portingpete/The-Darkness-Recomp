# The Darkness Recomp

A native Windows x64 port of **The Darkness (Xbox 360)**. The original executable
is translated to C++ ahead of time and built into `DarkRecomp.exe` with a D3D11
renderer. The Windows build can also run through Proton on Linux.

**[Download for Windows](https://github.com/portingpete/The-Darkness-Recomp/releases)**
· [Controls](CONTROLS.md)
· [Linux & Steam Deck](STEAM_DECK.md)
· [Build from source](#build-from-source)
· [Troubleshooting](#troubleshooting)

> **Bring your own game dump.** Original game code, assets, and binaries are not
> included. You need the extracted files from a supported Xbox 360 copy, including
> its original `default.xex`. An ISO alone will not work.

## Download and play

**Requirements:** 64-bit Windows 10/11 and a Direct3D 11-capable graphics device.
The Windows release includes runtime and audio dependencies; no compiler, Python,
or separate audio setup is needed to play.

1. [Download the Windows release](https://github.com/portingpete/The-Darkness-Recomp/releases).
   Under **Assets**, choose `The-Darkness-Recomp-v...-windows-x64.zip`, then
   right-click it and choose **Extract All**. GitHub's **Source code** downloads
   are for building the project yourself.
2. Copy **all files and folders** from your extracted Xbox 360 dump into the
   included `Darkness/` folder: `default.xex`, `Content`, `System`, and the rest
   of your dump. No XexTool preparation is required.
3. Double-click **Launch.cmd** to play with sound, or **LaunchWithSettings.cmd**
   to choose settings and language first.

Keep the whole release folder together. See [START_HERE.txt](START_HERE.txt) for
the expected folder layout and help with startup problems.

## Features and controls

- Full gameplay with keyboard, mouse, and controller input, XMA audio, saves,
  and persistent achievement tracking.
- Resolution, FOV, original Xbox gamma calibration and color profiles, bloom,
  frame cap, VSync, FXAA/SMAA/MSAA antialiasing, and 16x texture filtering.
- Remappable keys and mouse buttons, clickable menus, and adjustable subtitle size.
- Language selection and support for the original disc revision and the
  [Russian localization identified in issue #37](https://github.com/portingpete/The-Darkness-Recomp/issues/37).

The mouse captures automatically when keyboard/mouse gameplay starts or resumes
after a menu. Click the game window to recapture after a manual release or Alt-Tab.
**F1** shows controls, **F2** toggles capture, and **Esc** releases it.

Use **Options > Video Quality** for graphics and **Options > Controls > Keyboard
bindings** to remap inputs. The settings launcher uses the same saved graphics
settings. See [CONTROLS.md](CONTROLS.md) for the full input and settings guide.

<details>
<summary>Menu settings, languages, achievements, and developer tools</summary>

**Subtitles.** **Options > Subtitles > Subtitle Size** adjusts dialogue subtitles
from 50% to 200% and applies immediately in game. The default 100% retains the
original text size; the setting is saved for the next launch.
Higher-resolution subtitles use scalable Arial glyphs with grayscale antialiasing,
while retaining the game's spacing, wrapping, fades and outlines. Original bitmap
glyphs remain the fallback when the system font or a character is unavailable.
Encoded alphabets require a verified font atlas, independently of the executable's
language. Other translations retain their original bitmap subtitles.

**Menus and bindings.** Click menu choices to select them; the mouse wheel also
changes choices and **E** confirms. Use **Options > Controls > Keyboard bindings**
to remap gameplay keys and mouse buttons.
The five-page game menu supports primary/secondary keys, clear, defaults, and Save/Cancel.
Page 5 configures dialogue-choice controls independently of weapon switching;
the same keys and wheel directions can be used in both contexts.
**Exit Game** in the pause or main menu closes the game after confirmation.

**Achievements.** Choose **Extra Content > Achievements** in the main menu to see
the original achievement list. Gameplay unlocks persist in `saves/achievements.dat`.
The list uses your game dump's achievement text and selected language;
multiplayer achievements require their original gameplay conditions.
Xbox Live synchronization is unavailable.

**Developer tools.** Press **F5** for mission selection, player speed,
invincibility, noclip, all Darkness abilities, and maximum Darkness level.
Enable the F6 resolution shortcut there to allow live 720p/1440p switching;
the shortcut starts off. Mission loading and Darkness grants can autosave;
see [CONTROLS.md](CONTROLS.md#developer-tools) before changing progression.

**Languages.** Choose **Language** in **Options > Video Settings** or the settings
launcher: System default, English, German, French, Spanish, or Italian. In-game changes
take effect after restarting. See [CONTROLS.md](CONTROLS.md) for language overrides
and the full input guide.

The original supported disc executable and the Russian localization identified
in [issue #37](https://github.com/portingpete/The-Darkness-Recomp/issues/37) are
recognized automatically. Copy all files from the matching dump, including its
own `default.xex`. The Russian localization uses the game's English content
slot: choose **English**, or **System default** on Russian Windows. Its translated
text, fonts, and audio come from that dump.

The added in-game PC menus also use Russian with that dump: video settings,
keyboard and mouse bindings, and Exit Game. Their translation source is
[assets/localization/native_menu_ru.json](assets/localization/native_menu_ru.json).
See [the translation guide](assets/localization/README.md) to improve the wording
and rebuild the menus.

</details>

## Steam and Linux

Add the Windows executable to Steam using the instructions below. On Linux and
Steam Deck, the Windows build runs through Proton. Some releases include an
AppImage installer; see the [Linux and Steam Deck guide](STEAM_DECK.md) for setup.
**Steam Deck gameplay and performance remain unverified.**

<details>
<summary>Add the Windows release to Steam</summary>

In Steam, choose **Games > Add a Non-Steam Game > Browse** and select
`build_native/Release/DarkRecomp.exe` in the extracted release. Set **Start In**
to the extracted release folder and leave **Launch Options** empty. Sound and
normal gameplay are enabled by default. Keep the whole release folder together.

The shortcut must target the game executable so Steam can follow the running
game for its overlay, Steam Input, and session status. If you already added
`Launch.cmd` or `DarkRecompPreview.exe`, edit that shortcut's **Properties** to
use `DarkRecomp.exe` and remove `--sound` from Launch Options. The optional
`tools/add_steam_shortcut.py` helper preserves existing shortcuts; it does not
retarget them automatically.

</details>

<details>
<summary>Run the Windows build on Linux through Proton</summary>

For releases that provide a `The-Darkness-Recomp-vX.Y.Z-steam-deck-x86_64.AppImage`
asset, download it from [Releases](https://github.com/portingpete/The-Darkness-Recomp/releases)
in Steam Deck Desktop Mode. Mark it executable in Dolphin's **Properties >
Permissions**, then open it to select your own extracted game dump and a parent
folder for installation. The wizard creates `The-Darkness-Recomp` there and
installs the Windows release with verified UMU and GE-Proton downloads.
It requires Python 3.10+, KDialog or Zenity, internet access, and room for another
copy of the game and Proton on a Linux filesystem, such as an ext4 SD card.
Setup finishes without starting the game. Open `Play-The-Darkness.desktop` in
the chosen installation folder to play in Desktop Mode; see
[the AppImage instructions](STEAM_DECK.md#appimage-installer-in-desktop-mode)
for Steam shortcuts, updates, and the FUSE fallback.

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

</details>

## Updating

**To update without compiling,** close the game and extract a newer Windows
release ZIP into your existing game folder, replacing the included program files.
Keep `Darkness/`, `saves/`, and `DarkRecomp.settings.ini` to preserve your game
files, progress, achievements, and settings. You do not need to rebuild.

To check only published releases with the release updater, run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\update_release.ps1
```

When updating from v0.1.5, extract the new ZIP once to install the new launcher;
the older updater does not recognize the added Russian menu files.

**LaunchWithUpdates.cmd** instead builds and installs commits from the official
`main` branch before playing. It requires the [build tools below](#build-from-source)
and a complete supported game dump.

<details>
<summary>Commit updates and source checkouts</summary>

Close the game before updating. For downloaded installations, the updater keeps
a separate source/build copy outside your installation and copies your dump
there. Allow space for another game copy and compiler outputs; the first build
can take several minutes.

The updater preserves game files, saves, achievements, and settings. It tests the
build and verifies the resulting package and files with SHA-256 hashes before
installing. Recovery copies and build logs are kept in the folder printed by the
updater. Network failures still
let you play the installed version; build failures stop the update and leave the
previous program files available through **Launch.cmd**.

**LaunchWithUpdates.cmd** also updates source checkouts. The checkout must be
clean and able to fast-forward to official `main`; it preserves local edits and
commits and refuses divergent histories. It rebuilds and tests the selected
commit before marking the installed program current. Alternatively, pull and
build manually:

```powershell
git pull --ff-only
powershell -ExecutionPolicy Bypass -File tools\build.ps1
```

Keep your game dump and existing build directory. Unchanged generated files
retain their timestamps so the native build can reuse previous compilation work.

If an older setup stopped with `AOT gate: Generator failed (0)`, these same
commands install the missing generator patches and regenerate the output.

</details>

## Build from source

For Windows, install Git, Visual Studio 2022 with the **C++ and ClangCL tools**,
CMake 3.24+, Python 3.11+, MSYS2 MinGW64 GCC and `make` at `C:\msys64`, and
standalone LLVM at `C:\Program Files\LLVM` (including `llvm-lib.exe` for audio).
The resource-worker notification helper also requires Rust with the
`x86_64-pc-windows-msvc` target and `rustc` on PATH (tested with Rust 1.90).
It has no Cargo dependencies. Configure `-DDARK_RUST_RESOURCE_WAIT=OFF` to
build the original polling path without Rust.
Allow approximately 15 GB free. The first build requires an internet connection.

1. Copy your complete original extracted dump into `Darkness/`. Keep the included
   `darkness_switch_tables.toml`; other game files are not committed.
2. Run the build command below. It sets up dependencies, generates the C++
   translation and XMA codec, compiles, and runs the tests.
3. Double-click **Launch.cmd**. Windows output is in `build_native/Release/`.

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1
```

The build downloads pinned XenonRecomp source and applies the bundled Darkness
patches automatically. Checkouts from the older setup instructions are supported.

See [CPU capture and resource-wait changes](docs/performance/cpu-resource-waits.md)
for validation scope, diagnostic comparison modes, and runtime rollback.

<details>
<summary>Game-file layout and revision checks</summary>

Copy every file and folder from your dump, including any language content:

```text
Darkness/
  default.xex                  # Original encrypted executable
  Content/                     # Plus all other content folders from your dump
  System/                      # Engine shaders and system data
  darkness_switch_tables.toml   # Already included in the repository
```

Copy the original extracted files into **Darkness**, then launch the game.
The runtime uses Windows' built-in AES provider to decode `default.xex` and
expand its zero-filled blocks in memory. It checks both the original file and
decoded image against an approved pair of SHA-256 hashes before executing game code.
The Russian executable shares the same translated code, imports and TLS layout;
its localization data remains intact. Startup menu overrides also preserve the
dump's cached strings and fonts.
The source generator also reads the original `default.xex` directly.
Any approved executable can be used to build the shared translation.
Older `_uncrypted.xex` and `basefile.exe` files may remain in the folder; they
are not read.

The game checks the supported revision automatically at launch; no manual hash
check is needed. If it reports `default.xex differs from the supported AOT revision`,
your dump uses a different revision or a modified executable. Include your disc
revision and the newest `build_native/run/desktop-*/runtime.log` when reporting
the problem. Do not post game files.

</details>

<details>
<summary>Launchers and diagnostic recording</summary>

- `Launch.cmd` — Play with sound
- `LaunchWithSettings.cmd` — Choose settings and language, then play with sound
- `LaunchWithUpdates.cmd` — Check for updates, then play with sound
- `LaunchStallProfiler.cmd` — Play with sound and record runtime stalls in the runtime log
- `Launch.cmd mute` — Play muted
- `Launch.cmd preview` — Engine preview build (muted by default)
- `Launch.cmd stutter` — Play with sound while recording slow-frame timings
- `Launch.cmd performance` / `render-profile` / `steady-60` — Diagnostic recording runs
- `Launch.cmd help` — Full usage

Extra arguments reach the game untouched, e.g. `Launch.cmd play --fps 120`.

Downloaded releases supply `LaunchStallProfiler.cmd` and `LaunchShadowCapture.cmd`
in a separate optional **diagnostic-launchers ZIP**. Extract it into the installed
game folder, beside `Launch.cmd`. These launchers are also in source checkouts.

For runtime hitch diagnostics, use `LaunchStallProfiler.cmd`, reproduce the hitch,
then close the game. The newest `build_native/run/desktop-*/runtime.log` includes
`[STALL]` reports for frames over 16.67 ms and runtime/HLE calls over 2 ms,
`[WAIT]` details, and the largest slow-frame contributors. The launcher uses
your saved game settings. Profiling is disabled by default; set
`DARKRECOMP_STALL_PROFILE=1` to enable it for another launch, or use
`tools/run_native.py --stall-profile` for a bounded development run. Build with
`-DDARK_STALL_PROFILER=OFF` to exclude the profiler completely.

</details>

<details>
<summary>Tests, release packaging, and project layout</summary>

**Tests**

The build already runs the full suite. To re-run it later:

```powershell
ctest --test-dir build_native -C Release --output-on-failure
```

**Release packaging**

Maintainers can build, test, and package a Windows ZIP with:

```powershell
python tools/package_release.py --version v0.1.7
```

Run from a clean, committed checkout with the build prerequisites installed.
The Windows ZIP, optional diagnostic-launchers ZIP, and their SHA-256 checksums
land in `build_native/releases/`. The Windows ZIP includes the normal launchers,
runtime dependencies, notices, and audio-library source. Diagnostic launchers
are only in the separate ZIP, with instructions for extracting it beside
`Launch.cmd`. Game dumps, saves, settings, build tools, and diagnostic logs
are excluded from both packages.

To package the Steam Deck installer on Linux from that Windows ZIP, see
[the AppImage packaging instructions](STEAM_DECK.md#build-the-installer-appimage-maintainers).
The packager validates the release manifest and hashes, bundles the current
setup engine with the released Windows payload, and uses locally supplied
AppImage tools.

**Project layout**

- `app/` — Windows entry points and display settings
- `assets/` — Button prompts and PC menu localization
- `cmake/` — CMake helpers
- `config/darkness.toml` — Translation configuration
- `renderer/` — D3D11 backend and engine scene reconstruction
- `runtime/` — Native kernel, audio, input, XMA, and filesystem bridges
- `tools/` — Translator driver, `build.ps1`, shader/menu/XMA codegen
- `tests/` — Native and contract tests run by CTest

</details>

## Troubleshooting

- **Missing files or DLLs:** extract the whole release and keep the launchers and
  `build_native/` together. Check the [folder layout](START_HERE.txt).
- **Unsupported revision:** if startup reports `default.xex differs from the
  supported AOT revision`, your dump uses a different revision or modified
  executable. Include your disc revision and the newest
  `build_native/run/desktop-*/runtime.log` when
  [reporting an issue](https://github.com/portingpete/The-Darkness-Recomp/issues).
  Do not post game files.
- **Stutters or hitches:** use the optional diagnostic launchers described above
  and include the resulting runtime log.

<details>
<summary>Compatibility with other games and title updates</summary>

This executable ports **The Darkness**, using translated code and engine
integration for its supported Xbox 360 revision. It cannot load another Xbox
game by replacing the files or changing a hash. Supporting a different title
requires its own translation configuration, runtime integration, renderer,
and testing; the upstream XenonRecomp toolchain is a starting point for that
work. The automatic revision check also applies to The Darkness title updates: an
updated executable needs a separate verified translation before it can be used.

</details>

## Documentation

- [Getting started](START_HERE.txt) — release folder layout and startup help.
- [Controls and settings](CONTROLS.md) — bindings, launcher options, languages,
  and developer tools.
- [Linux and Steam Deck](STEAM_DECK.md) — AppImage installation, Proton setup,
  diagnostics, and validation scope.
- [Rendering](RENDERING.md) — renderer internals and frame pacing.
- [Translation guide](assets/localization/README.md) — edit and rebuild PC menus.
- [XWC audio tool](tools/xwc_audio/README.md) — read-only bank inspection, original XMA export and header timing diagnostics
- [DDS texture tool](tools/texture_mods/README.md) — RGBA8 DDS helper and experimental world texture packs; one 2x wall replacement verified in-game
- [Video pack tool](tools/video_mods/README.md) — imports timed WMV3/Theora replacements for the optional read-only video loader; playback qualification is in progress

## License and credits

GPLv3 — see [COPYING](COPYING), matching the upstream
[UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) /
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) toolchain this port builds on.

Development includes assistance from OpenAI Codex.
