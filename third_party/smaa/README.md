# SMAA upstream files

Source: https://github.com/iryoku/smaa

Pinned revision: `71c806a838bdd7d517df19192a20f0c61b3ca29d`.

`SMAA.hlsl`, `Textures/AreaTex.h` (stored here as `AreaTex.h`),
`Textures/SearchTex.h` (stored here as `SearchTex.h`), and `LICENSE.txt` were
downloaded unchanged from that revision. Copyright notices and the upstream
MIT license are retained. These are public algorithm and lookup assets, not
assets from The Darkness.

`SMAAEmbedded.h` wraps the exact `SMAA.hlsl` text in a C++ raw string so the
renderer can compile the official shader through an in-memory include. It
requires no runtime shader files. When updating the pinned shader, regenerate
the header using `R"SMAA_UPSTREAM(` and `)SMAA_UPSTREAM"` for ASCII byte ranges
in the `kSmaaHlsl` declaration. Preserve non-ASCII comment bytes with adjacent
`"\xHH"` literals instead of decoding them with replacement characters.

The integration uses SMAA 1x, the upstream `SMAA_PRESET_HIGH`, color edge
detection, blending-weight calculation with the official area/search textures,
and neighborhood blending. All passes use the owned scene image's dimensions,
before output fitting and brightness. Input uses the existing display encoded
image after the Xbox gamma LUT; filtering retains that encoded color space
(the upstream documented non-sRGB path). Its floating-point output avoids an
extra 8-bit quantization step for high-precision images. It has no temporal
history and never filters a previously filtered image.
