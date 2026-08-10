# ---------------------------------------------------------------------------
# client — SFML 3 renderer + input + snapshot buffer.
#
# The only target allowed to see SFML. Rendering stays deliberately boring.
# ---------------------------------------------------------------------------

add_executable(blob_client
    src/main.cpp
)

target_link_libraries(blob_client PRIVATE blob::core blob::enet blob::sfml)

target_include_directories(blob_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

blob_set_target_defaults(blob_client)

set_target_properties(blob_client PROPERTIES
    OUTPUT_NAME blob-client
    WIN32_EXECUTABLE OFF)   # keep the console attached while debugging netcode

if(WIN32)
    target_link_libraries(blob_client PRIVATE ws2_32 winmm)
endif()
