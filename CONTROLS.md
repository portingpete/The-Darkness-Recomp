# Native keyboard and mouse controls

Run `Launch.cmd` for audio, or `Launch.cmd mute` for a muted game.
Both run until you close the game window. `Launch.cmd preview` and
`build_native/Release/DarkRecompPreview.exe` still default to muted;
the preview executable also accepts `--sound` or `--mute`.

Normal launchers record lightweight slow-frame timings in their runtime log.
Automatic screenshots are disabled during normal play; use the preview executable's
`--capture-frames` option when diagnostic BMP captures are specifically needed.

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
preserving aspect at the width limit and rounding to even guest dimensions.

Open **Options > Video Settings** for the game's native graphics rows:

- **Gamma:** 0.50 to 1.50, with 1.00 neutral. Lower values darken the image;
  changes apply immediately and save automatically.
- **Field of view:** Original or **60�120 horizontal degrees at 16:9**;
  ultrawide displays show more at the sides.
- **Bloom:** on by default; toggle the final-composite glow.
- **Vertical sync:** off by default; synchronize presentation to the display.
- **Frame limit:** 30, 60 (default), 90, 120, 144, 165, 240, 360, or Unlimited.
- **Resolution:** 360p, 480p, 720p, 1080p, 1440p or 2160p; requires a restart.
- **Display:** windowed or borderless fullscreen.
- **Antialiasing:** Off (default) or FXAA; smooths edges immediately and saves automatically.

Motion blur is disabled. Use the normal menu navigation: Up/Down selects a row,
Left/Right changes its value, and confirm cycles forward. The controller D-pad
and A work through the same original menu controls. Escape/Backspace or B goes
back. Changes save automatically to `DarkRecomp.settings.ini` beside `Darkness`
and apply immediately except resolution. A failed save is shown in the row text.

All play modes use saved settings. Direct-launch options `--fov 100`, `--fps 120`,
`--render-height 720`, `--fullscreen` / `--windowed`, and `--vsync` / `--no-vsync`
override their saved values for that run. `--fov 0` selects Original. Editing a
menu option saves the current selection, including overrides. Valid custom
values remain unchanged until you adjust their row.

Click inside the game to capture the mouse. The first click only captures it.
Press **F1** for the controls guide, **F2** to toggle capture, or **Escape**
to pause and release the cursor. Alt-Tab, loss of focus, and closing the window
also release it. Click again after returning to the game.

- **WASD:** move; **mouse:** look.
- **Space:** jump while the mouse is captured; confirm or skip intros when released.
- **E:** use / confirm; **R:** reload; **Ctrl** or **C:** crouch.
- **Left click:** fire the right weapon; **right click:** fire the left weapon.
- **Middle click** or **Shift:** zoom.
- **Wheel** or **1 / 2:** cycle weapons.
- **Q:** manifest Darkness; **G:** use Darkness power; **3 / 4:** cycle powers.
- **F:** redirect Darkling; **Tab:** journal; **Enter:** Start / pause.
- **Menus:** arrows to navigate, **E** or released **Space** to confirm,
  **Escape** or **Backspace** to go back.
- **Keyboard alternatives:** I / J / K / L to look, Z / X to fire left / right.

The original game's camera sensitivity controls the controller. Mouse look
uses a separate, linear sensitivity and follows the game's inversion option.
When launching `DarkRecomp.exe` directly, `--mouse-sensitivity 1.0` sets the
mouse multiplier (0.1 to 10). Add `--mute --timeout-ms 0 --engine-preview` for a
muted interactive run. The play modes supply the interactive flags; the default
mode plays with sound instead of `--mute`.

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
bindings, such as R/Esc or Shift/middle mouse. The artwork is embedded in the
game; no separate icon download is needed.
