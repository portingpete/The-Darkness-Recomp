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

`xenonrecomp-register-locals.patch` follows the corrections. It lets
`non_volatile_local_ranges` keep non-volatile registers (r14-r31, f14-f31,
v14-v31, v64-v127) in C++ locals for functions in the listed address ranges,
with `non_volatile_context_functions` as exceptions. Context-mode functions keep
their save/restore helper calls; local-mode functions elide them but still store
the return address at -8(r1) and reload r12 and LR from it, as the GPR helpers
do. The elided helpers no longer fill the other save slots, so guest code that
reads uninitialized stack sees different stale words than before. A
`[[register_sync]]` table names a `bl` whose callee (or its native override)
reads the caller's non-volatile registers from `PPCContext`: those locals are
copied into the context for the call and the previous values restored after it.
A sync address that is not a recompiled `bl` is a semantic diagnostic. Output
for configurations that use none of these options is unchanged.

`xenonrecomp-non-argument-locals.patch` follows the register locals. Functions in
`non_argument_local_ranges` that also keep non-volatile registers in locals keep
r0, r2, r11, r12, f0 and v32-v63 in C++ locals too, with
`non_argument_context_functions` as exceptions. Those registers carry no values
across a call under the Xbox ABI except into the save/restore helpers, which
local-mode functions elide. A `[[register_sync]]` entry may name a call `target`
instead of an `address`; the listed locals are then published for every `bl` to
that function, such as a stack probe that takes the frame size in r12. A target
with no recompiled `bl` caller is a semantic diagnostic.

`xenonrecomp-stack-accesses.patch` follows the non-argument locals. With
`unchecked_stack_accesses`, displacement loads and stores whose base register is
r1 use `PPC_STACK_LOAD_*` and `PPC_STACK_STORE_*`. The stack is ordinary guest
memory, so these skip the null-page and MMIO checks of `PPC_LOAD_*` and
`PPC_STORE_*`; MMIO stores marked by `eieio` keep `PPC_MM_STORE_*`. Output without
the option is unchanged.

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
