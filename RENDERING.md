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
limit is reached and rounding to even guest dimensions. Internal resolution
changes apply on restart.

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

The original selected display mode retains its logical resolution identifiers
while its allocation extents change before buffer creation. The engine then
uses those dimensions for both the perspective projection and CPU visibility
checks. The vertical field of view stays constant as horizontal coverage grows.
Full-screen videos retain their source aspect, and the front-end menu retains
its 16:9 content width. The journal and gameplay HUD keep their original
height-based layout.

Alt+Enter toggles borderless fullscreen. Window resizing recreates only the
swapchain views, depth buffer and output readback resource, retaining the device,
world textures and internal scene buffers. Presentation fits the existing scene
without cropping or stretching. Restart to change the internal aspect ratio;
`--width W --height H` selects an explicit aspect and window size. See
[CONTROLS.md](CONTROLS.md) for launcher and input options.

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

Changes automatically save brightness, gamma, FOV, bloom, motion blur, antialiasing, VSync, frame cap, render height and display
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

Gamma follows Brightness, with 0.50..1.50 in 0.05 steps and
1.00 as the neutral default. Lower values darken the image. The portable INI
stores `GammaPercent=50..150`; absent or invalid values recover to 100.
The presentation shader applies `pow(rgb, 100 / GammaPercent)` after optional
FXAA, preserving alpha, black borders and the owned scene. Neutral brightness
and gamma retain the exact previous output and direct-copy path. Changes apply
immediately; repeated presents never compound the correction. Ten compact rows and their
help text fit the original twenty-cell menu grid.

Darkness Vision uses the original `WClientMod_DV5_0` and `WClientMod_DV5_1`
programs, including their base, radial-add and radial-multiply variants. Missing
native translations previously dropped these final draws, exposing the scene
and radial-blur atlas as stacked quadrants. The translator selects the Xenon
floating-depth branch and preserves ARB `KIL` as a conditional pixel discard.
GPU contracts cover both stages, all variants, atlas coordinates and far-depth
discard at scales 1/2/3 on hardware and WARP.

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

Parked: skipping the tone-map copy and re-present of an unchanged image while
the engine produces nothing (plus a 2ms yield to loader threads) was
implemented and audited — arming only after a confirmed `S_OK` present, with
new queue parts, settings saves, resizes, fullscreen toggles and test captures
all resuming normal display — but reverted unvalidated when the session's
display went fully occluded (`presentOccluded` on every present), which also
disarms the skip by design. To retry: restore the `displayCurrent`/`idleSkip`
logic in `native_main.cpp`, run the visible 150s route, and expect fewer
stale presents during load walls with identical `worldFPS`.

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
- Follow the original resource's primary/alternate texture selection, including
  inline texture objects and readiness checks.
- Follow completed sampler filtering, addressing, LOD and anisotropy settings.
  Generate native mip chains from decoded base images to reduce texture aliasing.
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

`Launch-Render-Profile.cmd` selects this diagnostic and ordinary hitch tracing,
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
