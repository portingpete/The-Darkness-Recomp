# Check contents on every build: restored files can have older timestamps than
# object files. A forced include invalidates every native translation unit;
# detect_mismatch prevents accidentally linking different native revisions.
function(dark_enable_native_compilation_stamp)
    set(stamp "${CMAKE_BINARY_DIR}/native_inputs.h")
    set(stamp_tool "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/update_native_stamp.py")
    add_custom_target(VerifyNativeInputs
        COMMAND "${Python3_EXECUTABLE}" "${stamp_tool}"
            --root "${CMAKE_SOURCE_DIR}" --output "${stamp}"
        BYPRODUCTS "${stamp}" "${CMAKE_BINARY_DIR}/native_inputs.json"
        VERBATIM)
    get_property(native_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(native_target IN LISTS native_targets)
        get_target_property(native_type "${native_target}" TYPE)
        if(NOT native_target STREQUAL "DarkRecompPPC" AND
           (native_type STREQUAL "STATIC_LIBRARY" OR native_type STREQUAL "EXECUTABLE"))
            add_dependencies("${native_target}" VerifyNativeInputs)
            target_compile_options("${native_target}" PRIVATE "/FI${stamp}")
        endif()
    endforeach()
endfunction()
