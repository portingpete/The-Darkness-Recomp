# A small dependency-free Rust library behind the existing C++/PPC ABI.
find_program(DARK_MOVEMENT_RUSTC rustc HINTS "$ENV{USERPROFILE}/.cargo/bin" REQUIRED)
set(DARK_MOVEMENT_SOURCE "${CMAKE_SOURCE_DIR}/runtime/movement/src/lib.rs")
set(DARK_MOVEMENT_ROOT "${CMAKE_BINARY_DIR}/rust-movement")
set(DARK_MOVEMENT_LIBRARY "${DARK_MOVEMENT_ROOT}/dark_movement.lib")
add_custom_command(OUTPUT "${DARK_MOVEMENT_LIBRARY}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DARK_MOVEMENT_ROOT}"
    COMMAND "${DARK_MOVEMENT_RUSTC}" --crate-name dark_movement --crate-type staticlib
        --edition 2021 "${DARK_MOVEMENT_SOURCE}" --target x86_64-pc-windows-msvc
        -C panic=abort -C opt-level=3 -C embed-bitcode=no -o "${DARK_MOVEMENT_LIBRARY}"
    DEPENDS "${DARK_MOVEMENT_SOURCE}" VERBATIM)
add_custom_target(DarkRustMovementBuild DEPENDS "${DARK_MOVEMENT_LIBRARY}")
add_library(DarkRustMovement STATIC IMPORTED GLOBAL)
set_target_properties(DarkRustMovement PROPERTIES IMPORTED_LOCATION "${DARK_MOVEMENT_LIBRARY}")
add_dependencies(DarkRustMovement DarkRustMovementBuild)
target_link_libraries(DarkRuntime PUBLIC DarkRustMovement)
if(BUILD_TESTING)
    set(DARK_MOVEMENT_TESTS "${DARK_MOVEMENT_ROOT}/movement_tests.exe")
    add_custom_command(OUTPUT "${DARK_MOVEMENT_TESTS}"
        COMMAND "${DARK_MOVEMENT_RUSTC}" --crate-name dark_movement --test --edition 2021
            "${DARK_MOVEMENT_SOURCE}" --target x86_64-pc-windows-msvc -o "${DARK_MOVEMENT_TESTS}"
        DEPENDS "${DARK_MOVEMENT_SOURCE}" VERBATIM)
    add_custom_target(DarkRustMovementTests ALL DEPENDS "${DARK_MOVEMENT_TESTS}")
    add_test(NAME MovementMathContract COMMAND "${DARK_MOVEMENT_TESTS}")
    set_tests_properties(MovementMathContract PROPERTIES TIMEOUT 15)
endif()
