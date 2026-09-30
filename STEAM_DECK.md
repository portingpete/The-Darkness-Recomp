# Steam Deck and Linux

The release is a Windows x64 program. Steam Deck gameplay, audio, performance,
and suspend/resume have **not been verified** for this release. The steps below
provide a Proton test setup; they do not establish compatibility or a native
Linux port.

[Valve's Proton](https://github.com/ValveSoftware/Proton) runs Windows programs
through Steam on Linux. Use the complete Windows release folder and your own
supported game dump, including the original `default.xex`.
Keep the audio and Visual C++ DLLs beside the executables in
`build_native/Release`; copying only an EXE leaves required dependencies behind.

## Set up a test in Steam

1. In Desktop Mode, extract the Windows release ZIP to a writable folder such
   as `/home/deck/Games/The-Darkness-Recomp`. Copy the game files into its
   `Darkness` folder using the layout in `START_HERE.txt`. Prepare the two
   executable images on Windows first if needed.
2. In Steam, choose **Games > Add a Non-Steam Game > Browse** and select
   `build_native/Release/DarkRecompPreview.exe` from the extracted folder.
3. Open the shortcut's **Properties**. Set **Start In** to the extracted release
   folder, and **Launch Options** to `--sound`. Quote paths containing spaces.
   For the example folder, the properties are:

   ```text
   Target: "/home/deck/Games/The-Darkness-Recomp/build_native/Release/DarkRecompPreview.exe"
   Start In: "/home/deck/Games/The-Darkness-Recomp"
   Launch Options: --sound
   ```

4. Under **Compatibility**, enable **Force the use of a specific Steam Play
   compatibility tool** and choose an installed Valve Proton version. Record
   the version when reporting results. There is no validated version for this
   project yet.
5. Use Steam Input's **Gamepad** template so the Deck controls reach the game's
   Xbox controller input. Launch the shortcut. If Desktop Mode succeeds, try
   Gaming Mode and test saves, audio, controller input, and suspend/resume.

Video options are in the game's **Options > Video Settings**. Begin with the
default 720p internal rendering. A stable frame rate on Deck has not been
established. Settings and saves stay in the extracted release folder; keep that
folder writable and preserve it when updating.

## Optional shortcut helper

The release includes `tools/add_steam_shortcut.py`. Python 3 is needed only for
this optional helper; the manual Steam steps above do not require it. The
helper discovers standard Linux and Flatpak Steam user folders as well as
Windows Steam installations, and targets the same EXE and `--sound` option.

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
can select another Steam installation.

## If launch fails

Enable Proton logging for one diagnostic run with:

```text
PROTON_LOG=1 %command% --sound
```

[Proton's documentation](https://github.com/ValveSoftware/Proton#runtime-config-options)
describes the log location, normally `steam-<shortcut-id>.log` in your home
folder. Also retain the newest `build_native/run/desktop-*/runtime.log` inside
the extracted release. Report both logs, the release version, Proton version,
SteamOS/Linux version, and whether the failure happens in Desktop or Gaming
Mode. Return Launch Options to `--sound` after testing. Do not upload game files.

The current build requires an x86-64 CPU with SSSE3. It does not select the
build machine's CPU instruction set, and the bundled XMA decoder disables
assembly optimizations. Graphics and audio still require Proton's Windows API
implementations; those paths need actual Linux hardware validation.
