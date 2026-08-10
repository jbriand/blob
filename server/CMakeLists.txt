# ---------------------------------------------------------------------------
# server — headless authoritative simulation host.
#
# Must build and run on a box with no GPU and no X11. Links core + ENet only.
# ---------------------------------------------------------------------------

add_executable(blob_server
    src/main.cpp
    src/config.cpp
    src/session.cpp
    src/snapshot_encode.cpp
    src/tick_loop.cpp
)

target_link_libraries(blob_server PRIVATE blob::core blob::enet)

target_include_directories(blob_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

blob_set_target_defaults(blob_server)

set_target_properties(blob_server PROPERTIES OUTPUT_NAME blob-server)

if(WIN32)
    # ENet needs the Winsock bits; upstream links them, this is belt and braces.
    target_link_libraries(blob_server PRIVATE ws2_32 winmm)
endif()

if(BLOB_BUILD_TESTS)
    add_subdirectory(tests)
endif()
