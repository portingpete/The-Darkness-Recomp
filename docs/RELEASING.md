# Release validation

Apply this checklist before publishing a release or replacing any published
binary. Record the source commit, build/profile hashes, target runtime versions,
commands, logs, captures, exit codes, skipped checks and known failures.

## Every release

- Build a clean, committed source revision. Keep an existing release and its
  evidence until the replacement has passed validation.
- Build and test the final optimized binaries. After PGO/profile, compiler or
  packaging changes, validate those final binaries and verify that the ZIP and
  AppImage contain them. Check architecture, dependencies, CRCs, manifests,
  source/license notices, private-file exclusions and all uploaded hashes.
- Exercise a fresh installation and an update from the previous published
  release using its original updater. Preserve game files, settings and saves.
  If a manual update is required, state that prominently before publication.
- Run the extracted package with isolated user data through startup, menus,
  gameplay, pause/resume and normal exit. Check logs and representative images,
  not just process creation or exit code. Protect real saves and settings.
- Recheck changes to settings/defaults, subtitle and prompt rendering, audio,
  input, and resource lifetime in the scenarios they affect. Keep profile
  training separate from performance measurements and fidelity claims.

## Linux / Steam Deck publication gate

Installer, AppImage `--help`, checksum, and Windows/WARP tests do **not**
establish that the Windows game runs through Proton. Before shipping a Linux
or Steam Deck artifact:

1. Run the shader compiler preflight below with the target Wine/Proton compiler.
   Keep the runtime version, compiler module/overrides and complete output.
   Include built-in compiler coverage; using a native Windows compiler or
   Windows-generated cached bytecode must not hide the default Proton path.
2. Start the actual extracted Windows payload through the intended Proton/UMU
   and DXVK stack, first with a fresh isolated prefix/user-data/shader cache,
   then with its warmed cache. Do not clear the user's real prefix or caches.
3. Reach gameplay, use the menus and pause/resume, inspect output and logs, and
   exit normally. Exercise AA off and every advertised MSAA mode, render-scale
   changes and feature shaders introduced or modified since the previous tag.
4. Reproduce on a previously working supported Proton version as well as the
   intended current runtime when the compiler/graphics path changes. Record
   distro/SteamOS, Proton, DXVK, GPU/driver, feature level and effective settings.
5. A shader compilation failure, fatal startup error or required path that was
   not exercised blocks publication of that platform artifact. A missing test
   environment is an unvalidated target, not a pass. Publish only independently
   validated targets until the failing or missing gate is resolved.

Claims of **Steam Deck hardware compatibility** additionally require a physical
Deck result. Record Desktop/Gaming Mode, controller input, audio and suspend/
resume coverage. A passing desktop Linux run is not hardware certification.

## Cold screen-fog compiler preflight

Build `WorldRendererTests`, then run:

```powershell
build_native\Release\WorldRendererTests.exe --screen-fog-compiler-only
```

The same entry point can run under the target Wine/Proton environment. It
directly compiles the renderer's generated single-sample and MSAA fog shaders
with `D3DCompile`, without a D3D device or the persistent bytecode cache. Preserve
the command prefix and compiler selection; an arbitrary system Wine installation
is a diagnostic result, not a substitute for the supported Proton stack.

This is a preflight, followed by the packaged-game checks above. The native
Windows CTest entry is `ScreenFogShaderCompilerContract`.

## Regression reference: issue #156

[Issue #156](https://github.com/portingpete/The-Darkness-Recomp/issues/156)
reports `E5005: Method 'GetSamplePosition' is not defined on type
'Texture2DMS<float>'` for `XREngine_DepthFog.screen.multisample`. v0.1.9 passed
Windows tests and Linux installer/entrypoint checks, but those did not exercise
Proton's shader compiler. The renderer prepares the multisample fog variant at
startup even when AA is off. Require cold compilation of every prepared variant;
neither disabling AA nor a warm shader cache is adequate coverage.

The cold preflight reproduced the same error with Wine 9.0's built-in
`d3dcompiler_47` in an isolated 64-bit prefix on 2026-11-11. Native Windows
compiled all eight variants successfully. This diagnostic reproduction does
not establish results for a particular Proton version or physical Deck.

Keep the failure and its reproduction evidence until a compatibility fix passes
the same target runtime. Do not label this issue fixed solely because a new
release was packaged or the Windows compiler accepted the shader.
