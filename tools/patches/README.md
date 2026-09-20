# Darkness generator patches

`xenonrecomp-darkness.patch` applies to hedge-dev/XenonRecomp commit
`c5bfd90d87f2ed0db8cff5c19ea3aff0e161e527`, the XenonRecomp revision referenced
by UnleashedRecomp `5e8695a157ce9d2a783944d63439cc8c76a38fc2`.

The patch carries the generator changes used by the Darkness development build:
additional PowerPC/VMX instruction translations, reachable-instruction and switch
recovery, explicit translation failures, diagnostic and translation-unit counts,
the guest timebase hook, and MSVC/C++20 build support. It contains no game binaries
or generated game code. The upstream tool and these modifications are GPLv3;
see the repository's `COPYING` and the upstream license.

`xenonrecomp-corrections.patch` follows the initial patch. It fixes unsigned
float conversion, overlapping vector packing, halfword comparison flags, fused
negative multiply-add, unsigned multiply-high record flags, and lost analysis
diagnostics. Reciprocal-square-root and negative-multiply-add record forms fail the semantic gate until
their FPSCR-to-CR1 behavior is implemented.

`tools/build.ps1` runs `tools/setup_generator.py` before compiling. Setup builds
the patch series in a temporary directory and recognizes the pinned original and
every shipped intermediate version. It upgrades old installs, preserves unchanged
timestamps, and validates all affected files before replacing any of them. Unknown
local edits are preserved and reported. The Python setup script also accepts
`--destination` for validating a separate clean generator checkout.

When changing the generator after a release, append a patch to `PATCHES` in
`tools/setup_generator.py` so existing installs remain recognizable. Validate the
instruction regressions, setup upgrades, and AOT generation using a fresh checkout
so local dependency edits cannot silently become a requirement.
