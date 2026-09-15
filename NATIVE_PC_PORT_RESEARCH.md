# The Darkness (2007) — native PC port research dossier

## Current implementation status — 2026-09-08

The native Windows AOT build renders the original first-level car scene and police chase through engine-level D3D11. Reversed face culling, stage0 depth-offset coordinates and the missing NDS lighting shader are fixed. The latest muted300-second run captured1,127,099 world draws with zero rejected snapshots, renderer errors or queue budget drops. All34 Release tests pass, including hardware/WARP pixel and vertex checks. Actual unedited late-scene BMPs were reviewed. See `build_native/run/codex-world-rendering-checkpoint-20260908.md` and its separate verification receipt. Full-game gameplay, saves and exact platform fidelity remain unverified. All launches stay muted. The earlier statuses below are historical.

## Historical implementation status — 2026-09-07

The September 3 research below is historical. A native Windows AOT executable,
Windows platform services, audio and input bridges, and an opt-in engine-level
D3D11 preview now exist in this workspace. Original copyright text and intro
video have rendered. The user has heard game audio. Owned stored geometry,
packed CPU attributes, model/projection constants and texture-stage preparation
constants, vertex descriptors, conversion pairs and matrix palette preparation
plus vertex-program keys/cache selection have original-AOT contract tests.
The original-template native vertex stage now passes144 explicit variants on
hardware and WARP (110,592 float outputs). The full suite passes31/31. Final original vertex descriptors and draw-time
constant banks now reach the native upload interface with47,112 bit-exact
original-AOT upload checks. Live variant selection remains unfinished.

Live program-key/condition mapping, shader binding, world materials and
render-target handling still need implementation, and controllable gameplay remains unverified. Matched
CPU data is not evidence of native world-mesh rendering. The architecture
still uses offline PPC-to-C++ AOT and native services without a runtime PPC
JIT or Xbox GPU packet renderer. See the current evidence and limitations in
`build_native/run/codex-bindings-checkpoint-20260907.md` and
`renderer/engine/README.md`. Historical statements below about absent runtime
or platform code describe the September 3 inventory, not today's workspace.

## Historical research — 2026-09-03

Research date: 2026-09-03  
Target examined: Xbox 360 world retail build, Title ID `545407EE` / `TT-2030`  
Goal: a native Windows port with a newly implemented PC renderer—no Xenia renderer, no ReXGlue/Xenos packet executor, and no runtime PPC JIT.

## Executive verdict

A native PC port is technically possible, and this particular dump is a considerably better candidate than a typical stripped Xbox 360 game. It contains a decrypted retail executable, unusually complete function metadata, a large relocation directory, readable C++ identifiers, loose cross-platform shaders, explicit desktop OpenGL material, self-describing resource paths, standard media formats, and evidence that the engine family had working PC renderers before and after The Darkness.

It is not an automated conversion. The game currently has no native runtime, no Windows platform layer, and no surviving Darkness PC executable in this workspace. The retail XEX contains the Xenon implementations of display, system, input, audio, and Xbox services. A complete port requires substantial reverse engineering and new code in every one of those areas.

The recommended implementation is a **clean-room hybrid native port**:

1. Ahead-of-time translate the PowerPC game/engine code to C++ and compile it into a normal x64/ARM64 executable.
2. Preserve the 32-bit guest address space and big-endian data contract during bring-up.
3. Replace the Xbox kernel and XAM imports with a deliberately small native Windows compatibility layer.
4. Hook the engine's high-level `CXR_*` rendering contract and implement a new D3D11 reference renderer, later optionally D3D12/Vulkan.
5. Translate or rewrite the shipped desktop ARB shaders offline; use Xenos shaders only to fill verified gaps, never through a runtime GPU emulator.
6. Replace sound, video, input, storage, profiles, and networking with native libraries and APIs.
7. Progressively replace opaque recompiled subsystems with readable native C++ where doing so reduces risk.

That product would be a native host executable with native platform and graphics APIs. It would not contain Xenia or execute an Xbox GPU command stream. During early development it would retain a guest-style register/memory ABI around AOT-translated functions. If “100% native” is intended to forbid even that ABI, the only routes are an authorized restoration of Starbreeze's original source or a much larger source-level clean-room reimplementation. Static recompilation is then useful as a behavioral oracle and transition scaffold, not the final architecture.

## What “native” should mean for this project

The word is overloaded, so the acceptance criteria should be explicit.

### Required shipping properties

- A normal Windows executable compiled ahead of time for the host CPU.
- No Xenia core, no runtime PPC decoder/JIT, and no Xbox system software.
- No guest GPU ring buffer, PM4 packet parser, Xenos register file, EDRAM command emulator, or runtime shader-microcode interpreter.
- A renderer that receives engine-level meshes, materials, lights, constants, render targets, and draw requests, then issues native PC API calls.
- Native input, audio, video, files, saves, timing, threading, and window management.
- An installer that verifies and derives copyrighted game data from a user's legitimate copy rather than redistributing it.
- Test use of real hardware or Xenia is permitted as an external reference oracle; neither is part of the shipped runtime.

### Two possible finish lines

**Practical native port.** The original PPC code is translated offline to C++ and compiled for x64. A small `PPCContext` and a big-endian 32-bit memory model remain inside the executable, while all host-facing systems are native. This is the most realistic no-emulator route.

**Strict source-native reimplementation.** Every meaningful subsystem is reconstructed as ordinary typed C++ with native pointers, data structures, exceptions, and calling conventions. This produces the cleanest result, but without official source it is a multi-year clean-room engine rewrite. It should be treated as a gradual endpoint rather than the first boot strategy.

## Exact local build inventory

The game folder contains 503 files totaling 7,295,179,155 bytes (6.794 GiB). The complete workspace contains 2,232 files totaling 7,330,372,884 bytes. No Darkness-specific C/C++ project, build system, runtime, or generated recompilation project existed when this research began.

The major game directories are:

- `Content`: 321 files / 6,798,983,926 bytes.
- Five localized content directories for English/French/German/Italian/Spanish support, roughly 10–12 MiB each.
- `ExtraContent`: 7 files / 363,697,408 bytes.
- `System`: 130 loose configuration and shader files / 1,309,809 bytes.

### Retail executable identity

Both XEX files are 11,120,640 bytes.

`Darkness/default.xex`:

- MD5 `28863923CC6446A848240D9106F13191`
- SHA-1 `8D91A85989E8567E51633364767AD5E521E53BC2`
- SHA-256 `AACE35A8F9BCDC7F28AEAB9FF8CF3BDF200353F5C83705F6284487347ACB3C5F`
- Retail, encrypted, basic-compressed XEX2 module.

`Darkness/_uncrypted.xex`:

- MD5 `2265D87E591AA48B7A54F3BDC75C71FD`
- SHA-1 `3C9E6D0997E91F7DD6F93F037E8695F6AF656A71`
- SHA-256 `A7CCD87860889FA082D62DCC24382467D59C8031F6E99D2ED9F3DBDDAA7D535E`
- Retail, unencrypted, basic-compressed XEX2 module.

Verified module metadata:

- Original image name `Exe_Main_Xenon.exe`.
- Title ID `545407EE`; execution-info media ID `0F213645`.
- Full 16-byte XGD2 media identifier `0F F0 5C 18 71 FE DB 86 09 17 3B 48 0F 21 36 45`.
- All-region, XGD2 original-disc media, disc 1 of 1.
- Load/image base `0x82000000`; entry point `0x828AA3E8`.
- Image-security size `0xB10000`; stack `0x40000`.
- XEX timestamp 2007-05-28 22:53:41 UTC.
- Embedded build strings: `May 29 2007`, `00:45:08`, `RTM`.

The original CodeView identity survived:

- PDB GUID `2f53d9fd-dfa7-4179-947a-e1937926f4e8`
- PDB age `1`
- Original path `s:\Source\P5\Projects\Main\Exe_Xenon\output\xbox 360\exe_main_xenon\rtm\Exe_Main_Xenon.pdb`

The exact GUID/age is the best archival search key for an authorized symbol recovery effort. The workspace also contains split IDA artifacts (`.id0`, `.id1`, `.nam`, `.til`) totaling 53,551,176 bytes. They show that prior static analysis was started, but there is no packed `.i64`, and the amount of useful naming still needs to be assessed in IDA.

### Embedded PowerPC PE

The unencrypted XEX contains a PE image beginning at file offset `0x3000`:

- Machine type `0x01F2`, the Xbox 360 big-endian PowerPC target.
- 17 sections.
- Main `.text` virtual size `0x906644`, plus eight executable embedded sections.
- Dedicated `.pdata`, `.data`, `.XBMOVIE`, `.tls`, `.idata`, `.XBLD`, and `.reloc` sections.
- Relocation directory size `0x777E4`.
- Exception/runtime-function directory at RVA `0x9F800`, size `0x1EB50`.

The `.pdata` directory contains **15,722 verified runtime-function records**. Starts are strictly increasing, from `0x820C0000` to `0x82A16A00`. The records account for about 9,277,460 function bytes; the median encoded function size is 248 bytes. Thirty-nine records carry the exception flag. This is an unusually strong initial function map, although leaf functions, thunks, handwritten assembly, and split functions still require control-flow discovery.

## Static recompilation experiment on this exact XEX

For this investigation, the current upstream [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) was built outside the workspace and run against `_uncrypted.xex`. This did not alter any game files.

The result is encouraging but very clearly pre-runtime:

- `XenonAnalyse` accepted the XEX and emitted a 285,625-byte TOML file containing **675 detected switch tables**.
- The save/restore helper patterns required by XenonRecomp were found at all eight expected addresses.
- XenonRecomp traversed the full image, reached 100%, and emitted 132 C++ files totaling 179,471,719 bytes in about five seconds. The output contains 32,636 generated function implementations, including functions discovered outside the 15,722 `.pdata` records.
- The generated code is not yet semantically usable as a game: the run reported 7,604 unsupported-instruction occurrences at 7,603 unique addresses across 40 unique mnemonics.
- The most common gaps were `vslh` (2,354), `frsqrte` (1,544), `vsrah` (979), `vsubshs` (901), and `vspltish` (602). Other gaps include VMX/VMX128 packing, comparison, saturation, reciprocal-square-root, integer, cache, and condition-register variants.
- It also reported 2,435 out-of-function switch-target lines. Those collapse to 75 switch sites, 407 unique destinations, and 412 unique site/target pairs, showing a function-boundary problem rather than 2,435 independent tables. The other 600 detected switch tables produced no such diagnostic.
- Thirty comparison instructions had record-bit behavior for which no condition result was generated.

This test proves that the current toolchain can parse and traverse the build structure. It does **not** prove that the output boots—or even that every generated function preserves its input semantics. Inspection of this XenonRecomp revision found two correctness hazards that must be converted into hard build failures:

- The process returns success even though the whole-image loop discards the per-function recompile result.
- In this output, an unsupported instruction is retained only as a disassembly comment, with no guest operation or guaranteed trap. A bad out-of-function switch target is emitted as a comment followed by `return;`. Both can therefore compile while silently changing behavior.

XenonRecomp's documentation also says generic jump-table recovery is unsolved, MMIO is unimplemented, exceptions are unsupported, and the tool supplies no runtime. Those limitations line up exactly with the observed results. The project must parse the log/output and enforce its own correctness gate rather than trusting the process exit code.

The first CPU milestone is therefore measurable:

1. Classify every unsupported opcode by address range and owning subsystem. The top five mnemonic families account for about 84% of occurrences, but heuristic-only ranges must be checked for embedded data or exception metadata before implementing or skipping them.
2. Implement the instruction variants that are reached by game/engine code.
3. Native-replace codec, media, and platform routines where doing so removes entire clusters of specialized VMX/MMIO code.
4. Repair the 75 problematic switch sites with analyzer changes or explicit function bounds.
5. Handle or hook the 39 exception-bearing functions before enabling link-register optimizations.
6. Compile all generated translation units only after code generation reports zero unsupported semantics, zero record-form comparison gaps, zero outside-function cases, and zero decoder failures in verified code ranges.
7. Run deterministic instruction-level differential tests against known PPC results before trying to boot the game.

### Why AOT translation is appropriate—but not magic

XenonRecomp directly converts PPC instructions to C++ while passing a PPC register context and a base-memory pointer to each function. Loads/stores perform big-endian conversion, vector lanes account for Xenon's ordering, indirect calls use a function-address dispatch structure, and output hooks can replace whole functions or individual instruction sites. These are exactly the mechanisms needed for a bring-up scaffold.

The output is deliberately low-level and not human decompilation. A long-lived project should isolate it as generated code, never hand-edit it, and put readable native replacements in separately reviewed modules. The generation config, analyzer patches, explicit boundaries, and hook manifest should be treated as source.

## Xbox import and runtime surface

Only two dynamically imported Xbox modules appear in the XEX:

- `xboxkrnl.exe`: 144 IAT slots, 134 code thunks, and 10 apparent data imports.
- `xam.xex`: 90 IAT slots and 90 code thunks.

That is 234 IAT symbols and 224 code/thunk entries. The surface is finite and tractable, but it spans kernel memory, threads, synchronization, files, virtual memory, time, exceptions, input, profiles, storage, achievements, networking, voice, content, and media. Implement only functions proven reachable, but give every unimplemented ordinal a loud diagnostic trap; silent stubs create nondeterministic failures.

Statically linked Xbox SDK libraries identify the likely clusters inside the translated image: `XONLINE`, `XHV`, `LIBCMT`, `XAPILIB`, `XGRAPHC`, `XAUD`, `XMP`, `XMEDIA`, `XBOXKRNL`, and `D3D9LTCG`, all reported as version `2.0.5632.0`. This is useful because specialized media or graphics routines can often be removed at a high-level boundary rather than faithfully reproducing their low-level instructions.

### Native runtime responsibilities

The runtime should be divided into auditable services:

- **Guest address space:** reserve a stable region representing the 32-bit Xbox address space. Keep all original pointers as 32-bit guest addresses until a subsystem is intentionally ported.
- **Endian-safe memory:** typed big-endian load/store wrappers; explicit alignment checks; correct atomic reservation semantics (`lwarx`/`stwcx.` and 64-bit variants); barriers; executable/data region validation.
- **Function dispatch:** map original PPC function addresses and vtable entries to AOT-compiled host functions without a runtime instruction decoder.
- **TLS and thread state:** reproduce Xenon TLS layout, thread-local PPC context, stack guarantees, priorities where observable, events, semaphores, critical sections, and condition behavior.
- **Time:** one monotonic clock with documented conversions for Xbox performance counters, system time, sleep, and frame pacing.
- **VFS:** mount original logical paths over an installation directory; normalize slash/case behavior; expose save, cache, and content namespaces without pretending the host filesystem is FATX.
- **Exceptions:** either model the small reached subset, patch those call paths to native code, or prove they are unreachable. Do not globally skip link-register state while exception-bearing code remains.
- **Diagnostics:** address-aware traces, import call counters, assertions on invalid guest pointers, and a symbol map that associates host frames with original PPC addresses.

## The strongest renderer evidence is already on the disc

The native-renderer case does not rest on wishful thinking or a later emulator. The shipped `Darkness/System/Gl` tree contains:

- 104 `!!ARBfp1.0` desktop OpenGL fragment programs.
- One shared `.fph` include and four `.xrg` generator/definition files.
- [`VP.xrg`](Darkness/System/Gl/VP.xrg), a “General vertex program pipeline” credited to Magnus Högdahl/Starbreeze, whose source explicitly says it is required by GL/D3D rendering contexts and emits `!!ARBvp1.0` / `NV_vertex_program2` programs.
- [`XRShader_DeferredMRT.fp`](Darkness/System/Gl/ARB_fragment_program/XRShader_DeferredMRT.fp), an ARB fragment shader using multiple render targets and identifying `CXR_Shader::RenderShading_FP20`.
- Riddick-era ScreenFX shaders whose comments identify *Chronicles of Riddick: Escape from Butcher Bay* post-processing work.
- At least seven PC-specific conditional branches in loose fragment programs.

The Xenon shader tree separately contains five `.fp` sources, two `.dym` shaders, four `.xrg` files, and `ProgramCache.xpc`. The cache contains 442 `vs_3_0` and 322 `ps_3_0` programs with Xbox compiler `2.0.5632.0` markers.

This material is not a complete PC renderer; the executable strings expose `CDisplayContextXenon` and `CSystemXenon`, not compiled Win32/GL classes. It does, however, preserve a substantial amount of the engine's cross-platform shading intent and naming vocabulary. That changes the renderer task from “infer every Xenos draw from packets” to “reconstruct the engine-facing render contract and port authored shader semantics.”

### Historical engine lineage

Take-Two described The Darkness as using Starbreeze's internally developed next-generation engine. Technical director Jerk Gustafsson explicitly called it “an evolved version of the Riddick engine” and described dynamic shadowed lights, light-field mapping, hybrid lighting, and retained per-light destruction. Lead designer Jens Andersson described semi-deferred rendering, HDR, rigid-body physics, cloth, per-pixel lighting, normal mapping, and dynamic shadows. Sources: [Take-Two announcement](https://ir.take2games.com/static-files/a487fd18-9aca-435b-9e1a-0a00c5333db0), [developer interview](https://www.xboxgazette.com/interview_the_darkness_en.php), and [NAG Xbox Insider interview](https://www.nag.co.za/wp-content/archives/2006/000NAG%20Xbox%20Insider%20December%202006.pdf).

An archived official Starbreeze page says its common engine supported PC, Xbox, PlayStation 2, and GameCube and included in-house level, script, material, animation, and build tools. That predates The Darkness but establishes a cross-platform architecture: [archived Starbreeze engine page](https://web.archive.org/web/20050204054558/http://www.starbreeze.com/engine.jsp).

Contemporary interviews say The Darkness itself was running on specific PC development hardware even though the announced product focus was consoles: [Shacknews interview](https://www.shacknews.com/article/43012/the-darkness-interview). This supports archival outreach for a lost host-PC target; it does not establish that a complete, shippable PC SKU ever existed.

The lineage continued on Windows. *The Chronicles of Riddick: Assault on Dark Athena* shipped with closely matching `.fp`, `.xrg`, `.xtc`, `.xmd`, `.xwc`, and `.xw` vocabulary. A later Starbreeze technical interview says *Syndicate* still used the Riddick/Darkness in-house technology, added a new DX9 renderer, and retained OpenGL internally even though the release was DX9-only: [PC Games Hardware interview](https://www.pcgameshardware.de/Syndicate-Spiel-44115/Specials/Syndicate-im-exklusiven-Technik-Interview-Von-Gesichts-Scans-in-3D-Vorteilen-fuer-PC-Spieler-und-dem-voruebergehenden-Ende-von-Open-GL-864537/).

These later games are useful black-box behavioral references if legally owned. Their binaries and renderer DLLs are not drop-in components and must not be copied or linked into this project.

## Native renderer design

### Boundary to hook

The executable contains high-level identifiers such as `CXR_Engine`, `CXR_EngineImpl`, `CXR_Shader`, render targets, pre-render events, materials, and texture/container classes. The renderer should intercept that layer—or the closest stable display-context vtable below it—not the Xbox D3D command submission layer.

The hook manifest should eventually cover:

- Display/window creation and swap/present.
- Texture, vertex buffer, index buffer, and render-target creation/destruction.
- Vertex declarations and stream bindings.
- Shader selection, shader constants, and material permutation keys.
- Blend, depth/stencil, rasterizer, alpha-test, sampler, viewport, scissor, and color-write state.
- Clear, draw, instanced draw, resolve/copy, and render-target transitions.
- Occlusion/query behavior only where the game actually observes it.
- Readback/screenshot paths and video-to-texture uploads.

The shipping renderer must not expose Xenos packets. Where the original engine has an EDRAM-specific resolve, reproduce the **semantic result** with PC render targets and copies. Where tiling or endian conversion is a resource-layout issue, convert at upload time and keep the GPU resource in native PC layout thereafter.

### API choice

A D3D11 renderer is the best first reference backend. It maps the 2007 engine's stateful GL/D3D9-era model directly, handles DXT/BC textures and multiple render targets, makes RenderDoc bring-up straightforward, and minimizes descriptor/PSO machinery while semantics are still changing. “Native” does not require D3D12.

After visual parity is stable, a D3D12 or Vulkan backend can share the engine-facing abstraction. Starting with D3D12 is defensible for a well-staffed team, but it combines renderer archaeology with resource-state, descriptor, and pipeline-cache engineering and therefore raises first-pixel risk.

### Shader strategy

1. Parse the small XRG include/conditional system and enumerate every concrete permutation used by the game.
2. Translate the on-disc ARB vertex/fragment programs offline to readable HLSL. The relevant instruction and MRT semantics are publicly specified by Khronos: [ARB fragment program](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_fragment_program.txt), [ARB draw buffers](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_draw_buffers.txt), and [NV vertex program 2 option](https://registry.khronos.org/OpenGL/extensions/NV/NV_vertex_program2_option.txt).
3. Preserve constant registers, swizzles, saturates, precision assumptions, texture coordinates, fog/depth conventions, alpha-test behavior, and render-target write masks exactly in the reference implementation.
4. Hash shader/permutation inputs and cache compiled native shaders and pipeline/state objects.
5. For a pass with no adequate PC source, use [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) as an **offline analysis/conversion tool**, then review and commit the resulting HLSL as a native asset. Do not ship a runtime Xenos translator.
6. Build scene captures comparing every pass against Xbox hardware or an external emulator oracle: G-buffer channels, shadow maps, light accumulation, transparency, post-processing, UI, video, and final tone mapping.

XenosRecomp is title-oriented and incomplete; its documented gaps include dynamic indexing, control flow, constants, endian/swizzle cases, fetch variants, instancing, exports, and bindings. It is a useful assistant, not a renderer architecture.

### Renderer proof-of-concept sequence

- Compile every shipped GL shader variant and produce a coverage report.
- Translate `VP.xrg` plus one untextured fragment program; draw a test triangle.
- Decode and upload one DXT texture from XTC/XT payloads.
- Render a native fullscreen/UI pass using an original shader.
- Intercept one engine-created texture and one vertex/index buffer.
- Reconstruct one `XMD` surface and draw it with an engine material.
- Reproduce the deferred MRT layout and one light pass.
- Bring up one room with camera, opaque geometry, lighting, shadows, and post.
- Only then expand to particles, skinning, cloth, transparencies, decals, video, and edge cases.

## Assets and file formats

The data is not opaque. It is still a major reverse-engineering workstream.

Important extension totals include:

- 218 `.xdf` packages / 2,975,719,701 bytes.
- 8 `.xwc` wave containers / 1,919,559,593 bytes.
- 8 `.xt0` / 740,381,080 bytes; 8 `.xt1` / 104,415,186 bytes; 7 `.xtc` / 359,427,596 bytes.
- 1 `.xac` / 203,871,452 bytes.
- 60 `.ogg` / 761,044,250 bytes.
- 9 `.wmv` / 102,435,962 bytes.
- 5 `.xcd` / 44,045,616 bytes.
- 29 `.xcr` / 6,849,371 bytes.

Fifty standalone files use the `MOS DATAFILE2.0` container. The 29 XCR files contain both `XCR_LE` and `XCR_BE` streams, which gives direct little-/big-endian serialization pairs for registry and UI data.

### XDF findings

All 218 XDF packages begin with `01 01 00 00`. Their verified front matter contains a version word, byte length for a null-separated logical path table, the paths, a resource count, index/metadata, and compressed payloads. A test zlib decompression produced a valid `MOS DATAFILE2.0` XCR resource.

The path tables contain 13,669 dependency occurrences and 1,512 unique logical resource names:

- 833 `.xmd` models.
- 498 `.xsa` animations.
- 50 `.xw` worlds.
- 40 `.xah` animation graphs.
- 35 `.rul` package/map rules.
- 14 `.xrg`, 11 `.xtx`, 8 `.xcr`, 7 `.xwc`, 5 `.xfc`, 3 `.xtc`, one `.xsu`, and one `.cfg`.

There are 35 principal maps: 27 campaign and 8 multiplayer maps. Treat the counts as path-table dependency names, not necessarily serialized object counts.

### Native asset-pipeline plan

- Write read-only parsers first and emit a machine-readable manifest containing offsets, sizes, checksums, dependency edges, endian, compression, and source package.
- Fuzz parsers against every shipped file and reject malformed bounds rather than trusting disc data.
- Use the original recompiled loader as a second oracle: capture decoded structures immediately before renderer submission and compare them with native parser results.
- Convert Xenos-tiled/split texture payloads to linear BC/DXT resources at install or upload time. Keep provenance and mip layout in a cache manifest.
- Do not require wholesale asset conversion for the first boot. The original loader can populate guest memory while hooks copy finalized resources into native renderer objects.
- Over time, replace XDF/MOS/XTC/XMD/XW loaders with typed native versions. Each replacement should pass byte- or structure-level comparison tests against the original loader.

[Dragon UnPACKer](https://github.com/elbereth/DragonUnPACKer) contains MPL-2.0 Starbreeze container/texture research, including Riddick/Dark Athena and experimental Darkness support. [vgmstream's XWC parser](https://github.com/vgmstream/vgmstream/blob/master/src/meta/xwc.c) documents Darkness header version 3 and Xbox XMA versus PC Vorbis variants. These are useful permissive implementations and format references; neither solves the complete world/model/animation pipeline.

## Audio, video, input, and services

### Audio

The executable identifies `CSoundContext_Xenon3`, `CSoundContext_XMA`, `CMSound_Codec_XMA`, and `CWaveContainer_XWC2`. The native path should hook above XMA hardware/MMIO:

- Parse XWC metadata and names natively.
- Decode XMA1/XMA2 using a legally distributable decoder, or transcode to a cache during installation.
- Feed PCM into a native mixer such as FAudio, miniaudio, SDL audio, or a project-specific WASAPI/XAudio2 layer.
- Reconstruct buses, 3D attenuation, occlusion, reverb, voice limits, streaming, and synchronization from observed engine calls.
- Preserve sample-accurate timing for cutscenes and scripted dialogue.

Implementing Xbox XMA MMIO is explicitly the wrong boundary for this project.

### Video

The 60 `.ogg` files are video-only Theora streams totaling approximately 15.97 hours. The nine `.wmv` files are ASF/WMV3 with WMA2 or WMA Pro audio and range from 640×480 to 1280×720. Use a native decoder—libtheora/FFmpeg for Theora and Media Foundation or FFmpeg for WMV—and upload decoded frames as ordinary renderer textures.

### Input and windowing

- Start with SDL3 or a Win32 window plus GameInput/XInput/Raw Input.
- Preserve the original controller behavior, dead zones, aim curves, vibration timing, and glyph switching.
- Loose config contains mouse sensitivity settings, but that is not proof of a finished mouse path. Implement raw mouse input and retune carefully against camera/controller behavior.
- Make frame pacing, focus loss, DPI, fullscreen/windowed/borderless, and arbitrary aspect ratios native features rather than patches to Xbox presentation code.

### Saves, profiles, achievements, and networking

- Map profiles/content containers to a versioned per-user save directory with atomic writes and backups.
- Keep an adapter interface for achievements; provide local state first and platform integration only where authorized.
- Single-player should be the first completion target. Xbox Live voice, matchmaking, invites, presence, and multiplayer services are a separate program of work.
- A fully functional multiplayer restoration requires a new authenticated service or peer model, security review, version negotiation, NAT behavior, moderation expectations, and likely rights-holder decisions. Do not let it block campaign bring-up.

## Recommended project architecture

Suggested source boundaries:

```text
installer/             verifies owned media; builds native caches
tools/xex/             metadata, imports, pdata, symbols, patch manifest
tools/assets/          XDF/MOS/XTC/XMD/XW/XWC inspectors and converters
tools/shaders/         XRG preprocessing and offline ARB/Xenos conversion
generated/ppc/         reproducible XenonRecomp output; never hand-edited
runtime/guest/         address space, endian, atomics, TLS, function dispatch
runtime/kernel/        xboxkrnl import implementations
runtime/xam/           profile, input, content, storage, achievements, network
runtime/vfs/           logical mounts and save/cache namespaces
engine/hooks/          typed replacements at original function addresses
renderer/api/          engine-facing resources, state, passes, draw lists
renderer/d3d11/        first native reference backend
renderer/d3d12/        optional later backend
media/audio/           XWC, XMA decode/cache, mixer integration
media/video/           Theora/WMV decode and frame upload
app/windows/           process, window, crash handling, input, packaging
tests/                 instruction, parser, ABI, frame, replay, save tests
```

Generated code should be hermetic: one known XEX hash plus one config/analyzer revision must reproduce the same output. Hooks should be keyed by original virtual address and documented with signature confidence, call sites, side effects, and validation evidence.

## Development roadmap and exit gates

Calendar estimates are unreliable before the first room renders, so each phase has a concrete gate.

### Phase 0 — provenance and reproducibility

- Freeze supported disc/build hashes and catalog title updates separately.
- Record extraction provenance without storing keys or redistribution-sensitive material.
- Pack or export the existing IDA analysis, recover names, and create a shared address database.
- Make XEX/image reconstruction, import naming, `.pdata` parsing, switch analysis, and C++ generation one reproducible command.

**Gate:** a clean machine can regenerate identical metadata and generated C++ from the supported user-supplied XEX.

### Phase 1 — CPU correctness

- Patch or extend XenonRecomp for the reached scalar, VMX, and VMX128 instruction variants.
- Resolve the 75 switch-boundary sites and audit all 675 detected tables.
- Classify 39 exception functions and handle `setjmp`/`longjmp` if used.
- Build generated units under sanitizers where possible; create instruction and ABI tests.

**Gate:** all reachable translated instructions are implemented or intentionally hooked; no debug-break placeholders in a smoke-test call graph.

### Phase 2 — native bootstrap runtime

- Address space, thread/TLS, time, synchronization, heap/virtual memory, VFS, logging, and initial imports.
- Enter the original entry point and trace initialization deterministically.

**Gate:** process reaches engine initialization and exits cleanly with a useful import/function trace, without graphics.

### Phase 3 — asset and renderer reconnaissance

- Native manifests for XDF/MOS/XTC/XWC.
- Identify display-context and CXR vtables/functions through strings, RTTI, call graphs, and runtime traces.
- Compile the GL shader corpus and build a standalone material viewer.

**Gate:** one original model/material/texture is rendered by the new native backend outside the game.

### Phase 4 — first pixels in game

- Hook window/display creation, resources, state, draws, render targets, and present.
- Implement UI/fullscreen passes, then deferred opaque lighting.

**Gate:** menus render and one controlled room appears with correct camera, opaque geometry, and basic lighting—without executing Xenos packets.

### Phase 5 — first playable slice

- Input, audio, video, saves, animation/skinning, particles, transparency, shadows, post-processing, and scripted timing.

**Gate:** a chosen campaign slice is playable start to finish with deterministic save/load and frame captures within agreed tolerances.

### Phase 6 — campaign coverage

- Automated boot/load/playback matrix across all 27 campaign maps.
- Fix streaming, rare shaders, physics/cloth, AI timing, cutscenes, localization, and content variants.

**Gate:** new-game-to-credits completion on supported hardware with no emulator component and no converted assets distributed by the project.

### Phase 7 — productization

- Installer/updater, crash symbols, settings, accessibility, controller/mouse UI, performance, GPU vendor testing, corruption recovery, and license notices.
- Multiplayer only as a separately scoped deliverable.

**Gate:** repeatable install from owned media, long soak tests, save compatibility policy, and release audit.

## The first 90-day research/engineering program

For a small serious team, the highest-information work is:

1. Pack and audit the existing IDA database; import the exact PDB identity and recover RTTI/vtables/string cross-references.
2. Turn the one-off executable analysis into checked-in, hash-gated tools and manifests.
3. Classify all 40 missing instruction mnemonics by reached code region; implement a representative scalar and VMX test suite.
4. Repair a sample of the 75 switch sites and determine whether one analyzer rule fixes most of them.
5. Build a guest-memory/import harness capable of calling a leaf recompiled function with deterministic inputs.
6. Write an XDF manifest extractor and XTC/XWC inspection tools; validate across the entire corpus.
7. Preprocess and compile all desktop GL shader variants; document failures and engine defines.
8. Build a native D3D11 viewer that renders one original texture, one model, and one deferred MRT shader.
9. Identify and hook the display-context constructor/vtable in a minimal translated-process harness.
10. Produce a risk review and updated staffing estimate based on first-pixel evidence.

This program avoids spending a year on a generic Xbox runtime before proving the unique renderer and asset boundaries.

## Effort and staffing reality

The following are planning ranges, not promises:

- **Authorized source restoration:** roughly 4–10 experienced engineers for 1–3 years if final source, PC target, tools, dependencies, and third-party rights are recovered in buildable form. Archive condition can move this dramatically.
- **Hybrid AOT native port from retail media:** roughly 6–15 experienced contributors for 3–7 years to a polished single-player release. A focused first playable slice could arrive much earlier if the CXR boundary is clean.
- **Strict source-level clean-room rewrite:** plausibly 50–150+ engineer-years, especially if every world, animation, physics, AI, renderer, tool, and multiplayer behavior must become ordinary typed native code.
- **Solo effort:** feasible for research, tools, an asset viewer, and perhaps a first-room prototype; a fully polished, content-complete port is likely a many-year community project.

The largest uncertainty is not raw PPC translation. It is whether the renderer and platform boundaries can be cleanly intercepted before Xbox-specific behavior fans out through thousands of call sites. The local `CXR_*` vocabulary and shipped GL shader path are unusually positive evidence for that boundary.

## Risks that can stop or radically reshape the project

- **Rights and source access:** a public release without authorization must distribute only original project code and a user-side derivation process. Publisher/platform terms and anti-circumvention law vary by jurisdiction.
- **Leaked proprietary source:** a public mirror describes a 2006 Starbreeze source snapshot obtained from a private developer forum and explicitly says it was never open-sourced. It is pre-gold, PS3-oriented, incomplete, and legally unsuitable. Do not clone, study implementation details from, or incorporate it into a clean-room project.
- **Exception semantics:** 39 flagged runtime functions plus imported unwind/raise functions mean exceptions cannot simply be assumed absent.
- **Compiler/runtime code:** unsupported VMX clusters may belong to codecs or SDK libraries, but that must be proven. Replacing a subsystem is safer than blindly skipping instructions.
- **Function boundaries:** switch targets crossing `.pdata` boundaries may represent split/cold functions, compiler outlining, analyzer errors, or shared tails. Wrong bounds corrupt control flow silently.
- **Floating-point/vector fidelity:** reciprocal estimates, NaNs, denormals, saturation, record bits, and lane order can break physics, animation, and rendering in non-obvious ways.
- **Streaming/resource ownership:** native GPU/audio objects must track the original engine's lifetime and asynchronous loading without host-pointer leakage into guest structures.
- **Visual semantics:** semi-deferred lighting, light fields, shadow filtering, EDRAM resolves, gamma, HDR, and post effects require pass-level validation; a scene that merely draws is not parity.
- **Third-party components:** FaceFX/TalkBack evidence and any other licensed technology require replacement, licensed redistribution, or rights-holder source access.
- **Title updates and regional builds:** world, German, and Japanese executables have different Title IDs. Stabilize one exact world build first; do not mix addresses or assets.

## Legal and clean-room release posture

This is practical engineering guidance, not legal advice.

- Keep the project repository free of game executables, decrypted images, keys, original shaders/assets where redistribution is not authorized, and generated translations that may reproduce copyrighted code.
- Ship an installer/patcher that asks the user for a legitimate source, verifies a known hash, and derives required caches locally.
- Maintain a provenance log for every specification, symbol name, signature, and test. Separate behavioral observers/specification writers from implementers if a formal clean-room process is needed.
- Use permissively licensed tools/libraries deliberately. XenonRecomp and XenosRecomp are MIT; Xenia is BSD-3-Clause and should remain an external analysis oracle; Unleashed Recompiled is GPL-3.0, so copying its runtime code would impose GPL obligations.
- Do not rely on the unlicensed Starbreeze source leak.
- Seek permission and archives from 2K/Take-Two first for the game/IP and final production materials, then Starbreeze for engine/tool archives. A Starbreeze prospectus says the company did not own the IP in historical developer-only titles such as The Darkness: [Starbreeze prospectus](https://media.starbreeze.com/2018/03/Prospectus-Starbreeze-AB-publ-2018-03-22-english.pdf).

The ideal authorized request is specific: the final `P5` game source, `Exe_Main_Xenon.pdb` matching GUID/age above, any host-PC/Win32 target, GL/D3D renderer modules, Ogier/build tools, format documentation, final scripts, third-party dependency list, and build-environment images.

## Existing projects: what to reuse and what not to become

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp): use as the AOT CPU translator and extend it with tests. It is not a runtime.
- [XenosRecomp](https://github.com/hedge-dev/XenosRecomp): use offline for shader archaeology or missing-program conversion, never as a runtime Xenos renderer.
- [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp): study the architecture and the idea of replacing engine draw calls with a new renderer. Respect its GPL-3.0 license and do not copy code into a differently licensed project without accepting those terms.
- [Xenia](https://github.com/xenia-project/xenia): use its XEX/kernel documentation, ordinal knowledge, and external execution as research aids. Do not link it into the product or adopt its GPU command processor.
- ReXGlue-based projects: useful as behavioral references, but architecturally outside this project's acceptance criteria.
- The local Quake III source: unrelated to Starbreeze technology and not a foundation for the port.

## Research conclusions

1. **The CPU image is structurally tractable.** The exact XEX is accepted by the current static-analysis/recompilation tools and has 15,722 runtime-function records.
2. **The CPU implementation is not ready.** Forty missing mnemonics, exception handling, and 75 switch-boundary sites must be solved or bypassed with principled native replacements.
3. **A non-Xenia renderer is credible.** The retail disc contains a genuine desktop GL shader lineage and a high-level XR renderer vocabulary. Reconstruct that contract.
4. **Do not build a Xenos emulator by another name.** Offline shader conversion and texture untile are asset/tooling steps; runtime packet execution is not part of the design.
5. **Asset work is large but observable.** XDF paths, zlib payloads, MOS containers, dual-endian XCR data, standard codecs, and community parsers provide strong footholds.
6. **The practical target is hybrid native first, source-native over time.** It is the shortest path to execution while preserving a route toward readable replacements.
7. **Authorization would change the economics completely.** A surviving production-era PC target, final PDB, tools, or source could remove years of reverse engineering.

The right immediate objective is not “port the whole game.” It is a falsifiable vertical proof: translate enough CPU code to initialize, decode one real room, and render it through a new D3D11 backend using the shipped PC shader semantics. If that succeeds cleanly, the project has a viable native architecture. If it fails because Xenos behavior is deeply inlined above the display context, the result will reveal that before the team commits to years of runtime work.


## Deep Technical Addendum: Engine Reversing, Binary Formats & Native Pipeline Implementation

### 1. Architectural Analysis: Why REXGlue Fails the "100% Native" Standard and How DarkRecomp Succeeds

Porting Xbox 360 games via static recompilation has developed two competing paradigms:

1. **The Virtual GPU / Xenia-Backed Approach (REXGlue / Blue Dragon recompilation):**
   - **Mechanism:** The CPU PowerPC binary is translated ahead-of-time (AOT) to C++, but graphics calls are routed to an embedded fork of Xenia's GPU subsystem.
   - **Operation:** When the guest game issues draw commands, it writes Direct3D 9 / PM4 packets into an emulated command ring buffer. Xenia's backend parses these packets, emulates Xenos hardware registers, models EDRAM tiling, decodes guest microcode shaders dynamically at runtime, and runs guest interrupt emulation.
   - **Why this fails the user's requirement:**
     - **Performance Overhead:** Parsing guest ring buffers and translating microcode introduces CPU overhead and frame pacing spikes.
     - **Shader Compilation Stutter:** Microcode shaders translated on-the-fly cause persistent shader compilation stutter.
     - **VRAM Inefficiency:** Simulating the Xbox 360's unified memory and EDRAM layout wastes gigabytes of host VRAM for render target mirroring.
     - **Rigid Graphics Limits:** True PC features (arbitrary ultrawide FOV without stretching, native DLSS/FSR/XeSS upscaling, unlocked framerate pacing without breaking guest GPU timers, modern HDR10/scRGB output, and custom HLSL shader replacement) cannot be natively cleanly integrated into an emulated packet pipeline.

2. **The Direct API Hook Approach (Sonic Unleashed Recompiled):**
   - **Mechanism:** In the Xbox 360 SDK, DirectX 9 is not a system DLL (`d3d9.dll`); it is a static library (`d3d9ltcg.lib`, Microsoft Xbox 360 D3D9 Link-Time Code Generation) compiled directly into the game executable.
   - **Operation:** UnleashedRecomp reversed the statically linked D3D9 C++ API entry points inside the game executable (`IDirect3DDevice9::CreateDevice`, `DrawIndexedPrimitive`, `SetVertexShader`, `SetTexture`, `CreateVertexBuffer`, etc.). It intercepted these functions at the guest C++ boundary using `GUEST_FUNCTION_HOOK` macros and translated them into high-level modern render commands for its native backend (`plume` D3D12/Vulkan), with all Xenos shaders translated offline into standard HLSL.
   - **No Xenia GPU runtime is present.**

3. **The Starbreeze P5 Engine Native Port (The DarkRecomp Architecture):**
   - *The Darkness* offers an even higher-level and cleaner native interception surface than Sonic Unleashed.
   - Starbreeze's P5 engine was designed from the ground up as a cross-platform modular engine (*Chronicles of Riddick: Escape from Butcher Bay* ran on PC OpenGL and Xbox DirectX; *Assault on Dark Athena* and *Syndicate* ran on PC OpenGL/D3D9, PS3, and Xbox 360).
   - Rather than only hooking raw D3D9 device calls, P5 encapsulates rendering inside `CDisplayContext` and `CXR_Engine`:
     - **Primary Hook (`CDisplayContext`):** Starbreeze's display device interface consists of a 47-slot virtual function table (`0x82066648`). By implementing a native `CDisplayContextD3D11` or `CDisplayContextVK`, the entire Xbox 360 D3D9 layer, PM4 packets, and EDRAM resolve mechanics are completely bypassed.
     - **Secondary Hook (`D3D9LTCG`):** The static D3D9 library (`v2.0.5632.16384`) is also present in the binary (`0x829C6800 - 0x82A19750`), providing a direct fallback for any low-level UI or media rendering.
     - **Authored PC Shaders on Disc:** The retail disc contains 104 loose desktop OpenGL fragment programs (`!!ARBfp1.0`) in `Darkness/System/Gl/ARB_fragment_program/` and the complete vertex pipeline compiler in `Darkness/System/Gl/VP.xrg`. These can be translated offline to native HLSL without any runtime emulation.

---

### 2. Deep Reverse Engineering: Starbreeze P5 Class Registry & Vtables

Reverse engineering of `Darkness/basefile.exe` revealed that Starbreeze embedded a complete reflection and factory registry in `.data` (`0x82A20000 - 0x82AC2FA0`).

#### 2.1 The 460-Class Engine Reflection Table
Every engine class registers a 32-byte descriptor containing:
1. `name_ptr` (PPC virtual address pointing to class name string in `.rdata`)
2. `factory_ptr` (PPC virtual address pointing to heap allocation and constructor stub in `.text`)
3. `category_ptr` (PPC virtual address pointing to base class / category descriptor)

A dedicated tool (`tools/dump_engine_classes.py`) was constructed in this workspace and extracted all 460 classes to `tools/engine_classes.json`. Key architectural classes discovered include:
- **Rendering & Display:** `CDisplayContextXenon` (`0x822432d8`), `CDisplayContextNULL` (`0x827aa8b0`), `CXR_Engine` (`0x8277d5b0`), `CXR_VBContext` (`0x825f6f08`), `CXR_SurfaceContext` (`0x825a7608`), `CDebugRenderContainer` (`0x823ea910`).
- **Geometry & Models:** `CXR_Model_BSP4` (`0x825f1300`), `CXR_Model_BSP4Glass` (`0x825b1830`), `CXR_Model_TriangleMesh` (`0x825cb940`), `CXR_Model_Particles` (`0x824f1b68`), `CXR_Model_EffectSystem` (`0x824e1128`).
- **Textures & Media:** `CTexture` (`0x8279b8a0`), `CTextureContainer_VirtualXTC` (`0x8279b970`), `CTextureContainer_VirtualXTC2` (`0x827a4fc8`), `CTextureContainer_Video_Theora` (`0x827a22c8`), `CTextureContainer_Video_WMV` (`0x8279e7e0`).
- **Audio:** `CSoundContext_Xenon3` (`0x827b8478`), `CMSound_Codec_XMA` (`0x82793988`), `CMSound_Codec_VORB` (`0x827b91a8` — confirming Starbreeze built native Ogg Vorbis decoding right into the game engine).
- **Core Engine & System:** `CSystemXenon` (`0x827aa840`), `CInputContext_Xenon` (`0x827953c8`), `CGameContext` (`0x822be360`), `CRegistry` (`0x8279b800`).

#### 2.2 `CDisplayContext` Vtable Architecture
Disassembly of `CDisplayContextXenon` (`factory=0x822432d8`, constructor=`0x82241200`) and `CDisplayContextNULL` (`factory=0x827aa8b0`) reveals the virtual function table layout in `.rdata`:

- **Primary Vtable:** Address `0x82066648`, exactly 47 virtual functions.
  - `Slot [ 0]`: Virtual Destructor (`0x8223d0b8` in Xenon, `0x827a6820` in NULL).
  - `Slot [ 8]`: Display Initialization & Mode Setup (`0x82241f98` in Xenon; reads display cvars).
  - `Slot [15]`: Viewport / Scissor Configuration (`0x822417d0` in Xenon).
  - `Slot [16]`: Clear Render Targets & Depth (`0x82241808` in Xenon).
  - `Slot [17]`: Present / Buffer Swap (`0x822418c8` in Xenon).
  - `Slot [18]`: Texture Binding (`0x823a4ad0` in Xenon).
  - `Slot [31]`: Set Render State / Blending (`0x82241a68` in Xenon).
  - `Slot [44]`: Begin Scene (`0x82241938` in Xenon).
  - `Slot [45]`: End Scene (`0x82241940` in Xenon).
- **Secondary Vtable:** Address `0x82066704`, 13 virtual functions dedicated to texture environment and attribute passes (`MRenderXenon_Attrib_TexEnvMode00` through `04`).
- **Display Engine CVars (`0x820665ec`):**
  - `r_vsync`: Vertical synchronization toggle.
  - `r_antialias`: Multisampling mode (MSAA).
  - `r_frontbuffer10bit`: 10-bit HDR frontbuffer flag.
  - `r_vmodes`: Display resolution and video modes.
  - `r_backbufferformat`: Surface pixel format.

#### 2.3 Game Initialization Pipeline (`Content/P5.xrg`)
Inspection of `Content/P5.xrg` identified the master startup sequence:
```text
*DLL "GameWorld"
*SERVERCLASS "CWServer_Mod"
*CLIENTCLASS "CWClient_Mod"
*CLIENTGAMECLASS "CWFrontEnd_Mod"
*RESOURCECLASS "CWorldDataCore"
*LOADRESOURCES "DLL:GameClasses"
*SERVERREG "Registry/Sv"
*DEFAULTGAME "Campaign"
*GAMENAME "PB"
*STRINGTABLES "Registry/StringTable_Eng.txt"
```
This confirms that the entire game logic executes inside `GameWorld`, with `CWFrontEnd_Mod` driving the UI and `CWServer_Mod` driving the campaign world simulation.

---

### 3. Native Graphics Pipeline & Shader Architecture

#### 3.1 The Deferred MRT G-Buffer (`XRShader_DeferredMRT.fp`)
*The Darkness* uses a deferred multiple render target (MRT) shading pipeline. Shipped shader `Darkness/System/Gl/ARB_fragment_program/XRShader_DeferredMRT.fp` documents the exact G-buffer layout:
- **MRT 0 (`result.color[0]`):** Tangent-space Normal map.
  - Formats: Encoded in 2 channels (Green and Alpha of DXT5 / DXN / ATI2N normal map).
  - Reconstruction: The shader reconstructs the normal Z component dynamically:
    `Z = sqrt(max(0.0, 1.0 - G^2 - B^2))`
  - Normal is converted to [0, 1] range: `Normal * 0.5 + 0.5`.
- **MRT 1 (`result.color[1]`):** Diffuse Albedo color (`DiffuseTexel`).
- **MRT 2 (`result.color[2]`):** Specular Power and Intensity (`SpecularTexel`).

#### 3.2 Vertex Pipeline Transpilation (`VP.xrg` -> HLSL)
`Darkness/System/Gl/VP.xrg` is a macro-based vertex pipeline designed by Magnus Högdahl. In `Darkness/System/Xenon/VertexProgram/VPDefines_HLSL.xrg`, Starbreeze defined the direct HLSL register bindings:
- Vertex Inputs: `_vPos` (position), `_vNrm` (normal), `_vT0`..`_vT7` (texture coords), `_vC0`..`_vC1` (vertex color), `_vMI` / `_vMW` (matrix indices/weights for character skinning).
- Vertex Outputs: `_oPos` (clip position), `_oT0`..`_oT7` (interpolated UVs), `_oC0`..`_oC1` (colors), `_oFog` (depth fog factor).
- Static Constants: `c[0..3]` = Model-View-Projection matrix, `c[4..6]` = Model rotate (3x3), `c[7]` = Model translate.

These match standard Direct3D 11/12 vertex input layouts 1-to-1 and can be compiled using DXC / FXC.

#### 3.3 HDR Post-Processing & Tone Mapping (`XREngine_Final.fp`)
The post-processing pass applies an exponential HDR tone mapping operator:
`Color_tone = 1.0 - exp(-Light * Exposure)`
where `Exposure` is supplied via `program.env[0]`.

#### 3.4 Shaders in `ProgramCache.xpc`
The binary cache `Darkness/System/Xenon/ProgramCache.xpc` (645,234 bytes) contains 442 `vs_3_0` and 322 `ps_3_0` precompiled shaders. Using [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) as an **offline build-time asset tool**, these microcodes are decompiled to human-readable HLSL, verified, and checked into the source tree as static assets. At runtime, the native renderer loads the precompiled native HLSL bytecodes via standard D3D11 `CreatePixelShader` / `CreateVertexShader`.

---

### 4. Reverse Engineered Asset Formats

#### 4.1 `.XDF` Package Format (v1.1)
All 218 packages in `Darkness/Content/Xdf/` use Starbreeze's XDF container format. Validated binary layout:
- `0x00 - 0x03` (4 bytes): Magic / Version word (`0x00000101` = Little Endian Version 1.1).
- `0x04 - 0x07` (4 bytes): String table byte length (`uint32_t str_table_len`).
- `0x08 - (0x08 + str_table_len)`: Null-delimited ASCII dependency and resource strings.
- `TOC Offset`: `0x08 + str_table_len`:
  - `0x00 - 0x03` (4 bytes): Number of entries (`uint32_t entry_count`).
  - Followed by `entry_count` records of 32 bytes each:
    - `+0x00`: String table index / name offset.
    - `+0x04`: Reserved / Flags.
    - `+0x08`: Unknown / Resource Type.
    - `+0x0C`: Uncompressed resource length (`uint32_t size`).
    - `+0x10 - +0x1F`: 16-byte metadata / GUID / timestamp.
- Payload: Followed by an intermediate chunk descriptor index and zlib-compressed streams (identifiable by `0x789C` / `0x7801` headers).

#### 4.2 `MOS DATAFILE2.0` Hierarchical Node Container
Used inside XDFs, `.XTC` textures, and `.XCR` registries:
- `0x00 - 0x0F` (16 bytes): ASCII Magic `MOS DATAFILE2.0 `.
- `0x10 - 0x17` (8 bytes): Reserved / flags.
- `0x18 - 0x1F` (8 bytes): Data offset (`uint64_t` or two `uint32_t`s).
- `0x20 - 0x27` (8 bytes): Container payload size.
- `0x28 - 0x2F` (8 bytes): Sub-node / chunk count.
- Sub-nodes can recursively nest child `MOS DATAFILE2.0` instances, forming a hierarchical filesystem tree.

#### 4.3 `.XTC` / `.XT0` / `.XT1` Split-Mip Texture Streaming
- `.XTC`: Texture Container catalog containing metadata, format identifiers, dimensions, and mip offsets.
- `.XT1`: Base / low-resolution mipmaps (loaded into resident host VRAM).
- `.XT0`: High-resolution mipmaps (streamed asynchronously during gameplay via `AsyncCopy.xrg`).
- Formats: `DXT1` (`DXGI_FORMAT_BC1_UNORM`), `DXT3` (`DXGI_FORMAT_BC2_UNORM`), `DXT5` (`DXGI_FORMAT_BC3_UNORM`), and `DXN` (`DXGI_FORMAT_BC5_UNORM` tangent-space normal maps).

---

### 5. CPU Static Recompilation: Instruction Gaps & Solutions

In the XenonRecomp trial against `_uncrypted.xex`, 40 instruction mnemonics were reported unsupported across 7,604 call sites. The top 5 account for 84% (6,380 occurrences). All 40 can be resolved with standard C++ / SIMD intrinsics:

1. **`vslh` (Vector Shift Left Halfword - 2,354 occurrences):**
   - Each of the 8 16-bit lanes in `vA` is shifted left by the low 4 bits of the corresponding lane in `vB`:
   ```cpp
   for (size_t i = 0; i < 8; i++) {
       uint8_t sh = vr(insn.operands[2]).u16[i] & 0xF;
       vr(insn.operands[0]).u16[i] = vr(insn.operands[1]).u16[i] << sh;
   }
   ```
2. **`frsqrte` (Floating Reciprocal Square Root Estimate - 1,544 occurrences):**
   - Computes `1.0 / sqrt(fB)` using double-precision hardware or `_mm_rsqrt_ss`:
   ```cpp
   fr(insn.operands[0]).f64 = 1.0 / std::sqrt(fr(insn.operands[1]).f64);
   if (strchr(insn.opcode->name, '.')) {
       cr(1).compare<double>(fr(insn.operands[0]).f64, 0.0);
   }
   ```
3. **`vsrah` (Vector Shift Right Algebraic Halfword - 979 occurrences):**
   - Signed arithmetic right shift on 8 16-bit lanes:
   ```cpp
   for (size_t i = 0; i < 8; i++) {
       uint8_t sh = vr(insn.operands[2]).u16[i] & 0xF;
       vr(insn.operands[0]).s16[i] = vr(insn.operands[1]).s16[i] >> sh;
   }
   ```
4. **`vsubshs` (Vector Subtract Signed Halfword with Saturation - 901 occurrences):**
   - 16-bit signed difference clamped to [-32768, 32767]:
   ```cpp
   for (size_t i = 0; i < 8; i++) {
       int32_t diff = int32_t(vr(insn.operands[1]).s16[i]) - int32_t(vr(insn.operands[2]).s16[i]);
       vr(insn.operands[0]).s16[i] = static_cast<int16_t>(std::clamp<int32_t>(diff, -32768, 32767));
   }
   ```
5. **`vspltish` (Vector Splat Immediate Signed Halfword - 602 occurrences):**
   - Broadcasts a signed 5-bit immediate sign-extended to 16 bits across all 8 lanes:
   ```cpp
   int16_t imm = static_cast<int16_t>(int8_t(insn.operands[1] << 3) >> 3);
   for (size_t i = 0; i < 8; i++) {
       vr(insn.operands[0]).s16[i] = imm;
   }
   ```

#### 5.2 Switch Table Boundaries (75 sites)
The 75 out-of-function switch sites occur because MSVC 8.0 on Xbox 360 optimized switch jump targets by merging common exit blocks or emitting cold landing pads outside the `.pdata` function range.
- **Fix:** Update `XenonAnalyse` to expand the candidate function boundary to encompass the maximum label address detected in `ReadTable()`, or emit explicit trampoline labels in the TOML configuration.

#### 5.3 Exception Handling (39 records)
The 39 functions flagged with exception metadata in `.pdata` utilize Microsoft C++ Exception Handling (`__CxxFrameHandler3`). In XenonRecomp, setting `skip_lr = true` globally can corrupt link-register restoration during stack unwinds. By maintaining `skip_lr = false` specifically for those 39 function addresses (or replacing them with native C++ hooks), full unwinding fidelity is guaranteed without sacrificing optimization across the other 15,683 functions.

---

### 6. Delivered Tooling in this Workspace

The following practical tools were authored and verified during this research phase:
- **`tools/dump_engine_classes.py`:** Parses `basefile.exe` and extracts the complete reflection registry. Generated `tools/engine_classes.json` containing 460 classes, virtual factory addresses, and category links.
- **`tools/xdf_tool.py`:** Standalone parser for Starbreeze `.XDF` packages; extracts TOC dependencies, entry sizes, and decompresses payload streams (`MOS DATAFILE2.0`).


## Source index

Primary and project sources used in this dossier:

- [Take-Two announcement describing Starbreeze's internal engine](https://ir.take2games.com/static-files/a487fd18-9aca-435b-9e1a-0a00c5333db0)
- [Jerk Gustafsson developer interview: evolved Riddick engine and lighting](https://www.xboxgazette.com/interview_the_darkness_en.php)
- [NAG Xbox Insider interview: semi-deferred/HDR/physics/cloth](https://www.nag.co.za/wp-content/archives/2006/000NAG%20Xbox%20Insider%20December%202006.pdf)
- [Archived official Starbreeze engine page](https://web.archive.org/web/20050204054558/http://www.starbreeze.com/engine.jsp)
- [Shacknews development interview mentioning the PC build](https://www.shacknews.com/article/43012/the-darkness-interview)
- [Official Xbox store entry](https://www.xbox.com/en-US/games/store/the-darkness/c035l0ns3sqn)
- [XenonRecomp repository/documentation](https://github.com/hedge-dev/XenonRecomp)
- [XenosRecomp repository/documentation](https://github.com/hedge-dev/XenosRecomp)
- [Unleashed Recompiled architecture](https://github.com/hedge-dev/UnleashedRecomp)
- [Xenia project](https://github.com/xenia-project/xenia)
- [Free60 XEX format documentation](https://free60.org/System-Software/Formats/XEX/)
- [Ghidra XEX loader](https://github.com/zerokilo/xexloaderwv)
- [vgmstream Starbreeze XWC parser](https://github.com/vgmstream/vgmstream/blob/master/src/meta/xwc.c)
- [Dragon UnPACKer](https://github.com/elbereth/DragonUnPACKer)
- [Khronos ARB fragment-program specification](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_fragment_program.txt)
- [Khronos ARB draw-buffers specification](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_draw_buffers.txt)
- [Khronos NV vertex-program 2 option](https://registry.khronos.org/OpenGL/extensions/NV/NV_vertex_program2_option.txt)
- [PC Games Hardware Starbreeze technology interview](https://www.pcgameshardware.de/Syndicate-Spiel-44115/Specials/Syndicate-im-exklusiven-Technik-Interview-Von-Gesichts-Scans-in-3D-Vorteilen-fuer-PC-Spieler-und-dem-voruebergehenden-Ende-von-Open-GL-864537/)
- [Starbreeze prospectus discussing historical IP ownership](https://media.starbreeze.com/2018/03/Prospectus-Starbreeze-AB-publ-2018-03-22-english.pdf)


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
