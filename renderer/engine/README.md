Current status (2026-09-09): The native Windows AOT build renders the original first-level car scene and police chase through engine-level D3D11. Reversed face culling, stage0 depth-offset coordinates and the missing NDS lighting shader are fixed. Offline user0 saves are implemented portably under `gameDirectory.parent_path()/saves` (`00000001_<filename>` content dirs; normal `DEFAULT` holds `_profile`, `Chapter1`, `Checkpoint`) with the title owning file formats and original assets read-only. Prompts default to keyboard/mouse artwork and follow the active input source (see `CONTROLS.md`). Final verification (`build_native/framerate-stability-20260909/FINAL-VERIFICATION.md`) passed all 47 Release tests; the final muted 300-second replay captured 14,158,804 world draws with zero rejections, queue drops or audio errors. Actual unedited late-scene BMPs were reviewed. See `build_native/run/codex-world-rendering-checkpoint-20260908.md` and its separate verification receipt. Full campaign play, in-game checkpoint Resume, exact platform fidelity and visible 60 FPS remain unverified; all presents report occluded. All launches stay muted. Earlier implementation notes below document previous milestones.

# Native rendering performance update

The current implementation and launch/diagnostic options are documented in
[`RENDERING.md`](../../RENDERING.md). The muted native launcher now targets
60 FPS and reports accepted engine frames separately from repeated presents.
Texture selection and completed sampler/fragment state are retained, resources
and unchanged D3D11 bindings are cached, and a late engine frame wakes the
consumer directly. Full-level 120 FPS and exact console fidelity must be judged
from the current measured checkpoint, not the configured frame cap.

The sections below are historical implementation milestones; their older
limitations and frame-delivery descriptions do not all describe the current build.

# Experimental native engine renderer

The opt-in native preview draws the original game's copyright text and intro
video through D3D11. It copies original engine CPU vertex/index arrays,
projection, vertex colors, font alpha pixels and decoded video planes.
It does not interpret Xbox GPU command packets.
All wrapped AOT functions still execute exactly once with their guest ABI.

Status: 2026-09-07. Color-image support, owned stored geometry, packed-attribute
decoding and basic engine transform constants are also implemented and tested.
General world materials, skinning, render targets and controllable gameplay
remain unfinished. A matched stored draw is not a rendered native world mesh.

From the workspace root:

```powershell
python tools/run_native.py --timeout-ms 60000 --engine-preview --trace-renderer
```

The launcher saves a timestamped log, JSON result and the first nonempty native
GPU frame as a BMP under `build_native/run`. It also captures the 30th and 90th
distinct video frames presented by the native renderer, with original frame
timestamps in the log. Diagnostic deadline exit **5**
is expected; it does not mean gameplay succeeded. The launcher reports a
nonzero status for that deadline. Without `--engine-preview`, the experimental
bridge is disabled.

## Supported contract

- Original triangle-list entry `8225DBC8`, including its original recursion
  through batched requests. CPU descriptor layout is the one consumed by
  `8225CDD8`: float3 positions, float2 UV0, one D3DCOLOR stream and BE16 indices.
- Identity model-view, original row-major projection, 1280x720 full viewport,
  the observed fixed-function font material, one texture and alpha blending.
- Original linear CImage format `0x40000`, one byte per pixel, with the row
  pitch and allocation-alignment rules from `82788B60`. Only the base mip is
  uploaded. Linear clamp sampling is a preview choice; full sampler/mip parity
  has not been established.
- Frame handoff observed at `82241A68`. The window thread owns D3D11 drawing.
  Complete CPU frames are handed over through a bounded queue; when the game
  outruns the display, only the most recent complete frame is retained.
- Original `CMWnd_ModTexture_PaintVideo_YUV2RGB` material with two video
  textures, full viewport, identity model-view and ONE/ZERO blending. The
  original decoder continues through AOT and supplies linear Y and BE A8L8
  chroma images. Host R8G8 upload preserves raw V,U bytes; the native shader
  swizzles them into logical U,V. Conversion coefficients and zero output
  alpha come from the original ARB fragment source under
  `Darkness/System/Gl/ARB_fragment_program`. Only the observed white uniform
  color state is accepted when no vertex-color stream exists.
- Original opaque no-program color material and BC1/BC3 CImage pixels, with
  validated block/header/allocation extents and cropped edges. Passing GPU
  fixtures do not establish observed in-game color draws; the latest live
  boot still accepted zero color meshes.
- Original packed-index expansion is observed at its proven immediate caller.
  List/strip/fan traversal stays in AOT and the produced index count is bounded.

Unsupported materials, stored vertex buffers, other stream layouts, transforms,
viewports and image formats are counted and skipped by this experimental
preview. Unsupported passes leave a black background. This is not a complete
frame renderer: general menu imagery, world rendering and controllable
gameplay remain unfinished. Exact decoder/frame-timing parity is not established.

Guest memory reads use ReadProcessMemory with range/extent validation so
invalid or decommitted data rejects a preview submission. Host copies own
their pixel/geometry storage. CPU alpha caching is bounded at 32 MiB, color
at 64 MiB, queued unique color data at 64 MiB and GPU color at 64 MiB/256
entries. Caches are invalidated before an observed engine texture rebuild;
unobserved resource lifetime paths still need reconstruction.
Video caching retains at most four selected frame copies, with at most
32 MiB of video data referenced per queued frame. Only owned plane copies
cross into the window thread. The GPU retains one cached video upload pair.

`EngineMeshContract` tests endian conversion, the original AOT stream-size
helper, image/video pitch and alignment, invalid memory and actual indexed
D3D11 drawing with alpha blending, video chroma ordering, the original YUV
conversion formula, projection and GPU readback. These are host contract
tests, not evidence of playable gameplay or full hardware parity.

## Owned stored geometry and attributes

`stored_geometry.h/.cpp` capture the original converted interleaved vertices,
formats, scale/offset constants and decoded BE16 indices. Publication waits
for completed original preparation and matching validity/bindings. Replacement
generations reject stale completions. Matching checks the actual bound index
buffer and requested index-element range, including separate VB/IB subsets.
Immutable draws survive resource replacement and eviction. The cache is capped
at 64 MiB/256 resources.

CPU attribute decoding uses the original converter's float1..4, unsigned
u16x1..4, two 11:11:10 and three byte formats. Slot 9 is NORMAL, not color.
Scale/offset is applied only to the original first five slots. Unknown formats,
invalid extents and nonfinite data fail without changing output. This CPU
interpretation does not establish vertex-shader or skinning semantics.

`StoredGeometryContract` compares original conversion/copy/index wrapper ABI
and packed-attribute values, and checks cache ownership, lifetime and ranges.

## Native engine transform constants

`engine_transforms.h/.cpp` reconstruct the eight-vector output of original
routine `82248A78` from owned model-view/projection data. Vectors 0..3 contain
the transposed projection product with model row 3 replaced by `(0,0,0,1)`;
vectors 4..6 retain model columns including translation; vector 7 contains the
original dot/column-length-squared quotients with w cleared. This operation is
not a general inverse or the conventional full model-view product.

Native arithmetic matches the current AOT's sequential SIMDe dot reduction
and two separately rounded reciprocal refinements. It restores the caller's
host FP state and rejects nonfinite data/results and zero-length columns.
Bit-exact agreement with the current AOT is tested; Xbox hardware rounding
parity is not claimed.

Actual stored draw observations now carry optional owned transform snapshots.
Source pointers/data are rechecked. Comparison with the original engine's CPU
constant output distinguishes equal, different and unavailable; no GPU packets
are interpreted. Snapshots do not enable unsupported materials or submit stored
world meshes. Vertex-stage meaning, texture transforms, palettes and render
targets remain to be connected.

`EngineTransformContract` verifies **32,768 float bit patterns over 1,024 matrix
cases**, ownership, unchanged guest memory/host FP state, invalid data and
comparison classification. The transform milestone's Release suite passed **24/24**. Its boot
`boot-20260907-172549-928475` matched **37,152/37,152** stored draw transform
snapshots with zero differences; that is a data contract, not rendered gameplay.

## Native texture-stage constants

`engine_texture_constants.h/.cpp` reconstruct original `8224A2E8` as owned
data for eight engine texture stages. Optional matrices are transposed; modes
0..22 select zero to ten parameter vectors per stage, with exact original
defaults and mode-1 component masks. Parameter words retain their original
bits because their complete shader semantics are still unknown. The original
descriptor references are retained for comparison, not used as a GPU register
file or as native shader bindings. A zero descriptor count preserves the
original early-out behavior. The maximum bounded output is 112 vectors.

`EngineTextureContract` passes 1,730 original-AOT cases with 91,436 bit-exact
constant words, including every mode at every stage, all mode-1 masks,
defaults, matrices, mixed stages and the maximum output. Another 92 cases
compare the wrapper's complete PPC register context, guest fixture, stack
and host FP state against the original. Invalid bounds/data, source changes,
owned storage and comparison classification are checked. The texture milestone's
full Release CTest passed **25/25**.

The strong wrapper observes only the proven caller LR `822490D4` and runs
the original exactly once. It compares actual returned count, descriptor
writes, output cursor and all constant words. Sampled `texture_constants`
trace events and runtime counters distinguish exact, different, unavailable
and empty observations. This preparation contract does not yet attach a
complete material to native world draws; vertex programs, skinning and
render-target lifecycle still need implementation.

## Native vertex descriptors and conversion constants

`engine_vertex_descriptor.h/.cpp` reconstruct the typed 80-byte descriptor
defaults from `8224DC70`, the 27 mode-table entries initialized by `8223AFE8`,
descriptor updates from `8224DAE0`, and packed conversion scale/offset pairs
from `8224A0A8`. All unrelated descriptor fields and raw constant bits are
preserved. Mode 0 falls back to 4 when its coordinate is disabled. Material
bit `0x8000` contributes descriptor flag `0x00800000`. Optional matrices
reserve four vectors each. Reservation is distinct from the later texture
routine's actual output: original mode 19 reserves one vector and writes two.

Conversion data consists of dense original scale/offset pairs for position
and selected coordinate slots. The original repeated write to all eight
conversion references is preserved, including its coordinate-mapping test;
it is not replaced with an inferred per-destination remapping. These numeric
references are engine metadata for comparison, not a GPU register emulator.

`EngineVertexDescriptorContract` checks 4,480 original-AOT descriptor cases,
2,048 conversion cases spanning all 512 masks, 73,728 bit-exact conversion
words and 116 complete wrapper ABI cases. It verifies original initialization,
opaque field preservation, owned snapshots, address bounds, unchanged host
FP state and mismatch classification. This descriptor milestone passed **26/26**.
Observation accepts only the proven callers `82248E98` and `822490B8`, checks
the original initialized tables, and keeps original execution exactly once.

## Matrix palette preparation

`engine_palette.h/.cpp` reconstructs original `8224AB18`: up to52 matrices,
contiguous or selected by original BE16 indices, with three transposed columns
per matrix and descriptor reference96 even for an empty palette. Output is
owned and bounded to156 vectors. Selected nonfinite/subnormal components are
unsupported; normal values and signed zero are copied bit-exactly without FP
arithmetic. The unused fourth column is ignored. This does not yet establish
bone weights or vertex shader semantics.

Preview observations at caller82249110 compare original CPU output, cursor,
descriptor and returned count. They preserve original execution exactly once,
guest ABI and host FP state. `EnginePaletteContract` passes134 original-AOT
cases,50,544 bit-exact words and268 complete wrapper ABI cases, including all
loop tails, count clamping, indexed/contiguous selection and source ownership.
That milestone passed **27/27**. Palette runtime counters are
separate from bounded trace samples; no palette binary sampling was added.

## Vertex program selection

`engine_vertex_program.h/.cpp` reconstructs the six-word cache key assembled
by original `82248C80`: declaration identity, packed coordinate mapping, mode
bytes, declaration flags and matrix/palette/conversion flags. It preserves
the original high coordinate-source bits instead of truncating every byte to
three bits. The native cache lookup follows the original tagged tree links
and unsigned ordering, including hits, misses and alternate binding storage.
Owned snapshots reject unreadable, cyclic or changing source data.

`EngineVertexProgramContract` verifies3,139 original-AOT cases,18,834 exact key
words and139 complete wrapper ABI cases. Full Release CTest passes **28/28**.
The wrapper executes the original exactly once and compares its emitted key
and selected cache record. A bounded32-key trace records source context,
original keys and visited cache nodes inside the existing constants budget.
These are verified selection inputs; native vertex shader compilation and
world draws are still unfinished.

The original general vertex-program template is `Darkness/System/Gl/VP.xrg`;
the matching Xenon variable definitions and arithmetic helpers are in
`Darkness/System/Xenon/VertexProgram/VPDefines_HLSL.xrg` and
`Darkness/System/Xenon/VPInclude_Xenon.xrg`. Use these original sources when
implementing variant bodies, rather than inferring weight/normal semantics
from copied constant words alone.

## Read-only tracing

Add `--trace-renderer` to save sampled engine state under a new `.render`
directory. Captures are bounded at 16 MiB and include JSONL metadata and raw
snapshots. They never modify guest registers, memory or original assets.
Half the byte budget is reserved for stored geometry evidence. Sampled stored
draws include decoded position/UV/normal, the full selected 656-byte matrix
payload, and native/original transform constants when available. Checkpoint
hashing must traverse all files in each trace directory.
Texture-stage preparation retains up to 32 distinct populated states and
sparse empty-call samples. A separate 512 KiB reservation protects its source
matrices, parameters, descriptor and output evidence from earlier image dumps.
Vertex-descriptor and conversion observations share this constants reservation
and retain their own bounded samples, including source tables and original
output bytes. The inspection script reports each comparison category separately.
`render_trace.cpp` contains both the optional trace wrappers and the bridge
that observes supported meshes/images when preview mode is enabled.

The verified first-frame milestone, exact build/test/boot logs and follow-up
addresses are recorded in
`build_native/run/codex-engine-renderer-checkpoint-20260907.md` and the later
`build_native/run/codex-video-renderer-checkpoint-20260907.md`. The current
checkpoint is `build_native/run/codex-bindings-checkpoint-20260907.md`.
The checkpoint writer creates timestamped patches/manifests without replacing
older evidence.

The build-time original vertex-template translator and `EngineVertexShaderD3D11`
compile 144 explicitly selected variants (0..8 weights, position/UV conversion,
UV matrix, vertex/constant color). Hardware and WARP stream-output tests cover
110,592 float values. These shaders are not connected to live draws: retail
key/condition mapping, complete bindings and material/target semantics still
need proof. See the current checkpoint for limits. Native `--mute` silences the
mastering voice while processing continues; the desktop GUI launcher uses it.

Completed vertex descriptors are retained per thread after verified82248C80.
Stored draws now carry owned final bindings from device+1920; the native shader
import copies only consumed registers and leaves unused registers zero. The
retail key-to-condition mapping is still required before live submission.
Captured c8.w is765.00030517578125; do not hardcode the template comment's510.


## Current world pass continuation — 2026-09-07

Native stored geometry now submits depth and motion passes. Hardware and WARP
verify skinning/basis outputs and actual depth/stencil/motion pixels. The muted
80-second boot215216 submitted1,824 of each pass; later pipeline samples show
rasterized triangles and motion PS invocations. Lighting remains unsubmitted.
Exact target formats/resolves/composition and controllable world gameplay are
still unfinished. Keep the full goal active. Read
`build_native/run/codex-world-checkpoint-20260907.md` for evidence and next actions.


## Current lighting continuation - 2026-09-07

Original NDSP lighting now submits with diffuse/specular/DXN-normal and six-face
luminance cube maps. The missing stencil128 clear and original immediate shadow
volumes are queued. Muted75-second boot223800 submitted4,464 depth/stencil,1,488
motion and1,488 lighting draws. Readbacks of lighting draws64/512 changed13,827/
24,158 RGB pixels with zero nonfinite channels. All34 CTest checks pass. These
are offscreen passes: exact formats/lifetimes/resolves/postprocessing/display
composition and controllable gameplay remain unfinished. Keep the full goal
active. Current evidence and next actions:
`build_native/run/codex-lighting-checkpoint-20260907.md`.


## Native composition continuation — 2026-09-07

The renderer now queues engine resolves at82865FD0 and publishes the frame at
the actual82867620 present call, including the final frontbuffer resolve. The
old82241A68 frame boundary remains only for pre-world text/video. Owned immediate
geometry includes float UV streams and packed vertex color. The original
postprocess programs and their flags select28 pinned shader variants (including
the two fixed fragment programs). Resolves preserve cropped rectangles and
destination offsets, original -4/+4 HDR conversion/sample exponents, and cube
faces selected by r9. Uncompressed XRGB/ARGB uploads now retain row pitch and
original BGR byte order. Every wrapper still calls its original once.

The original viewport is explicitly initialized to depth range(1,0) at82246F20.
Native raster shaders now apply that depth transform after the unchanged
original vertex program. This corrects visibility and prevents the background
fade from covering the model. Hardware and WARP tests cover reversed depth,
cropped resolves, HDR scaling and cube-face isolation. The original3D title
scene is visible in native backbuffer captures. This is not gameplay proof.

Current limits include logical RGBA16F/D24S8 surfaces (not bit-exact Xbox
10-bit-float/MSAA), a bounded shader/material subset, and incomplete native
storage/profile integration. The active user goal still requires an actual
in-game screenshot; do not mark it complete from a title-screen capture.
Opt-in --test-start, --test-skip-intros and --test-input exercise the native
keyboard adapter without sending input to other processes. Normal launches
keep user input unchanged. Tests and launches remain muted.


Native level-renderer continuation (2026-09-08)
Shared physical A/C/E memory views corrected first-level loading; the6252 original geometry resources (37,721,688 bytes) now remain cached without eviction. The original no-save route reaches NY1_Tunnel using local-player sign-in and explicit cancelled storage selection. All runs stay muted; save support remains unfinished.
Original CPU tiling oracles verify streamed RGBA8/BC1/BC3/DXN and RGBA cube ownership.73 fragment variants from20 unchanged pinned assets now include LF, anisotropic NDSEATP, environment mapping and fog. Stage-specific vertex modes9/13/17/18 were matched to original ProgramCache.xpc; runtime remains engine-level D3D11 with native AOT PPC, with no Xbox shader-bytecode interpreter. Hardware/WARP tests check72 vertex variants plus covered pixels and compiled texture-binding requirements.
The latest immutable checkpoint and actual visual review are documented in build_native/run/codex-compose-checkpoint-20260907.md and codex-compose-visual-review-20260907.md. World rendering remains incomplete; title/menu/tutorial text alone does not satisfy the active in-game-screenshot goal. Small packed texture bases, compressed cubes, exact sampler/mip and surface lifetimes remain limited. Intro video after the physical-map change is not reverified (recent auto-skip runs recorded0 native video images).
