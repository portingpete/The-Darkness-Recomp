# Native keyboard and mouse controls

Run `Launch.cmd` for audio, or `Launch.cmd mute` for a muted game.
Keyboard/mouse play captures the cursor as soon as Continue or a confirmed New
Game starts loading. Camera movement begins when the gameplay client is ready;
mouse movement during loading is discarded. Escape and Alt-Tab release capture.
Run `LaunchWithSettings.cmd` to choose video options and language before the game
opens, then play with audio. It reads and saves `DarkRecomp.settings.ini`, so
the pre-game launcher and in-game Video Settings share your choices.
Choose **Exit Game** in the pause or main menu and confirm to close the game,
or close its window. `Launch.cmd preview` and
`build_native/Release/DarkRecompPreview.exe` still default to muted;
the preview executable also accepts `--sound` or `--mute`.

Choose **Extra Content > Achievements** in the main menu to view achievement names,
descriptions and unlock status. Select a row to read its details; **Esc** returns
to the menu. The Xbox achievement screen opens a native viewer with earned points
and a **Close** button. Progress is saved locally in
`saves/achievements.dat`; keep that file when updating the port.

Normal launchers record lightweight slow-frame timings in their runtime log.
Automatic screenshots are disabled during normal play; use the preview executable's
`--capture-frames` option when diagnostic BMP captures are specifically needed.

Use `LaunchStallProfiler.cmd` to identify runtime hitches while playing with sound
and your saved settings. Close the game after reproducing the hitch, then open the
newest `build_native/run/desktop-*/runtime.log`. `[STALL]` lines report frames over
16.67 ms, runtime/HLE calls over 2 ms, and the largest contributors to a slow
frame. `[WAIT]` lines include duration, thread ID, guest PC/caller, function, and
the waited object or fence when available. Sections cover guest code, rendering,
audio, file I/O, waits, and Present. Profiling starts disabled; the launcher enables
`DARKRECOMP_STALL_PROFILE=1` only for its run. Extra game arguments are accepted.
Frame totals include normal pacing and VSync waits. Contributors use exclusive
wall time and show the frame thread separately from workers; concurrent worker
times can sum to more than the frame total. `pending=1` identifies a call still
in progress at the frame boundary. `dropped` or `incomplete_thread_intervals`
indicates that a bounded buffer or snapshot could not retain every observation.

For a requested rendering-stutter diagnostic, use `Launch.cmd render-profile`.
It records active render-thread instruction samples and their rendering phase in
`build_native/run/render-profile-*.log`. Close the game to finish recording. This
mode adds measurement overhead; use the normal launcher for regular play.

To record a gameplay stutter for later analysis, use `Launch.cmd stutter`.
It plays with sound under your saved graphics settings (no captures, no profiling),
auto-skips intro videos, and writes lightweight slow-frame timings to
`build_native/run/stutter-*.log` plus a slow-frame count at the end.

Both launchers default to borderless fullscreen using your monitor's aspect ratio,
including ultrawide and 32:9 displays. **Alt+Enter** switches between fullscreen
and a resizable window. Gameplay expands horizontally; movies keep their
original proportions. Resizing the window fits the current image without
stretching. Restart on a different monitor to select its aspect ratio.

The internal render height defaults to 720 pixels. Video Settings offers 360p,
480p, 720p, 1080p, 1440p and 2160p. Higher resolutions use native PC rendering
while keeping the original console allocations within their limits. Changes
apply after restarting the game. `--windowed --width 2560 --height 1080` selects
a window size and aspect ratio. Internal rendering supports up to 4096 x 2160,
preserving aspect at the width limit and rounding to supported game dimensions.

Open **F5** and check **Enable F6 resolution shortcut**, then press **F6** during
play to switch between **720p and 1440p** without reloading the level. The shortcut
starts disabled each launch; unchecking the option also cancels a pending switch.
A brief message shows the new resolution, which saves
automatically. The switch preserves the launch aspect ratio and existing game
buffers. It works with a 720-pixel-high game buffer: normal 16:9 launches at
720p, 1440p or 2160p, and typical 21:9 launches at 720p or 1440p. Other launch
resolutions or capped wide aspects show an unavailable message; choose 720p
before launching on a normal or 21:9 display to enable it.

Open **Options > Video Settings** for the game's native graphics rows:

- **Brightness:** 50% to 200%, with 100% neutral.
- **Gamma:** the original 12-step calibration control, with the middle position
  as default. Use Left/Right to adjust and choose Yes when leaving Options to save.
- **Field of view:** Original or **60�120 horizontal degrees at 16:9**;
  ultrawide displays show more at the sides.
- **Bloom:** on by default; toggle the final-composite glow.
- **Vertical sync:** off by default; synchronize presentation to the display.
- **Frame limit:** 30, 60 (default), 90, 120, 144, 165, 240, 360, or Unlimited.
- **Resolution:** 360p, 480p, 720p, 1080p, 1440p or 2160p; requires a restart.
- **Display:** windowed or borderless fullscreen.
- **Motion blur:** Off (default) or On.
- **Antialiasing:** Off (default) or FXAA; smooths edges immediately and saves automatically.
- **Language:** System, English, German, French, Spanish, or Italian; requires a restart.
- **Texture filtering:** Original, 2x, 4x, 8x, or **16x anisotropic (default)**.
  Applies immediately to world textures while preserving the original UI and effects sampling.
  Explicit levels override the original world-texture setting; Original preserves it.

Use the normal menu navigation: Up/Down selects a row,
Left/Right changes its value, and confirm cycles forward. The controller D-pad
and A work through the same original menu controls. Escape/Backspace or B goes
back. PC graphics changes save automatically to `DarkRecomp.settings.ini` beside
`Darkness` and apply immediately except resolution and language. Gamma uses the
original game profile and save confirmation. A failed PC settings save is shown in the row text.

All play modes use saved settings. Direct-launch options `--fov 100`, `--fps 120`,
`--render-height 720`, `--fullscreen` / `--windowed`, and `--vsync` / `--no-vsync`
override their saved values for that run. `--fov 0` selects Original. Editing a
menu option saves the current selection, including overrides. Valid custom
values remain unchanged until you adjust their row.

Choose **Language** in **Options > Video Settings**, or in the settings launcher
opened by `LaunchWithSettings.cmd`. In-game changes save immediately and take
effect after restarting; the launcher applies your choice when you click Play.
System follows the Windows UI language when it is English, German, French,
Spanish, or Italian; other UI languages use English. You can also edit
`DarkRecomp.settings.ini` beside `Darkness` and restart:

```ini
[Game]
Language=en
```

Use `en`, `de`, `fr`, `es`, or `it`; `auto` restores automatic selection.
The matching language content must be present in your own dump. Run
`Launch.cmd play --language en` to choose English for one run. When launching
`DarkRecomp.exe` directly, use the same `--language en` option; full English
language names are also accepted. This override applies only to that run and
does not replace your saved language choice when you adjust graphics settings.

The supported Russian localization uses the English content slot. With that
dump, choose **English**, or **System** on Russian Windows, to load its translated
text and audio. Keep its own executable and all matching content files together.

In menus, move the cursor over a choice and click to select it. The game releases
mouse capture while a menu is open and captures it automatically when keyboard/mouse
gameplay starts or resumes. Controller menu navigation leaves the mouse released.
After a manual release or Alt-Tab, click inside gameplay to recapture the mouse;
that first gameplay click only captures it.
Press **F1** for the controls guide, **F2** to toggle capture, or **Escape**
to pause and release the cursor. Alt-Tab, loss of focus, and closing the window
also release it. Click again after returning to the game.

- **WASD:** move; **mouse:** look.
- **Space:** jump while the mouse is captured; confirm or skip intros when released.
- **E:** use / confirm; **R:** reload; **Ctrl** or **C:** crouch.
- **Left click:** fire the right weapon; **right click:** fire the left weapon.
- **Middle click** or **Shift:** zoom.
- **Wheel:** previous/next choice in dialogue and menus, even with the mouse
  captured; during gameplay with mouse capture, cycle weapons. Releasing
  capture with **F2** disables gameplay wheel input. **1 / 2:** cycle weapons.
- **Q:** manifest Darkness; **G:** use Darkness power; **3 / 4:** cycle powers.
- **F:** redirect Darkling; **Tab:** journal; **Enter:** Start / pause.
- **Menus:** arrows to navigate, **E** or released **Space** to confirm,
  **Escape** or **Backspace** to go back.
- **Keyboard alternatives:** I / J / K / L to look, Z / X to fire left / right.

These are the default bindings. Open **Options > Controls > Keyboard bindings**
to change gameplay keys and mouse buttons inside the original game menu. **Previous** and **Next**
switch between four pages of actions. Select a primary or secondary slot, then
press the new key or mouse button; **Escape** cancels capture and **Delete** clears the slot.
Left, right, middle, and the two side buttons (**M4 / M5**) can be assigned,
along with **Alt**. Release the click used to select a slot before pressing
the button to bind. **Alt+Enter** and other host shortcuts stay available.
**Defaults** stages the original selection. **Save** applies the changes
and writes the `[Keyboard]` section of `DarkRecomp.settings.ini`.
**Cancel** or Back discards unsaved edits. Menu navigation stays available outside
key capture; reserved host shortcuts cannot be assigned.
In-game keyboard prompts follow the selected bindings.

The original game's camera sensitivity controls the controller. Mouse look
uses a separate, linear sensitivity and follows the game's inversion option.
When launching `DarkRecomp.exe` directly, `--mouse-sensitivity 1.0` sets the
mouse multiplier (0.1 to 10). Add `--mute --timeout-ms 0 --engine-preview` for a
muted interactive run. The play modes supply the interactive flags; the default
mode plays with sound instead of `--mute`.

## Developer tools

Press **F5** to open or close the developer panel. Select a campaign destination
and press **Load**, choose a player speed from **0.25× to 4×**, or toggle
**Invincible** and **Noclip**. Player controls become available once a player is active.
**Unlock all Darkness** grants all six original Darkness abilities.
**Max Darkness level** raises progression to the original maximum, level 5;
it preserves an existing higher heart count. These one-shot grants can be included
in later autosaves. Back up your checkpoint before changing progression.
**Enable F6 resolution shortcut** allows the live resolution hotkey for this session;
it starts off. **Restore defaults** returns to 1× speed and disables invincibility,
noclip and the F6 shortcut. It does not undo Darkness progression grants.
**Load** closes the panel before the mission's opening sequence; press F5 to reopen it.

Loading a mission restarts the current session, then loads the destination
through the game's commands. It can write an autosave. Back up your checkpoint
before jumping between missions if you want to preserve your current progress.
The panel releases mouse capture and
blocks gameplay input while open; close it, then click the game or press F2
to resume mouse control.

## Implementation

Keyboard input uses Win32 window messages. Mouse look uses foreground Raw Input
(`WM_INPUT` / `GetRawInputData`) with relative mouse counts. These native C++
paths feed the AOT game's existing input ABI directly. No virtual controller,
external remapper, or runtime interpreter is installed or required. Physical
XInput controllers remain supported.

Relative mouse counts are kept separately from the XInput state. Reading
controller input does not consume mouse movement or turn it into a held stick
velocity. Mouse counts feed the game's relative-angle commands, bypassing
stick dead zones, acceleration and speed limits while retaining the original
player look restrictions, pitch bounds and zoom behavior. The default is
8/65536 turn per count (about 0.044 degrees). Fractions smaller than one game
angle unit carry into the next movement; fast swipes split into complete angle
commands instead of wrapping their signed-short fields. Releasing capture
clears held mouse buttons, queued wheel events and pending motion.
Absolute-position raw devices are not used for look.

Action bindings follow the original [2K game manual](https://device.report/m/ba98250caa0f4727559b5c36a08a35cb76cb0d40ca14be8787638ba49d708053).
Raw Input uses the [Microsoft Win32 interface](https://learn.microsoft.com/en-us/windows/win32/inputdev/using-raw-input).

## Prompt artwork source

In-game button prompts use the light keyboard and mouse artwork from
[Xelu's free CC0 prompt pack](https://thoseawesomeguys.com/prompts/). When a physical
controller becomes the primary active input, prompts switch to the original
controller artwork; keyboard/mouse activity, focus loss, or controller
disconnect restores the keyboard/mouse set. Shared action icons combine their
bindings, such as R/Esc in menus or Shift/middle mouse during gameplay.
Gameplay prompts use their action bindings, so portal choices show R without
Esc. Keyboard and mouse keycaps use 64px artwork and appear 25% larger when
the screen layout has room. The artwork is embedded in the
game; no separate icon download is needed.
