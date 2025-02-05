include(GoogleTest)

set(ARENAALLOC_TEST_SOURCES
  test_size_class.cpp
)

add_executable(arenaalloc_tests ${ARENAALLOC_TEST_SOURCES})
target_link_libraries(arenaalloc_tests PRIVATE arenaalloc GTest::gtest_main)
gtest_discover_tests(arenaalloc_tests DISCOVERY_TIMEOUT 60)
