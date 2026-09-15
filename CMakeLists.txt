cmake_minimum_required(VERSION 3.24)
project(DarkRecomp LANGUAGES CXX)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "DarkRecomp requires native 64-bit Windows")
endif()
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "Use the Visual Studio ClangCL toolset for generated weak function wrappers")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Build-time only: parallelize source compilation within each target.
# No runtime code change; speeds up the optimize+test loop on multi-core hosts.
if(MSVC)
    add_compile_options(/MP)
endif()
set(DARK_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated" CACHE PATH "Verified AOT output")
option(DARK_ALLOW_INCOMPLETE "Compile explicit runtime traps for unresolved translation (diagnostics only)" OFF)
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
set(GATE_ARGS --output "${DARK_GENERATED_DIR}" --verify)
if(DARK_ALLOW_INCOMPLETE)
    list(APPEND GATE_ARGS --allow-incomplete)
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/recompile.py" ${GATE_ARGS}
    RESULT_VARIABLE GATE_RESULT)
if(NOT GATE_RESULT EQUAL 0)
    message(FATAL_ERROR "AOT verification failed. Run tools/build.ps1 to generate a consistent build.")
endif()
include("${DARK_GENERATED_DIR}/sources.cmake")
add_custom_target(VerifyAOT
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/recompile.py" ${GATE_ARGS}
    VERBATIM)
add_library(DarkRecompPPC STATIC ${DARK_PPC_SOURCES})
add_dependencies(DarkRecompPPC VerifyAOT)
target_include_directories(DarkRecompPPC PUBLIC
    "${DARK_GENERATED_DIR}"
    "${CMAKE_SOURCE_DIR}/refs/UnleashedRecomp/tools/XenonRecomp/thirdparty/simde")
target_compile_definitions(DarkRecompPPC PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(DarkRecompPPC PRIVATE /Zi /fp:strict)
option(DARK_NATIVE_SSSE3 "Use native SSSE3 byte shuffles for AOT vector operations" ON)
if(DARK_NATIVE_SSSE3)
    # Integer byte permutation is exact. Leave SSE4.1 dot products and FMA
    # disabled so the established ordered floating-point reduction is retained.
    target_compile_options(DarkRecompPPC PRIVATE /clang:-mssse3)
    target_compile_definitions(DarkRecompPPC PUBLIC DARK_NATIVE_SSSE3=1)
endif()
add_library(DarkRuntime STATIC runtime/native/runtime.cpp runtime/native/kernel.cpp runtime/native/dispatcher.cpp runtime/native/filesystem.cpp runtime/native/storage.cpp runtime/native/guest_runner.cpp runtime/native/audio_driver.cpp runtime/native/input.cpp runtime/native/xma_bridge.cpp renderer/d3d11/display_context_d3d11.cpp)
target_sources(DarkRuntime PRIVATE runtime/native/fov_settings.cpp runtime/native/fov_camera.cpp)
target_sources(DarkRuntime PRIVATE runtime/native/graphics_settings.cpp runtime/native/video_settings_menu.cpp)
target_sources(DarkRuntime PRIVATE runtime/native/mouse_look.cpp)
target_include_directories(DarkRuntime PUBLIC "${CMAKE_SOURCE_DIR}")
target_sources(DarkRuntime PRIVATE renderer/engine/render_trace.cpp)
target_sources(DarkRuntime PRIVATE renderer/engine/stored_geometry.cpp)
target_sources(DarkRuntime PRIVATE renderer/engine/scene_work.cpp)
target_sources(DarkRuntime PRIVATE renderer/engine/world_mesh.cpp renderer/d3d11/world_renderer.cpp)
target_sources(DarkRuntime PRIVATE renderer/engine/engine_transforms.cpp)
target_sources(DarkRuntime PRIVATE renderer/engine/engine_texture_constants.cpp)
target_sources(DarkRuntime PRIVATE renderer/engine/engine_vertex_descriptor.cpp)
target_sources(DarkRuntime PRIVATE renderer/engine/engine_palette.cpp)
target_sources(DarkRuntime PRIVATE renderer/engine/engine_vertex_program.cpp)
set_source_files_properties(renderer/engine/engine_transforms.cpp PROPERTIES COMPILE_OPTIONS "/fp:strict")
target_sources(DarkRuntime PRIVATE renderer/engine/simple_mesh.cpp renderer/d3d11/engine_preview.cpp)
target_link_libraries(DarkRuntime PUBLIC d3dcompiler)
set(DARK_SHADER_DIR "${CMAKE_BINARY_DIR}/native_shaders")
add_custom_command(OUTPUT "${DARK_SHADER_DIR}/engine_vertex_template.generated.h"
    "${DARK_SHADER_DIR}/engine_vertex_template.hlsl" "${DARK_SHADER_DIR}/engine_vertex_template.source.json"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/compile_vertex_template.py"
        --game-dir "${CMAKE_SOURCE_DIR}/Darkness" --output-dir "${DARK_SHADER_DIR}"
    DEPENDS tools/compile_vertex_template.py Darkness/System/Gl/VP.xrg
        Darkness/System/Xenon/VertexProgram/VPDefines_HLSL.xrg Darkness/System/Xenon/VPInclude_Xenon.xrg
    VERBATIM)
target_sources(DarkRuntime PRIVATE renderer/d3d11/engine_vertex_shader.cpp
    "${DARK_SHADER_DIR}/engine_vertex_template.generated.h")
target_include_directories(DarkRuntime PRIVATE "${DARK_SHADER_DIR}")
add_custom_command(OUTPUT "${DARK_SHADER_DIR}/engine_world_template.generated.h"
    "${DARK_SHADER_DIR}/engine_world_template.hlsl" "${DARK_SHADER_DIR}/engine_world_template.source.json"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/compile_world_template.py"
        --game-dir "${CMAKE_SOURCE_DIR}/Darkness" --output-dir "${DARK_SHADER_DIR}"
    DEPENDS tools/compile_world_template.py tools/compile_vertex_template.py Darkness/System/Gl/VP.xrg
        Darkness/System/Xenon/ProgramCache.xpc
        Darkness/System/Xenon/VertexProgram/VPDefines_HLSL.xrg Darkness/System/Xenon/VPInclude_Xenon.xrg
    VERBATIM)
target_sources(DarkRuntime PRIVATE "${DARK_SHADER_DIR}/engine_world_template.generated.h")
add_custom_command(OUTPUT "${DARK_SHADER_DIR}/engine_world_fragments.generated.h" "${DARK_SHADER_DIR}/engine_world_fragments.source.json"
    "${DARK_SHADER_DIR}/engine_world_fragment_bindings.generated.h"
    "${DARK_SHADER_DIR}/XRShader_FP20_NDSP.native.hlsl" "${DARK_SHADER_DIR}/XRShader_MotionMap.native.hlsl"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/compile_world_fragment.py"
        --game-dir "${CMAKE_SOURCE_DIR}/Darkness" --output-dir "${DARK_SHADER_DIR}"
    DEPENDS tools/compile_world_fragment.py tools/compile_vertex_template.py Darkness/System/Gl/ARB_fragment_program/XRShader_FP20_NDSP.fp
        Darkness/System/Gl/ARB_fragment_program/XRShader_FP20_NDS.fp
        Darkness/System/Gl/ARB_fragment_program/WClientMod_DV5_0.fp
        Darkness/System/Gl/ARB_fragment_program/WClientMod_DV5_1.fp
        Darkness/System/Gl/ARB_fragment_program/XRShader_MotionMap.fp Darkness/System/Xenon/FPInclude_Xenon.xrg
        Darkness/System/Gl/ARB_fragment_program/GUIFadeToWhite.fp
        Darkness/System/Gl/ARB_fragment_program/XRShader_FP20_LFM.fp
        Darkness/System/Gl/ARB_fragment_program/XRShader_FP20_LF.fp
        Darkness/System/Gl/ARB_fragment_program/XRShader_FP20_NDSEATP.fp
        Darkness/System/Gl/ARB_fragment_program/VBOp_GenEnv2.fp
        Darkness/System/Gl/ARB_fragment_program/VBOp_Fresnel.fp
        Darkness/System/Gl/ARB_fragment_program/XREngine_DepthFog.fp
        Darkness/System/Gl/ARB_fragment_program/XREngine_GaussClampedHurt.fp
        Darkness/System/Gl/ARB_fragment_program/XREngine_Histogram.fp
        Darkness/System/Gl/ARB_fragment_program/XRUtil_RenderSurface.fp
        Darkness/System/Gl/ARB_fragment_program/Include_XREngine_Fog.fph
        Darkness/System/Gl/ARB_fragment_program/XREngine_GaussClamped.fp
        Darkness/System/Gl/ARB_fragment_program/XREngine_RadialBlur.fp
        Darkness/System/Gl/ARB_fragment_program/XREngine_RadialBlurHurt.fp
        Darkness/System/Gl/ARB_fragment_program/XREngine_RadialBlurInvert.fp
        Darkness/System/Gl/ARB_fragment_program/XRUtil_ShrinkTexture8.fp
        Darkness/System/Gl/ARB_fragment_program/XREngine_CCFuser.fp
        Darkness/System/Gl/ARB_fragment_program/XREngine_Final5.fp
        Darkness/System/Gl/ARB_fragment_program/XREngine_MulFilter.fp
        Darkness/System/Xenon/FragmentProgram/MRenderXenon_Attrib_TexEnvMode00.fp
        Darkness/System/Xenon/FragmentProgram/MRenderXenon_Attrib_TexEnvMode01.fp
        Darkness/System/Xenon/FragmentProgram/MRenderXenon_Attrib_TexEnvMode02.fp
    VERBATIM)
target_sources(DarkRuntime PRIVATE "${DARK_SHADER_DIR}/engine_world_fragments.generated.h" "${DARK_SHADER_DIR}/engine_world_fragment_bindings.generated.h")
target_link_libraries(DarkRuntime PUBLIC DarkRecompPPC bcrypt dbghelp d3d11 dxgi xaudio2 ole32 xinput)
target_compile_options(DarkRuntime PRIVATE /Zi)
add_executable(DarkRecomp app/windows/native_main.cpp app/windows/display_settings.cpp)
set(DARK_VIDEO_MENU "${CMAKE_BINARY_DIR}/native_assets/CubeWnd.pc.xcr")
set(DARK_VIDEO_ARCHIVE "${CMAKE_BINARY_DIR}/native_assets/GameContext_Create.pc.xdf")
add_custom_command(OUTPUT "${DARK_VIDEO_MENU}" "${DARK_VIDEO_ARCHIVE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/compile_video_menu.py"
        --source "${CMAKE_SOURCE_DIR}/Darkness/Content/Gui/CubeWnd.xcr" --output "${DARK_VIDEO_MENU}"
        --archive-source "${CMAKE_SOURCE_DIR}/Darkness/Content/Xdf/GameContext_Create.XDF" --archive-output "${DARK_VIDEO_ARCHIVE}"
    DEPENDS tools/compile_video_menu.py Darkness/Content/Gui/CubeWnd.xcr Darkness/Content/Xdf/GameContext_Create.XDF VERBATIM)
add_custom_target(NativeVideoMenu ALL DEPENDS "${DARK_VIDEO_MENU}" "${DARK_VIDEO_ARCHIVE}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DARK_VIDEO_MENU}" "$<TARGET_FILE_DIR:DarkRecomp>/CubeWnd.pc.xcr"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DARK_VIDEO_ARCHIVE}" "$<TARGET_FILE_DIR:DarkRecomp>/GameContext_Create.pc.xdf")
add_dependencies(DarkRecomp NativeVideoMenu)
target_link_libraries(DarkRecomp PRIVATE DarkRuntime winmm comctl32)
target_link_options(DarkRecomp PRIVATE /DEBUG /STACK:67108864,1048576)
include(CTest)
set(DARK_XMA_ROOT "${CMAKE_SOURCE_DIR}/build_native/deps/ffmpeg-darkxma")
add_library(DarkXmaRaw STATIC runtime/native/xma_raw_decoder.cpp)
target_include_directories(DarkXmaRaw PRIVATE "${DARK_XMA_ROOT}/include")
target_link_libraries(DarkXmaRaw PUBLIC "${DARK_XMA_ROOT}/lib/avcodec-darkxma.lib" "${DARK_XMA_ROOT}/lib/avutil-darkxma.lib")
target_link_libraries(DarkRuntime PUBLIC DarkXmaRaw)
function(stage_dark_xma target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DARK_XMA_ROOT}/bin/avcodec-darkxma-62.dll" "$<TARGET_FILE_DIR:${target}>/avcodec-darkxma-62.dll"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DARK_XMA_ROOT}/bin/avutil-darkxma-60.dll" "$<TARGET_FILE_DIR:${target}>/avutil-darkxma-60.dll")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${DARK_XMA_ROOT}/bin/libwinpthread-1.dll" "$<TARGET_FILE_DIR:${target}>/libwinpthread-1.dll")
endfunction()
stage_dark_xma(DarkRecomp)
add_executable(DarkRecompPreview WIN32 app/windows/preview_launcher.cpp)
target_compile_definitions(DarkRecompPreview PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_dependencies(DarkRecompPreview DarkRecomp)
if(BUILD_TESTING)
    add_executable(DisplaySettingsTests tests/display_settings_tests.cpp app/windows/display_settings.cpp)
    target_link_libraries(DisplaySettingsTests PRIVATE DarkRuntime comctl32)
    stage_dark_xma(DisplaySettingsTests)
    add_test(NAME DisplaySettingsContract COMMAND DisplaySettingsTests)
    add_test(NAME NativeVideoMenuAssetContract COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_video_menu.py")
    set_tests_properties(DisplaySettingsContract PROPERTIES TIMEOUT 30)
    add_test(NAME AOTGenerationContract COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_recompile.py")
    set_tests_properties(AOTGenerationContract PROPERTIES TIMEOUT 30)
    add_test(NAME NativeCompilationRollback COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_native_stamp.py")
    set_tests_properties(NativeCompilationRollback PROPERTIES TIMEOUT 120)
    add_test(NAME WorldFragmentContract COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_world_fragment.py")
    add_executable(WorldRenderStateTests tests/world_render_state_tests.cpp)
    target_include_directories(WorldRenderStateTests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_compile_definitions(WorldRenderStateTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    add_test(NAME WorldRenderStateContract COMMAND WorldRenderStateTests "${CMAKE_SOURCE_DIR}/Darkness/basefile.exe")
    set_tests_properties(WorldRenderStateContract PROPERTIES TIMEOUT 30)
    add_executable(WorldRendererTests tests/world_renderer_tests.cpp)
    # Opt-in timings, separate from correctness tests: no machine-dependent threshold.
    add_executable(WorldGeometryBenchmark EXCLUDE_FROM_ALL tests/world_geometry_benchmark.cpp)
    target_link_libraries(WorldGeometryBenchmark PRIVATE DarkRuntime)
    stage_dark_xma(WorldGeometryBenchmark)
    add_executable(SceneSubmissionBenchmark EXCLUDE_FROM_ALL tests/scene_submission_benchmark.cpp)
    target_link_libraries(SceneSubmissionBenchmark PRIVATE DarkRuntime)
    stage_dark_xma(SceneSubmissionBenchmark)
    target_link_libraries(WorldRendererTests PRIVATE DarkRuntime)
    target_compile_options(WorldRendererTests PRIVATE /fp:strict)
    stage_dark_xma(WorldRendererTests)
    add_test(NAME WorldRendererContract COMMAND WorldRendererTests)
    add_test(NAME WorldRendererWarpContract COMMAND WorldRendererTests --warp)
    add_test(NAME WorldVertexPreparationContract COMMAND WorldRendererTests --cpu-only)
    set_tests_properties(WorldVertexPreparationContract PROPERTIES TIMEOUT 30)
    set_tests_properties(WorldRendererContract WorldRendererWarpContract PROPERTIES TIMEOUT 60)
    add_executable(WorldTextureBindingTests tests/world_texture_binding_tests.cpp)
    target_link_libraries(WorldTextureBindingTests PRIVATE DarkRuntime)
    stage_dark_xma(WorldTextureBindingTests)
    add_test(NAME WorldTextureBindingContract COMMAND WorldTextureBindingTests "${CMAKE_SOURCE_DIR}/Darkness")
    set_tests_properties(WorldTextureBindingContract PROPERTIES TIMEOUT 30)
    add_executable(WorldSurfaceBindingTests tests/world_surface_binding_tests.cpp)
    target_link_libraries(WorldSurfaceBindingTests PRIVATE DarkRuntime)
    stage_dark_xma(WorldSurfaceBindingTests)
    add_test(NAME WorldSurfaceBindingContract COMMAND WorldSurfaceBindingTests "${CMAKE_SOURCE_DIR}/Darkness")
    set_tests_properties(WorldSurfaceBindingContract PROPERTIES TIMEOUT 30)
    add_executable(EngineVertexShaderTests tests/engine_vertex_shader_tests.cpp)
    add_executable(NativeRenderSamplerTests tests/native_render_sampler_tests.cpp)
    target_link_libraries(NativeRenderSamplerTests PRIVATE DarkRuntime)
    stage_dark_xma(NativeRenderSamplerTests)
    add_test(NAME NativeRenderSamplerContract COMMAND NativeRenderSamplerTests)
    set_tests_properties(NativeRenderSamplerContract PROPERTIES TIMEOUT 20)
    add_executable(TransientGeometryBufferTests tests/transient_geometry_buffer_tests.cpp)
    add_executable(DescriptorCacheTests tests/descriptor_cache_tests.cpp)
    add_executable(PresentationTests tests/presentation_tests.cpp renderer/d3d11/display_context_d3d11.cpp)
    target_include_directories(PresentationTests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_compile_definitions(PresentationTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(PresentationTests PRIVATE d3d11 dxgi)
    add_test(NAME PresentationContract COMMAND PresentationTests)
    set_tests_properties(PresentationContract PROPERTIES TIMEOUT 30)
    target_include_directories(DescriptorCacheTests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_compile_definitions(DescriptorCacheTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    add_test(NAME DescriptorCacheContract COMMAND DescriptorCacheTests)
    set_tests_properties(DescriptorCacheContract PROPERTIES TIMEOUT 30)
    target_include_directories(TransientGeometryBufferTests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_compile_definitions(TransientGeometryBufferTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(TransientGeometryBufferTests PRIVATE d3d11)
    add_test(NAME TransientGeometryBufferContract COMMAND TransientGeometryBufferTests)
    add_test(NAME TransientGeometryBufferWarpContract COMMAND TransientGeometryBufferTests --warp)
    set_tests_properties(TransientGeometryBufferContract TransientGeometryBufferWarpContract PROPERTIES TIMEOUT 30)
    target_link_libraries(EngineVertexShaderTests PRIVATE DarkRuntime)
    target_compile_options(EngineVertexShaderTests PRIVATE /fp:strict)
    stage_dark_xma(EngineVertexShaderTests)
    add_test(NAME EngineVertexShaderContract COMMAND EngineVertexShaderTests)
    add_test(NAME EngineVertexShaderWarpContract COMMAND EngineVertexShaderTests --warp)
    set_tests_properties(EngineVertexShaderContract EngineVertexShaderWarpContract PROPERTIES TIMEOUT 60)
    add_test(NAME EngineVertexTemplateContract COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_vertex_template.py")
    add_executable(EngineVertexProgramTests tests/engine_vertex_program_tests.cpp)
    target_link_libraries(EngineVertexProgramTests PRIVATE DarkRuntime)
    stage_dark_xma(EngineVertexProgramTests)
    add_test(NAME EngineVertexProgramContract COMMAND EngineVertexProgramTests "${CMAKE_SOURCE_DIR}/Darkness")
    set_tests_properties(EngineVertexProgramContract PROPERTIES TIMEOUT 30)
    add_executable(EnginePaletteTests tests/engine_palette_tests.cpp)
    target_link_libraries(EnginePaletteTests PRIVATE DarkRuntime)
    stage_dark_xma(EnginePaletteTests)
    add_test(NAME EnginePaletteContract COMMAND EnginePaletteTests "${CMAKE_SOURCE_DIR}/Darkness")
    set_tests_properties(EnginePaletteContract PROPERTIES TIMEOUT 30)
    add_executable(EngineVertexDescriptorTests tests/engine_vertex_descriptor_tests.cpp)
    target_link_libraries(EngineVertexDescriptorTests PRIVATE DarkRuntime)
    stage_dark_xma(EngineVertexDescriptorTests)
    add_test(NAME EngineVertexDescriptorContract COMMAND EngineVertexDescriptorTests "${CMAKE_SOURCE_DIR}/Darkness")
    set_tests_properties(EngineVertexDescriptorContract PROPERTIES TIMEOUT 30)
    add_executable(EngineTextureTests tests/engine_texture_tests.cpp)
    target_link_libraries(EngineTextureTests PRIVATE DarkRuntime)
    stage_dark_xma(EngineTextureTests)
    add_test(NAME EngineTextureContract COMMAND EngineTextureTests "${CMAKE_SOURCE_DIR}/Darkness")
    set_tests_properties(EngineTextureContract PROPERTIES TIMEOUT 30)
    add_executable(EngineTransformTests tests/engine_transform_tests.cpp)
    target_link_libraries(EngineTransformTests PRIVATE DarkRuntime)
    target_compile_options(EngineTransformTests PRIVATE /fp:strict)
    stage_dark_xma(EngineTransformTests)
    add_test(NAME EngineTransformContract COMMAND EngineTransformTests "${CMAKE_SOURCE_DIR}/Darkness")
    set_tests_properties(EngineTransformContract PROPERTIES TIMEOUT 30)
    add_executable(StoredGeometryTests tests/stored_geometry_tests.cpp)
    target_link_libraries(StoredGeometryTests PRIVATE DarkRuntime)
    stage_dark_xma(StoredGeometryTests)
    add_test(NAME StoredGeometryContract COMMAND StoredGeometryTests "${CMAKE_SOURCE_DIR}/Darkness")

    add_executable(FrameResourceSetTests tests/frame_resource_set_tests.cpp)
    target_include_directories(FrameResourceSetTests PRIVATE "${CMAKE_SOURCE_DIR}")
    add_test(NAME FrameResourceSetContract COMMAND FrameResourceSetTests)
    set_tests_properties(FrameResourceSetContract PROPERTIES TIMEOUT 30)
    add_executable(DrawPoolTests tests/draw_pool_tests.cpp)
    target_link_libraries(DrawPoolTests PRIVATE DarkRuntime)
    stage_dark_xma(DrawPoolTests)
    add_test(NAME DrawPoolContract COMMAND DrawPoolTests)
    set_tests_properties(DrawPoolContract PROPERTIES TIMEOUT 30)
    set_tests_properties(StoredGeometryContract PROPERTIES TIMEOUT 30)
    add_executable(EngineMeshTests tests/engine_mesh_tests.cpp)
    target_link_libraries(EngineMeshTests PRIVATE DarkRuntime)
    stage_dark_xma(EngineMeshTests)
    add_test(NAME EngineMeshContract COMMAND EngineMeshTests "${CMAKE_SOURCE_DIR}/Darkness")
    set_tests_properties(EngineMeshContract PROPERTIES TIMEOUT 30)
    add_executable(RendererTests tests/renderer_tests.cpp)
    target_link_libraries(RendererTests PRIVATE DarkRuntime)
    add_test(NAME RendererContract COMMAND RendererTests)
    set_tests_properties(RendererContract PROPERTIES TIMEOUT 30)
    add_executable(NativeTests tests/native_tests.cpp)
    add_dependencies(NativeTests NativeVideoMenu)
    target_link_libraries(NativeTests PRIVATE DarkRuntime)
    stage_dark_xma(NativeTests)
    target_link_options(NativeTests PRIVATE /DEBUG /STACK:67108864,1048576)
    add_test(NAME NativeRuntime COMMAND NativeTests "${CMAKE_SOURCE_DIR}/Darkness")
    add_test(NAME NativeTextureMipContract COMMAND NativeTests "${CMAKE_SOURCE_DIR}/Darkness" --texture-mip-layout)
    add_test(NAME NativeTextureUploadContract COMMAND NativeTests "${CMAKE_SOURCE_DIR}/Darkness" --texture-upload)
    set_tests_properties(NativeTextureUploadContract PROPERTIES TIMEOUT 30)
    set_tests_properties(NativeTextureMipContract PROPERTIES TIMEOUT 30)
    add_test(NAME NativeFormatAbiContract COMMAND NativeTests "${CMAKE_SOURCE_DIR}/Darkness" --format-abi)
    add_test(NAME NativeFormatWidthContract COMMAND NativeTests "${CMAKE_SOURCE_DIR}/Darkness" --format-width)
    add_test(NAME NativeFileBufferContract COMMAND NativeTests "${CMAKE_SOURCE_DIR}/Darkness" --file-buffers)
    set_tests_properties(NativeFormatAbiContract NativeFormatWidthContract NativeFileBufferContract PROPERTIES TIMEOUT 30)
    add_test(NAME NativeSaveStorageContract COMMAND NativeTests "${CMAKE_SOURCE_DIR}/Darkness" --save-storage)
    set_tests_properties(NativeSaveStorageContract PROPERTIES TIMEOUT 60)
    add_test(NAME RendererSmoke COMMAND "$<TARGET_FILE:DarkRecomp>" "--game-dir" "${CMAKE_SOURCE_DIR}/Darkness" "--timeout-ms" "5000" "--renderer-smoke")
    add_test(NAME InvalidDispatchFails COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/check_failure.py" "$<TARGET_FILE:NativeTests>" "${CMAKE_SOURCE_DIR}/Darkness")
    add_test(NAME LauncherStatusPropagates COMMAND "$<TARGET_FILE:NativeTests>" "${CMAKE_SOURCE_DIR}/Darkness" "--launcher-status")
    add_test(NAME NullRecordAssertionFails COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/check_null_record.py" "$<TARGET_FILE:NativeTests>" "${CMAKE_SOURCE_DIR}/Darkness")
    add_test(NAME ResolveBoundaryContract COMMAND "$<TARGET_FILE:NativeTests>" "${CMAKE_SOURCE_DIR}/Darkness" "--resolve-boundary")
    add_test(NAME TimestampBundleAdvances COMMAND "$<TARGET_FILE:NativeTests>" "${CMAKE_SOURCE_DIR}/Darkness" "--timestamp-bundle")
    add_test(NAME SuspendResumeContract COMMAND "$<TARGET_FILE:NativeTests>" "${CMAKE_SOURCE_DIR}/Darkness" "--suspend-resume")
    add_test(NAME NativeDelayContract COMMAND "$<TARGET_FILE:NativeTests>" "${CMAKE_SOURCE_DIR}/Darkness" "--native-delay")
    set_tests_properties(NativeDelayContract PROPERTIES TIMEOUT 30)
    add_test(NAME SystemVersionContract COMMAND "$<TARGET_FILE:NativeTests>" "${CMAKE_SOURCE_DIR}/Darkness" "--system-version")
    add_test(NAME XmpControlContract COMMAND "$<TARGET_FILE:NativeTests>" "${CMAKE_SOURCE_DIR}/Darkness" "--xmp-control")
    add_test(NAME AudioDriverContract COMMAND "$<TARGET_FILE:NativeTests>" "${CMAKE_SOURCE_DIR}/Darkness" "--audio-driver")
    add_test(NAME DispatcherContract COMMAND "$<TARGET_FILE:NativeTests>" "${CMAKE_SOURCE_DIR}/Darkness" "--dispatcher")
    add_test(NAME CriticalSectionContract COMMAND NativeTests "${CMAKE_SOURCE_DIR}/Darkness" --critical-sections)
    set_tests_properties(CriticalSectionContract PROPERTIES TIMEOUT 30)
    add_test(NAME KernelObjectOutputContract COMMAND NativeTests "${CMAKE_SOURCE_DIR}/Darkness" --object-outputs)
    set_tests_properties(KernelObjectOutputContract PROPERTIES TIMEOUT 30)
    add_test(NAME InputContract COMMAND "$<TARGET_FILE:NativeTests>" "${CMAKE_SOURCE_DIR}/Darkness" "--input")
    add_test(NAME VideoSettingsContract COMMAND NativeTests "${CMAKE_SOURCE_DIR}/Darkness" --video-settings)
    set_tests_properties(VideoSettingsContract PROPERTIES TIMEOUT 30)
    set_tests_properties(AudioDriverContract DispatcherContract PROPERTIES TIMEOUT 60)
    add_test(NAME XmaLifecycleContract COMMAND "$<TARGET_FILE:NativeTests>" "${CMAKE_SOURCE_DIR}/Darkness" "--xma-lifecycle")
    add_test(NAME XmaBridgeContract COMMAND "$<TARGET_FILE:NativeTests>" "${CMAKE_SOURCE_DIR}/Darkness" "--xma-bridge" "${CMAKE_SOURCE_DIR}/build_native/run/xma-capture-000.bin")
    add_test(NAME XmaDecodeLive COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/check_xma_decode.py" "$<TARGET_FILE:NativeTests>" "${CMAKE_SOURCE_DIR}/Darkness" "--xma-submit-live")
    add_test(NAME XmaDecodeStaged COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/check_xma_decode.py" "$<TARGET_FILE:NativeTests>" "${CMAKE_SOURCE_DIR}/Darkness" "--xma-submit-staged")
    add_test(NAME OriginalGameInflate COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/check_inflate.py" "$<TARGET_FILE:NativeTests>" "${CMAKE_SOURCE_DIR}/Darkness")
    set(FFMPEG_SHARED_ROOT "${CMAKE_SOURCE_DIR}/build_native/deps/ffmpeg-8.1.2-full_build-shared")
    add_executable(XmaAdapterTests tests/xma_adapter_tests.cpp runtime/native/xma_decoder.cpp)
    target_include_directories(XmaAdapterTests PRIVATE "${CMAKE_SOURCE_DIR}" "${FFMPEG_SHARED_ROOT}/include")
    target_link_directories(XmaAdapterTests PRIVATE "${FFMPEG_SHARED_ROOT}/lib")
    target_link_libraries(XmaAdapterTests PRIVATE avcodec avutil)
    target_compile_definitions(XmaAdapterTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_compile_options(XmaAdapterTests PRIVATE /Zi)
    target_link_options(XmaAdapterTests PRIVATE /DEBUG /STACK:67108864,1048576)
    add_custom_command(TARGET XmaAdapterTests POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${FFMPEG_SHARED_ROOT}/bin/avcodec-62.dll" "$<TARGET_FILE_DIR:XmaAdapterTests>/avcodec-62.dll"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${FFMPEG_SHARED_ROOT}/bin/avutil-60.dll" "$<TARGET_FILE_DIR:XmaAdapterTests>/avutil-60.dll"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${FFMPEG_SHARED_ROOT}/bin/swresample-6.dll" "$<TARGET_FILE_DIR:XmaAdapterTests>/swresample-6.dll")
    add_test(NAME XmaAdapterContract COMMAND XmaAdapterTests
        "${CMAKE_SOURCE_DIR}/build_native/run/xma-capture-000.bin")
    add_executable(XmaRawTests tests/xma_raw_tests.cpp)
    target_include_directories(XmaRawTests PRIVATE "${CMAKE_SOURCE_DIR}" "${DARK_XMA_ROOT}/include")
    target_compile_definitions(XmaRawTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(XmaRawTests PRIVATE DarkXmaRaw)
    add_dependencies(XmaRawTests XmaAdapterTests)
    stage_dark_xma(XmaRawTests)
    add_test(NAME XmaRawContract COMMAND XmaRawTests "${CMAKE_SOURCE_DIR}/build_native/run/xma-capture-000.bin")
    add_executable(TestInputParserTests tests/test_input_parser_contract.cpp)
    target_include_directories(TestInputParserTests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_compile_definitions(TestInputParserTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    add_test(NAME TestInputParserContract COMMAND TestInputParserTests)
    set_tests_properties(TestInputParserContract PROPERTIES TIMEOUT 30)
    add_executable(FramePacerTests tests/frame_pacer_tests.cpp)
    target_include_directories(FramePacerTests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_compile_definitions(FramePacerTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(FramePacerTests PRIVATE winmm)
    add_test(NAME FramePacerContract COMMAND FramePacerTests)
    set_tests_properties(FramePacerContract PROPERTIES TIMEOUT 30)
    add_executable(FrameMetricsTests tests/frame_metrics_tests.cpp)
    add_executable(SceneWorkTests tests/scene_work_tests.cpp)
    target_link_libraries(SceneWorkTests PRIVATE DarkRuntime)
    stage_dark_xma(SceneWorkTests)
    add_test(NAME SceneWorkContract COMMAND SceneWorkTests)
    set_tests_properties(SceneWorkContract PROPERTIES TIMEOUT 45)
    add_test(NAME SceneWorkDefaultSerialContract COMMAND SceneWorkTests --default-serial)
    set_tests_properties(SceneWorkDefaultSerialContract PROPERTIES TIMEOUT 45)
    target_include_directories(FrameMetricsTests PRIVATE "${CMAKE_SOURCE_DIR}")
    target_compile_definitions(FrameMetricsTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    add_test(NAME FrameMetricsContract COMMAND FrameMetricsTests)
    set_tests_properties(FrameMetricsContract PROPERTIES TIMEOUT 30)
endif()

include(cmake/NativeCompilationStamp.cmake)
dark_enable_native_compilation_stamp()
