include(GoogleTest)

add_executable(blob_core_tests
    test_vec2.cpp
    test_quantize.cpp
    test_world.cpp
)

target_link_libraries(blob_core_tests PRIVATE blob::core GTest::gtest_main)

blob_set_target_defaults(blob_core_tests)

gtest_discover_tests(blob_core_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS core)
