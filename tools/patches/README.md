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

`tools/build.ps1` runs `tools/setup_generator.py` before compiling. Setup accepts
the original submodule layout, skips patches already applied, and refuses to
overwrite conflicting local edits. The Python setup script also accepts
`--destination` for validating a separate clean generator checkout.

When changing the generator, update this patch from the pinned base along with
the repository changes that need it. Validate setup and AOT generation using a
fresh checkout so local dependency edits cannot silently become a requirement.
