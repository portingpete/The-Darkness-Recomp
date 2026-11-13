# Issue 121: bloom atlas fringe

The native 1080p mode exposes an edge-sampling error in the HDR bloom pass.
The guest buffer is 960x540, and the engine draws its eight-times-smaller bloom
image into a 120x67 rectangle. Its normalized UV extent still reaches 67.5 rows.
The Gaussian's maximum UV permits a bilinear fetch into row 67, outside the
written rows 0..66. That row can still contain bright geometry from the full
scene sharing the target. The Gaussian center fetch also bypasses its offset
tap clamps.

The correction recognizes the opaque HDR atlas Gaussian pass from its sampler,
verified integer quad, UVs and original clamp constants. It bounds the center
coordinate and offset taps to the last valid texel center, 66.5/540. The original
kernel, bias, radiance, arm markings, world geometry and assets remain in use.
The original 1280x720 case has an integral eighth and retains its previous path.
Geometry repeats recheck the bounds in native modes with nonintegral eighths.
The live engine places its postprocess plane close to the far clipping boundary.
The atlas bounds check accepts this uniform-depth plane while retaining the
existing strict depth proof for copy optimizations. It requires identical Z/W
operands and no XY contribution to clip Z; hardware clipping therefore keeps or
rejects the whole quad. The projection and clipping state remain unchanged.

## Regression evidence

A hardware GPU case with an active gray value of 0.25 beside bright scene pixels
failed against the unchanged renderer: maximum error **0.749996**, reaching 1.0.
This isolates sampling outside the active rectangle without using game assets.
The final regression also includes a spatially varying interior light, an
independent CPU bilinear/Gaussian oracle, the 1280x720 control, and cached geometry
transitions in both directions. It runs at native scales 1, 2 and 3.
Both orthographic and the live engine's numeric perspective projection are tested.
The final maximum RGB error was 0.000738 on hardware and 0.000601 on WARP.

The candidate passed all six focused hardware and WARP checks in **14.97 seconds**:

- WorldBloomResolutionContract and WorldBloomResolutionWarpContract.
- WorldPostCopyFilterContract and WorldPostCopyFilterWarpContract.
- DarknessArmBloomContract and DarknessArmBloomWarpContract.

`DARK_REPEAT_SHORT_PATH` was unset, leaving the geometry shortcut enabled for
the regression. The native build verified 94 AOT files with zero semantic
diagnostics. Existing PGO profile-discard warnings and an unrelated indentation
warning remained.

## Gameplay evidence

Terra Ultra ran the same Pier19 camera-sweep script on clean main and the
candidate, with fresh isolated user data, bloom on, 1920x1080 rendering, 60 FPS,
vsync off and FOV 108. The camera probe confirmed the FOV setting. Both processes
loaded System32 D3D11/DXGI. Mission loading and the power unlock were setup; no
original assets were changed.

The baseline reproduced large orange lower-edge flashes at **99.833 seconds**,
following an earlier independent witness at 99.867 seconds. The candidate kept
the bright head markings and world lighting without the detached bottom patch.

- [Baseline clip, source seconds 99..101](before.mp4).
- [Candidate clip, source seconds 100..102](after.mp4).
- [Baseline full frame at 99.833 seconds](before.png).
- [Candidate full frame at 100.583 seconds](after.png).

These are real-time excerpts from separate replays. Their camera/animation poses
are similar, not identical; the candidate still was selected by upper-image
similarity to the baseline. Neither clip is retimed. Window recording is
1920x1060 at 60 FPS; the render buffer is 1920x1080. Capture overhead is unmeasured,
and the recording includes duplicate frames rather than measuring physical scanout.

The candidate's final full-frame inspection recorded the correction on both HDR
Gaussian passes: source 960x540, active 120x67, maximum y changed from 67/540 to
66.5/540, including the center fetch and uniform-depth admission. A color screen
checked all **1,022 recorded frames** in source intervals 97..106 and 112..120,
finding no orange patch candidates in the bottom 160 rows. This threshold is a
candidate finder, not a guarantee against every faint or differently colored
artifact; its exact conditions are in proof.json. The matching stills and clips
provide visual evidence for the reproduced bright orange flash.

Both game and recorder exited normally. Before/after membership, size and
SHA256 checks confirmed real settings and all three saves were unchanged.
This completed comparison used orange markings. Green markings have now been
acquired by cycling forward twice; that separate comparison is in progress.
The correction operates on the common HDR bloom pass.

[Machine-readable identities and results](proof.json) identify the frozen builds.
Original-derived command, geometry, texture and GPU dumps remain private and are
excluded from this evidence directory. Real saves and settings are isolated from
the test runs.
