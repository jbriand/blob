# ---------------------------------------------------------------------------
# core — static library. Pure simulation + protocol.
#
# HARD RULE: no SFML, no ENet, no filesystem, no sockets, no logging to stdout.
# Enforced at configure time by blob_assert_no_transitive_deps() in the root
# CMakeLists. Everything here must be buildable and testable in isolation.
# ---------------------------------------------------------------------------

add_library(blob_core STATIC
    src/math/vec2.cpp
    src/sim/world.cpp
    src/sim/spatial_grid.cpp
    src/net/protocol.cpp
)
add_library(blob::core ALIAS blob_core)

target_include_directories(blob_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

blob_set_target_defaults(blob_core)

set_target_properties(blob_core PROPERTIES
    EXPORT_NAME core
    OUTPUT_NAME blob_core)

if(BLOB_BUILD_TESTS)
    add_subdirectory(tests)
endif()
