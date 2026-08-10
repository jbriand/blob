include(GoogleTest)

# tick_loop.cpp is compiled straight into the test binary rather than linked:
# the server is an executable, so there is no library to link against, and the
# file has no dependencies of its own beyond <chrono>. No ENet here — nothing
# under test touches the socket.
add_executable(blob_server_tests
    test_tick_loop.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/tick_loop.cpp
)

target_include_directories(blob_server_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_link_libraries(blob_server_tests PRIVATE GTest::gtest_main)

blob_set_target_defaults(blob_server_tests)

gtest_discover_tests(blob_server_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS server)
