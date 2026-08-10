include_guard(GLOBAL)

# blob_set_target_defaults(<target>)
#
# One place for warning flags and per-target language settings. Kept driver
# aware: clang-cl takes MSVC-style flags, clang++ takes GNU-style ones.
function(blob_set_target_defaults target)
    target_compile_features(${target} PUBLIC cxx_std_23)

    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        # clang-cl (or MSVC itself)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /EHsc /Zc:__cplusplus /Zc:preprocessor
            $<$<CXX_COMPILER_ID:Clang>:-Wextra-semi>
            $<$<CXX_COMPILER_ID:Clang>:-Wno-unused-command-line-argument>)
        target_compile_definitions(${target} PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
        if(BLOB_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        # clang++ / gcc
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow -Wconversion -Wsign-conversion
            -Wnon-virtual-dtor -Wold-style-cast -Wdouble-promotion)
        if(BLOB_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()

    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON)
endfunction()
