# Steam Deck and Linux

The build is a Windows x64 program that runs through Proton on Linux. The current
source includes a Linux launcher and file I/O compatibility fixes. Older release
ZIPs might not contain these changes. Steam Deck gameplay, performance and
suspend/resume have **not been verified**. There is no native Linux executable.

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
