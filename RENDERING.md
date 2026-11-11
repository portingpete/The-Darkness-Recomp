# Native rendering and frame performance

The game runs as a native Windows x64 AOT executable and renders through the
engine's D3D11 backend. The renderer uses original engine geometry, shader
sources and completed device state. It does not interpret console GPU packets.

## Display size and ultrawide

The sound and muted launchers use saved graphics settings and default to
borderless fullscreen. The monitor's
aspect ratio determines the internal render width; the default render height
remains 720. Video Settings now offers 360p, 480p, 720p, 1080p, 1440p and
2160p. A 3440x1440 display can render at 3440x1440 when 1440p is selected;
a 3840x2160 display can render at 3840x2160 when 2160p is selected. Dimensions
are capped at 4096 pixels wide and 2160 high, preserving aspect when the width
limit is reached and rounding to four-pixel guest viewport boundaries. Internal
resolution changes apply on restart.

After enabling **F6 resolution shortcut** in the F5 developer panel, F6 switches
native rasterization between 720p and 1440p during play when the
startup guest buffer is 720 pixels high. It retains the guest dimensions and
aspect, waits for a completed frame, and migrates retained color, depth,
stencil and resolved texture contents before committing the new targets.
Shader and geometry caches remain warm; pending exposure queries keep their
original normalization scale. The selected height saves automatically. A
short status message shows the actual dimensions; unsupported launch modes
or capped aspects show an unavailable message instead. Menu resolution changes
retain their restart behavior.

The PC renderer scales render targets, viewports and copied regions together
by an integer factor of one, two or three. It rasterizes geometry at the selected
resolution; asset textures retain their original size. The guest engine uses
the same aspect ratio at dimensions no greater than 2560x720. For example,
3440x1440 uses a 1720x720 guest mode and two-times native rasterization.
This keeps the original console tile planner and texture memory budgets intact.
Directly enlarging guest allocations previously overflowed its fixed rectangle
descriptor; removing that overflow still exhausted the console texture heap.

`RendererContract` checks scale factors 1/2/3 with offset viewports, scissors,
partial clears, retained color/depth growth, eight-pixel resolve alignment,
resolved-texture sampling and exposure histogram normalization.
`WorldRendererContract` and its WARP variant verify that diagonal geometry has
new edge coverage inside logical pixels and retains it through resolve/present.
Startup captures at 3440x1440 and 3840x2160 also exercise the original game.

Shadow projectors retain the original Xenon fractional comparison filter and its
three-texel box footprint. At native scales two and three, resolved shadow atlases
contribute every physical depth texel covered by that box, with fractional weights
at its edges. Sampling only the logical texel centers discarded additional caster
coverage and caused moving shadows to shimmer despite higher-resolution maps.
Unscaled atlases and CPU textures retain the original sixteen comparisons and
normalization by nine. Depths are compared before their results are blended,
preserving shadow softness, reversed depth, texture addressing and border depths.
The original caster bias is retained. GPU contracts sweep fractional vertical,
horizontal, diagonal and corner edges at scales one, two and three on hardware
and WARP, including rasterized caster coverage inside logical texels and switches
between resolved atlases and CPU textures.

A 2560x1440 in-game Jackie monologue capture exercises this projector. The
earlier fractional-filter repair removed coverage quantization to 1/16. Native
atlas integration also uses the distinct caster depths and partial coverage that
the logical-center filter skipped.

The original selected display mode retains its logical resolution identifiers
while its allocation extents change before buffer creation. The engine then
uses those dimensions for both the perspective projection and CPU visibility
checks. The vertical field of view stays constant as horizontal coverage grows.
Full-screen videos retain their source aspect, and the front-end menu retains
its 16:9 content width. The journal and gameplay HUD keep their original
height-based layout.

The full-screen movie painter's original black background covers the entire
display, including ultrawide pillars after the intro crash. Its separate video
draw retains the fitted menu matrix. The temporary background transform uses
the exact original canvas and restores the fitted matrix before the movie and
following controls, preserving embedded videos and other menu draws.

Authored white intro, character-name and credit text fits its 853x480 canvas
uniformly on displays narrower than 16:9, keeping it centered and inside the
viewport. The original font renderer adds an alpha-matched black shadow while
retaining each glyph's authored position. This adjustment is scoped to the
original heading and title/credits painters; their fades, pictures and other HUD text retain
their existing layout.

Dialogue subtitle rendering retains the original glyph geometry, wrapping,
clipping, colors, fades and outline passes. At higher pixel densities, an owned
atlas rasterizes system Arial outlines through DirectWrite with grayscale
antialiasing. Each glyph fits its original ink rectangle within the unchanged
normalized UV cell; font metrics and the 50%..200% Subtitle Size setting remain
independent of raster quality. The original bitmap is retained at native 720p
and 100% size, and for unsupported glyphs or unavailable font/raster resources.
The scalable typeface follows the original proportions but its letter shapes
can differ from the shipped fonts.

Only the nine verified subtitle string callers tag their draws. Deferred native
outline copies retain that tag through the original VB manager's frame generation
and allocation lifetime. Font metadata binds to the actual owned upload generation
for the Text or Darkness atlas, including both modern color and legacy alpha
uploads. Texture ID reuse cannot attach old glyph metadata to a new image.
Because the native atlas uploads lazily, verified subtitle producers refresh
registration around their draw; unchanged font/image pairs retain the cached atlas.
Raster density uses factors two, four or eight according to render height and
subtitle size. The generated atlas cache has a 64 MiB budget; shipped image limits
remain unchanged. Russian byte-coded fonts use the verified CP1251 content slot.

Alt+Enter toggles borderless fullscreen. Window resizing recreates only the
swapchain views, depth buffer and output readback resource, retaining the device,
world textures and internal scene buffers. Presentation fits the existing scene
without cropping or stretching. Restart to change the internal aspect ratio;
`--width W --height H` selects an explicit aspect and window size. See
[CONTROLS.md](CONTROLS.md) for launcher and input options.

Tentacle menus retain the original gray fade, destination-alpha mask and
radial/additive lighting, including the warm floating glow around the tentacles.
`GUIFadeToWhite` uses unboosted source RGB as the fade's backdrop while keeping the
original fade timer and alpha arithmetic. Scaled color-cube copies use point
sampling to prevent pink contamination while the original color lookup keeps its
interpolation. Hardware and WARP contracts cover source color, the gray glow,
the reveal mask and retained light extraction.

Options > Video Settings uses the original CubeMenu and CubeButton classes.
`tools/compile_video_menu.py` builds private `CubeWnd.pc.xcr` and
`GameContext_Create.pc.xdf` files beside the executable. Startup prefetches the
MOS header and XCR_BE menu from the XDF cache, bypassing loose-file opens; both
read paths must be replaced. The archive packager updates only those cached
menu reads, their sizes and offsets. Other cached bytes, links and timestamps
are preserved. The file resolver substitutes both files; shipped assets, other
menu pages, and first-run calibration are unchanged. Both endian pools, shared
child-list offsets, and original key hashes are preserved.

The original button property parser binds each PC action. Names and values use
separate columns, with small text consistently limited to the cell capacity.
The original TEXT callback sizes a button's construction rectangle to its
first label. Initial and live PC values use sixteen small glyphs (including
the arrows and interior padding), reserving eight cells for every choice and
the save-error state. The original property order and sizing callback are
unchanged; there is no construction-rectangle override. Narrow pressed/input
hooks change the setting; the original menu retains focus,
navigation and text rendering. ALWAYSPAINT refreshes the current value through
the original CStr and menu text path. No host controls, popup, or F3 panel is used.

Changes automatically save brightness, FOV, bloom, motion blur, antialiasing, VSync, frame cap, render height and display
mode to an adjacent staged INI copy, then replace the configuration while
preserving other keys. Save failures appear in the native row labels. Frame cap,
VSync and fullscreen apply between completed render frames. Internal height
applies on restart. Bloom and motion blur are sampled together at the beginning
of each render frame, so both toggles apply without restarting.

Brightness is the first Video Settings row, with 50%..200% in 5% steps and
100% as the neutral default. It scales final RGB intensity after Gamma,
clamps highlights to the display range and preserves alpha and black borders.
The portable INI stores `BrightnessPercent=50..200`; absent or invalid values
recover to 100 without changing Gamma or other preferences. Changes apply
immediately, and repeated presents do not accumulate the adjustment.

Gamma follows Brightness using the shipped `CubeOptionButton` and
`CubeOptionMeter`, bound to `OPTG\\VIDEO_GAMMA` with 12 steps. The original
`CubeMenu_VideoSettings` class applies calibration; leaving Options and choosing
Yes saves `VID_GAMMA` in the game profile. The original normalized default is
0.5; its calibration exponent is `1 + (0.5 - VID_GAMMA) * 0.75`.
The former PC `GammaPercent` INI key is ignored and preserved as legacy data.

The native renderer observes completed original gamma tables at the Xbox PM4
emitters, retaining their channel order and frame ordering. `VdGetCurrentDisplayGamma`
returns the HDTV transfer type and power expected by the original D3D helpers.
The 128-segment PWL table consumes rounded 10-bit RGB; the 256-entry table uses
rounded 8-bit indices. Guest frontbuffer format54 and the live preview retain
10-bit storage. Calibration runs once on source texels before output scaling
and optional FXAA, with PC Brightness applied afterward. The raw scene stays
owned separately, so repeated presentation does not accumulate calibration.
Diagnostic BMP captures include the original display calibration, before
optional PC Brightness/FXAA/output fitting. Eleven PC settings and the original
Gamma control fit the original twenty-cell menu grid.

Color grading retains the original 18-by-18-by-18 color cube, stored as a
324x18 lookup texture, at every render resolution. At two-times and three-times
rasterization, each table texel occupies a repeated block of physical pixels.
The final composite now interpolates between the original table entries with
their original weights. The append stage also retains its authored linear
filter instead of inheriting the point filter used for spatial table copies.
This corrects the resolution-dependent grading and banding reported in
[issue #25](https://github.com/portingpete/The-Darkness-Recomp/issues/25).

The correction applies to recognized resolved color tables with supported
base-level linear sampling. Scene resolution, render-target formats, exposure
and the user's gamma preference retain their existing behavior. Hardware and
WARP contracts check all color-table stages and the original copy/append/gamma
chain at scales 1/2/3, including the retail 16-bit table with exponent scaling,
black and near-black colors, fractional interpolation, and renderer-state
changes. A scale-1 result is checked against an independent trilinear oracle;
the higher scales then agree within one 8-bit output level.

Live Consite captures at neutral presentation gamma show the 1440p hand and
metal color bands disappearing. The 720p, 1080p and 2160p outputs were also
inspected, along with the dark Other World sewer scene at 1440p. These live
checks confirm the visible resolution fix; animation and noise make them
unsuitable for pixel-equivalence comparisons.

Darkness Vision uses the original `WClientMod_DV5_0` and `WClientMod_DV5_1`
programs, including their base, radial-add and radial-multiply variants. Missing
native translations previously dropped these final draws, exposing the scene
and radial-blur atlas as stacked quadrants. The translator selects the Xenon
floating-depth branch and preserves ARB `KIL` as a conditional pixel discard.
The first stage writes a depth coverage mask with depth testing disabled;
the second stage uses that mask to fill only discarded pixels. Native depth
writes remain enabled with an ALWAYS comparison in that first stage, matching
the original state, so the second stage cannot overwrite the gold glow.
GPU contracts cover both stages, all variants, atlas coordinates, nonuniform
gold edge glow, the two-stage depth mask, intensity and near/far fades, and
far-depth discard at scales 1/2/3 on hardware and WARP. The original noise maps
in stage-0 slots 2 and 4
can be recovered after a missed or evicted CPU texture upload. Scene, depth
and effect-atlas inputs still come from their GPU resolves.

Other World uses the original `WClientMod_OW1_1` grain pass and
`WClientMod_OW1_2` scene composite. Missing native translations left a
half-size scene view exposed in the upper-left corner of the sewers.
Both programs now retain the Xenon floating-depth reconstruction, world-space
noise, distance fade and scene-minus-grain composition. Only the grain pass's
authored noise in slot 1 is recovered from CPU memory; scene, depth and mask
inputs retain their GPU resolves. Hardware and WARP contracts cover full-screen
composition, separate atlas coordinates, subtraction and alpha, noise and
distance fades at scales 1/2/3. A 3440x1440 sewer replay reproduced the duplicate
before the fix and showed one full-screen view afterward.

`--shadow-capture <new directory>` enables two on-demand F8 captures at complete
render-frame boundaries. Each retains the screenshot, ordered commands and
owned draws. Projector evidence includes the resolved atlas and scene depth,
with depth/stencil and target alpha before and after its draw. Captures leave
subsequent rendering unchanged; the hardware/WARP contracts compare inspected
and ordinary frames. Screenshot I/O failures are logged while gameplay continues,
and existing captures are never overwritten. The preview contract covers failed
writes, continued rendering, recovery and overwrite protection.

Antialiasing offers Off (default) and FXAA, stored as `Antialiasing=0` or `1`.
Missing or invalid values default to Off. Changes apply at the next presentation
without restarting. Directional FXAA runs on the completed display-encoded image,
including HUD/menu edges, using source-texture dimensions and clamped sampling.
The presentation pass combines filtering with output scaling; it preserves alpha
and fitted black borders. Off retains the original copy/scaling path. The owned
render target and diagnostic screenshots remain unfiltered, so repeated display
copies cannot compound the effect. Integration reference: [NVIDIA FXAA whitepaper](https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/FXAA_WhitePaper.pdf).

Motion blur defaults to Off, including when an older INI has no MotionBlur key.
Its native menu toggle clears only the motion-blur bit in the final composite
shader selection when Off and preserves the original game's bit when On.
Bloom defaults to the original effect; disabling it
also clears the glow bit. Existing shader permutations preserve exposure and
color mapping. Velocity generation, radial Darkness effects and the original
shader assets remain intact. The original captured draw metadata is unchanged;
the selected shader's reflected texture mask controls its actual bindings.

At 1440p and 4K, bloom retains the original 720p sampling footprint. The
sixteen-tap `XRUtil_ShrinkTexture8` filter and seventeen-tap
`XREngine_GaussClamped` filter sample a cached logical-size floating-point
view of resolved color data, averaging every 2x2 or 3x3 physical pixel block.
Their output is evaluated at logical pixel centers, and Final5 filters its
bloom input on that same grid. Original weights, offsets, atlas clamps,
exposure and fetch exponents remain intact. This prevents gaps between taps
from dropping small highlights or producing patterned halos. Each resolve
invalidates the logical view; resolution changes discard it. Helper draws
are excluded from exposure histogram queries. CPU images, point samplers,
scene detail and the sharp component of the damage effect keep their existing
sampling. Hardware and WARP bloom contracts cover fine-light phases and
independent filter oracles at 720p, 1440p and 4K.

Lamp coronas also use the original flare visibility callback. Its Xbox query
fallback supplied a nearly zero count to the native game, suppressing the
flare before bloom. The native renderer now measures the original colorless,
depth-tested patch asynchronously and feeds completed logical pixel counts
back to the callback. Flare IDs have separate result owners from exposure
histograms; pending measurements retain the last completion, including zero.
Tests verify visible, partial and hidden patches, unchanged scene color/depth,
and ordered completion at every supported rendering scale.

The FOV preference remains portable;
`--fov` overrides it for the current run. Values are horizontal degrees at
16:9, with zero preserving Original. The native player-camera query applies
the adjustment to fresh FOV input before the original viewport store and
shared projection/culling dirty flag. Initialization and property hooks retain
the player's unzoomed profile; copied state retains that same profile. The
adjustment preserves the tangent ratio of zoom and scripted FOV changes.
Unknown profiles, other client types, and authored square cameras retain their
original values. Original has a direct-call fast path. `DARK_FOV_PROBE=1`
enables bounded camera evidence for diagnostic runs.

Validation for the native menu correction is limited to compilation and
non-rendering contracts: startup archive reads and unrelated cached bytes,
guest file imports returning the packaged replacements, registry encoding/preservation, original button
construction and property/input callbacks in private memory, persistence,
resolution recovery, shader flags and frame pacing. No game launch, screenshot,
GPU test or visual inspection is performed. In-game appearance, navigation and
launch recovery require user testing.

## Frame pacing

`build_native/Release/DarkRecompPreview.exe` launches the interactive game muted
with the saved frame cap (60 FPS by default). The native executable and
`tools/run_native.py` also default to 60 FPS; `--fps 120` selects the higher target and `--fps 0` disables
the frame cap. A high-resolution timer
paces new frame submissions without changing the original simulation clock or speeding
up gameplay. The window title reports accepted new engine frames; repeated
DXGI presents are counted separately in the log.

The interactive launcher no longer requests automatic preview, video, color and
world screenshots during normal play. These diagnostic captures synchronously
read back the GPU, convert every pixel and write a BMP on the display thread.
The 2026-09-13 desktop run wrote eleven 19,814,454-byte BMPs; some later hitches
occurred after its final capture, so removing captures is not a complete explanation
of that run's stutter. `DarkRecompPreview.exe --capture-frames` retains the opt-in
capture path; direct native runs still accept `--preview-frame <new BMP path>`.

Normal interactive launches pass `--trace-frame-hitches`. This enables the existing
`FrameOutlier` stage-boundary timings without per-draw profiling or instruction
sampling. It records accepted-frame intervals over 34 ms, at most once per second
and at most 256 lines per run. Durations include waiting and scheduling, not just
CPU execution; queue waits, rendering, post-render work, pacing, presentation and
loop-tail work are distinguished. `FrameHitchTrace` logs the active diagnostics
and capture mode at startup. `--profile-engine` continues to enable this trace too.

Stdout/stderr are fully buffered (64KB) so steady-state logging is memcpy-fast
and no hot thread pays disk latency per write; `DARK_UNBUFFERED_LOG=1` restores
unbuffered output for debugging. Buffers flush every 5s in the report block, on
clean shutdown, before every fatal `ExitProcess`, in the SEH fault filter, and
via an unhandled-exception filter that flushes before WER teardown. A live
150s run (`stutter-diag-20260915-114201`) caught a metronomic ~73ms tail hitch
on the first accepted frame after each 5s report: per-call `[ReportCost]`
timing showed the block itself costing 45-60ms (preview ~28ms, audio ~11ms,
thread-CPU ~4ms, scene ~3ms) purely in unbuffered write stalls. After
buffering, the same scenario (`stutter-diag3-20260915-115058`) holds 120 FPS
with p99 under 9.2ms and no report-linked hitches. `[ReportCost]` logs only
totals over 10ms, so steady runs stay quiet while regressions resurface.

The display reuses an unchanged completed image after a confirmed `S_OK`
present. New queue parts, settings changes, repaint/show/activation events and
resizes invalidate that reuse. Occlusion and other present statuses keep the
normal retry path active. The existing queue wait still wakes on new engine
work; the fast path adds no sleep or simulation delay. `[DisplayReuse]` reports
skipped copies/presents, and accepted-frame metrics still count only newly
rendered frames. `DisplayReuseContract` checks real GPU output with a quiescent
producer, repaint, resize, partial frames, gamma/brightness and FXAA changes;
hidden-window checks model acceptance separately from actual DXGI statuses.

Completed authored texture uploads are prepared on the display thread during
loading, between complete render frames. A queue of at most 4,096 weak image
references avoids extending the lifetime of evicted CPU snapshots. Each loop
examines at most 64 entries and stops after 4 MiB or 1 ms of preparation. The
first image can exceed these thresholds because a device call cannot be
preempted. Preloading uses spare GPU-cache capacity; unused prepared images
receive the lowest eviction priority until a draw samples them.

Complete authored mip chains initialize all texture faces and levels through
one immutable D3D11 texture creation. Sparse resident chains and generated mips
retain their existing upload paths. Hardware and WARP contracts check exact
authored pixels, all 24 cubemap subresources, mip selection, binding preservation,
cache pressure and upload reuse. `[TexturePreload]` reports preparation time,
backlog, expired entries and failures. Routine APC completions in alertable
audio waits share the bounded startup trace instead of logging every refill.

Small deadline overruns retain the existing pacing schedule to avoid accumulating
timing drift. A delay of at least one additional frame period resets the schedule
so recovery does not chase a backlog of missed deadlines. Timer failures report
an error and terminate safely while the guest still owns live worker threads.

Performance log version 3 adds `renderedFPS` and separate `presentOk`,
`presentOccluded`, `presentFailed` and `presentOther` counts. These distinguish
continued rendering on an occluded desktop from a lack of new engine frames.
Frame percentiles and `frameMaxms` still describe completed accepted-frame
intervals; `intervalSamples=0` means there are no such samples. The unfinished
gap is reported separately as `lastAcceptedAgeMs`, with `haveAcceptedFrame`
distinguishing startup from a gap after an accepted frame. These are application
timings, not measurements of monitor scanout.

### Frame-drop timeline

`Launch.cmd frame-drops` (or `LaunchFrameDrops.cmd`) runs `DarkRecomp.exe` with
`--frame-timeline <dir>/frames.csv`, `--trace-frame-hitches` and
`DARKRECOMP_STALL_PROFILE=1`. The CSV has one row per accepted frame; rows are
buffered on the display thread and written by a below-normal-priority thread about
once per second, so a crash loses at most the last second. `--frame-timeline`
requires the gameplay renderer and a CSV path that does not exist yet.

Each row covers the interval since the previous accepted Present:

- Display-thread stages, summed over every loop iteration in the interval (the
  existing `FrameOutlier` boundaries): `take_ms` waiting for the engine frame,
  `render_ms` D3D11 submission, `post_ms` captures and display copy, `pacer_ms`
  the frame limiter, `present_ms` DXGI Present, `tail_ms` work after Present (the
  5 s report, texture preloads), `pump_ms` window messages.
- `gpu_ms`: D3D11 timestamp span of the frame's commands, read back without
  stalling. `gpu_idle_before_ms` is the gap since the previous frame's commands
  finished; near zero on consecutive frames means the GPU is saturated.
- `engine_*`: the engine (guest) frame shown by this Present, from the producer
  timeline: wall time between engine frame ends, engine-thread CPU time,
  back-pressure wait, Present latency and command/draw counts. A wall time far
  above its CPU time means the engine thread waited or was descheduled.
- `io_*`: guest `NtReadFile` reads completed in the interval; `engine_io_ms` is the
  part on the engine thread.
- Render work: draws, image/geometry/transient uploads (count, KiB, ms), surface
  creation, vertex-decode time, new and compiled vertex shaders, and draws
  deferred by the per-frame upload budget.
- `display_cpu_ms`, `process_cpu_ms` and `page_faults` from
  `QueryThreadCycleTime`, `QueryProcessCycleTime` and `GetProcessMemoryInfo`.
  Cycle counts are converted with the time-stamp counter rate measured over the
  first second, so these columns (and `engine_cpu_ms`, from the engine thread's
  cycle count) are empty during that second. `GetThreadTimes` was used before;
  it advances in 15.6 ms scheduler ticks, so per-frame CPU read as 0 or 15.62.
- `markers` counts F9 presses; `report` marks the interval that absorbed the 5 s
  report; `stall_serial` matches `[STALL] frame=N stream=display`.

The stall profiler does not time the calls made for every world draw (the
`StallProfiler::hot` scopes: the captured D3D, vertex-program, palette and
geometry functions, `KeTlsGetValue`, and `WorldRendererD3D11::draw`); waits
inside them keep their own scopes. Timing them made engine frames about 20%
slower in busy views (1.60-1.67 instead of 1.30-1.34 microseconds per world
draw on the village route), which distorted the very frame times this mode
records. With them off the capture costs within 2% of normal play (1.32-1.36,
and 1.25-1.28 on the final build in `build_native/run/claude-fd-22917-26382`;
normal play options measured 1.30-1.34 in `build_native/run/claude-play-14586-1245`).
Set `DARKRECOMP_STALL_PROFILE=2` to time them anyway.

In this mode the stall profiler reports only frames slower than
`max(1.5 x budget, budget + 4 ms)` (25 ms at 60 FPS) and omits standalone
`[STALL] call`/`[WAIT]` lines; each slow-frame report keeps its ranked
contributors. `[STALL]` frame headers carry `end_qpc`, which converts to the CSV
clock with its `origin_qpc` and `qpc_frequency` comments, so engine-thread
(`stream=guest`) reports can be joined to rows through `engine_boundary_ms`.
`tools/analyze_frame_drops.py <run folder>` classifies each slow frame (engine
CPU, engine file reads, engine waits, shader compiles, uploads, render CPU,
GPU-bound, Present, frame limiter, post-Present work), groups episodes, finds
periodic hitches, and lists the stall contributors of the worst frames. A frame
whose engine frame lasts at least 85% of the interval and longer than the
display thread's own work counts as an engine frame even when rendering was the
largest display stage; before, such frames were reported as gpu-bound because
the GPU span follows the interval. The game log ends with two
`[FrameDropSummary]` lines.

## Saves

Offline single-player (user0) saves live portably under
`<gameDirectory>/../saves` (normally `K:/DarkRecomp/saves`) as
`00000001_<filename>` content directories; normal play uses `DEFAULT`,
holding `_profile`, `Chapter1` and `Checkpoint`. The title owns the file
formats and the original assets stay read-only. Writable device selection and
content create/open/enumerate plus file write/flush/size/disposition paths are
implemented through the native XAM/NT imports. Final verification
(`build_native/framerate-stability-20260909/FINAL-VERIFICATION.md`) passed
all 47 Release tests; the final muted 300-second replay captured 14,158,804
world draws with zero rejections, queue drops or audio errors. Displayed FPS
remains unverified (all presents occluded) as do full campaign play and
in-game checkpoint Resume.

For a bounded, muted run from the workspace root:

```powershell
python tools/run_native.py --engine-preview --mute --fps 120 --timeout-ms 150000
```

Space skips intro videos. Keyboard and mouse behavior is documented separately
in `CONTROLS.md`. On-screen prompts default to the keyboard/mouse set and
follow the active input source; see `CONTROLS.md` for the switching behavior.

## Rendering changes

- Uploaded world geometry retains vertex count and skinning extrema instead
  of a second expanded CPU vertex array. GPU bytes, immutable packed ownership,
  upload budgets and index validation are unchanged. Metadata takes 68 bytes
  per cached geometry instead of 240 bytes per vertex. Present index streams
  are scanned together with SSE2, preserving the previous extrema on equal
  values; unskinned meshes need no full scan. Explicit inspection reconstructs
  just its three selected vertices from the immutable packed source.
  `WorldVertexMetadataContract` compares the former algorithm across supported
  formats, signed zeros and subnormals, and checks diagnostic subset failures.
- Position-only immediate meshes with at least 128 vertices copy directly to
  their final owned storage, avoiding a temporary allocation and interleave
  pass. Other captures retain the original stream-copy order. SSE2 checks raw float exponent
  bits without byte swaps or floating arithmetic; packed fields remain exempt
  and scalar tails read only complete words. The existing bounded retention
  cache still compares complete metadata and owned bytes. Scene contracts check
  exact bytes, hash collisions, eviction, snapshot lifetime, changed/retained
  sources, sparse/all-stream layouts, finite edge cases and nonfinite values in
  vector lanes and tails.
- Compiled world vertex and fragment shaders use a content-keyed disk cache.
  Keys include the complete source and wrappers, entry/profile, compiler flags,
  macros and the exact compiler DLL. Cached bytes retain driver creation and
  fragment reflection checks. Invalid, corrupt or unavailable entries fall
  back to compilation; successful compilation atomically repairs old entries.
  Vertex cache hits reuse the validated shader object. `[RenderShaderCache]` reports
  fragment hits, misses and initialization time. The hardware/WARP cache
  contracts cover identity, reflection, corruption, truncation and failed writes.
- Native audio submission reuses sixteen PCM slots and a fixed diagnostic
  snapshot, eliminating steady-state vector allocations. SSSE3 byte shuffles
  convert planar big-endian samples to interleaved host order without floating
  arithmetic; the baseline CPU build keeps scalar conversion. Audio contracts
  compare IEEE bits, alignment and protected-page boundaries, then exercise
  queue saturation, completion, errors, registration and teardown on XAudio2.
  Optional `AudioPcmBenchmark --benchmark` times conversion and preparation;
  these operation timings do not establish a gameplay FPS improvement.
- Draw-object recycling transfers empty snapshots in batches of 32 between
  the producer and consumer. Up to eight rendering threads receive a private
  reserve; additional threads use the synchronized shared pool directly.
  The shared pool retains at most 4,096 objects, plus 256 across all reserves.
  These bounded stores have process lifetime, so snapshots can be released
  after a producing thread exits or during static teardown. Geometry/images
  are released before an object enters either store, and capture retains the
  same unused-bank resets and failure handling. Shared-pointer control blocks
  still use the normal allocator; this does not remove every draw allocation.
  `DrawPoolContract` retains 14,400 snapshots across 24 producer lifetimes,
  checks unused-bank clearing after failed captures and overflow, and verifies
  that recycled objects do not retain images or geometry. The optional
  `SceneSubmissionBenchmark --threaded` uses live 512-command streaming,
  validates every depth draw, and includes consumer release before finishing
  each measured frame. It measures the CPU handoff without rendering on a GPU.
- Stored geometry lookups use a bounded index for the original 16-bit resource
  IDs. The owning map and its live binding, generation, range and budget checks
  remain authoritative; replacement, address reuse and eviction clear the index
  under the same mutex. Retained draws keep shared ownership independently.
  `StoredGeometryContract` covers index lifetime through map growth, ID bounds,
  failed replacement, address reuse and byte/entry eviction.
- Queue resource accounting reuses a non-owning, open-addressed pointer set
  across batches. Epoch clears retain its storage instead of destroying one
  allocated node per resource each batch. Geometry accounting also borrows the
  identities already owned by the caller and captured draw, avoiding temporary
  shared ownership increments on the producer/consumer path. Queue order,
  unique-byte charging, resource ownership and limits are unchanged.
  `FrameResourceSetContract` compares membership and duplicate charging against
  `unordered_set` through growth and 128 reused batches; `EngineMeshContract`
  exercises live queue publication and resource budgets.
  Build the optional `SceneSubmissionBenchmark` target and run
  `build_native/Release/SceneSubmissionBenchmark.exe` for CSV medians of seven
  batches: stored lookups plus 3,072-draw depth-frame capture/queue/drain with
  64/512/2,048 resource working sets. This uses a private guest-memory fixture,
  no game saves, and same-thread draining. It measures CPU work, not GPU time,
  cross-thread contention, textured scene cost or displayed gameplay FPS.
- Validate immediate vertex snapshots directly in their packed storage before
  publication. Float fields retain the decoder's NaN/Inf checks, including
  unused semantic slots; integer and normalized packed fields are finite by
  construction. Validation and decoding share the same format and extent
  checks. This removes a discarded 240-byte-per-vertex expansion on cache
  misses. Index capture also converts endian order in its owned output buffer,
  avoiding a temporary allocation and preserving queued snapshots. Existing
  recovery of unreferenced invalid vertices stays intact.
  `WorldVertexPreparationContract` compares validation with full decoding over
  all supported formats and malformed inputs. `WorldRendererContract` and its
  WARP variant exercise the recovery and rendered output. For repeatable CPU
  timings, build the optional `WorldGeometryBenchmark` target and run
  `build_native/Release/WorldGeometryBenchmark.exe`; its CSV reports medians of
  nine batches, including changing and repeated geometry. These are capture
  timings, not displayed FPS measurements.
- Preserve the original texture and fragment shader for depth-only alpha
  coverage and alpha-tested materials. Grass cutouts must mask depth before
  later EQUAL-depth lighting; previously these draws wrote solid polygons.
  Opaque depth draws with an ALWAYS alpha comparison still skip texture work.
  Capture tests cover both alpha modes and color write masks; GPU tests check
  transparent/opaque pixels, depth/stencil preservation and the subsequent
  lighting pass at native render scales 1/2/3.
- Capture whole-resource indexed draws as well as explicit subsets. The
  original `8225E218` path supplies both VB and IB from one resource ID and
  submits at `8225E2E0`; omitting it hid held pistols and some level objects.
  The native bridge now retains that scope through the completed triangle-list
  draw, checks actual buffer bindings and complete index range, and preserves
  every original material pass and its order.
- Read fragment constants from the original completed device upload, including
  the shader's intentional unused NaN values during the zero-blur GUI fade.
  Retain up to 64 vectors so lit blood/decal programs can use their original
  lighting and projector constants beyond vector 15.
- Translate the original water, projected-mark, lit-decal and world-video
  fragment programs. Water retains authored normal/fog maps alongside GPU
  reflection/refraction inputs; decals retain plane clipping, lighting,
  projection and alpha. Video snapshots retain raw big-endian V,U chroma;
  native sampling converts it to logical U,V without changing immutable frame
  generations. Hardware/WARP tests exercise their rendered
  output at 1x, 2x and 3x resolution; capture tests call the original binders.
- Retain each original decoded triangle-list index chunk until its matching
  indexed draw, then copy it into the owned render queue. Invalid or mismatched
  draws consume the pending chunk so later draws cannot reuse stale indices.
- Follow the original resource's primary/alternate texture selection, including
  inline texture objects and readiness checks.
- Follow completed sampler filtering, addressing, LOD and anisotropy settings.
  The texture-filtering menu applies explicit 2x/4x/8x/16x selections to eligible
  owned 2D surface images, overriding their authored anisotropy. Original
  preserves the captured game setting, which already requests 16x on many world
  textures. Point/mixed/base-only sampling, cubemaps, resolved textures, video,
  GUI and postprocess filters retain their original behavior. Generate native
  mip chains from decoded base images to reduce texture aliasing.
- Capture and retain only texture slots declared by the original fragment
  permutation. Depth and texture-free passes skip texture work. Shader creation
  checks that this conservative mask includes every compiled texture binding;
  unknown permutations retain all slots for existing diagnostics.
- Retain immutable vertex/index buffers and resident images, with bounded caches
  and ownership-aware retirement. The image budget adapts to available hardware.
- Cache D3D11 state objects and unchanged constant uploads. Validate skinning
  indices using cached stream extrema instead of rescanning each material pass.
- Upload per-draw constants through DISCARD-mapped dynamic buffers instead of
  UpdateSubresource; content-change gating is unchanged, only the mechanism.
  No measured frame-time delta (uploads were already rare); it removes a
  documented-slower driver path and its per-vendor variance.
- Reuse unchanged D3D11 bindings between world draws, with explicit invalidation
  for GUI rendering, resolves, resized surfaces and external context changes.
  Cache index bounds while preserving valid subsets of shared index buffers.
- Reuse identical immediate vertex/index data by comparing complete owned
  bytes in a bounded cache. Changed source data creates a separate snapshot.
- Restore render targets after presentation, including a later identical pass.
- Observe clear rectangles after the original engine applies viewport and
  scissor clipping. Clear selected color, depth and stencil planes within that
  rectangle, preserving other pixels and larger retained attachment borders.
  Whole-surface clears use the D3D11 clear path; partial clears use a native
  shader pass. Split and resume active exposure queries so helper triangles
  do not contribute to the original scene's measured samples.
- Snapshot original engine memory with range-checked, exception-guarded copies.
  Preserve completed binding validation while removing redundant reconstruction.
- Keep expensive render readbacks and comparisons out of normal play. They
  remain available through explicit renderer diagnostics.
- Build draw snapshots directly in their final owned allocation. Retain only
  prepared constants in the render queue instead of duplicating the source bank.
- Wake the consumer when an engine frame becomes available. A late frame no
  longer incurs an additional full display-period wait.
- Retain the video luma/chroma textures and their shader views while dimensions
  are unchanged, updating their pixels in draw order. A size change creates a
  complete replacement pair before publishing it. This removes per-video-frame
  texture/view creation while preserving earlier draws and retained video
  snapshots. Pixel tests cover interleaved frames in separate screen regions,
  row pitch, chroma order, resizing and invalid input.
- Pace fresh stored geometry/index decode and upload at 4 MiB per render frame.
  A dense first-sight page-in measured at ~25MB in a single frame froze one
  render for 81ms; the budget spreads it over subsequent frames instead.
  Deferred draws are rejected for that frame only and reappear via the
  engine's later snapshots. Transient immediate snapshots are exempt (small
  steady churn that cannot amortize), the first upload of a frame always
  proceeds (no starvation), and small scenes never reach the budget.
  `WorldRendererContract` (hardware and WARP) covers burst deferral, drain to
  completion, and pixel-identical output; `[RenderBudget]` reports cumulative
  deferrals plus the window's worst single decode/upload (ms, bytes, site:
  vertex/index/image/surface, transient or stored) to attribute future hitches.
  A live deep route showed 21k deferrals inside one page-in window and zero
  afterwards across 23M draws; the tripwire then showed worst singles of only
  ~9ms, proving the remaining heavy frames are aggregate bursts rather than
  one giant upload.

## Native CPU build and diagnostics

### Execution flicker: completed surface storage

World draw, clear and resolve commands now own the completed surface binding
from the original device (`82861698` and `8285E378`): the shared layout word
at device+10368 and the five color/depth allocation words. Temporary surface
object pointers are retained for diagnostics, not used as native storage keys.
Different descriptors for the same tile base, pitch/sample layout and storage
pixel width share the same retained native surface. Reusing a descriptor for a
different allocation no longer selects its old scene. Capture never dereferences
a mutable surface header and publishes nothing when the completed state changes
during the copy. Null attachments do not revive leftover device register words.

The existing normalized native color storage is shared across color precision
and exponent views (including the original 2/10 and 3/12 switches). The 64-bit
storage formats 5, 7 and 15 remain separate from 32-bit storage. Different tile
bases or layouts are not guessed to be equivalent, even if their allocations
overlap; this change does not implement bitwise EDRAM reinterpretation between
incompatible views. Color and depth remain distinct native planes. Resolved
texture ownership, partial/cube resolves, source-region clears, exposure
queries and retained extents keep their existing behavior. Resolve source
clears copy the same owned binding, and optional readbacks use that binding too.
Synthetic renderer commands use a disjoint identity namespace.

`WorldSurfaceBindingContract` invokes the original surface initializer and
binder in private CPU fixture memory, then verifies owned draw/clear/resolve
lookup across descriptor replacement/recycling, original format adjustments,
layout changes, all attachments and queued/pool lifetime. It creates no graphics
device or game window. Its pre-correction run reproduced a missing resolve
source when object 03001000 was replaced by 03001100 with identical completed
device storage. The latest user's text log also contains full-screen color
resolves rejected as `source-missing`; those messages now include the owned
storage key and original completed words. Gameplay appearance and whether the
intermittent flicker is resolved require user testing.

Native builds hash the contents of application, renderer, native runtime and
C++ test inputs on every build. A forced-include fingerprint header is rewritten
only when those contents change, so a rollback with older file timestamps still
recompiles native objects. Linker mismatch records reject native objects from
different fingerprints. The executable logs its fingerprint at startup; the
matching input inventory is `build_native/native_inputs.json`. Unchanged builds
preserve the stamp and binary timestamps. The separate AOT manifest still
verifies the generated instruction code.

`DARK_NATIVE_SSSE3=ON` enables native integer byte shuffles in the generated AOT
code. The executable checks CPU support at startup. Configure with
`-DDARK_NATIVE_SSSE3=OFF` to build for baseline x64. Strict floating-point behavior
and the existing dot-product reduction order are retained; SSE4.1 dot products
and FMA are not enabled by this option.

Game worker affinity maps the six original processor identities onto distinct
physical host cores when available. On hosts with at least eight fast cores,
the first two remain available for the engine/display scheduling hints.
The original processor numbers and synchronization slots remain unchanged.
Topology comes from Windows [CPU Sets](https://learn.microsoft.com/en-us/windows/win32/procthread/cpu-sets).
Process affinity restrictions and smaller CPU configurations are respected.
The display (present/input) thread additionally runs at
`THREAD_PRIORITY_ABOVE_NORMAL` while the engine thread keeps normal priority,
so presentation stays responsive when engine workers saturate cores during
level loads. A 150s run showed 1.3-1.6s `Present` blocks at transitions with
normal priority (`stutter-diag-20260915-114201`); with the boost the same
route holds 120 FPS with worst 5s windows under 18ms
(`stutter-diag4-20260915-120956`). Guest timing is unaffected (priority only
changes scheduling, and the engine's own priority is untouched).

The native delay import retains the original signed 100 ns interval using a
Windows [high-resolution waitable timer](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw).
Negative intervals remain relative and positive intervals remain absolute UTC
deadlines, following [SetWaitableTimer](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setwaitabletimer).
Zero intervals still yield through SleepEx. Alertable waits preserve APC status;
an APC that reenters the import receives a separate timer until the outer wait
returns. Each thread caches its ordinary timer, cancels interrupted waits and
closes the handle at thread exit. Windows versions without high-resolution
timers retain the previous SleepEx fallback. Original game sleep calls and
simulation intervals are unchanged.

Draw capture retains descriptor metadata separately from the constant bank.
It fills the final owned bank once, then performs an exception-guarded byte
comparison bracketed by the original source/binding checks. Failed captures
are discarded before publication.

The vertex-shader caches own their keys, but hits use length-aware string
views rather than allocating temporary strings. D3D11 state caches own fixed
byte keys, mix eight-byte words on lookup and bypass hashing for an exact repeat
of the last descriptor. Equality checks every descriptor byte, preserving the
previous selection semantics; the existing 512-entry clearing rule and retained
material-slot references still apply. Inspection labels are
constructed only when inspection actually needs them. Per-draw CPU clock reads
are disabled during ordinary play; the log reports `timing=disabled` rather than
reporting an unmeasured zero cost.

`--profile-engine` adds inclusive engine and renderer phase timings. `--sample-engine` adds a
local native instruction sampler and can perturb frame performance; leave it
off when measuring FPS. `--sample-workers` also samples registered runtime
worker threads, logging TID/creation identity and cumulative CPU/user times.
Compare CPU-time deltas separately from round-robin wall-clock samples, which
include waits. Source lines are optimized-code regions, not exclusive timings;
reported top lists are censored. Symbol lookup uses the executable directory's PDB.
Periodic sampler reports are requested asynchronously and resolved on the
sampler worker, with all sampled threads resumed. This keeps symbol lookup and
report formatting off the display thread. Older profiling builds performed
this work synchronously and could add a pause at each five-second report;
that diagnostic artifact does not explain stutter in normal launches.
With `--profile-engine`, `vqXma`, `vqKernel`, `vqDispatcher`, `vqAudio`, and
`vqFile` report elapsed milliseconds / actual `VirtualQuery` calls. XMA cache
hits are excluded. These elapsed times aggregate across threads and include
scheduling delays. `[XmaBatch]` reports batches entered, requested contexts
(including an undecoded suffix on failure), and batches of size one.
`--trace-renderer` enables expensive validation and is also unsuitable for
normal performance measurement.

The current pacing loop waits after rendering and immediately before Present.
The log's `engineFPS` counts new engine frames accepted by Present with `S_OK`,
`worldFPS` counts accepted frames containing the resolved world, and
`presentsFPS` counts all DXGI Present calls, including repeated buffers.
Frame intervals use the timestamp immediately after accepted Present returns;
P95, P99 and maximum intervals include capture/readback stalls. Render CPU time
excludes pacing and captures. These are submission-cadence measurements, not
independent measurements of monitor scanout. Historical logs before the
versioned metric-definition marker measured intervals at render completion.
A 120 FPS cap alone does not prove that a scene sustains 120 FPS. Loading,
shader compilation and explicit BMP captures may stall individual frames.

## Transient geometry buffers (2026-09-13)

The no-capture desktop run `desktop-20260913-205037-334-40040` still contained
38.783 and 39.563 ms accepted-frame intervals, with 35.648 and 33.613 ms inside
rendering. Those are elapsed stage durations, not exclusive CPU or GPU timings.
The same run repeatedly uploaded thousands of short-lived vertex/index snapshots
per five-second interval while retained geometry stayed below its cache budgets.

Immediate snapshots (zero source address and ID) now use dynamic D3D11 buffers.
When an obsolete snapshot leaves the renderer cache, only its GPU buffer can enter
a reusable reserve; its CPU snapshot and resource references are still released.
The next upload uses WRITE_DISCARD so queued GPU commands retain their previous
contents. Vertex and index storage never mix. Best-fit reuse accepts at most twice
the requested size; retained spare storage is capped at 256 buffers and 64 MiB
(raised from 128/32 MiB after live runs showed the reserve pegged at the entry
cap with ~67 evictions/s of churn). Returned buffers carry their last-use frame
and are rehanded only after an event query proves the GPU passed that frame's
marker; polling never blocks, stale evictions reuse immediately, and a removed
device retires everything instead of hanging. Without any frame markers every
entry stays reusable, preserving the original behavior exactly.
Active cache budgets count actual buffer capacity. Stored game resources keep
their existing immutable GPU buffers. TransientGeometry logs creation, reuse and
spare-storage counts independently of logical vertex/index upload counters.

Hardware and WARP debug tests queue 96 distinct copies across repeated buffer
reuse before reading them back, then check every byte, binding compatibility,
entry/byte limits and invalid uploads. Existing rendering tests cover the integrated
path. The isolated hardware probe alternates 64 vertex and 64 index uploads with
indexed draws and verifies every output. Across three process runs, the median of
batch medians fell from 1.523 ms to 0.134 ms for this synthetic upload workload.
This is not a game FPS estimate or evidence that every gameplay hitch is fixed.
The comparison synchronizes after each batch and does not model a full scene's
GPU backlog. A separate constant-buffer mapping experiment was slower and was
not adopted. Evidence is in `build_native/render-hitches-20260913`.

### Adapting the reserve to a changed scene

The first live run with transient buffers, `desktop-20260913-210901-329-32028`,
created 75,592 buffers and reused 33,830 (about 31% reuse). The reserve repeatedly
hit its 128-entry cap while occupying only 5-7 MiB. The initial drop-on-full policy
kept incompatible old buffers indefinitely, rejecting newly returned storage.
The prior empty-reserve benchmark did not exercise this scene-change failure.

The reserve now replaces the oldest returned, unused buffers until the incoming
buffer fits both limits. An oversized incoming buffer is dropped without flushing
useful entries. The 128-entry and 32 MiB bounds remain. Diagnostics also report
cumulative evictions. Regression tests first failed on the prior implementation,
then passed with entry pressure, byte pressure, multiple necessary evictions and
oversized returns, on hardware and WARP with D3D11 debug validation.

A probe prefilled with 128 incompatible buffers made 4,096 allocations and zero
reuses before this fix. With replacement it made 256 allocations (including the
128 seed buffers) and 3,840 reuses, with every indexed GPU output verified. This
explains the reserve policy failure, not the cause of every recorded gameplay
pause. Full-game performance still needs a new run. Reproduction, original sources
and benchmark limits are under `build_native/buffer-retention-20260913`.

### Render-thread sampling

The corrected reserve reached 126,505 reuses and 4,770 creations (96.4% reuse) in
`desktop-20260913-213559-950-4680`, yet gameplay still had 36-40 ms rendering
hitches. The reserve improvement alone did not solve the reported stutter.

`--sample-renderer` samples the calling display thread only while it is inside
`EnginePreviewD3D11::render`. An owned duplicate thread handle is acquired before
the sampler worker starts. An epoch rejects samples crossing render boundaries;
a lock-free phase read occurs while the target is suspended, then the target is
resumed before allocation, locking, symbol lookup or logging. Existing failure
handling never leaves an unresolved suspended thread running silently. Samples
are grouped by textures, vertex geometry, constants, indices, states, submission,
clears, resolves, copies, queries, frame cleanup and other preview work. Reports
run on the sampler worker. External modules are identified even without symbols;
nearest exports are explicitly labelled, not treated as exact function names.

`Launch.cmd render-profile` selects this diagnostic and ordinary hitch tracing,
with no per-draw CPU timing, automatic image capture or guest/worker sampling.
The render sampler is off for normal launchers. Sampling itself perturbs execution,
includes waits inside rendering, and provides instruction/phase observations rather
than stack traces or exclusive CPU times. Broad five-second profiles still cannot
attribute every individual hitch. The sampler contract checks disabled and inactive
gates, phase attribution, safe suspension/resumption, reporting and shutdown.
Evidence for this investigation is in `build_native/render-sampling-20260913`.

## Validation and remaining limits

The 60 FPS default and before-Present pacing build passed all 44 Release tests
in 97.20 seconds. Its initial verification run rendered 6,486,408 world draws
with zero rejections, but the strict new metric accepted zero frames. A separate
smoke test returned `DXGI_STATUS_OCCLUDED` (`0x087A0001`). Interactive desktop
launch approval timed out. Visible 60 FPS pacing is therefore **not yet verified**;
the goal remains open until a visible run can be measured. Repeated Present
calls in the occluded test are not evidence of a steady displayed framerate.

The current memory-layout candidate keeps the C alias at the same guest
addresses but maps it through 32 views of 16 MiB each, backed by the same
physical store. A and E retain their original mappings and E's 4 KiB bias.
Internal commit/release operations split protection changes at view edges.
Kernel validation crosses only these artificial boundaries when region
attributes match; genuine protection changes retain the old rejection rules.
Native code using raw `VirtualProtect` on C must split calls at view boundaries.
The audited production callers outside `Memory` protect worker stacks outside C.

This candidate passed all 44 Release tests, including new shared-backing,
cross-view protection/reuse and kernel boundary fixtures. Muted unprofiled boot
`boot-20260909-110157-642240` averaged 98.18 engine/world FPS across the final
twelve five-second tunnel windows (86.94–112.90), versus 82.14 in preceding
control `boot-20260909-103735-209985`. Mean window P95 fell from 27.70 to
18.43 ms; this is not a pooled percentile. All 10,807,058 world draws were
captured with zero rejections. Repeat `boot-20260909-110504-031244` averaged
97.23 FPS (86.58–113.57), with 18.81 ms mean window P95 and all 10,686,377
world draws captured without rejection. These two runs support retaining the
mapping change; they do not demonstrate steady 120 FPS.

The following measurements describe earlier checkpoints.

The September 9 renderer-loop cleanup passed all 44 Release tests, including
hardware/WARP rendering and native build-rollback checks. The original draw
allocator is retained. A matched allocation experiment used the identical
executable in both modes: the heap control averaged 78.86 tunnel FPS with no
world rejections, while batched allocation averaged 82.15 FPS but rejected 181
world snapshots. That experiment was rejected and its source/tests archived;
step-2 diagnostics alone do not identify the cause of those rejected draws.

With only the renderer-loop cleanup active, muted 170-second boot
`boot-20260909-090901-977308` averaged 82.30 actual engine/world FPS over the
final twelve complete five-second tunnel windows (70.16–98.51 FPS). Mean render
CPU time fell from 6.258 to 4.883 ms versus the matched heap control; the mean
of window P95 frame times fell from 30.293 to 28.169 ms. Window P95 means are
not pooled percentiles. All 9,307,911 world draws were captured, with no world
rejections, queue budget rejections or audio errors. This is partial performance
progress, not steady 120 FPS. The initial September 9 executable predates other
existing source fixes and is not the matched control for this comparison.

Repeat boot `boot-20260909-091618-463635` averaged 82.855 FPS (72.29–94.70)
and 4.905 ms render CPU time, confirming the reduction in render overhead.
Its mean window P95 was 28.737 ms. It also recorded six step-2 skinned-draw
rejections with the original allocator, so those failures are not unique to
the rejected batching experiment. Their cause remains unproven: preparation
currently checks all 156 palette vectors even when a draw uses fewer rows,
but that checkpoint's diagnostic did not report the offending constant. The
current failure-only diagnostic reports the vector, lane and IEEE bits. Both runs
have zero queue budget rejections and audio errors. Stable full rendering and
steady 120 FPS remain unfinished; passing the regression suite does not prove
either condition across gameplay.

An earlier native SSSE3 AOT build passed all 36 Release tests. Hardware and WARP
tests cover sampling, mip filtering, GUI fade pixels, skinning bounds, resource
lifetime, shared index subsets, restored context bindings, texture selection,
consumer wakeups, memory faults, constant bit preservation and original binding ABI.
The suite also verifies physical-core mapping without changing guest processor
identities, direct snapshot capture, and depth/stencil rendering after presentation.
Rectangular-clear tests cover all seven color/depth/stencil combinations,
clipped, one-pixel, empty and full regions, HDR values, retained borders, depth
normalization and queries spanning multiple clears. The GPU tests also pass
with the D3D11 debug layer enabled and reject invalid rendering commands.
The native delay contract verifies relative/absolute deadlines, zero yields,
APC delivery and suppression, reentrant waits, reuse after interruption, and
the original SDK's zero/infinite sleep result mapping.

The September 8 opening-level run `boot-20260908-113314-382754` held about
120 FPS in the light scene. Twelve five-second samples after the car progression
measured 67.99–88.98 actual engine FPS (mean 78.44), excluding the mixed transition
sample. This is not sustained 120 FPS. All 8,951,948 captured world draws were
accepted, with no world command or vertex budget drops. The worker mapping was
confirmed live, but did not produce a material heavy-scene FPS gain in this run.

The subsequent rectangular-clear run `boot-20260908-121015-866540` passed all
34 tests before launch and preserved the same light-scene 120 FPS. Twelve
five-second tunnel samples after the mixed transition measured 65.28–89.98 FPS
(mean 77.66). It accepted all 8,898,728 world captures with no queue budget drops.
The last periodic clear counters recorded 84,826 whole-surface regions and
37,580 partial regions. This correctness fix did not provide a material FPS gain;
the heavy scene still does not sustain 120 FPS.

Selective texture capture passed all 34 tests in 47.90 seconds. All 107 existing
generated shader outputs remain byte-for-byte unchanged; the additional header
contains only source binding metadata. Boot `boot-20260908-122636-855862` again
held about 120 FPS in the light scene. Its twelve tunnel samples measured
68.48–85.99 FPS (mean 77.32), with zero rejected world draws or queue budget drops.
The changed capture path removes unused CPU work, but this run shows no material
heavy-scene FPS gain. CPU profiles are diagnostic runs, not FPS benchmarks.

The native precise-delay build passed all 35 tests in 50.85 seconds; all 92
generated AOT outputs remain byte-for-byte unchanged. Muted 170-second boot
`boot-20260908-125148-283939` measured 72.08–97.31 actual engine FPS across twelve
five-second tunnel samples after the mixed transition (mean 83.55, versus 77.32
in the preceding run). The mean of the twelve reported frame-time P95 values
fell from 31.12 to 27.50 ms; this is not a pooled whole-run percentile. It accepted
all 9,142,042 world draws with no queue budget drops or audio errors. This one-run
comparison indicates improvement, not sustained 120 FPS or a repeatability claim.
The separate delay contract's printed SleepEx timings use the test process's
default timer resolution and must not be substituted for the game benchmark.

An experiment sharing prepared vertex state was subsequently rejected and fully
reverted. Comparing entire source banks reused 52.2% of states but averaged
76.18 tunnel FPS (`boot-20260908-130431-837968`). A refinement comparing only
validated read ranges reused 63.3%, reduced each draw to 1,560 bytes and averaged
80.69 FPS (`boot-20260908-131208-856503`). Both passed all 35 tests and accepted
all world draws, but neither improved on the preceding 83.55 FPS baseline.
The current renderer therefore retains its inline prepared vertex constants.
The rejected patch and benchmark evidence are archived under `build_native/run`;
smaller allocations alone were not evidence of better frame delivery.

A subsequent profile exposed stale native objects after that rollback: restored
source timestamps predated experimental objects, and a later incremental build
mixed draw layouts and crashed. The earlier post-restore test pass therefore
did not prove the rolled-back implementation was rebuilt. The content-fingerprint
guard above fixes this build issue; its integration test exercises backdated
header/source edits, rollback, unchanged builds and rejection of a stale archive.
The rebuilt executable and runtime library were checked for removal of the
rejected cache code before resuming live tests.

With that guard enabled, all 36 Release tests passed in 61.18 seconds, including
hardware/WARP D3D11 debug validation. Muted diagnostic boot
`boot-20260908-133633-375730` completed 170 seconds without a fault and accepted
all 8,767,913 world captures, with no queue budget drops or audio errors. Its
startup fingerprint matched all 73 current native inputs. Instruction sampling
was enabled: this run identifies CPU work and is not a new FPS benchmark.
The reported hotspots include the original job-wait routine `821F1548`, native
waits and draw preparation. That original routine also executes queued work;
replacing it with a sleep would not preserve its behavior.

Bounded missing-texture diagnostics classified all 32 rejected GUI draws in
that run as video materials with no retained decoded texture, across four intro
texture pairs. This does not establish an untextured menu/text rendering defect
or justify a white-texture fallback. Exact initial-video-frame timing remains
unverified. Normal launches do not enable these diagnostics or the CPU sampler.

Generated mip chains are not the original authored console mips. Exact console
surface precision, full-game rendering and gameplay remain unverified. The
current opening-level measurements and visual review are recorded in the
timestamped rendering checkpoint under `build_native/run`.
Some missing-texture GUI draws remain unsupported; zero rejected world draws
does not prove full fidelity.

## Original visual references

The original game's strong bloom remains the default fidelity target. Bright
halos or speckled mirrors alone are not evidence of native rendering defects.
The native Video Settings menu permits the user to turn bloom off explicitly.

On September 8, 2026, visual comparison used WikiGameGuides' July 6, 2007
[opening car chase recording](https://www.youtube.com/watch?v=VlQOpuaYWsI),
whose description identifies Xbox 360 capture. Inspected frames at 0:02 show
strong glow on sunlit faces and surfaces. Tunnel frames at 1:01, 1:06, 1:16 and
1:31 show pronounced lamp halos; the left mirror at 1:06 and 1:16 has a mottled
appearance similar to the native capture's right mirror. These are qualitative
observations, not proof that the native effect matches exactly.

The native `build_native/run/boot-20260908-122636-855862-input-13.bmp` appears
warmer, with broader bright coverage of the ceiling, than the inspected tunnel
frames. This remains a candidate exposure/color difference, not a confirmed
defect: the truck position, camera and simulation instant differ, and the old
video is heavily compressed with unverified capture levels. Compare matching
scene progression, camera and exposure adaptation before changing the renderer.
Preserve localized lamp bloom while checking surrounding wall/ceiling detail,
highlight color and shadow brightness. Brightness alone is not the oracle.

Gamersyde's June 15, 2007
[screenshot gallery](https://www.gamersyde.com/news_images_of_the_darkness-4460_en.html)
provides additional original art reference. The inspected 1280x720
[courtyard image](https://images.gamersyde.com/image_the_darkness-5606-788_0004.jpg)
shows warm highlights and deep shaded surfaces. It is prerelease material tagged
for both Xbox 360 and PS3, in a different scene; use it for broad visual style,
not retail Xbox 360 pixel or exposure calibration. No bloom, exposure or color
parameters were changed as a result of this research.

### State lookup CPU cost (2026-09-13)

The render-only recording `render-profile-30687-28451.log`, reports at
25-55 seconds, captured 7,413 active-render instruction samples. Blend-state
lookup alone accounted for 941; depth-state lookup for 189 and the sampler
function for 442. These top-symbol counts are censored, include waits and
cannot attribute an individual hitch. Source regions and the native code
identify repeated byte-wise descriptor hashing as avoidable CPU work.

`DescriptorCacheTests --benchmark` compares the previous string-view cache
with the new fixed-byte cache at the observed state cardinalities (18 blend,
26 depth, 6 raster and 34 sampler). Seven alternating-order Release trials
check every returned value. Median blend lookup fell from about 312 to
48 ns with shuffled states, and from 314 to 14 ns in repeated material runs;
the other shuffled lookups improved by 55-69%. This measures cache lookup
only, not whole-frame time or gameplay FPS. Tests cover every descriptor
byte, forced hash collisions, source mutation, rehash/reference stability,
clear and selected-value ownership; existing hardware/WARP renderer tests
exercise actual graphics state and sampler eviction during multi-slot draws.
Evidence and before copies: `build_native/state-lookup-20260913/`.

### VSync-off presentation and variable refresh (2026-09-13)

The 60 FPS diagnostic `smoothness-60-5603-22555.log` used build a4abd6b9.
After loading it mostly returned accepted frames near 60 FPS with ~17 ms
P99 intervals, but the user still reported stutter during movement and mouse
aim. One heavy window averaged ~50 FPS; isolated longer pauses remained.
Application frame-return timings do not prove smooth scanout or simulation.

The display previously used FLIP_DISCARD with no swap-chain flags and always
passed zero Present flags. Microsoft requires ALLOW_TEARING on both creation
and sync-interval-zero windowed/borderless presentation for native variable
refresh support: https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays
The backend now queries IDXGIFactory5 feature support before creating the
chain, preserves that flag through ResizeBuffers, and passes
DXGI_PRESENT_ALLOW_TEARING only for interval zero outside exclusive mode.
Unsupported systems retain zero flags. Synchronized presents retain zero
flags even when the chain supports tearing. The optional backend constructor
argument false exercises the compatibility path without changing the machine.

Presentation logs record eligibility and the actual submitted flags, not
confirmation of monitor/driver VRR activation. No frame-cap, VSync preference,
monitor mode, graphics-quality setting or simulation-clock change is included.
The local desktop mode was 3440x1440 at 120 Hz; driver support was reported.
PresentationContract checks real GPU pixels, legal synchronized/unsynchronized
Present calls, repeated resize, failed initialization, replacement, destruction
and reinitialization with support enabled and explicitly disabled. Its hidden
windows are occluded, so it validates API correctness, not visible smoothness.
Evidence: `build_native/presentation-20260913/`. Gameplay confirmation remains
pending; the separate busy-scene game-thread CPU limit is not removed here.


## Experimental parallel scene work

Large owned mesh conversion, float validation, and BC1/BC3 texture decoding can
use a shared host worker pool through `DarkRecomp.exe --scene-workers N`. This
is experimental and defaults to **0 helpers**, including normal preview launches
and lazy initialization from decoder callers. Existing engine, display, audio,
and loader threads are unaffected. The disabled pool executes each job directly
without taking the dispatch mutex or touching its thread-local dispatch state.

The initial automatic setting created 30 helpers on the test machine. The user
reported worse stuttering. The subsequent runtime log showed that nearly all
parallel batches occurred while preparing the area; during gameplay, the engine
thread still used roughly 96-98% of one core while the helper counters barely
changed. The short PresentMon capture also included a 302.6 ms area-transition
stall, so it is not a controlled comparison against the earlier gameplay capture.
Do not enable the automatic pool by default based on mesh microbenchmarks alone.
Use the same route and frame-time recording to validate a future change.

Evidence: `build_native/multicore-rollback-20260913/`. The default-serial contract
checks that no helpers start and geometry/texture output remains correct; the
explicit parallel contract continues to cover worker execution and failure paths.

## CPU submission cleanup (2026-09-13)

StoredGeometryCache validates the shared vertex/index descriptor once when both
IDs select the same resource, while checking both bindings. Separate resources
retain independent liveness checks. Added regressions cover a changed index
descriptor on a combined resource and invalidation of either separate resource.

previewObserveWorld walks active texture slots and keeps a compact list of unique
owned images for batch-budget accounting. WorldDraw partial-bank reset now occurs
before returning the last-released object to the pool. Normal queued draws retire
on the existing render thread, removing that reset from the engine's capture
path. Fresh allocations and failed captures retain the same initialized fields.
Shared ownership, bounded pooling, and prompt geometry/image release are retained.
No additional gameplay workers are enabled.

The normal Release build passed all 64 CTest tests (132.90 seconds), including
DrawPoolContract's 14,400 retained draws and cross-thread/failure/teardown cases.
The threaded depth-submission benchmark reduced per-draw CPU time by 27-30%
across its three resource working sets. This is a synthetic submission workload,
not a claim of a matching whole-game FPS gain.

A hands-off checkpoint comparison, using isolated save copies and matching final
camera screenshots, measured 77.08 FPS before and 83.02 FPS after in six five-second
windows ending between 65 and 92 seconds. Both runs recorded zero mouse input,
zero occluded presents, and zero failed presents. Mean per-window P99 was 16.04 ms
before and 15.94 ms after; these are not pooled percentiles. The average-FPS result
is promising for this stationary view, but does not establish that movement or
area-dependent stutter is fixed. Frame-time spikes still occurred.

A separate ThinLTO experiment measured only 78.42 FPS in the same hands-off view
and did not establish better frame pacing. It remains outside normal launchers;
the normal build retains its previous compiler settings. Earlier interactive
comparison runs had different live camera input and are not controlled FPS tests.

Evidence and source/executable hashes: build_native/general-performance-20260913/.
Final native input fingerprint:
b8673a22da8ef15f0b9292639f89afe5eb0719b0f09672ddda781cdbe792371b.
The original save files and graphics settings were verified unchanged.

## Busy-scene engine thread speedup (2026-11-09)

In the Chapter 2 village (OW1_Village), views with 10,000-12,000 world draws
ran at 36-42 FPS. The engine thread, which runs the game's own code and
captures each draw, was the limit; the GPU was mostly idle. Changes:

- Generator: `xenonrecomp-non-argument-locals.patch` and
  `xenonrecomp-stack-accesses.patch` (see `tools/patches/README.md`), enabled
  in `config/darkness.toml`.
- Guest loads and stores keep their null-page and MMIO checks, but the rare
  case now takes an out-of-line cold path (`PPCLoadSpecial`).
- The engine can publish up to `previewReadyPartDepth` (3) finished command
  parts ahead of the display thread instead of one, so neither thread waits on
  momentary differences in part cost. Backpressure is unchanged.
- The D3D11 world vertex shader uploads only the planned constant rows while
  its constant references are unchanged; any reference change, unproven palette
  or full plan still uploads every row. The texture loop and image cache avoid
  per-draw shared_ptr copies and repeated validity checks.
- `configureGameThread` no longer sets an ideal processor. In the same views an
  ideal-processor hint for the first two performance cores made engine frames
  1-5% slower, and restricting the engine to the first performance core about
  13% slower (its frame time also switched between two levels during a run).
- Profile-guided optimization (PGO) of the game executable: CMake option
  `DARK_PGO` (`generate` or `use`, default off) with `DARK_PGO_PROFILE` and
  `DARK_PGO_RUNTIME`. It applies to the recompiled game code, the native
  runtime and the app. `build_native` is configured with `DARK_PGO=use` and
  `build_native/pgo/darkrecomp.profdata`; `tools/build.ps1` keeps those cache
  values. A fresh build directory builds without PGO. To refresh the profile,
  configure a separate directory with `-DDARK_PGO=generate` and
  `-DDARK_PGO_RUNTIME=<VC\Tools\Llvm\x64\lib\clang\N\lib\windows\clang_rt.profile-x86_64.lib>`,
  build `DarkRecomp`, play the busy scenes with `LLVM_PROFILE_FILE` set (the
  counters are written when the game exits), merge the `.profraw` files with
  `llvm-profdata merge`, and reconfigure `build_native` with `-DDARK_PGO=use`.
  The helper scripts `build_native/claude-pgo-gen.cmd`, `claude-pgo-bench.cmd`
  and `claude-pgo-use.cmd` do this for the automated village route. Profile
  data never changes behavior; a stale profile only loses speed (its warnings
  are disabled).
- `--sample-engine` reports `[CpuSystemCaller]` lines: samples in system DLLs
  (for example SRW lock functions) with the executable source line that called
  them, and lists 40 engine functions instead of 12.

Same automated route in every run (isolated settings and saves,
`dev mission OW1_Village:ow1+layer1`, noclip to a fixed view with about 10,800
world draws, 55-75 seconds of frames; diagnostic sampling enabled):

| Build | Median frame | FPS (median) | p95 | p99 | Frames over 16.7 ms |
|---|---|---|---|---|---|
| Before | 23.02 ms | 43.4 | 25.77 ms | 27.32 ms | 100% |
| After, without PGO | 15.35 ms | 65.1 | 17.06 ms | 18.49 ms | 7.8% |
| After, with PGO (run 1) | 13.91 ms | 71.9 | 15.29 ms | 16.45 ms | 0.8% |
| After, with PGO (run 2) | 13.94 ms | 71.7 | 15.23 ms | 16.27 ms | 0.5% |

Display-thread render time per frame fell from 14.0 ms to 10.1-10.2 ms. Nearby
views with 10,300-11,250 draws took 21.9-23.9 ms of engine time per frame
before, 15.0-16.0 ms after without PGO and 13.3-14.5 ms with PGO; the
11,247-draw view took 16.02 ms without and 14.46 ms with PGO. The first
profile was recorded on this route only. The current profile merges it with a
second run that also covers the checkpoint tunnel on foot, firing, reloading,
crouching and manifesting and using Darkness powers, so code outside the
aerial route is not treated as cold. With it the same route measured 14.36 ms
(69.6 FPS, p99 16.89 ms, 1.3% over 16.7 ms) at 11,066 draws
(`build_native/run/claude-bench-3155-11485`), within about 2% of the first
profile. All 137 CTest tests pass with and without PGO.

Also measured and not adopted: moving world-draw queueing (texture resolution,
budgets, part streaming) to a helper thread behind a lock-free ring passed all
tests but gained only about 1% (13.74 ms at 10,818 draws), so it was reverted.
ThinLTO on top of PGO (CMake option `DARK_THINLTO`, off by default; it needs
lld-link and llvm-lib, for example Ninja with clang-cl, so the Visual Studio
generator used by `tools/build.ps1` does not use it) gained 2-3.5% in the same
views (13.77 ms at 11,123 draws); `build_native/claude-lto3.cmd` builds it in
`build_lto` as `build_native/Release/DarkRecomp-lto.exe`. Compiling the game code, runtime and app with the
integer instructions of Haswell/Zen-era CPUs (MOVBE for the guest's byte-swapped
loads and stores, LZCNT, POPCNT, BMI1/BMI2; no AVX, FMA or SSE4) passed all tests
but was not faster on an i9-14900K: 1.30-1.37 instead of 1.30-1.32 microseconds
per world draw on the same route with normal play options
(`build_native/run/claude-play-27905-10433` against
`build_native/run/claude-play-25224-583`), so it was reverted. These results
suggest the engine thread is now limited by moving each draw's data through
memory rather than by the instructions it runs.

Remaining limit: the engine thread still bounds frames at about 1.3-1.5
microseconds per world draw, and that cost depends on what is in view. The
user's own heaviest spot (36-37 FPS in the older build) was re-measured with
`LaunchFrameDrops.cmd` on the PGO build: 11,783 draws at 55.6 FPS, median
frame interval about 18 ms, engine frame 17.9-18.3 ms, display-thread render
12.1 ms (`build_native/run/frame-drops-20906-14478`). That capture still timed
every per-draw call (see "Frame-drop timeline"), about 1.53 microseconds per
draw; on the same route the capture cost was a factor of about 1.2, so that
view takes about 15 ms per engine frame (roughly 66 FPS) in normal play and in
the corrected capture. Its report called the slow frames "gpu-bound" because
the GPU span tracks the frame interval; the engine thread is the limit there.
With normal play options on the final build, the same village from the air
measured 10,400-11,190 draws at 13.6-14.7 ms per engine frame (68-73 FPS,
`build_native/run/claude-play-28142-20632`), and the 8,163-draw view that the
user's capture also shows took 7.65 ms instead of 8.39 ms. The user's own
`LaunchFrameDrops.cmd` capture on that build (`build_native/run/frame-drops-8429-1145`)
then held a heavier village view of 12,811-12,825 draws for about 30 seconds at
63.1 FPS: median frame 15.76 ms, p95 16.94 ms, p99 17.65 ms, 8.3% of frames over
16.7 ms, engine CPU 15.4 ms of a 15.7 ms engine frame. Evidence:
`build_native/run/claude-bench-11741-13008` (before),
`build_native/run/claude-bench-10814-7884` (without PGO),
`build_native/run/claude-bench-19311-24134` and
`build_native/run/claude-bench-21211-21018` (with PGO).

### Level of detail in these views (checked 2026-11-09)

Model and world level of detail behave as the original game data defines, so
they do not explain the draw counts. Checked on the automated village route
with a temporary opt-in probe (not kept in the source tree; a copy is in
`build_native/claude-stage/lod-probe`):

- Model LOD: `xr_lodoffset` is 0 and `xr_lodscale` 1 (the menu's "Model
  quality: dynamic"; "high" would set the offset to -1000000). The LOD
  distance is (camera distance + offset) * FOV / 90 * scale. Models with LOD
  meshes (thresholds 150/300/600) at about 1,300 units all used their lowest
  LOD. Forcing full detail (offset -1000000) changed world draws by under 1%
  (9,810 to 9,882), and a 16x scale by none: LOD meshes have fewer triangles,
  not fewer draws.
- World LOD (`CWObject_WorldLOD`, `sv_wlod_*`): OW1_Village's ENGINESETTINGS
  only sets color correction, so every range is 0, which means unlimited
  (setting 2000, 4000 or 100000 gives the same draws as 0). A shorter range is
  a real lever but changes the original look: `sv_wlod_range_onclientrender`
  of 1000 cut that view from 9,681 to 6,873 draws, 300 to 6,861, and adding
  `sv_wlod_range_onclientrender_object(300)` to 6,259.
- Draw distance (`[Display] DrawDistance`, `--draw-distance N`, settings launcher
  row; 0 = Original; default 1500 = Near when nothing is saved): `runtime/native/draw_distance.cpp`
  wraps 82498C48, which copies the view's WorldLOD ranges into the client render
  context (enable bits at +80: 0x10 generic, 0x08 characters, 0x04 engine paths,
  0x02 objects, 0x01 pickups, 0x20 any; squared ranges at +88..+104). It shortens
  only the generic and object ranges in that per-frame copy (a level's shorter
  range stays), so no game or save state changes and characters, pickups and
  engine paths are unaffected. Through the console, a generic range of 1000 units
  cut the aerial village view from 9,681 to 6,873 draws, and generic plus object
  ranges of 300 units to 6,259.
  With the option at 1000 on the built game (`build_native/run/claude-play-11489-12626`),
  the automated route's heaviest aerial view drew 5,938 draws at 7.8 ms per engine
  frame (127 FPS) instead of about 10,990 at 13.8-14.8 ms; the level geometry stays.
  At the Near default (1500 units, `build_native/run/claude-play-21041-27203`) the
  same route's view drew 10,130-10,360 at 12.3-12.9 ms (78-81 FPS): about 6-8% fewer
  draws there, since most of that view is within 1500 units of the camera.
- FieldOfView=95 (guest FOV 78.6 instead of 70) adds about 6% of draws and 7%
  of engine time in that view (9,683 draws and 12.85 ms versus 9,128 and
  12.00 ms with the original FOV).

## Redundant geometry passes (2026-11-10)

Frame time in busy scenes follows the number of world draws the game emits: the
engine thread spends about 1.3 microseconds on each. Two of the game's passes
redraw geometry for a result that is not used, or that one pass over the screen
can produce.

### Velocity pass with motion blur off

The "MotionMap" pass redraws moving and static geometry into a velocity target
that only the motion-blur input of the final composite reads. With motion blur
off, `renderer/engine/render_trace.cpp` returns from its emitter (82276148)
without drawing. `DARK_VELOCITY_PASS=1` keeps the original call; motion blur on
is unchanged. On the automated village route this removed 17-23% of the draws
and gave 8-25% more frames per second in its five views (for example 9,177
draws at 88.0 FPS to 7,575 at 96.6).

### Screen-space depth fog

The Otherworld levels (`OW1_*`, `OW2_*`) fog the scene by drawing every
depth-prepass object again with `XREngine_DepthFog`: depth EQUAL, stencil EQUAL
then increment, blended. Both of its interpolants are linear texture-generation
stages of the clip position and are the same function for every object of a
view, so one draw's constants give the fog inputs of any pixel from its
position and stored depth (`renderer/engine/screen_fog.h`).

- Engine thread (`simple_mesh.cpp`, `world_mesh.cpp`, hook 82868FE8): the first
  fog draw of a run is captured and held. Later fog draws whose device state
  bytes and vertex constants match it are absorbed before capture; the original
  indexed draw is not called for them either. The held draw is queued when the
  run ends.
- Display thread (`world_renderer.cpp`, `drawScreenFog`): the held draw becomes
  one pass over its viewport that reads stored depth, rebuilds the two
  interpolants and runs the original fragment program. The original stencil
  test and increment stay in the output merger.
- Which pixels: the pass covers pixels that still hold their prepass depth. The
  depth target is copied before the first depth write outside the prepass, and
  the pass requires the stored depth to equal that copy and to differ from the
  cleared value. Geometry that wrote depth after the prepass keeps its own fog
  draw (matched by resource ids and index range), drawn before the pass, so the
  stencil increment keeps the pass off its pixels. A fog draw with different
  state, an inconsistent mapping, or prepass clip planes that differ from its
  own also stays a per-object draw.
- Multisampling: the pass runs per sample, at the sample's own position and
  depth. The per-object draws shade a triangle once per pixel, so silhouette
  pixels can differ slightly.
- `DARK_SCREEN_FOG=0` restores the original per-object pass.
  `DARK_SCREEN_FOG_DEBUG=1` paints the pass's decisions instead (red cleared
  depth, green depth changed since the prepass, blue fog amount).

`DARK_SCREEN_FOG_VERIFY=1` checks the equivalence in running frames. Every fog
draw stays per object; for sampled runs the renderer keeps the color and
depth-stencil targets from before the first fog draw, then restores them and
draws the run again as the screen-space mode queues it, and logs the difference
between the two color results in the target's own half-float values
(`[ScreenFogVerify]`). On the village recording with the final build (1280x720
guest viewport, 2560x1440 render):

| Antialiasing | Sampled runs | Fogged pixels | Over 1/255 | Largest difference |
|---|---|---|---|---|
| Off | 183 | 537 million | 1 | 0.0041 |
| 4x MSAA | 173 | 547 million | 5 | 0.0048 |
| 8x MSAA | 174 | 514 million | 2 | 0.0079 |

An earlier 4x run at 3440x1440 with a longer view to the horizon found about
12,000 silhouette pixels over 1/255 in 954 million (largest 0.053). The
Chapter 2 hills at 2x MSAA gave 26 in 488 million. New York levels have no such
pass (checked in Chinatown) and are unaffected.

Frame times on the same recording, original pass (`DARK_SCREEN_FOG=0`) against
the screen-space pass, profile-guided build:

| View | Draws | Frame ms | FPS |
|---|---|---|---|
| Spawn | 6,296 to 4,526 | 5.88 to 4.90 | 170 to 204 |
| Aerial 1 | 7,472 to 5,917 | 9.66 to 9.08 | 104 to 110 |
| Aerial 2 | 7,273 to 5,755 | 9.35 to 8.75 | 107 to 114 |
| Aerial 3 | 6,599 to 5,347 | 8.75 to 8.20 | 114 to 122 |
| Aerial 4 | 5,253 to 4,181 | 6.82 to 6.45 | 147 to 155 |

An absorbed fog draw is not free: the game still prepares its state, and the
match against the held draw costs time, so a removed draw saves about 0.4 of
the 1.3 microseconds. Evidence: `build_native/run/claude-play-19456-17682`
(original), `claude-play-19887-27448` (screen-space), `claude-play-19011-30459`,
`claude-play-21085-7185` and `claude-play-20612-21530` (verification, off, 4x
and 8x).

### Recordings

A `--test-input` script can be written in advance and plays from launch to
exit without a driver. Besides the existing commands it accepts `wait <ms>`
(1-600000), `wait world` (the game renders its world), `wait menu` (a mission
can be loaded), `wait player` (a player is active; `menu` and `player` are
published only while the developer panel is open, and `dev mission` closes
it), `#` comment lines and `quit`. Waits ahead of the first command keep
`--test-skip-intros` active. `tools/recordings/village-route.txt` is the route
used above; `hills-tour.txt`, `trench-tour.txt` and `castle-tour.txt` look
around from a spawn point, and `level-changes.txt` loads three missions in one
session. `build_native/claude-play.cmd` plays the file named by
`BENCH_RECORDING`.

## Per-draw overheads (2026-11-10)

After the redundant passes above, both threads spend their time on work that
is repeated for every draw. Samples of the engine thread in Chinatown put 60%
of it in the native capture (`renderer/engine`), 26% in the game's own code and
9% in locks. The display thread spent about 40% of its rendering time in
`WorldRendererD3D11::draw` itself rather than in Direct3D. The changes below
remove work whose result is not used or is already known; none changes what is
drawn.

### The original indexed draw

In preview play the hook on 82868FE8 returns after the native observation
instead of running the original draw (`DARK_SKIP_XBOX_DRAW=0` runs it again;
renderer tracing and non-preview runs always do). That call flushes the
device's dirty state into the Xbox command stream and appends the draw packet;
nothing in this port reads that stream, and its five callers ignore the result.

What else it changes was measured rather than assumed: a temporary audit
copied the device object (16 KiB) and the render context around 6.8 million
original draws on the village route and recorded every word that differed,
with a control window (the same copies, no call) to tell the call's effects
from other threads' writes. The call changes only

- the dirty masks (device+0..39), which then stay set until the next original
  flush (clear, resolve, non-indexed draw),
- command-buffer bookkeeping (+48..56, +10908, +13224..13360, +14892..14908),
- values the flush derives from the bound programs and depth state and that
  only the flush paths consume (+10528..10536, the low bits of +10560, +10580,
  flag bytes +10941/10942, +11824).

None of it is state the native capture reads (samplers +1152, constants +1920
and +6016, clip planes +10272, surface words +10368..10391, clip control
+10564, targets +12432, bound textures +12536, the program binding +12688).
The one draw-time write into that range found by reading the code (82872238,
color-info format at +10372 under device flag +10943 bit 2) never happened:
the flag is clear in this title. Words of the render context that changed in
the audit changed as often in the control windows.

Effect on the village route, same build: 1.50 to 1.34-1.37 microseconds of
engine CPU per draw, 110 to 120 FPS in the first aerial view.

### Queued prompt and subtitle contexts

The game queues every draw in its vertex-buffer manager and executes the queue
later. The port records, per queued draw, whether it belongs to a gameplay
prompt or a subtitle (`simple_mesh.cpp`, hooks 8259D098, 8259CA10, 82299EA0),
which took the same mutex five times per draw. The sets are empty unless a
prompt or subtitle is on screen, so the functions now return before the lock
while an atomic count of tracked entries is zero (an erase or a lookup cannot
find anything then), and a thread reuses the generation it last read for a
manager until an epoch counter says a frame's identity changed. The unit tests
of these functions are unchanged.

### Display thread: lookups repeated for every draw

`renderer/d3d11/world_renderer.cpp`:

- Views and samplers chosen for a draw were kept alive with an AddRef/Release
  pair per slot until the context held them, in case a later slot's lookup
  evicted them. The caches now park what they give up while a draw is in
  progress (`retiredViews_`, `retiredSamplers_`) and release it when the
  outermost `draw()` returns; the selection keeps plain pointers.
- The last sampler per slot, the depth-stencil, blend and rasterizer state of
  the last draw (keyed by attribute bytes 92..147, render scale and sample
  count) and the two target surfaces are remembered, with the invalidation
  noted beside each member in `world_renderer.h`.
- A 64-bit mask of hashed keys skips the lookup in the resolve-destination map
  for ordinary textures; the fetch exponent scale comes from a table of the
  exact `std::exp2` values.
- `beginFrame()` walked all cached images, vertex buffers and index buffers
  every frame to find orphans, reading one shared reference count each. The
  complete walk now runs every 16th frame (and at once while the image cache
  is nearly full); immediate geometry, which is owned by a single frame, is
  still checked every frame from the list of what `draw()` uploaded.
- `engine_preview.cpp` asks the processor for the next captured draw's lines
  while the current one is submitted, and `ColorImage` is cache-line aligned
  so its reference counts do not share a line with the fields the display
  thread reads.

`DARK_DISPLAY_MEMO_CHECK=1` runs every replaced lookup as well and compares:
534 million comparisons on the village recording and 89 million on the
Chinatown walk, no mismatch. The screen-space fog verification
(`DARK_SCREEN_FOG_VERIFY=1`) on the same build: 237 runs, 528 million fogged
pixels, none over 1/255 (largest 0.0036); on the regenerated profile-guided
build 233 runs, 519 million pixels, 4 over 1/255 (largest 0.0063).

### Fog pass tracking

`screenFogObserveDraw`/`screenFogObserveClear` now run only while fog draws
keep arriving: the first fog draw starts the watch, 8,192 depth clears without
one end it. Levels without the pass skip the bookkeeping; a pass is tracked
from its first observed clear after the watch starts, and until then its fog
draws are per object as before.

### Result

Frame times of steady views, 1280x720 guest, 2560x1440 render, antialiasing
off. Chinatown is a noclip view over the streets from the first mission's
spawn (`tools/recordings/chinatown-under.txt`); "before" there already has the
original draw skipped, so the row shows the context and display-thread changes
alone. The village rows compare with the state at the end of the previous
section.

| View | Draws | Before | After |
|---|---|---|---|
| Chinatown, noclip | 8,267 | 8.45 ms, 118 FPS (engine 7.44, display 8.36) | 6.91 ms, 145 FPS (engine 6.79, display 6.06) |
| Village, aerial 2 | 5,755 | 8.75 ms, 114 FPS (engine 8.54, display 6.9) | 7.7 ms, 129 FPS (engine 7.6, display 5.6) |
| Village, spawn | 4,526 | 4.90 ms, 204 FPS | 4.50 ms, 222 FPS |

Engine and display are the two threads' CPU milliseconds per frame. In
Chinatown the display thread was the limit before (its CPU time equalled the
frame time); the engine thread is the limit in every view now, at about 0.82
microseconds per draw in Chinatown and 1.32 in the village.

The profile for the profile-guided build was regenerated afterwards from three
recordings (village route, Chinatown noclip, Fulton Street). By then the test
machine itself had changed state: every configuration, including code that had
not changed, ran about 10% slower than an hour earlier, the window message
pump and `Present` took longer in the game's own frame log, and the processor
showed 25-35% load between runs. Compared with the previous build in that
same state, the regenerated build needed the same engine time and about 5%
less display rendering time (`claude-play-14295-19046` against
`claude-play-17364-30004` and `claude-play-20996-21912`).

Timed again that afternoon, the regenerated build ran Chinatown in 7.16 ms
(140 FPS; engine 7.01, display 6.07), village aerial 2 in 7.78 ms (128 FPS)
and the village spawn in 4.46 ms (224 FPS) (`claude-play-2391-8886`,
`claude-play-1473-3240`). The slow state had passed by itself. What ran
beside the game then used about 10% of the processor: two renderer processes
of the Oculus desktop client at 45% of a core each (running for three days),
the desktop compositor, the Claude desktop app and vendor monitoring services.
Nothing was closed.

Evidence under `build_native/run`: `claude-play-1040-5472` and
`claude-play-619-27952` (original draw run and skipped, same build),
`claude-play-3953-24795` and `claude-play-10595-30797` (Chinatown before and
after), `claude-play-7421-3570` (village after), `claude-play-11450-28832` and
`claude-play-11862-6875` (lookup comparison), `claude-play-13162-24901` and
`claude-play-20561-1398` (fog verification), `claude-play-31620-13000` and `claude-play-32391-26488` (draw
audit).

### Tried without gain

- PREFETCHW of the pooled draw the next capture fills (engine thread).
- Following the manager's record list to prefetch the next records' stored
  geometry, guest resource and attribute block. For reference, the list as
  825A24D0 walks it: a record's node at +20 points to the next record's node,
  a set low bit in the node's second word ends the list; record +4 is the
  geometry chain (with record flag 8: vertex id +8, kind +12, index id +16),
  +12 the attribute block. The requests were issued for 97% of the records and
  changed nothing measurable, so the engine thread's time is instructions, not
  cache misses on these structures.
- Draw references that own nothing were first tried in an unsafe form
  (about 4.5% on each thread). The safe version is the next section.

## Draw references and queued fog records (2026-11-10)

Two more costs that the hot threads paid for every draw. Neither changes what
is drawn.

### Draws refer to their resources without counting

A queued world draw held a counted reference to its vertices, its indices and
every image it samples: 4.9 per draw on the village route. The engine thread
took them at capture, the display thread released them when it retired the
part, and each count is a locked write, usually to a cache line the other
thread wrote last.

A draw now holds references that own nothing (`unowned_reference.h`). The
owners are kept once per pin epoch of eight frames (`previewPinEpochFrames`):

- `StoredGeometryCache::drawFast` keeps one owner per cache entry and epoch,
  under the cache lock, before the reference leaves it.
- `queueCapturedWorldLocked` adds an image's owner the first time a draw that
  samples it is queued in the epoch, under the queue lock and before anything
  later in the same call can replace or evict the cache entry.
- Every part published in the epoch shares the epoch's set through its first
  command (`SimpleMesh::retained`), so a part's draws are valid for as long as
  the part exists, wherever it is. The engine holds the current set and the
  one before; at an epoch boundary it releases the set before that, outside
  the queue lock. Its parts are long retired by then, so in steady state only
  the engine thread ever writes these counts.
- A draw captured before an epoch boundary and queued after it (another thread
  ending the frame while it waited, or a held fog draw) takes its owners from
  the previous set into the current one. A draw older than that cannot be
  shown to be alive and is not drawn (`[EngineFramePins] staleDrops`). Neither
  case occurred in any run.

Immediate geometry, images decoded while a draw is queued, alpha images, the
slow capture path and hand-built draws keep owning references; the two kinds
mix freely. Whatever outlives a part takes the owner with `ownerOf()`: the
renderer's image, vertex and index caches on a miss (their orphan sweeps still
see a count of one when only the cache is left), the texture-mod worker and
the subtitle font registry. `keepWorldDraw()` keeps a single draw together
with its part's owners.

One behaviour changed: a resource that is replaced or evicted while in use is
released once two further epochs have closed (at most sixteen frames later),
no longer when the part that drew it retires. The unit tests that kept a
draw's image, or a single draw, beyond its frame were adapted to that.

`DARK_OWNED_DRAW_REFERENCES=1` restores counted references.
`DARK_DRAW_PIN_CHECK=1` confirms, before each part is published, that
everything its draws refer to without owning is pinned. On the final build:
three level changes in one session (`tools/recordings/level-changes.txt`), the
hills, the trench, the castle and a Chinatown walk, together 247,000 parts, 66
million draws and 308 million references (4.6% of them owning), none unpinned.

Same executable, Chinatown noclip view (8,267 draws; 5,102 geometry objects
and 995 images pinned per epoch): 7.55 ms with counted references, 7.10 ms
without (132 to 141 FPS); engine thread 7.42 to 6.97 ms, display thread 6.60
to 6.09 ms (`claude-play-9287-1296`, `claude-play-8925-20639`; one epoch was
one frame in that build).

### Queued fog records

The screen-space fog pass above decides at the indexed draw whether a fog
draw belongs to the held run, after the game has bound the object's geometry
and prepared its transform, vertex program and constants. The game queues
these draws as records of many elements that share one set of attributes and
one transform: 82299EA0 (called from 825A2E18) walks the record's element
chain and calls the indexed subset draw 8225E2F0 once per element. On the
village route a fog record has 236 elements on average
(`claude-play-11724-24216`).

Once one element's draw has been absorbed, the state that decision compared
is the state of every later element of the record. Preparing another element
binds its vertex and index buffers and, when the vertex format differs,
selects the vertex program; it applies no attributes, no transform and no
view, and writes none of the device words `screenFogRawMatches` reads. The
hook on 82299EA0 therefore executes such a record itself while fog draws keep
arriving (`ScreenFogState::watching`): it reads the chain once
(`screenFogQueuedElements`: flag 8, every element of kind -4 or without a
vertex resource; any other record runs the original), calls 8225E2F0 for each
element as the original does, and leaves out the elements the run is known to
cover. An element whose geometry wrote depth after the prepass (a late twin)
is always executed, and so are the first element of a record and every
element after one that was not absorbed. The manager only adds the returned
element count to a statistic. What a skipped element's preparation would have
bound is bound again by the next element that is executed, as after any other
draw.

`DARK_FOG_EMITTER=0` executes every element as before. `DARK_FOG_EMITTER=2`
executes them all and compares each prediction with the decision at the draw:
21.0 million predictions on the village route, the hills, the trench and the
castle, every one confirmed. In normal play one predicted element in 64 is
executed and compared in the same way, and a single disagreement ends the
skipping for the session (`[ScreenFogEmitter]` reports the counts).

The prediction covers 95% of the absorbed fog draws on the village route, 98%
in the castle, 87% in the hills and 84% in the trench; the rest are
single-element records and the elements at the start of a run. Same
executable, village aerial view 2: 7.74 ms with every element executed, 7.40
ms without (129 to 135 FPS); engine time per captured draw 1.33 to 1.26
microseconds (`claude-play-13471-7445`, `claude-play-13040-30447`).

### Result

Profile-guided build regenerated from the same three recordings, 1280x720
guest, 2560x1440 render, antialiasing off. "Before" is the build at the end of
the previous section as timed that afternoon.

| View | Draws | Before | After |
|---|---|---|---|
| Chinatown, noclip | 8,267 | 7.16 ms, 140 FPS (engine 7.01, display 6.07) | 6.86 ms, 146 FPS (engine 6.75, display 5.63) |
| Village, aerial 1 | 5,918 | 8.04 ms, 124 FPS (engine 7.91, display 5.51) | 7.61 ms, 131 FPS (engine 7.49, display 5.31) |
| Village, aerial 2 | 5,763 | 7.78 ms, 128 FPS (engine 7.64, display 5.43) | 7.31 ms, 137 FPS (engine 7.20, display 5.16) |
| Village, spawn | 4,526 | 4.46 ms, 224 FPS | 4.00 ms, 250 FPS |

The engine thread remains the limit in every view, at 0.82 microseconds per
captured draw in Chinatown and 1.25 in the village. Samples of it on the
village route (`claude-play-23294-29403`) put 10% in the fast snapshot of a
draw's state, 8% in queueing the draw and close to 4% in taking and releasing
the two locks every draw still passes (the queue and the geometry cache); the
rest is spread over the game's own draw preparation and the capture.

On this build: CTest 137 of 137, including new cases for the cache's pins
(`pinContracts`) and the element chain (`queuedElementContracts`) in
`tests/stored_geometry_tests.cpp`; `DARK_SCREEN_FOG_VERIFY=1` on the village
route, 258 runs and 503 million fogged pixels, one over 1/255 (largest
difference 0.0060); `DARK_DISPLAY_MEMO_CHECK=1` on the Chinatown walk, 80.7
million comparisons without a mismatch; recordings of every level above plus
Pier 19 and Fulton Street played to their end.

Evidence under `build_native/run`: `claude-play-19803-8157` and
`claude-play-20129-1654` (result), `claude-play-20550-11942`,
`claude-play-21001-20662`, `claude-play-21331-24907`, `claude-play-21615-9747`
and `claude-play-22330-4352` (pin check; fog predictions compared in the
second to fourth; lookup comparison in the last), `claude-play-12573-753`
(fog predictions compared, village), `claude-play-21902-5334` (fog
verification).

## Repeated elements of a queued record (2026-11-10)

The game queues most of its draws as records of elements (82299EA0, see
"Queued fog records" above). The elements of a record share the record's
attributes, transform, view, textures and constants and differ in geometry.
Each element was still captured in full: the engine thread read the device
state again, filled a pooled `WorldDraw`, resolved its images and queued it,
and the display thread translated the same state once more.

### How often an element repeats the one before

A probe (`DARK_RECORD_REPEAT=2`) runs the
record loop for every record and compares each captured element with the
previous capture of the same record in everything but geometry and image
references (`sameCapturedWorldDraw(..., stateOnly)`). It also tests a
prediction made before the element is prepared: nothing is waiting to be
applied (context+340 has no bits beyond 0x1800, context+8228 and
context+8344 are zero: `fogEmitter.elementClean`, see 82249488 under
"Functions" in the handoff) and the game ran no vertex program preparation
(82248C80, counted by `engineVertexProgramPrepares()`) since that capture.

| Recording | Captured draws | Identical to the previous element | Predicted | Predicted wrongly |
|---|---|---|---|---|
| `chinatown-under` | 30.87 M | 24.73 M | 24.71 M | 0 |
| `village-route` | 31.53 M | 21.45 M | 19.82 M | 0 |
| `trench-tour` | 12.49 M | 7.22 M | 6.58 M | 0 |

(`claude-play-4744-2493`, `claude-play-5364-13074`, `claude-play-5074-6738`.)
The differences that remain are vertex constants and vertex options after
the game re-prepared the element; the prediction excludes them.

### Repeats

- The hook on 82299EA0 now executes every record of the element shape it
  handles (flag 8, elements of kind -4 or without a vertex resource), not
  only while fog draws arrive (`screenFogEmitterBegin`). Records of any
  other shape, single-element records and records begun inside another
  one run the original, as before.
- A draw captured in full inside the loop and queued becomes the record's
  anchor (`repeatAnchor`: the part being built, its index in `pending`, the
  pin epoch, the record's serial number and the preparation count).
- At the indexed draw of a later element of the same record, if the element
  was clean and nothing was prepared since the anchor, the geometry is
  resolved as before and queued as a command that shares the anchor's
  `WorldDraw` and carries its own geometry (`SimpleMesh::repeat`,
  `queueRepeatedWorld`). The device is not read, no `WorldDraw` is filled,
  no image is looked up.
- The full capture is used instead, and becomes the next anchor, when the
  anchor's part has been published, a command or geometry budget of the part
  is reached, or the fog pass needs this draw's own flags (it would be the
  first depth write of a tracked pass after the prepass). A fog draw
  (`XREngine_DepthFog`) or a draw that carries fog requests is never an
  anchor: every fog draw is still offered to the fog pass.
- The fog pass's bookkeeping for a repeat (`screenFogObserveRepeat`) is the
  bookkeeping `screenFogObserveDraw` does for a full draw, with the repeat's
  geometry key.
- `WorldRendererD3D11::draw(draw, repeat)` takes the geometry from the
  repeat everywhere it reads geometry (vertex and index buffers, the flat
  quad used for blur footprints, rejection reports). Inspection, capture
  and fog paths are given an equivalent complete draw.
- One predicted repeat in 64 is captured in full and compared with its
  anchor; a single difference ends repeats for the session
  (`[RecordRepeat] MISMATCH`). `[RecordRepeat]` reports the counts every
  1000 frames.
- `checkDrawPins` (`DARK_DRAW_PIN_CHECK=1`) checks the repeats' geometry too.

`DARK_RECORD_REPEAT=0` captures every element in full. With it,
`DARK_FOG_EMITTER=0` again leaves every record to the original 82299EA0.

### Checks

- Repeat audits: 0 mismatches in every run (Chinatown 775,625 compared,
  village 342,709, trench 104,452, level changes 149,409).
- `DARK_DRAW_PIN_CHECK=1`: nothing unpinned (Chinatown 28.6 M draws,
  village 36.7 M, trench 14.7 M, level changes 18.5 M).
- `DARK_DISPLAY_MEMO_CHECK=1`: 0 mismatches in 488 M (village), 181 M
  (trench) and 223 M (level changes) comparisons.
- Fog predictions (change of the previous section): all confirmed.
- Captures of the same views with and without repeats differ only as much as
  two runs without them do (camera timing).
- `DARK_SCREEN_FOG_VERIFY=1` on the village route, twice with repeats: the
  1280x720 passes agree as before (176 and 185 runs, none over 1/255). In
  the first run a 512x512 view seen in no other run had 1 to 4 pixels per
  verified pass (11 of 60 passes) fogged by the per-object draws and not by
  the pass (largest difference 0.22); the second run (78 passes at 512x512)
  and the runs with repeats off or with the loop alone had none. Earlier
  verification runs show only differences below 0.016 there. Not resolved;
  see the handoff.

### Result

Same executable (not profile-guided for the edited functions), Chinatown
noclip view (8,267 draws): 7.22 ms with `DARK_RECORD_REPEAT=0`, 4.88 ms with
repeats (139 to 205 FPS); engine thread CPU 7.06 to 4.61 ms, display thread
CPU 6.12 to 4.56 ms (`claude-play-11928-23259`, `claude-play-11592-30285`).
Village (`claude-play-17868-12114`, `claude-play-17450-12574`): engine
microseconds per draw 1.29 to 1.06; the spawn view 4.18 to 2.80 ms. (The
aerial views of that pair ended at slightly different camera positions,
5,917 and 6,009 draws; the route reaches either one in runs with and
without repeats.)
The display thread gains as well because consecutive draws now share one
`WorldDraw`, so its memos (previous section) hit more often.

Profile-guided build regenerated from the same three recordings with
repeats on (17:45), against the profile-guided build at the end of the
previous section (changes 1-8). Same settings as there.

| View | Draws | Before | After |
|---|---|---|---|
| Chinatown, noclip | 8,267 | 6.86 ms, 146 FPS (engine 6.75, display 5.63) | 4.52 ms, 221 FPS (engine 4.37, display 4.13) |
| Village, aerial 1 | 5,917 | 7.61 ms, 131 FPS (engine 7.49, display 5.31) | 6.35 ms, 157 FPS (engine 6.25, display 4.62) |
| Village, aerial 2 | 5,762 | 7.31 ms, 137 FPS (engine 7.20, display 5.16) | 6.15 ms, 163 FPS (engine 6.03, display 4.52) |
| Village, spawn | 4,526 | 4.00 ms, 250 FPS | 2.77 ms, 361 FPS (the 360 FPS limit of the settings) |

(`claude-play-20788-20165`, `claude-play-21121-2391`.) In those runs 87% of
the draws in Chinatown were repeats (54.7 M repeats, 8.3 M full captures)
and 77% in the village (30.5 M, 9.1 M); 869,705 and 485,138 audits agreed.

The engine thread is still the limit. Samples of it in the Chinatown view
(`claude-play-21549-1408`): geometry verification (`fastVerifyDrawImpl`)
9.7%, lock release and acquire 9%, queueing full captures 6%, the game's
resource lookup 822527F0 5.5%, the native observation 5.3%, the geometry
resolve 4.3%, the fast snapshot 4.2%, appending the command 2.9%, the rest
the game's own per-element work. Display thread by phase: states 44%,
textures 26%, geometry 11%, constants 9%.

On this build: CTest 137 of 137, including `testRepeatComparison`.

Evidence under `build_native/run`: `claude-play-4744-2493`,
`claude-play-5364-13074`, `claude-play-5074-6738` (probe),
`claude-play-11229-16860`, `claude-play-12291-3916`,
`claude-play-15367-3603`, `claude-play-15670-20165`,
`claude-play-17163-16986` (checks), `claude-play-11592-30285`,
`claude-play-11928-23259`, `claude-play-17450-12574`,
`claude-play-17868-12114` (same executable, on and off),
`claude-play-12797-31519`, `claude-play-13296-4857`,
`claude-play-13741-24848`, `claude-play-14890-7199` (fog verification),
`claude-play-20788-20165`, `claude-play-21121-2391` (result),
`claude-play-21549-1408` (samples).

### Display thread: repeats drawn with their geometry only

A repeat usually follows its anchor directly in the command list. Its
`WorldDraw` is the anchor's, so everything `WorldRendererD3D11::draw` derives
from the state (targets, the three state objects, viewport, scissor,
shaders, textures and samplers, all constants) is what the previous call
just bound. `engine_preview.cpp` now tells `draw()` when the command before
was a world draw of the same `WorldDraw` that was drawn (`continues`), and a
full `draw()` notes (`shortPathDraw_`) when it ended with every binding of
its `WorldDraw` in place and nothing depending on its geometry beyond the
vertex and index buffers. That excludes a fog pass or fog verification
(bindings stale), the prompt quad transform of flat UI rectangles (computed
from the vertices), color targets with a tracked blur footprint (computed
from the geometry's flat quad), inspection, capture and diagnostics.

`drawRepeat()` then does only the geometry's part: same vertex formats as
the previous draw, index range, vertex and index buffers already uploaded
(a miss takes the full path, which owns uploads and their budget), the
skinning index-bounds check `bind()` makes
(`WorldVertexShaderD3D11::indexBoundsValid`), the index check against the
vertex count, `IASetVertexBuffers` / `IASetIndexBuffer` when they change,
`DrawIndexed`, and the counters.

`DARK_REPEAT_SHORT_PATH=0` draws repeats with the full path.
`DARK_REPEAT_SHORT_PATH=2` draws them with the full path and requires that
it bound nothing but the vertex and index buffers and uploaded no constants
(`[RepeatShortPath] checks=.. mismatches=.. rejected=..`; `rejected` counts
repeats the full path refused because their geometry upload exceeded the
frame's budget while a level loads, where `drawRepeat` falls back to it as
well): Chinatown 13.6 M checked repeats, village 30.4 M, trench over 5 M,
no mismatch (`claude-play-23207-22111`, `claude-play-24390-13642`,
`claude-play-24817-12659`).

Same executable, Chinatown noclip view: 4.53 ms with
`DARK_REPEAT_SHORT_PATH=0`, 4.27 ms with the short path (221 to 234 FPS);
display thread CPU 4.32 to 2.71 ms, render 3.87 to 2.16 ms; engine thread
CPU 4.33 to 4.18 ms (`claude-play-23910-6490`, `claude-play-23576-24264`).
The engine thread is now the only limit.

Profile-guided build regenerated with the short path (18:28), against the
17:45 build of the previous result:

| View | Draws | 17:45 build | 18:28 build |
|---|---|---|---|
| Chinatown, noclip | 8,267 | 4.52 ms, 221 FPS (engine 4.37, display 4.13) | 4.23 ms, 236 FPS (engine 4.15, display 2.61) |
| Village, aerial 1 | 5,917-5,920 | 6.35 ms, 157 FPS (display 4.62) | 6.31 ms, 158 FPS (display 3.72) |
| Village, spawn | 4,526 | 2.77 ms | 2.73 ms (the 360 FPS limit) |

(`claude-play-29239-17009`, `claude-play-29569-21254`.) The village views are
limited by the engine thread alone; there the change shows as display
headroom. CTest 137 of 137, including a repeat drawn with the short path
in `deadNormalContract` (`tests/world_renderer_tests.cpp`) that must match
the pixels of the equivalent complete draws.
