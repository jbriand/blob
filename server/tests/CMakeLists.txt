include(GoogleTest)

# The server sources under test are compiled straight into the test binary
# rather than linked: the server is an executable, so there is no library to
# link against. main.cpp stays out (it has main() and owns the socket loop).
add_executable(blob_server_tests
    test_config.cpp
    test_loopback.cpp
    test_session.cpp
    test_snapshot_encode.cpp
    test_tick_loop.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/snapshot_encode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/tick_loop.cpp
)

target_include_directories(blob_server_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# blob::enet because test_loopback.cpp drives real sockets over 127.0.0.1 —
# the one test tier where the transport itself is the thing under test.
target_link_libraries(blob_server_tests PRIVATE blob::core blob::enet GTest::gtest_main)

if(WIN32)
    # ENet needs the Winsock bits; mirrors server/CMakeLists.txt.
    target_link_libraries(blob_server_tests PRIVATE ws2_32 winmm)
endif()

blob_set_target_defaults(blob_server_tests)

gtest_discover_tests(blob_server_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS server)
