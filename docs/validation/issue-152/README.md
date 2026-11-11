# Light flicker reduction: real-room verification

The optional **Reduce flicker** setting holds animated light intensity steady.
**Off** preserves the original animation; **On** reduces flicker. It is available
in **Options > Video Settings** and as **Reduce light flicker** in the launcher.
It defaults to Off, applies immediately, and persists as `[Display] ReduceLightFlicker`.

6.1 Terra Ultra tested the original construction-site interior after the crash
tutorial, facing the stacked crates beside the overhead fluorescent, brick wall
and duct. The mission was `NY1_Consite:ny1+layer1`, with the player near
`(-2184.31, -1423.22, -255.65)`. Original enabled periodic Light2 objects were
confirmed by read-only inspection, including object 49 at `(-2224, -1424, -176)`.
No light flags or authored lighting were changed to create this test.

[Watch the Off / On / Off comparison](off-on-off-comparison.mp4). The three panels
show separate ten-second excerpts from the same session at the same viewpoint.
The left and right panels use Off; the middle uses On. The full source captures
lasted approximately 11–12 seconds per state.

Brightness standard deviation in a fixed region on the crate was **7.295 Off →
0.0822 On → 7.300 Off**, a **98.9% reduction** while enabled. Flicker returned
when the setting was disabled. These are encoded RGB luma measurements on a
0–255 scale, rather than physical luminance measurements. The camera position
changed by only 0.00012 world units; minor framing and idle differences remain.

The test used a separate user-data directory for settings, saves and caches.
The executable and original game content were unchanged during the comparison.
Process inspection used read-only access; no process memory was written.
No original game binaries, extracted data or decompiled code are included here.

Automated coverage exercises all five animation modes through the original
guest color evaluator, compares the default output with the original waveform,
checks live restoration, disabled lights, color fades and source-state
preservation, and verifies settings persistence and both menu layouts.

This gameplay evidence covers this room on native Windows. It does not establish
that every flashing effect is suppressed, or validate a Proton release. Detailed
identities, measurements and limits are in [proof.json](proof.json).
