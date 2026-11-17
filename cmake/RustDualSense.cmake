find_program(DARK_DUALSENSE_CARGO cargo HINTS "$ENV{USERPROFILE}/.cargo/bin" REQUIRED)
set(DARK_DUALSENSE_ROOT "${CMAKE_BINARY_DIR}/rust-dualsense")
set(DARK_DUALSENSE_LIBRARY "${DARK_DUALSENSE_ROOT}/dark_dualsense.lib")
add_custom_target(DarkRustDualSenseBuild
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/build_dualsense.py"
        --root "${CMAKE_SOURCE_DIR}" --output "${DARK_DUALSENSE_LIBRARY}" --cargo "${DARK_DUALSENSE_CARGO}"
    BYPRODUCTS "${DARK_DUALSENSE_LIBRARY}" VERBATIM)
add_library(DarkRustDualSense STATIC IMPORTED GLOBAL)
set_target_properties(DarkRustDualSense PROPERTIES IMPORTED_LOCATION "${DARK_DUALSENSE_LIBRARY}")
add_dependencies(DarkRustDualSense DarkRustDualSenseBuild)
target_sources(DarkRuntime PRIVATE runtime/native/dualsense_controller.cpp)
target_link_libraries(DarkRuntime PUBLIC DarkRustDualSense hid setupapi cfgmgr32 userenv ws2_32 ntdll)
if(BUILD_TESTING)
    add_test(NAME DualSenseProtocolContract
        COMMAND "${DARK_DUALSENSE_CARGO}" test --manifest-path "${CMAKE_SOURCE_DIR}/runtime/dualsense/Cargo.toml"
            --locked --lib --target x86_64-pc-windows-msvc --target-dir "${DARK_DUALSENSE_ROOT}/cargo")
    set_tests_properties(DualSenseProtocolContract PROPERTIES TIMEOUT 120)
endif()
