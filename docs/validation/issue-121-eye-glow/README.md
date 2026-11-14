# Issue 121: layered eye glow at the camera edge

PR [168](https://github.com/portingpete/The-Darkness-Recomp/pull/168) fixed
sampling outside the native 1080p bloom atlas. The remaining sharp green sheets
are a separate visual problem. This candidate softens the recognized Darkness
eye volumes near the camera and at grazing viewing angles.

This is an intentional adjustment to the original effect. Inspection did not
establish a missing original angle fade or corrupted eye geometry. The earlier
[detached green comb](../issue-121/remaining-green-event.png) remains the specific
closure target; a different pose with a visible eye does not prove it fixed.

## Change

The eye material uses seven surfaces, each with 600 indices and 130 vertices.
The classifier verifies the repeated index topology, eight-weight
`XRUtil_RenderSurface` flags 8 shader, additive ONE/ONE blend and reversed depth.
It does not rely on a transient texture ID.

For a proved perspective projection `clipZ = a*clipW + b`, the physical near
margin is recovered from native depth as `a/(depth+a-1)-1`. RGB fades smoothly
from zero at the near plane to its original value at twice that distance.

An orthogonal world projection also supplies the camera ray from the actual
native viewport, including its origin and rendering scale. Reciprocal depth is
affine across a triangle; its screen derivatives recover the triangle normal.
The additional RGB gain is zero at view-normal cosines of 0.5 or less and
unchanged at 0.75 or greater, with a smooth transition. Depth-biased or nonorthogonal projections
retain the near-only adjustment. Degenerate angle calculations retain angle
gain 1. The original alpha comparison and alpha output are preserved.

The angle adjustment is confined to the camera's vicinity: its original gain
returns smoothly between 1.5 and 2.5 near-plane distances. At greater distances,
the original eye glow remains unchanged at every viewing angle.

Opaque head shading and the existing bloom pass retain their existing paths.
Repeated geometry must retain the same eye classification before it can reuse
the bound state. Temporary event-capture code is absent from this candidate.

## Renderer checks

Self-authored geometry, textures and independent geometric oracles cover:

- Near distance and tilted planes, with finite and infinite far projections.
- Native scales 1, 2 and 3, and unchanged additive alpha.
- Unrelated topology, orthographic projection and ordinary depth-range controls.
- Scale-relative oblique rejection and viewport origin/scale calculations.
- Geometry continuations changing eye classification in both directions.

All eight focused D3D11 debug checks passed on hardware and WARP in 17.74 seconds
with a valid warm shader cache (the changed shader has a new source key):
eye glow, bloom resolution, Darkness arm bloom and final capture. The AOT
manifest check verified 94 files with zero semantic diagnostics. The original
shader assets and raw GPU/geometry captures are not included here.

Candidate executable SHA256:
`4a73a4a3681b5fb51f563b1d9a2a7fe6ce2469702b7836a60b7d642253268460`.
Native input fingerprint:
`2e73f69d75d1454da265d9069265573111aa39887fda3e67ab82e3a8e091a546`.
All 395 source input hashes matched the frozen manifest after the build.

## Gameplay acceptance

**Partial improvement; issue 121 remains open.** A close-up can still expose
layers on the head. The older standalone comb has not received a reliable,
matched before-and-after validation, so this is not a full-fix claim.

Terra tested the frozen candidate in Pier19 with green heads, bloom enabled,
motion blur and AA off, FOV 108, native 1920x1080 rendering and a 60fps limit:

- Eight rapid turns using the earlier input tape and diagnostic launch settings.
  The WGC recording has 7,956 frames at 60fps, or 132.6 seconds.
- Thirty-two rapid turns with diagnostics and preview readbacks off, followed by
  a five-second settled pause. The WGC recording has 10,841 frames at 60fps,
  or 180.683 seconds.

Both games and recorders exited 0. Both runs loaded the system D3D11/DXGI
libraries without a proxy or RenderDoc. Before/after audits found identical
membership, sizes and SHA256 for the real settings file and three save files.
The test settings, saves and application caches were isolated.

The media below are full frames and original-speed two-second clips:

- [Baseline layers](before-layers.png), video 125.033s, frame 7502 from the
  matched-launch baseline, and its [124–126s clip](before-turn.mp4).
- [Candidate edge view](candidate-edge.png), video 122.933s, frame 7376 from the
  eight-turn candidate, and its [122–124s clip](candidate-turn.mp4). This is a
  similar world view with a different head pose, not an identical animation frame.
- [Normal setup glow](normal-setup.png), a direct native setup capture converted
  losslessly from BMP to RGB PNG, and [settled ordinary play](normal-settled.png),
  video 178.683s, frame 10721. Green markings remain visible on both heads.
- [Remaining visible layers](remaining-layers.png), video 157.167s, frame 9430 of
  ordinary play, and its [156.5–158.5s clip](remaining-layers.mp4). The stepped
  shapes overlap a visible head; their continued visibility is included here
  rather than presented as a clean result.

The baseline executable was SHA256
`cc754112bb87aa896929ae9c951bcf7d21504c030e925cc3a5c5227ba652a426`.
The same original-condition input tape and launch settings were used for the
baseline and eight-turn final candidate, with fresh isolated data each time.
Those two WGC streams are both 1920x1080. The much earlier standalone-comb
recording was 1920x1060, which further limits an exact pose comparison.

A CPU color finder helped select frames; its thresholds do not prove absence.
WGC, preview readbacks and diagnostics add unmeasured observer overhead. CFR
frame timestamps are not physical scanout measurements. Clips were cut and
re-encoded without scaling or speed changes; PNG video frames preserve the
decoded RGB values. Raw shaders, geometry and GPU captures remain private.
