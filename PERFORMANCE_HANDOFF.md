# DarkRecomp performance work: handoff (state on 2026-11-10, 18:40 local)

This file is for whoever continues the performance work on The Darkness Recomp
(`K:\DarkRecomp`), with or without an assistant. It covers what exists, how to
build and measure without help, what was learned about the game's renderer,
and what is left, most promising first.

Companion documents:

- `RENDERING.md` in the repository: the long-form write-up of every change,
  with evidence run names. Sections "Redundant geometry passes", "Per-draw
  overheads", "Draw references and queued fog records" and "Repeated
  elements of a queued record" are this work.
- Project doc `claude/performance-findings.md`: the short summary.

Contents

1. State at a glance
2. Working without an assistant (build, test, run, measure, profile build)
3. How a frame flows (what is hot and where it lives)
4. The ten changes: what, where, why it is safe, how to switch off, evidence
5. Change 7 in detail: draw references and pin epochs
6. Change 8 in detail: queued fog records
6a. Change 9 in detail: repeated elements of a record
6b. Change 10 in detail: repeats drawn with their geometry only
7. What was learned about the game's renderer (addresses, layouts)
8. Measuring: method and traps
9. Remaining work, most promising first
10. Known issues and loose ends
11. Files touched, backups, helper scripts
12. Glossary

---------------------------------------------------------------------------

## 1. State at a glance

- Machine: i9-14900K, RTX 5080, Windows. Test settings: motion blur off,
  1280x720 guest viewport rendered at 2560x1440, antialiasing off.
- `build_native\Release\DarkRecomp.exe` is the profile-guided (PGO) build of
  2026-11-10 18:28, built from the current working tree with changes 1-10
  (repeats and their display short path on by default). Its profile was
  collected from that tree. No source file is newer than it.
- Nothing is committed. HEAD is `eda6047`; `git status` also shows unrelated
  work in progress in the same tree.
- CTest: 137 of 137 pass on this build.
- Frame times (median of steady views):

| View | Draws | 14:49 build (changes 1-8) | 17:45 build (1-9) | 18:28 build (1-10) | Engine / display CPU ms (18:28) |
|---|---|---|---|---|---|
| Chinatown, noclip (`chinatown-under`) | 8,267 | 6.86 ms, 146 FPS | 4.52 ms, 221 FPS | 4.23 ms, 236 FPS | 4.15 / 2.61 |
| Village, aerial 1 (`village-route`) | 5,917-5,920 | 7.61 ms, 131 FPS | 6.35 ms, 157 FPS | 6.31 ms, 158 FPS | 6.16 / 3.72 |
| Village, aerial 2 | 5,762 | 7.31 ms, 137 FPS | 6.15 ms, 163 FPS | (the route ended at the 5,802-draw view: 6.09-6.13 ms) | 5.95 / 3.65 |
| Village, spawn | 4,526 | 4.00 ms, 250 FPS | 2.77 ms, 361 FPS | 2.73 ms, 366 FPS (settings cap 360) | 2.63 / 1.63 |

  Runs: 18:28 `claude-play-29239-17009`, `claude-play-29569-21254`; 17:45
  `claude-play-20788-20165`, `claude-play-21121-2391`.
- For scale: the village aerial views ran at 88-98 FPS before any of this.
- The engine thread limits every view (its wall time equals the frame time):
  0.50 microseconds per draw in Chinatown, 1.03 in the village. The display
  thread has 35-50% headroom. Next: section 9.1b.

---------------------------------------------------------------------------

## 2. Working without an assistant

Everything an assistant did went through plain `.cmd` files in
`build_native`. They can be double-clicked or run from `cmd`. One at a time:
they share log files and the build tree.

### 2.1 Build and test

| Goal | Script | Log | Time |
|---|---|---|---|
| Rebuild `DarkRecomp.exe` only (regenerates guest code first) | `build_native\claude-native.cmd` | `build_native\claude-native.log` | 0.5-2 min |
| Rebuild everything and run all tests | `build_native\claude-queue\buildtest.cmd` | `build_native\claude-queue\buildtest.log` | about 5 min |
| Build one test and run it alone | see 2.6 | | |

The build tree is configured for PGO "use" with
`build_native\pgo\darkrecomp.profdata`. After editing a hot function the
compiler prints `function control flow change detected (hash mismatch) ...
count discarded` for it: that function is then compiled without profile data
until the profile is regenerated (2.4), and is slower than it will be. Do
not compare such a build with an older fully profiled one. Compare inside one
executable with the environment switches (section 4), then regenerate the
profile.

### 2.2 Play a recording (the benchmark)

Recordings are text scripts in `tools\recordings` that play from launch to
exit. `build_native\claude-queue\play.cmd` calls `env.cmd` next to it, then
`build_native\claude-play.cmd`.

1. Edit `build_native\claude-queue\env.cmd`. It holds one line:
   `set "BENCH_RECORDING=%~dp0..\..\tools\recordings\village-route.txt"`.
   Change the file name to the recording wanted.
2. For one-off switches, edit `build_native\claude-queue\experiment.cmd`
   (called by `claude-play.cmd` if present), for example
   `set "DARK_FOG_EMITTER=2"` or
   `set "BENCH_EXTRA_ARGS=--sample-engine --sample-renderer"`.
   Put it back to comments afterwards; it applies to every later run.
3. Run `build_native\claude-queue\play.cmd`. Do not touch the game window.
   It closes by itself (the script's `quit`), or after 10 minutes.
4. The run folder is `build_native\run\claude-play-<n>-<n>`; its name is
   also written to `build_native\claude-perf-last.txt`. It holds `game.log`
   (stdout and stderr), `frames.csv` (one row per frame), `input.txt` (the
   script used), `exe.txt` and the captures (`shot-*.bmp`).

Recordings:

| Name | What |
|---|---|
| `village-route` | Otherworld village, noclip route over four views. The main benchmark for fog levels. |
| `chinatown-under` | Chinatown, noclip below street level, 8,267 draws. The heaviest New York view found. |
| `chinatown-tour`, `fulton-tour`, `pier-tour` | New York, on foot. |
| `hills-tour`, `trench-tour`, `castle-tour` | Otherworld, noclip look-around. The trench has skinned characters. |
| `level-changes` | Village, Chinatown and hills in one session (level changes replace most resources). |

Script commands: a virtual-key code with optional hold time (`87 1500` holds
W for 1.5 s; 74 is J = turn), `mouse dx dy`, `capture`, `wait <ms>`,
`wait world|menu|player`, `dev open|close|mission <name>|noclip on|invincible
on`, `#` comments, `quit`. Mission names used: `OW1_Village:ow1+layer1`,
`OW1_Hills:ow1+layer1`, `OW1_Trench:ow1+layer1`, `OW2_Castle:ow2+layer1`,
`NY1_Chinatown:ny1+layer1`, `NY1_Pier19:ny1+layer1`, `SUB_Fulton:ny1+layer1`.

### 2.3 Read a run

Python 3 on Windows is enough:

```
python build_native\claude-queue\tools\views.py build_native\run\claude-play-XXXX-YYYY [minDraws]
```

It groups consecutive frames with the same draw count into "views" and prints
per view: draws, median frame ms, FPS, p95/p99, engine wall and CPU ms,
display render ms and CPU ms, GPU ms, and engine microseconds per draw
(`us/draw`). `minDraws` defaults to 4000; use 6000 for Chinatown.

```
python build_native\claude-queue\tools\verify.py build_native\run\claude-play-XXXX-YYYY
```

summarizes a `DARK_SCREEN_FOG_VERIFY=1` run (pixels compared, how many differ
by more than 1/255, 4/255, 16/255).

Lines worth grepping in `game.log`:

| Line | Meaning | Healthy |
|---|---|---|
| `=== exit 0` (last line) | Process exit code | 0 |
| `[EngineFramePins] owned=0 epoch=.. lastFrameGeometry=.. lastFrameImages=.. staleRepins=0 staleDrops=0` | Change 7, every 1000 frames | `staleDrops=0` |
| `[EngineFramePins] dropped a draw ...` | A draw older than the previous pin epoch was discarded | never seen |
| `[DrawPinCheck] parts=.. draws=.. references=.. owning=.. unpinned=0` | Only with `DARK_DRAW_PIN_CHECK=1` | `unpinned=0` |
| `[ScreenFogEmitter] mode=1 active=1 ... skipped=.. compared=.. agreed=.. mismatches=0` | Change 8 | `active=1`, `mismatches=0`, `compared == agreed + noDraw` |
| `[ScreenFog] passes=.. runs=.. absorbed=.. perObject=.. lateTwins=..` | Change 2 | `unsupported=0` |
| `[DisplayMemoCheck] checks=.. mismatches=0` | Only with `DARK_DISPLAY_MEMO_CHECK=1` | 0 |
| `[NativeLayer] frames=.. fast=.. resolveFallback=0 bindingFallback=0 snapshotFallback=..` | Fast capture path (full captures only) | fallbacks near 0 |
| `[RecordRepeat] active=1 repeats=.. anchors=.. declined=.. audits=.. agreed=.. mismatches=0` | Change 9, every 1000 frames | `active=1`, `audits == agreed` |
| `[EngineWorldDrawRejected] material=0` (20 lines) | Upload budget deferrals while a level loads | normal |
| `[EngineWorldPaletteFallbackRejected]`, `[EngineWorldSnapshotRejected] step=2` | Skinned draws with NaN bone matrices (trench) | predates this work |

### 2.4 Regenerate the PGO profile (do this after any change to hot code)

1. Move the old raw profiles away: everything matching
   `build_native\pgo\*.profraw` (the merge takes every `.profraw` it finds).
   Earlier sets are in `build_native\pgo-old-profraw`.
2. Run `build_native\claude-pgo-gen.cmd`. It builds an instrumented copy in
   `build_pgo` and copies it to `build_native\Release\DarkRecomp-pgogen.exe`.
3. Play three recordings with the instrumented executable: `village-route`,
   `chinatown-under`, `fulton-tour`. For each, in a `cmd` window:

   ```
   set "BENCH_EXE=DarkRecomp-pgogen.exe"
   set "LLVM_PROFILE_FILE=K:\DarkRecomp\build_native\pgo\darkrecomp-%p.profraw"
   set "BENCH_RECORDING=K:\DarkRecomp\tools\recordings\village-route.txt"
   K:\DarkRecomp\build_native\claude-play.cmd
   ```

   (`%p` is literal: the runtime replaces it with the process id. In a
   `.cmd` file write `%%p`.) Each run leaves one 13 MB `.profraw`.
4. Run `build_native\claude-pgo-use.cmd`. It merges the profiles, rebuilds
   everything with them and runs the tests (about 6 minutes). Log:
   `build_native\claude-pgo.log`; it must end with `=== use exit 0` and
   contain no `hash mismatch` after its last `=== use start`.

### 2.5 CPU sampling

`set "BENCH_EXTRA_ARGS=--sample-engine --sample-renderer"` in
`experiment.cmd`. The log then has `[CpuHotspot]`, `[CpuRegion]` (source
lines) and `[CpuRenderHotspot]`/`[CpuRenderSource]` blocks every few seconds.
`build_native\claude-queue\tools\analysis\prof.py <game.log> <firstLine>
<lastLine> [N]` adds them up over a line range of the log (pick the range of
the view of interest by the `[InputTest]` lines around it). Sampling costs a
few percent; do not take timings from sampled runs.

### 2.6 One unit test alone

```
cmake --build build_native --config Release --target EngineMeshTests --parallel
build_native\Release\EngineMeshTests.exe K:\DarkRecomp\Darkness
```

Tests print progress with `puts` (buffered) and errors to stderr; on a crash
the buffered lines are lost, so the last visible line is not where it died.

### 2.7 The "DarkRecomp runner" (assistant only)

`build_native\claude-queue\darkrecomp-runner.mcpb` is a small local MCP
extension for the Claude desktop app with three tools: start a job (`build`,
`build_and_test`, `play` with a recording and optional switches,
`pgo_instrument`, `pgo_use`), job status, stop job. It only starts the
scripts above. It whitelists five switches; anything else goes through
`experiment.cmd`. Another assistant needs either this extension or a normal
terminal.

---------------------------------------------------------------------------

## 3. How a frame flows

Two threads carry a frame.

Engine (guest) thread: the recompiled game code. The game's renderer walks
its scene, queues draws in its "VB manager", then executes that queue; each
executed draw goes through the Xbox D3D runtime's indexed draw, which is
hooked.

```
825A24D0  VB manager executes its queue          hook: render_trace.cpp:1290
  per record: attributes, view block, clip planes (only when changed)
  82299EA0  one record                           hook: render_trace.cpp:1297  (changes 8, 9)
    per element: 8225E2F0 indexed subset draw    hook: render_trace.cpp:1441
      8225D780  bind vertex buffer
      82249488  apply pending attributes/transform/constants/program
      82868FE8  Xbox indexed draw                hook: render_trace.cpp:1661  (changes 2, 3)
        screenFogAbsorb                          simple_mesh.cpp:1655         (change 2)
        nativeLayerObserveStoredDraw             simple_mesh.cpp (the fast capture)
          resolveStoredDrawFast -> StoredGeometryCache::drawFast   stored_geometry.cpp:440
          queueRepeatedWorld (a repeat: returns here)              simple_mesh.cpp          (change 9)
          currentEngineVertexBindingFast                            engine_vertex_program.cpp
          captureWorldDrawFast -> fastSnapshotImpl                  world_mesh.cpp:1834 / 1672
          screenFogTake                                             simple_mesh.cpp:1474
          previewQueueCapturedWorld -> queueCapturedWorldLocked     simple_mesh.cpp
```

A captured draw is a pooled `WorldDraw` (`world_mesh.h`): geometry
references, 256 vertex constants, 64 fragment constants, 160 attribute bytes,
viewport, 16 texture slots with samplers and images, targets. Draws are
queued as commands (`SimpleMesh`) in `pending`; every 512 commands a "part"
is published to the display thread (`publishPreviewPart`,
simple_mesh.cpp:2316), the last part of a frame at `previewEndFrame`. Up to
three published parts wait; then the engine blocks (backpressure).

Display thread: `takePreviewFrame` (simple_mesh.cpp:2434) hands over a part
and retires the previous one; `EnginePreviewD3D11::render`
(`renderer\d3d11\engine_preview.cpp`) loops over the commands;
`WorldRendererD3D11::draw` (`world_renderer.cpp:1991`) turns one `WorldDraw`
into Direct3D 11 state and a draw call; `beginFrame` (3185) sweeps caches.

Where the engine thread's samples fall in the Chinatown view on the 17:45
build (change 10 does not touch the engine thread) (`claude-play-21549-1408`, `--sample-engine --sample-renderer`):
geometry verification `fastVerifyDrawImpl` 9.7% (`stored_geometry.cpp`:
guest resource objects, table and validity bytes read per draw), lock
release and acquire 9% (the geometry cache mutex in `drawFast` and the
queue mutex, per draw and per repeat), queueing full captures 6%, the
game's resource lookup by id 822527F0 5.5% (called for the vertex and the
index buffer of every element), `nativeLayerObserveStoredDraw` 5.3%,
`resolveStoredDrawFast` 4.3%, the fast snapshot 4.2% (full captures only),
appending a command (`SimpleMesh` is about 330 bytes) 2.9%, 822569E0 3.2%,
the hook on 82868FE8 2.6%, 82249488 2.5%, 82248C80 2.3%, 8225E2F0 1.7%,
8225D780 1.2%. Display thread by phase: states 44%, textures 26%, geometry
11%, constants 9%, submit 6%. (Before change 9, on the village route: fast
snapshot 10%, queueing 8%, verification 3.3%, locks about 4%; states 39%,
textures 21%, constants 12%, geometry 11%, submit 9%.)

---------------------------------------------------------------------------

## 4. The ten changes

Each has an environment switch (set before launch) unless noted. "Same
executable" figures compare the switch on and off in one build.

### 1. Velocity pass skipped while motion blur is off

- Where: `render_trace.cpp`, hook on 82276148 (line 1463). Both callers
  (825BDDA4, 825D0930) ignore its result.
- Why safe: the velocity ("MotionMap") pass feeds only the final composite's
  motion-blur input; with motion blur off the composite permutation chosen
  for the frame does not sample it.
- Switch: `DARK_VELOCITY_PASS=1` runs it again.
- Effect: 17-23% fewer draws, +8-25% FPS in the village.

### 2. Depth fog as one screen-space pass (Otherworld levels)

- Where: `renderer\engine\screen_fog.h` (math and contract),
  `simple_mesh.cpp` (`ScreenFogState`, `screenFogTake`, `screenFogAbsorb`,
  `screenFogObserveDraw/Clear`), `world_mesh.cpp` (`screenFogReferenceFrom`,
  `screenFogRawMatches`, `screenFogMatches`, `screenFogRebase`),
  `world_renderer.cpp` (the pass and the verification mode).
- Idea: the game redraws every depth-prepass object with the fragment
  program `XREngine_DepthFog` (depth EQUAL, stencil EQUAL then INCR, blended).
  Its two interpolants are linear in the post-skin position, as is the clip
  position, so for all objects of one view they are one linear function N of
  the clip position. The first fog draw of a run is captured and held as the
  reference; later fog draws whose device state is byte-identical
  (`screenFogRawMatches`) and whose constants reproduce N within 2e-3
  (`screenFogMatches`) are absorbed: not captured, not drawn. The held draw
  is queued at the end of the run and rendered as one pass over the pixels
  that still hold their prepass depth. Geometry that wrote depth after the
  prepass ("late twins") keeps its own fog draw.
- Switches: `DARK_SCREEN_FOG=0` restores per-object fog.
  `DARK_SCREEN_FOG_VERIFY=1` absorbs nothing, draws everything per object and
  has the renderer compare with the pass it would have drawn
  (`[ScreenFogVerify]` lines; `verify.py`). `DARK_SCREEN_FOG_DEBUG=1`.
- Evidence on the final build: 258 runs, 503 million fogged pixels, one pixel
  over 1/255 (largest difference 0.0060).
- Effect: 28-30% fewer captured draws; +6-7% FPS aerial, +20% at the spawn.

### 3. The original Xbox indexed draw is not run in preview play

- Where: `render_trace.cpp`, hook on 82868FE8 (line 1661): after the native
  observation the hook returns instead of calling `__imp__sub_82868FE8`.
- Why safe: the call flushes dirty state into the Xbox command stream and
  appends a draw packet; nothing in this port reads that stream and its five
  callers ignore the result. A temporary audit copied the device object
  (16 KiB) and the render context around 6.8 million original draws with
  control windows: the call changes only the dirty masks (device+0..39),
  command-buffer bookkeeping and flush-derived words that only flush paths
  read. Details and offsets: `RENDERING.md`, "The original indexed draw".
- Switch: `DARK_SKIP_XBOX_DRAW=0`. Renderer tracing and non-preview runs
  always run the original.
- Effect: about 10% less engine CPU per draw.

### 4. Prompt and subtitle context lookups return before their mutex

- Where: `simple_mesh.cpp` (`queuedPromptGeneration`,
  `record/forgetQueuedPromptContext`, `queuedPromptContext`,
  `record/queuedSubtitleContext`), hooks 8259D098, 8259CA10, 82299EA0.
- What: an atomic count of tracked entries; while it is zero the functions
  return without the lock. A thread-local cache of the manager generation,
  invalidated by an epoch counter.
- Switch: none. Unit tests of these functions are unchanged and pass.
- Effect: five lock round trips fewer per queued draw.

### 5. Display thread: lookups repeated for every draw

- Where: `world_renderer.cpp/.h`, `descriptor_cache.h`, `engine_preview.cpp`.
- What: no AddRef/Release per texture slot (the caches park what they evict
  during a draw: `retiredViews_`, `retiredSamplers_`); last sampler per slot,
  last depth/blend/raster state, last two target surfaces remembered; a
  64-bit hash filter in front of the resolve map; exponent table; complete
  orphan sweep every 16th frame (immediate geometry every frame); prefetch of
  the next command; `ColorImage` cache-line aligned.
- Switch: none. `DARK_DISPLAY_MEMO_CHECK=1` runs every replaced lookup as
  well and compares (`[DisplayMemoCheck]`): 80.7 million comparisons on the
  final build, 0 mismatches.
- Effect: display CPU per frame 8.36 to 6.06 ms in Chinatown at the time.

### 6. Fog bookkeeping only while fog draws arrive

- Where: `ScreenFogState::watching` / `quietClears` in `simple_mesh.cpp`.
  The first fog draw starts the watch; 8,192 depth clears without one end it.
- Effect: the New York levels pay nothing for change 2.

### 7. Draws refer to geometry and images without counting references

Section 5. Switch: `DARK_OWNED_DRAW_REFERENCES=1` restores counted
references. Check: `DARK_DRAW_PIN_CHECK=1`. Same executable, Chinatown:
7.55 to 7.10 ms.

### 8. Queued fog records are decided before the game prepares them

Section 6. Switch: `DARK_FOG_EMITTER=0` executes everything as before; `=2`
executes everything and compares every prediction. Same executable, village
aerial 2: 7.74 to 7.40 ms. Since change 9 the record loop also runs for
repeats; `DARK_FOG_EMITTER=0` together with `DARK_RECORD_REPEAT=0` leaves
every record to the original 82299EA0.

### 9. Repeated elements of a record share the first one's captured state

Section 6a. An element of a queued record whose state is known to equal the
previous captured element's is queued as "that draw's state, this geometry"
without reading the device. Switch: `DARK_RECORD_REPEAT=0` captures every
element in full; `=2` captures every element and counts how often the
prediction holds (`[RecordProbe]`). Same executable, Chinatown: 7.22 to
4.88 ms. Profile-guided, against the 14:49 build: Chinatown 6.86 to 4.52 ms,
village aerial 7.61 to 6.35 ms.

### 10. Repeats drawn with their geometry only (display thread)

Section 6b. A repeat drawn right after its `WorldDraw` binds only its vertex
and index buffers and draws. Switch: `DARK_REPEAT_SHORT_PATH=0` draws every
repeat with the full path; `=2` does that and requires the full path to
change no other binding and upload no constants. Same executable, Chinatown:
4.53 to 4.27 ms; display CPU 4.32 to 2.71 ms.

All switches found in the source by `getenv("DARK_...")`: the ones above plus
`DARK_NATIVE_LAYER` (0 original capture, 1 fast, 2 run both and compare),
`DARK_UNBUFFERED_LOG`, and older probes (`DARK_OW_PROBE`, `DARK_OW_BYPASS`,
`DARK_TEXTURE_TRACE`, `DARK_FILE_TRACE`, `DARK_RESOURCE_WAIT`, `DARK_*_PROBE`).

---------------------------------------------------------------------------

## 5. Change 7 in detail: draw references and pin epochs

### Problem

Each captured draw held about five `std::shared_ptr` copies (vertices,
indices, one per sampled image). The engine thread incremented the counts,
the display thread decremented them when it retired the part: locked writes
on cache lines the other thread wrote last.

### Design

- `renderer\engine\unowned_reference.h`: `unownedReference(ptr)` builds a
  `shared_ptr` with no control block (aliasing constructor on an empty
  pointer): copying or destroying it touches no count, `use_count()` is 0.
  `ownerOf(ref)` returns a real owner through `weak_from_this()`.
  `StoredGeometry` and `ColorImage` derive from
  `std::enable_shared_from_this` for that (and got a user-provided default
  constructor, because an aggregate with that base cannot be brace-initialized).
- A pin epoch is `previewPinEpochFrames` = 8 frames (`simple_mesh.h`).
- Geometry: `StoredGeometryCache::drawFast` (stored_geometry.cpp:440), under
  the cache mutex, pushes the entry's owner into `pins_` the first time the
  entry is used in the epoch (`Entry::pinEpoch`), and returns unowned
  references plus `StoredDraw::pinEpoch` (carried into
  `WorldGeometry::pinEpoch`; 0 means "these references own").
  `takePins(out)` moves the epoch's owners out and starts the next epoch.
- Images: in `queueCapturedWorldLocked`, where a slot's image comes from the
  `colorTextures` cache (the memo path), the owner is pushed into
  `framePins->images` the first time in the epoch (`CachedColor::pinEpoch`)
  and the draw gets an unowned reference. This happens under `queueMutex`,
  before later slots of the same draw can replace or evict the cache entry.
- `struct FramePins { geometry; images; }` (simple_mesh.cpp:189).
  `framePins` is the current epoch's set, `previousFramePins` the one before.
  `publishPreviewPart` stores `framePins` in the first command of every part
  (`SimpleMesh::retained`). At the end of every 8th frame it calls
  `takePins` into `framePins->geometry`, rotates the sets, and releases the
  set that was "previous" outside the queue lock. In steady state the engine
  thread is the last owner, so only it ever writes the counts.

### Invariants (these are the safety argument)

1. Everything a queued draw refers to without owning has its owner in the
   set of the epoch the draw was queued in, or still in the cache's `pins_`
   of that epoch (which moves into that set when the epoch ends).
2. Every part holds the set of the epoch it was published in. The set
   exists while any holder exists; the engine holds current and previous.
3. A draw whose `geometry.pinEpoch` is not the current epoch when it is
   queued (`admitUnownedGeometry`, simple_mesh.cpp:213): epoch 0 owns its
   geometry; the previous epoch is still held by the engine, so the owners
   are copied into the current set (`repinGeometry`, counter `staleRepins`);
   anything older is dropped (`staleDrops`, log line). This is checked
   before anything dereferences the geometry.
4. Publishing a part from inside `queueCapturedWorldLocked` releases the
   queue mutex for a moment (it may wait for the display thread). The
   function keeps the set alive across that (`pinsBeforePublish`) and, if
   the epoch changed meanwhile, re-pins the draw's geometry and images into
   the new set before the draw is queued.

### Who still owns

Immediate geometry (per-frame snapshots), images decoded while a draw is
queued, alpha images converted to color, the slow capture path
(`StoredGeometryCache::draw`), hand-built draws in tests. Owning and unowned
references mix freely in one draw.

### Consumers that keep something beyond a part

They must call `ownerOf()`:

- `WorldRendererD3D11::image` (cache entry `Image.source`),
  `draw()` at the vertex and index cache misses (`Geometry.source`,
  `Indices.source`). The orphan sweeps (`use_count()==1`) work as before
  because the caches hold real owners.
- `failedSubtitles_` (weak references) in `world_renderer.cpp`.
- `TextureMods::Worker::request` in `texture_mods.cpp` (entry and job).
- `subtitleFontReplacementFor` in `subtitle_font.cpp`.
- `keepWorldDraw(part, index)` (`world_mesh.h`) returns one draw that shares
  its part's owners; the tests use it where they return a draw from a helper.

### Behaviour change

A resource replaced or evicted while in use is released after two further
epochs have closed (at most 16 frames), not when its part retires. During a
long load without frames, what the last frames drew stays alive until frames
resume. Measured pins per epoch: 5,102 geometry objects and about 990 images
in Chinatown noclip; 641 and 328 in the village.

### Evidence

- `tests\stored_geometry_tests.cpp`, `pinContracts`: unowned fast references,
  one owner per entry and epoch, replacement safety, hand-over.
- `tests\engine_mesh_tests.cpp`: the streaming-image test now asserts that a
  replaced generation survives while its frame's parts are in use and is
  gone after 2 x `previewPinEpochFrames` further frames.
- `DARK_DRAW_PIN_CHECK=1` (`checkDrawPins`, simple_mesh.cpp:2294): before
  each part is published, every unowned reference must be in the current
  set or in the cache's pins. Final build: 247,000 parts, 66 million draws,
  308 million references, 0 unpinned, across level changes.

### Not exercised in play

The texture-mod path (no mod pack was installed; its unit tests pass) and
subtitles in world space with a scaled font (unit tests only).

---------------------------------------------------------------------------

## 6. Change 8 in detail: queued fog records

### Observation

The game queues draws as records. A record carries one attribute block, one
transform and a chain of elements (vertex resource id, index resource id,
triangle count, first index). 82299EA0 walks the chain and calls 8225E2F0
per element. Fog records on the village route average 236 elements. Before
this change every element went through the game's full preparation before
the hook on the indexed draw decided "absorbed".

### Rule

Once one element's draw has been absorbed at the indexed draw (the exact,
existing decision), every later element of the same record is predicted to
be absorbed, unless its geometry is a late twin. Reason: between elements of
one record the game binds vertex and index buffers and, when the vertex
format differs, selects the vertex program. It applies no attributes, no
transform, no view, and writes none of the bytes `screenFogRawMatches`
reads; the constants c0..c7 are not recomputed.

### Implementation

- `screenFogQueuedElements` (world_mesh.cpp:1941) reads the chain into a
  scratch array (limit 4,096). It accepts only records with flag 8 whose
  elements are all kind -4 or have no vertex resource; anything else returns
  false and the original 82299EA0 runs.
- Hook `sub_82299EA0` (render_trace.cpp:1297): when called from 825A2E18,
  not tracing, preview on, and `screenFogEmitterBegin` accepts the record, it
  loops over the elements itself: `screenFogEmitterSkip(element)` true means
  leave it out; otherwise set r3..r6 and lr=0x82299F14, call `sub_8225E2F0`
  (the hooked function) and then `screenFogEmitterExecuted()`. It returns
  the element count in r3 as the original does and restores lr.
- `screenFogEmitterBegin` (simple_mesh.cpp:1581) requires
  `ScreenFogState::watching` (so New York levels never enter), mode not 0,
  not failed, not verify mode, fast capture on, a single emitter thread, at
  least two elements, and no record already executing (re-entrancy guard).
- State machine (`FogEmitter` in simple_mesh.cpp): `covered` becomes true
  when an executed, non-late element was observed absorbed
  (`screenFogEmitterObserved`, called from the 82868FE8 hook right after
  `screenFogAbsorb`), and is tied to the run (`fog.runs`). A late twin is
  always executed and does not change `covered`. An executed element that
  was not absorbed clears it.
- Audit: in mode 1, one predicted element in 64 is executed and compared.
  One disagreement sets `failed`: skipping stops for the session and a line
  is logged. Mode 2 compares all and never skips.

### What a skip leaves undone, and why that is fine

The skipped 8225E2F0 would have: stored the vertex id in context+12564 and
restored it; cleared the two "rejected" bits of context+17180 (the next draw
clears them itself); bound the vertex buffer and updated the "last bound"
words (skipped together, so they stay consistent); found nothing pending in
82249488; set the index buffer; drawn. The next executed element or record
binds what it needs.

### Evidence

- `DARK_FOG_EMITTER=2`: village 10,557,381 predictions, hills 1,521,808,
  trench 3,068,367, castle 5,836,624; all confirmed, none without a draw.
- Normal play audits on the final build: 173,047 on the village route, all
  agreed.
- Coverage of absorbed fog draws: village 95%, castle 98%, hills 87%,
  trench 84%. The rest: single-element records, and the elements before the
  first absorbed one in each record.
- `tests\stored_geometry_tests.cpp`, `queuedElementContracts`: the chain
  reader.

---------------------------------------------------------------------------

## 6a. Change 9 in detail: repeated elements of a record

### Observation

A record's elements share attributes, transform, view, textures and
constants (section 6). A probe compared every captured element with the
previous capture of the same record, ignoring geometry and image references
(`sameCapturedWorldDraw(a, b, field, true)`):

| Recording | Captured | Identical to previous element | Predicted (rule below) | Predicted wrongly | Run |
|---|---|---|---|---|---|
| `chinatown-under` | 30.87 M | 24.73 M | 24.71 M | 0 | `claude-play-4744-2493` |
| `village-route` | 31.53 M | 21.45 M | 19.82 M | 0 | `claude-play-5364-13074` |
| `trench-tour` | 12.49 M | 7.22 M | 6.58 M | 0 | `claude-play-5074-6738` |

The New York levels have as many multi-element records as the Otherworld
ones (Chinatown: 2.28 M records with 28.4 M elements in that run).

### Rule

Before the element's 8225E2F0 call (in `screenFogEmitterSkip`):
`(context+340 & ~0x1800) == 0 && context+8228 == 0 && context+8344 == 0`
(`fogEmitter.elementClean`: 82249488 will apply no attributes, no constants,
no clip planes for it), and at its draw: no call of 82248C80 (vertex program
preparation; runs when the vertex format changes) since the earlier capture
(thread-local counter `engineVertexProgramPrepares()`, incremented in the
82248C80 hook in `engine_vertex_program.cpp`), and the same record
(`fogEmitter.recordSerial`).

### Implementation (all in `simple_mesh.cpp` unless noted)

- `screenFogEmitterBegin` accepts records whenever repeats are on, not only
  while fog draws arrive; `fogActive` tells `screenFogEmitterSkip` whether
  fog elements may be skipped in this record. `recordSerial` counts accepted
  records.
- `queueCapturedWorldLocked` notes where it queued a draw
  (`lastQueuedWorld`: part serial `pendingPartSerial`, index in `pending`,
  the `WorldDraw`, pin epoch) unless the draw is a fog draw or carries fog
  requests. `publishPreviewPart` increments `pendingPartSerial`.
- `nativeLayerObserveStoredDraw`: after a full capture queued inside the
  loop, `repeatAnchor` = `lastQueuedWorld` + record serial + preparation
  count. On a later element: if the rule holds, `queueRepeatedWorld(owned)`
  appends a command with `world` = the anchor's `WorldDraw` (a counted copy
  of its `shared_ptr`) and `repeat` = the element's geometry. It declines
  (returns false, nothing changed) when the anchor is no longer in the part
  being built, the part's command or geometry budget would be exceeded, or
  `screenFogObserveRepeat` reports that this would be the first depth write
  after the prepass of a tracked fog pass (that draw may need the depth
  snapshot flag). A declined or non-repeat draw is captured in full and
  becomes the next anchor.
- Every 64th predicted repeat is captured in full and compared
  (`auditRepeatedWorld`); a mismatch sets `recordRepeatCounts.failed`.
- `screenFogEmitterSkip` clears `repeatAnchor.valid` when an element is not
  clean.
- Display: `SimpleMesh::repeat` (`simple_mesh.h`, where `WorldGeometry` now
  lives), `engine_preview.cpp` passes it to
  `WorldRendererD3D11::draw(draw, repeat)`, which uses
  `repeat ? *repeat : draw.geometry` for the geometry stage, `flatPostQuad`
  and the rejection report, and hands inspection/capture/fog paths a
  complete copy.
- `checkDrawPins` checks `command.repeat` as well.
- Unit test: `testRepeatComparison` in `tests\engine_mesh_tests.cpp`.

### Why it is safe

- The repeat's state is the anchor's captured state. The rule says nothing
  was applied to the device between the two draws that the capture reads;
  the probe found no exception in 51 M predictions, and the audit keeps
  checking in play.
- The geometry is the element's own, resolved exactly as before.
- Pins: the anchor's images are pinned in the current epoch (the anchor
  must be in the part being built, and parts never span epochs); the
  repeat's geometry carries its pin epoch like any draw.
- Fog: fog draws are never anchors, so every one still reaches
  `screenFogTake`; repeats do the depth-writer bookkeeping of
  `screenFogObserveDraw`; the draw that may carry the snapshot flag is
  never a repeat.

### Evidence (build of 17:00, `DARK_RECORD_REPEAT=1`, before it became the default)

| Run | Recording | Checks | Result |
|---|---|---|---|
| `claude-play-11229-16860` | chinatown-under | pins | 21.5 M repeats, 3.9 M anchors, 342,355 audits agreed, 0 unpinned |
| `claude-play-11592-30285` / `claude-play-11928-23259` | chinatown-under | timing on / off | 4.88 / 7.22 ms |
| `claude-play-12291-3916` | village-route | pins, display memo | 21.6 M repeats, 342,709 audits agreed, 488 M memo checks, all 0 |
| `claude-play-15367-3603` | trench-tour | pins, display memo | 6.6 M repeats, 104,452 audits agreed, 0 |
| `claude-play-15670-20165` | level-changes | pins, display memo | 9.4 M repeats, 149,409 audits agreed, 0 |
| `claude-play-17163-16986` | castle-tour (default-on build) | pins, display memo | 12.3 M repeats, 195,678 audits agreed, 0 |
| `claude-play-12797-31519`, `claude-play-14890-7199` | village-route | fog verify | see section 10 |

Captures of the same views with repeats on and off differ only as much as
two runs without them do (camera timing).

Village, same 17:22 build (`claude-play-17450-12574` on,
`claude-play-17868-12114` off): spawn 2.80 / 4.18 ms; engine microseconds
per draw 1.06 / 1.29. (The aerial views of that pair ended at different
camera positions, 6,009 and 5,917 draws; the route reaches either in runs
with and without repeats, e.g. `claude-play-23294-29403` from before change
9 shows 6,012.)

Profile-guided 17:45 build: Chinatown 54.7 M repeats and 8.3 M full
captures (87% repeats), 869,705 audits agreed; village 30.5 M and 9.1 M
(77%), 485,138 agreed.

---------------------------------------------------------------------------

## 6b. Change 10 in detail: repeats drawn with their geometry only

### Observation

After change 9 the display thread cost about as much as the engine thread in
Chinatown, and 87% of its draws were repeats: commands whose `WorldDraw` is
the previous command's. Everything `WorldRendererD3D11::draw` derives from
the `WorldDraw` (targets, depth/blend/raster states, scissor, viewport,
shaders, textures and samplers, vertex, pixel, viewport and clip constants)
is then already bound by the previous call; its memos (change 5) find that
out step by step (states 44% and textures 26% of display samples).

### Implementation

- `engine_preview.cpp` keeps `drawnWorld`: the previous command's `WorldDraw`
  if that command was a world draw and `draw()` returned true; reset by any
  other command. A repeat of that `WorldDraw` is passed `continues=true`.
- At the end of the full path, `draw()` sets `shortPathDraw_ = &draw` (and
  `shortPathFormats_`) when the bindings are valid and nothing depended on
  the geometry beyond the buffers: no screen-space fog or fog verification,
  no measurement or inspection, no prompt quad transform
  (`viewportValues[1]` is the identity), and no tracked blur footprint on
  the color target (`logicalBlurRegion` empty and not the bloom snap case).
  Every full-path call clears it first; `invalidateBindings()` clears it.
- `draw(draw, repeat, continues)` takes `drawRepeat()` when `continues`, the
  draw is `shortPathDraw_`, the bindings are valid and no inspection,
  capture or diagnostics run. `drawRepeat()` requires the same 16 vertex
  formats, a valid index range and both buffers already in `geometry_` /
  `indices_` (a miss returns false and the full path uploads, with its
  budget); repeats the skinning index-bounds check
  (`WorldVertexShaderD3D11::indexBoundsValid`, the same test `bind()` makes);
  checks indices against the vertex count; binds the buffers if they differ;
  `DrawIndexed`; updates the use clocks and counters (`drawOrdinals_`,
  `profileDraws_`, `workTotals_`, `recordWorldSubmission`).
- Check mode (`DARK_REPEAT_SHORT_PATH=2`): such repeats take the full path
  between two snapshots of every remembered binding plus a count of
  constant uploads (`worldConstantUpdates`, incremented in
  `updateConstants` and `updateConstantRows`); anything but the vertex and
  index buffers changing is a mismatch.
- Unit test: in `deadNormalContract` (`tests\world_renderer_tests.cpp`), a
  repeat with a shifted triangle drawn after its `WorldDraw` must take the
  short path and produce exactly the pixels of the two complete draws.

### Evidence

| Run | Recording | Result |
|---|---|---|
| `claude-play-23207-22111` | chinatown-under, check | 13.6 M+ checked, 0 mismatches; 1,133 repeats the full path rejected for the upload budget while loading (counted as `rejected` since) |
| `claude-play-24390-13642` | village-route, check | 30.4 M checked, 0 mismatches, 45 rejected |
| `claude-play-24817-12659` | trench-tour, check | over 5 M checked, 0 mismatches |
| `claude-play-23576-24264` / `claude-play-23910-6490` | chinatown-under, on / off | 4.27 / 4.53 ms; display CPU 2.71 / 4.32 ms |
| `claude-play-26186-27025` | level-changes, default on, pins and display memo | all checks 0 |
| `claude-play-29239-17009`, `claude-play-29569-21254` | profile-guided 18:28 build | Chinatown 4.23 ms (display CPU 2.61); village aerial 6.31 ms (display 3.72; engine-limited) |

---------------------------------------------------------------------------

## 7. What was learned about the game's renderer

Addresses are guest addresses of the original executable. The recompiled
code is in `build_native\generated\ppc_recomp.N.cpp`, one C++ function per
guest function, with the PowerPC assembly as `// ` comments; search for
`PPC_FUNC_IMPL(__imp__sub_82299EA0)`. Hooks are `PPC_FUNC(sub_X)` in
`renderer\engine\render_trace.cpp`; a hook calls the original as
`__imp__sub_X(ctx, base)`. Guest memory is big-endian; `base` is the host
address of guest address 0.

All of this was read from the recompiled code during this work. What the
changes rely on was confirmed by the check modes; the rest (for example the
exact matrix arithmetic of 82248A78, or what the record's rectangle index
means) is a reading, not a verified fact.

### Functions

| Address | Role |
|---|---|
| 825A24D0 | VB manager: executes the queued records. Per record: scissor rect; `82299E30` when the record's attribute pointer (+12), object (+8) or its 8 texture ids (+44) differ from the previous record; copies the 416-byte view block and calls `82249580` when byte +40 differs; clip-plane set when byte +41 differs; then `82299EA0`; adds the returned count to a statistic. Call site return address 825A2E18. |
| 82299E30 | Applies a record's attributes: `82763D50(context, record+12, record+44)` writes them into the current attribute stack entry and sets dirty bits; then the object at record+8 (`821F1548`, `82236B18`). |
| 82236908 | Sets the transform pointer (stack entry +76); sets dirty bit 0x400. |
| 82299EA0 | Executes one record. Flag 8: element chain at record+4. Element kinds: -4 `8225E2F0(vertices, indices, triangles, first)`; -3 `8225E218(vertices)` whole resource; -2, -1, 1 immediate paths (`8225DD40`, `8225DE78`, `8225DBC8`) after `821F1548`. Without flag 8: 76-byte inline descriptors copied to context+12480. Returns the element count. |
| 8225E2F0 | Indexed subset draw: `8225D780`, `82249488`, index lookup `822527F0`, set indices `82860FC8`, `82868FE8(device, 4, 0, first, triangles*3)` from return address 8225E3B8. |
| 8225D780 | Binds the vertex resource (context+12564) if it differs from the last bound (+16532): lookup, stream source `82860E20`; if the vertex format words change, sets 0x200000 in context+17180. With an immediate source (+16512) it goes to `8225CDD8`. |
| 82249488 | Applies what is pending: `82247FE8(entry)` if the dirty word (+340) has bits beyond 0x1800; `82248A78` if +8228 has its low bits; `82249250` if +8344 (clip planes); `82248C80` if +17180 has 0x200000. Clears +340. |
| 82247FE8 | Applies an attribute block (675 instructions): render state, textures, programs. |
| 82248A78 | Writes vertex constants c0..c7 at device+1920: c0..c3 = model-view (without translation) times projection (context+17088); c4..c6 = model-view 3x3; c7 = the translation as an object-space offset (the template adds c7 to the position first). |
| 82248C80 | Vertex program preparation (calls 8224DC70, 8224DAE0, 8224A0A8, 8224A2E8, 8224AB18, then device calls). Writes the texture-coordinate stage constants. |
| 82249580 | Applies the current view block: viewport from its rectangle (+316), two floats (+300, +304), projection (+0..63) into context+17088 scaled by 1/width and 1/height; marks constants dirty. |
| 822478C0 | Copies a fragment program's constants (program+12 points to them, program+16 low byte is the count) into device+6016. |
| 82868FE8 | Xbox indexed draw: flushes dirty state (828714A0, 82872238, 82870D68, 82871100, 828711C0, 82871340) and appends a draw packet at the command cursor (device+48). |
| 822527F0 | Resource lookup by id (table at context+16636, validity bytes at [context+160]+24). |
| 82276148 | Velocity pass emitter (callers 825BDDA4, 825D0930). |
| 8259DE30 / 8259CA10 / 8259D098 / 8259D3B8 / 8259C490 | VB manager: begin frame / init a 48-byte VB / enqueue one / enqueue many / retire. |

### Queued record (as 825A24D0 and 82299EA0 read it)

| Offset | Content |
|---|---|
| +0 | flags (u16): 8 = element chain; 4 = the index at +2 selects a rectangle (it looks like a scissor rectangle) |
| +4 | first element of the chain |
| +8 | an object applied with the attributes (purpose not determined) |
| +12 | pointer to the 160-byte attribute block |
| +20 | list node: points to the next record's node; low bit of the node's second word ends the list; record = node - 20 |
| +40 | view block index (byte) |
| +41 | clip-plane set index (byte) |
| +44 | pointer to 8 texture ids |

Element (flag 8): +0 next, +8 vertex resource id (u16; 0 = skip), +10
triangle count (u16), +12 kind (s16), +14 an id for `821F1548`, +16 index
resource id (u16), +18 first index (u16).

### Render context (0x82A69B00)

| Offset | Content |
|---|---|
| +132, +136 | view-block stack: [+132]+24 is the array of 416-byte blocks, +136 the current index |
| +160 | manager; [+160]+24 = validity bytes by resource id |
| +336 | attribute stack index |
| +340 | pending attribute dirty word (0x400 = transform pointer; 0x1800 alone does not trigger an apply) |
| +352 | attribute stack: 48 entries of 160 bytes (entry+76 = transform pointer) |
| +8032 | accumulated dirty words per stack entry |
| +8224, +8232 | model-view payloads (656 bytes each, matrix at +16) and the selected index |
| +8228 | constants dirty (low two bits: c0..c7) |
| +8336, +8340, +8344 | clip-plane sets (272 bytes each), index, pending flag |
| +12564 | vertex resource id of the draw being issued |
| +15748 | device object |
| +16420, +16424, +16428 | current vertex format words |
| +16512 | immediate vertex source (nonzero: not a stored draw) |
| +16532 | last bound vertex resource id |
| +16636 | resource table |
| +16896 | applied attribute block, 160 bytes: what the capture reads. +0 fragment program, +8 texture ids, +76 transform pointer, +92.. state flags, +96 depth, +120.. stencil |
| +17088 | projection, 4 vectors |
| +17152 | viewport (x, y, w, h, then depth range floats) |
| +17180 | flags: 0x200000 = prepare the vertex program again; top two bits = this draw is rejected |

### Device object

+0..39 dirty masks; +48 command cursor; +1152 samplers (24 bytes per slot);
+1920 vertex constants; +6016 fragment constants; +10272 clip planes;
+10368 surface words (24 bytes); +10564 clip control; +12428 bound index
buffer; +12432 render targets (4 color, depth at +12448); +12536 bound
textures (4 bytes per slot); +12688 program binding.

### View block (416 bytes)

+0..63 projection; +240 a rectangle; +300, +304 floats; +316 viewport
rectangle (x0, y0, x1, y1). The rest was not decoded.

### Fog pass facts

Fragment program `XREngine_DepthFog`, material "post". Both texture stages
are "linear" (template mode 1) without a texture matrix; their four rows
start at the stage's TEXPARAM register. A draw whose clip z row equals its w
row (the backdrop) has constant depth and always stays per object. Typical
village numbers per route: 22,849 runs, 11.7 million absorbed, 957,000 per
object, of which 951,000 late twins.

---------------------------------------------------------------------------

## 8. Measuring: method and traps

- Compare frame time per view and engine microseconds per draw, not only
  FPS. The noclip camera moves per frame, so timed key presses land in
  slightly different places when the frame rate changes; draw counts of "the
  same view" differ by a few percent between runs (5,745 to 6,008 was seen).
- Machine state matters. On 2026-11-10 around noon everything ran 10% slower
  for an hour for no identified reason. Background load found later: about
  10% of the processor, chiefly two renderer processes of the Oculus desktop
  client (`C:\Program Files\Oculus\Support\oculus-client\Client.exe`) at 45%
  of a core each, running since Nov 7; closing that app frees a core. Always
  re-time the baseline in the same session as the candidate.
- A build with edited hot functions has lost their profile (2.1). Use the
  switches for same-executable comparisons, then regenerate the profile.
- `[InputTest]` lines are written with `printf` to a redirected stdout, so
  they reach `game.log` in bursts, long after the fact. A script that looks
  stuck in the log may be fine. `DARK_UNBUFFERED_LOG` exists in the source
  and is worth trying for this (not verified here).
- Screenshots of two runs differ by camera jitter and temporal effects; use
  the in-frame checks (`DARK_SCREEN_FOG_VERIFY`, `DARK_DISPLAY_MEMO_CHECK`,
  `DARK_DRAW_PIN_CHECK`, `DARK_FOG_EMITTER=2`, `DARK_NATIVE_LAYER=2`).
- The settings limit of 360 FPS caps light views.
- Two results of this work do not join: the build before the noon profile
  regeneration measured 6.91 ms in Chinatown in the morning; the regenerated
  one 7.16 ms in the afternoon. Profile or machine: unknown (see 9.8).

---------------------------------------------------------------------------

## 9. Remaining work, most promising first

Estimates are from sample shares and counts, not from experiments, unless a
measurement is stated.

### 9.1 (done: change 10) Display thread: a short path for repeats

The display thread now has headroom (2.7 ms CPU per frame in Chinatown
against 4.2 ms on the engine thread). What remains there: retiring finished
parts on a worker thread (9.5).

### 9.1b Engine: what a repeat still costs (the limit now)

Shares are from section 3 (Chinatown, 17:45 build). Candidates, in order of
expected gain per effort:

- Geometry verification, 9.7%. `fastVerifyDrawImpl` re-reads the guest
  resource objects, the id table and the validity bytes for every draw.
  Counted (a temporary probe in `drawFast`, Chinatown, 73 M draws,
  `claude-play-30490-4881`): 84% of draws use the same vertex resource as
  the draw before, but only 2% the same index resource. Each element brings
  its own index buffer, so a memo of the vertex half saves little; the cost
  is mostly the new index resource's host and guest objects. A prefetch of
  the next element's index entry from the record loop (the chain is known
  there) is the idea left; prefetching across records did nothing earlier.
- The game's per-element work for a repeat: 822527F0 (resource by id, twice
  per element) 5.5%, 8225E2F0 / 8225D780 / 82249488 / 82860FC8 together
  about 6%, the 82868FE8 hook 2.6%. Change 8 already skips whole elements
  for fog; skipping the preparation of repeat elements would need the
  geometry resolved from the element's ids without the game's binding
  (`resolveStoredDrawFast` reads the bound vertex id at context+16532 and the
  bound index buffer at device+12428), and the last element's binding left
  in place for what follows.
- Locks, 9%: see 9.2.
- Appending a command, 2.9%: `SimpleMesh` is about 330 bytes, mostly
  `shared_ptr` members that a world command never uses; a separate compact
  command type for world draws and repeats would shrink it.

### 9.2 Two locks per draw (about 4% of engine samples before change 9, 9% after)

Every captured draw, and every repeat, takes the geometry cache mutex
(`drawFast`) and the queue mutex (`queueCapturedWorldInto` /
`queueRepeatedWorld`). Both are uncontended almost always. With most draws
now repeats, these, the geometry resolve (`resolveStoredDrawFast`,
`fastVerifyDrawImpl`) and the game's own work per element are most of what
a repeat still costs the engine thread; sample it on the profile-guided
build before choosing.
Options: hold the queue mutex across a whole record in the 82299EA0 loop
(the display thread only needs it once per part; texture uploads on other
threads would wait up to a record's length), and the same for the cache
mutex. Mind the unlock inside `publishPreviewPart` and its callers.

### 9.3 The fog draws still prepared

Single-element fog records and the elements before the first absorbed one:
5% of absorbed fog draws in the village, 13-16% in hills and trench. A first
attempt decided single-element records from the dirty word (context+340)
plus a byte comparison of the view block: it never disagreed with the draw
(20,842 cases) but applied to only 16% of them, because most records arrive
with attribute bits pending. To go further, compare the pending attribute
block (the stack entry at context+352+index*160, without bytes 76..79) with
the reference's applied block, and the fragment program's constants at
their source (program+12) with what the reference uploaded. Small gain.

### 9.4 Fewer passes per light

The frame redraws the scene per light. This is where a large step would
come from, and it needs an understanding of the game's light loop that does
not exist yet. Nothing was investigated beyond counting passes.

### 9.5 Display thread

Besides 9.1: retire finished parts on a worker thread (releasing draws
after rendering is a visible share of the display thread).

### 9.6 `PPC_TRACE_ENTRY`

Every recompiled function records itself in a 64-entry call trace on entry
(`runtime\guest\ppc_context.template.h`). Estimated 1-2% of the engine
thread, not measured. It is a crash diagnostic; make it optional rather
than remove it.

### 9.7 ThinLTO

Measured earlier at 2-3.5%, but it needs the separate Ninja/clang-cl/lld-link
tree (`build_lto`, `build_native\claude-lto*.cmd`). Not part of the normal
build.

### 9.8 The profile question

Try a profile from `chinatown-under` alone, or with it run twice, and
compare Chinatown and village times in one session. If the three-recording
profile really costs Chinatown 3%, weighting recordings is free speed.

### 9.9 Pin epoch length

`previewPinEpochFrames` is 8. Going from 1 to 8 was not measured separately;
the cost left is about 6,000 count pairs per epoch in Chinatown, on one
thread. Nothing to gain here.

---------------------------------------------------------------------------

## 10. Known issues and loose ends

- Loading `SUB_Fulton` from another mission (not from the menu) stalled a
  recording at its first `capture`; a video plays there (`shot-video-*.bmp`
  appeared). Whether the game or the script handling is at fault was not
  investigated; the run was stopped by hand. `level-changes` therefore has
  three missions. `fulton-tour` (from the menu) works.
- The trench reports NaN bone matrices for some skinned draws; those draws
  are rejected. Present before this work, with and without each switch.
- Fog verification with repeats (`DARK_SCREEN_FOG_VERIFY=1`, village route):
  in `claude-play-12797-31519` one 512x512 view that appeared in no other
  run (verified passes with `stay=11` and `stay=8`) had 1 to 4 pixels in 11
  of its 60 verified passes that the per-object fog draws fogged and the
  pass did not (`onlyOriginal`, largest difference 0.22). A second run with
  repeats (`claude-play-14890-7199`, 78 passes at 512x512) and runs with
  repeats off (`claude-play-13296-4857`) or with only the record loop
  (`DARK_RECORD_REPEAT=2`, `claude-play-13741-24848`) had none, but none of
  them showed that view either: which views the 512x512 target shows differs
  from run to run, and only every 37th fog run is verified. Older
  verification runs (before change 9) show differences there only below
  0.016 and never `onlyOriginal` pixels. No mechanism was found: the fog
  bookkeeping of a repeat is that of a full draw, and the display takes the
  repeat's geometry everywhere it reads geometry. To settle it, run the
  verification several times with and without `DARK_RECORD_REPEAT=0` and
  compare the views by their `stay=` counts.
- `build_native\claude-stage\_to_delete` (24 MB) holds old scratch files of
  the assistant; safe to delete. `build_native\pgo-old-profraw` holds earlier
  raw profiles (`before-repeats\`: the three behind the 14:49 build); safe to
  delete.
- `build_native\claude-queue\experiment.cmd` holds only comments; it applies
  to every play run, so check it before timing anything.
- `build_native\run` holds 56 entries (18 GB): the runs `RENDERING.md` cites.
- The Fulton, Pier and profile-collection runs of the final afternoon were
  deleted after checking them (exit 0, no drops, no mismatches).
- Nothing was committed. A sensible split, if committing: (a) recordings and
  the script commands (`test_input_parser.h`, `native_main.cpp`,
  `tools\recordings`), (b) changes 1-3 and 6 (passes), (c) changes 4-5,
  (d) change 7, (e) change 8, (f) changes 9 and 10, each with its part
  of `RENDERING.md`. The tree
  also contains unrelated work in progress; review `git status` first.

---------------------------------------------------------------------------

## 11. Files touched, backups, helper scripts

### Source files of this work

| File | Changes |
|---|---|
| `renderer\engine\render_trace.cpp` | hooks: 82276148 (1), 82868FE8 (2, 3, 8 observe), 82299EA0 (4, 8), 8259D098/8259CA10 (4) |
| `renderer\engine\simple_mesh.cpp` | fog state (2, 6), prompt contexts (4), frame pins and queue (7), fog emitter (8), repeats, anchors, audit and probe (9), counters |
| `renderer\engine\simple_mesh.h` | `ColorImage` base and alignment, `SimpleMesh::retained`, `previewPinEpochFrames`, `WorldGeometry` (moved here), `SimpleMesh::repeat` (9) |
| `renderer\engine\engine_vertex_program.cpp` | preparation counter `engineVertexProgramPrepares()` (9) |
| `renderer\engine\screen_fog.h` | new: fog math, reference, element chain reader declaration |
| `renderer\engine\unowned_reference.h` | new: `unownedReference`, `ownerOf` |
| `renderer\engine\stored_geometry.h/.cpp` | pins in `drawFast`, `takePins`, `pinnedNow`, `drawReferencesOwned` |
| `renderer\engine\world_mesh.h/.cpp` | `WorldGeometry::pinEpoch`, `keepWorldDraw`, fog functions, `screenFogQueuedElements`, `sameCapturedWorldDraw(..., stateOnly)` (9) |
| `renderer\engine\native_layer.h` | declarations for the fog absorb and emitter interface, `engineVertexProgramPrepares` |
| `renderer\engine\subtitle_font.cpp` | `ownerOf` |
| `renderer\d3d11\world_renderer.cpp/.h` | fog pass and verification (2), memoized lookups (5), `ownerOf` at cache misses (7), `draw(draw, repeat)` (9), `drawRepeat`, `shortPathDraw_`, check mode, `indexBoundsValid` (10) |
| `renderer\d3d11\descriptor_cache.h`, `engine_preview.cpp` | (5); `engine_preview.cpp` passes `SimpleMesh::repeat` (9) and `continues` (10) |
| `renderer\d3d11\texture_mods.cpp` | `ownerOf` |
| `app\windows\cpu_sampler.h` | who called into system code on the display thread |
| `app\windows\test_input_parser.h`, `native_main.cpp` | recordings (`wait`, `quit`, `dev ...`) |
| `tests\stored_geometry_tests.cpp` | `pinContracts`, `queuedElementContracts` |
| `tests\world_renderer_tests.cpp` | repeat short-path case in `deadNormalContract` (10) |
| `tests\engine_mesh_tests.cpp`, `tests\world_*_capture_tests.h`, `tests\world_source_recovery_tests.h` | adapted to unowned references; `testRepeatComparison` (9) |
| `tools\recordings\*.txt` | the recordings |
| `build_native\claude-play.cmd`, `build_native\claude-queue\*` | run scripts, runner, tools |

### Backups

`build_native\claude-queue\backup-2026-11-10\before-references-and-fog\`
holds the 14 source files as they were immediately before changes 7 and 8
(with changes 1-6 in them). File names are the paths with `\` replaced by
`_`. Restoring them, deleting `unowned_reference.h`, and reverting the test
edits gives the state of the morning's final build. The two
`RENDERING.before_*.md` files next to them are `RENDERING.md` before the
last two sections.

`...\before-record-repeats\` holds the 9 files change 9 edits as they were
after the probe was added and before the repeats themselves (the probe is
diagnostic and default-off): `simple_mesh.h/.cpp`, `world_mesh.h/.cpp`,
`native_layer.h`, `engine_vertex_program.cpp`, `world_renderer.cpp/.h`,
`engine_preview.cpp`. `tests\engine_mesh_tests.cpp` only gained
`testRepeatComparison`.

`...\before-short-path\` holds `world_renderer.h/.cpp` and
`engine_preview.cpp` as they were with change 9 and before change 10.

There is no backup of the state before changes 1-6 other than `git diff`
against HEAD, which also contains the unrelated work in the tree.

### Helper scripts

`build_native\claude-queue\tools\`: `views.py`, `verify.py` (section 2.3).

`build_native\claude-queue\tools\analysis\` (copied from the assistant's
scratch space; written for Linux paths, adjust paths to use them):

- `prof.py`: sums CPU sampler output over a log line range.
- `checkrun.sh`: one-screen health summary of a run (bash).
- `stores.py <function>`: lists stores a guest function makes, transitively;
  `readers.py <offsets...>`: who reads given offsets; `ctx.py`: context
  accesses of functions. They parse `build_native\generated`.
- `patchlib.py`, `pins*.py`, `fog*.py`, `docs1.py`, `probe*.py`,
  `repeat*.py`, `short*.py`: the exact edits of changes 7-10 as scripts. They assert on the old text, so they document
  the edits; they cannot be re-run on the current tree.
- `route*.py`, `ab*.py`: older analysis of village route runs.
- `diffshots.py <runA> <runB>`: per-capture difference between two runs
  (mean, max, share of pixels off by more than 2 and 8).

---------------------------------------------------------------------------

## 12. Glossary

- Engine thread: the thread running recompiled game code and the capture.
- Display thread: the thread issuing Direct3D 11 calls.
- Capture: turning the game's device state at a draw into a `WorldDraw`.
- Part: up to 512 queued commands handed to the display thread at once.
- Record, element: the game's queued draw group and its members (section 7).
- Run (fog): the consecutive fog draws of one pass that one held reference
  draw stands for.
- Absorbed: a fog draw covered by the held run; not captured, not drawn.
- Late twin: a fog draw whose geometry wrote depth after the prepass; it is
  always drawn per object.
- Repeat, anchor: a queued command that shares an earlier command's
  `WorldDraw` with its own geometry; the full capture it shares (change 9).
- Clean element: one for which 82249488 has nothing to apply.
- Pin: an owner kept so that unowned references to its object stay valid.
- Pin epoch: 8 frames; the unit in which pins are collected and released.
- PGO: profile-guided optimization; the build uses counts collected from
  instrumented runs of three recordings.
- Same executable comparison: one build, a switch on and off, same session.
