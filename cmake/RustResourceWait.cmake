option(DARK_RUST_RESOURCE_WAIT "Use Rust completion notifications for resource dependency waits" ON)
function(dark_link_resource_wait target)
    if(NOT DARK_RUST_RESOURCE_WAIT)
        return()
    endif()
    find_program(DARK_RESOURCE_WAIT_RUSTC rustc HINTS "$ENV{USERPROFILE}/.cargo/bin" REQUIRED)
    set(root "${CMAKE_BINARY_DIR}/rust-resource-wait")
    set(source "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../renderer/rust_resource_wait/src/lib.rs")
    if(WIN32)
        set(output "${root}/dark_resource_wait.lib")
        set(platform --target x86_64-pc-windows-msvc)
    else()
        set(output "${root}/libdark_resource_wait.a")
    endif()
    add_custom_target(DarkRustResourceWaitBuild
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${root}/fresh"
        COMMAND "${DARK_RESOURCE_WAIT_RUSTC}" --crate-name dark_resource_wait --crate-type staticlib
            --edition 2021 "${source}" ${platform} -C panic=abort -C opt-level=3
            -C codegen-units=2 -C embed-bitcode=no -C debuginfo=1 -o "${output}.fresh"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${output}.fresh" "${output}"
        BYPRODUCTS "${output}" SOURCES "${source}" VERBATIM)
    add_library(DarkRustResourceWait STATIC IMPORTED GLOBAL)
    set_target_properties(DarkRustResourceWait PROPERTIES IMPORTED_LOCATION "${output}")
    add_dependencies(DarkRustResourceWait DarkRustResourceWaitBuild)
    target_link_libraries(${target} PUBLIC DarkRustResourceWait)
    target_compile_definitions(${target} PRIVATE DARK_RUST_RESOURCE_WAIT=1)
endfunction()
