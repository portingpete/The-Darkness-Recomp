# Steam Deck and Linux

The build is a Windows x64 program that runs through Proton on Linux. The current
source includes a Linux launcher and file I/O compatibility fixes. Older release
ZIPs might not contain these changes. Steam Deck gameplay, performance and
suspend/resume have **not been verified**. There is no native Linux executable.

## AppImage installer in Desktop Mode

When a release includes `The-Darkness-Recomp-vX.Y.Z-steam-deck-x86_64.AppImage`,
it provides a graphical installer for that released Windows build and Proton.
It contains no original game files. You need your own complete extracted Xbox
360 dump, Python 3.10 or newer and either KDialog or Zenity available on the
system, plus an internet connection. Allow space for another copy of your game,
the release, Proton downloads and extracted tools; Steam Runtime may download
on first play. Choose writable Linux storage, such as the internal drive or
an ext4-formatted SD card.

1. Switch the Deck to **Desktop Mode** and download the AppImage from the
   release's **Assets** list.
2. In Dolphin, right-click it, open **Properties > Permissions**, enable
   **Is executable**, and apply the change. Open the AppImage.
3. Select the folder containing your own original `default.xex`, `Content` and
   `System`, including the rest of your extracted dump. The installer copies
   these files; it does not extract an Xbox disc image or download game data.
4. Choose an existing parent folder outside your dump. For example, selecting
   `/home/deck/Games` installs into `/home/deck/Games/The-Darkness-Recomp`.
5. Review the destination and confirm installation. Setup verifies the UMU and
   GE-Proton downloads, copies the release and your dump, then shows the launcher
   and setup log paths. It does not start the game or install system packages.

The installation remains in the selected folder after the AppImage closes.
Open `Play-The-Darkness.desktop` there to play in Desktop Mode. Program files,
your copied dump, saves and settings live in its `game` subfolder; the Proton
prefix lives in `ge-prefix`. Keep the whole installation folder together.
The installer AppImage can be moved or removed afterward.

For Gaming Mode, add the installed game EXE through Steam's **Games > Add a
Non-Steam Game > Browse**. With the example destination above, set:

```text
Target: "/home/deck/Games/The-Darkness-Recomp/game/build_native/Release/DarkRecomp.exe"
Start In: "/home/deck/Games/The-Darkness-Recomp/game"
Launch Options:
```

Under **Properties > Compatibility**, force an installed Proton tool. Select
Steam Input's **Gamepad** template, then test in Desktop Mode before Gaming
Mode. Steam uses the Proton tool selected in its shortcut; the desktop launcher
uses the UMU and GE-Proton installed by the wizard. Steam Deck gameplay,
performance, controls and suspend/resume still require actual hardware testing.

To update, open a newer installer AppImage and select the same dump and the same
parent folder. Setup updates managed program files while preserving existing
saves, settings and the Proton prefix. Conflicting game assets or files you
modified are preserved and reported as errors; consult the displayed setup log.

If the AppImage reports a FUSE error, run its
[extract-and-run fallback](https://docs.appimage.org/user-guide/troubleshooting/fuse.html)
in Konsole, replacing the example version with the downloaded filename:

```sh
./The-Darkness-Recomp-vX.Y.Z-steam-deck-x86_64.AppImage --appimage-extract-and-run
```

## Verified development setup

On 2026-10-01, the Windows build was tested from a Windows PC in Ubuntu 24.04
under WSL2/WSLg, using GE-Proton11-7 (Wine 11.0 Staging), UMU 1.4.4 and Steam
Runtime 4. The original game executable loaded successfully. The intro and menus
first rendered through DXVK with Vulkan software rendering (llvmpipe). A later
WineD3D/OpenGL run used the PC's RTX 5080 and reached the opening first-person
level after New Game / Medium difficulty selection, creating Chapter1 and
Checkpoint saves. XAudio2 started with audio callbacks and no reported audio
errors. Native memory, import-boundary, file I/O and save-storage contract tests
passed. All 73 Windows CTests passed in the tested development checkout, which
included other development changes. Wine 9.0 failed physical memory alias
setup and is not a suitable fallback for this build.

The WSL hardware test used a private runtime overlay exposing WSL's graphics
drivers. The automated installer applies that overlay only under WSL2.
A stalled WSLg PulseAudio/RDP bridge required a user-approved
WSL restart before the final run. Tests used a separate game folder and Proton
prefix without existing Windows saves or settings.

The opening scene reached the requested 30 FPS at 640x360 after initial shader
compilation. Menus were slower, and compilation caused a substantial first-load
stall. This does not establish full-game, Linux hardware or Steam Deck
performance. Menu confirmation worked. Automated look/movement commands reached
the input adapter, but visible camera movement and completion of the opening
tutorial were not confirmed. Audio quality, physical gamepad input, a full
playthrough and suspend/resume still need target-device testing.

[Valve's Proton](https://github.com/ValveSoftware/Proton) runs Windows programs
through Steam on Linux. Use the complete Windows release folder and your own
supported game dump, including the original `default.xex`.
Keep the audio and Visual C++ DLLs beside the executables in
`build_native/Release`; copying only an EXE leaves required dependencies behind.

## Automatic setup from Windows

With Ubuntu 22.04 or newer configured in WSL2 (24.04 was tested), put your complete
game dump in `Darkness`, then
double-click **SetupLinux.cmd**. It installs missing Ubuntu Python, graphics and
audio libraries, downloads the tested UMU 1.4.4 and GE-Proton11-7 with pinned
SHA-256 checks, and copies the Windows build and game into Linux storage. The
WSL graphics configuration is applied only inside the game's Steam Runtime.
After setup, double-click **PlayLinux.cmd**.

The default destination is `~/.local/share/darkrecomp` inside Ubuntu. Allow space
for another copy of the game and Proton. Rerunning setup updates program files
and retains Linux saves, settings and the Proton prefix. Existing game assets
are compared before copying; conflicting dump files cause an error. Windows
saves/settings are not copied. Neither script restarts WSL automatically.

If WSL or Ubuntu is missing, run `wsl --install -d Ubuntu` in an administrator
terminal, restart Windows if requested, and complete Ubuntu's first-run user
setup. Then run **SetupLinux.cmd**. A packaged release supplies the CRT DLLs;
when using a built checkout, the installer can locate its matching Visual Studio
CRT automatically.

For optional arguments, call the PowerShell script directly so paths with shell
symbols are preserved. You can select the distro, Linux destination, or an
extracted game dump elsewhere:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\setup_linux.ps1 -Distro Ubuntu -InstallDir /home/your-user/Games/darkrecomp
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\setup_linux.ps1 -GameDirectory 'D:\My Games\Darkness'
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\setup_linux.ps1 -Check
```

The selected distro and destination are saved locally in
`build_native/linux-setup.json` for later runs. `-Check` validates setup without
downloads, package installation or file changes. `-SkipDependencies` disables
Ubuntu package installation. `-UmuArchive`, `-ProtonArchive` and `-SkipDownload`
support verified local archives for offline setup; hashes are still required.

On an ordinary Linux desktop, the same installer can be run directly with Python
3.10 or newer, using the complete extracted Windows release as its source:

```sh
python3 tools/setup_linux.py --source "$PWD"
bash "$HOME/.local/share/darkrecomp/play-linux.sh"
```

The installer requires a supported original game dump and built Windows
binaries; it does not build the project or download game files. Automated WSL
setup reproduces the validation scope above, including its input limitations.

## Set up a test in Steam

1. In Desktop Mode, extract the Windows release ZIP to a writable folder such
   as `/home/deck/Games/The-Darkness-Recomp`. Copy the game files into its
   `Darkness` folder using the layout in `START_HERE.txt`. Only the original
   extracted files are needed; no executable preparation is required.
2. In Steam, choose **Games > Add a Non-Steam Game > Browse** and select
   `build_native/Release/DarkRecomp.exe` from the extracted folder.
3. Open the shortcut's **Properties**. Set **Start In** to the extracted release
   folder, and leave **Launch Options** empty. Sound is enabled by default.
   Quote paths containing spaces.
   For the example folder, the properties are:

   ```text
   Target: "/home/deck/Games/The-Darkness-Recomp/build_native/Release/DarkRecomp.exe"
   Start In: "/home/deck/Games/The-Darkness-Recomp"
   Launch Options:
   ```

4. Under **Compatibility**, enable **Force the use of a specific Steam Play
   compatibility tool** and choose an installed Valve Proton version. Record
   the version when reporting results. There is no validated version for this
   project on Steam Deck yet; the WSL test used GE-Proton11-7 through UMU.
5. Use Steam Input's **Gamepad** template so the Deck controls reach the game's
   Xbox controller input. Launch the shortcut. If Desktop Mode succeeds, try
   Gaming Mode and test saves, audio, controller input, and suspend/resume.

The shortcut targets the running game directly for Steam's overlay, Steam Input,
and session status. For an existing shortcut to `Launch.cmd` or
`DarkRecompPreview.exe`, edit its **Properties** to target `DarkRecomp.exe` and
remove `--sound` from Launch Options.

Video options are in the game's **Options > Video Settings**. Begin with the
default 720p internal rendering. A stable frame rate on Deck has not been
established. Settings and saves stay in the extracted release folder; keep that
folder writable and preserve it when updating.

## Run a Proton test outside Steam

The release also includes `Launch.sh` for Linux systems with
[UMU](https://github.com/Open-Wine-Components/umu-launcher) installed. UMU runs
Proton in its required Steam Runtime outside Steam. From the extracted release
folder, use:

```sh
bash Launch.sh check
bash Launch.sh
bash Launch.sh mute
```

`check` checks the release, game files and `umu-run` without launching or
downloading anything. The first play run lets UMU download its default
`UMU-Proton` tool and matching Steam Runtime. Extra arguments are passed to the
Windows launcher, for example `bash Launch.sh play --fps 60`.

To reproduce the tested Proton version, install GE-Proton11-7 and select its
extracted directory explicitly:

```sh
PROTONPATH=/path/to/GE-Proton11-7-x86_64 bash Launch.sh
```

The default Proton prefix is `$XDG_DATA_HOME/darkrecomp/proton`, or
`$HOME/.local/share/darkrecomp/proton` when `XDG_DATA_HOME` is unset. Existing
`WINEPREFIX`, `GAMEID` and `PROTONPATH` environment settings override the defaults.
Keep the prefix on the Linux filesystem when testing from WSL. For a diagnostic
run, use `PROTON_LOG=1 bash Launch.sh`; retain the Proton log and the game's
`build_native/run/desktop-*/runtime.log`.

This launcher does not establish verified Steam Deck gameplay or performance.
Use Steam's shortcut method above when you want Steam Input and Gaming Mode.

## Optional shortcut helper

The release includes `tools/add_steam_shortcut.py`. Python 3 is needed only for
this optional helper; the manual Steam steps above do not require it. The
helper discovers standard Linux and Flatpak Steam user folders as well as
Windows Steam installations, and targets the same game EXE with empty launch
options.

Close Steam completely before running the helper, because Steam writes
`shortcuts.vdf` when it exits. Preview the entry first:

```sh
python3 tools/add_steam_shortcut.py --dry-run
```

Then run it without `--dry-run` to add the entry and restart Steam. If multiple
Steam users exist, pass one user's file explicitly:

```sh
python3 tools/add_steam_shortcut.py "$HOME/.local/share/Steam/userdata/USER_ID/config/shortcuts.vdf"
```

Replace `USER_ID` with the numeric folder for your Steam user. The helper
preserves existing entries and keeps an initial `.bak` backup. Select Proton in
the shortcut's Compatibility properties after adding it. `--root /path/to/game`
can target an extracted release elsewhere; `--steam-userdata /path/to/userdata`
can select another Steam installation. Existing shortcuts are preserved; edit
their Properties manually using the target above to switch from an older launcher.

## If launch fails

Enable Proton logging for one diagnostic run with:

```text
PROTON_LOG=1 %command%
```

[Proton's documentation](https://github.com/ValveSoftware/Proton#runtime-config-options)
describes the log location, normally `steam-<shortcut-id>.log` in your home
folder. If you also reproduce through `Launch.sh`, retain its newest
`build_native/run/desktop-*/runtime.log` inside the extracted release.
Report the available logs, the release version, Proton version,
SteamOS/Linux version, and whether the failure happens in Desktop or Gaming
Mode. Clear Launch Options after testing. Do not upload game files.

The current build requires an x86-64 CPU with SSSE3. It does not select the
build machine's CPU instruction set, and the bundled XMA decoder disables
assembly optimizations. Graphics and audio still require Proton's Windows API
implementations; those paths need actual Linux hardware validation.

## Build the installer AppImage (maintainers)

Complete the [release validation gates](https://github.com/portingpete/The-Darkness-Recomp/blob/main/docs/RELEASING.md) before publication.
Installer `--help` and Windows tests do not validate Proton shader compilation
or game startup. Retain cold-cache and warm-cache gameplay evidence for the
supported Proton/UMU + DXVK stack; see issue #156 for the regression case.

Run the packager on Linux with a complete Windows release ZIP built by
`tools/package_release.py`. It validates the release manifest, file hashes and
allowed contents before extracting the exact released payload. The AppImage
also bundles this checkout's current setup engine. Supply locally verified
`appimagetool` and an x86_64 type-2 AppImage runtime; packaging does not download
these tools or game files.

By default, packaging builds the Rust wizard with Cargo for
`x86_64-unknown-linux-musl`. Install that Rust target and a suitable musl C linker
in the build environment first:

```sh
rustup target add x86_64-unknown-linux-musl
python3 tools/package_appimage.py \
  --release-zip /abs/The-Darkness-Recomp-vX.Y.Z-windows-x64.zip \
  --appimagetool /abs/appimagetool \
  --runtime-file /abs/runtime-x86_64 \
  --output-dir /abs/out
```

If needed, configure Cargo's `x86_64-unknown-linux-musl` linker for your toolchain.
To reuse an already compiled Linux x86_64 musl wizard, add
`--installer-binary /abs/darkrecomp-deck-installer`; Cargo is then unnecessary
for packaging. The output directory receives
`The-Darkness-Recomp-vX.Y.Z-steam-deck-x86_64.AppImage` and its `.sha256` file.
Each completed file is published atomically without replacement; the pair is
not a cross-file transaction. The output filesystem must support hard links,
and packaging fails cleanly when it does not. The image is published before
the checksum, so interruption between those steps can leave a complete image
without its checksum. A rerun reuses an existing ordinary output only when its
bytes exactly match the newly staged file, then publishes the missing partner.
Conflicting files and symlinks are preserved and cause packaging to fail.
No AppImage is published by this command.

The bundle retains the Rust wizard's source project, license and build
instructions in `usr/share/darkrecomp/installer-source`, alongside provenance
hashes in `INSTALLER.json`. To check packaging and the installer without
launching the game, build the wizard first, then run on Linux:

```sh
cargo test --manifest-path tools/deck_installer/Cargo.toml --locked
python3 -m unittest discover -s tests -p test_appimage.py
python3 -m unittest discover -s tests -p test_deck_installer.py
python3 -m unittest discover -s tests -p test_linux_setup.py
```

The process tests use synthetic dumps and fake dialogs. They look for the
default musl release binary; `DECK_INSTALLER_BINARY=/abs/path` can select a
different compiled Linux binary. They skip when no executable is available.
