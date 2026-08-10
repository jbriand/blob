include(GoogleTest)

# The server sources under test are compiled straight into the test binary
# rather than linked: the server is an executable, so there is no library to
# link against. main.cpp stays out (it has main() and owns the socket loop).
add_executable(blob_server_tests
    test_config.cpp
    test_tick_loop.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/tick_loop.cpp
)

target_include_directories(blob_server_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# blob::core for the headers config.hpp pulls in (sim::Tuning) — header-only
# use, but the include path comes from the target.
target_link_libraries(blob_server_tests PRIVATE blob::core GTest::gtest_main)

blob_set_target_defaults(blob_server_tests)

gtest_discover_tests(blob_server_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS server)
