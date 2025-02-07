include(GoogleTest)

set(ARENAALLOC_TEST_SOURCES
  test_size_class.cpp
  test_heap_basic.cpp
  test_coalesce.cpp
  test_realloc_aligned.cpp
  test_fixed_buffer.cpp
  test_pool_threads.cpp
  test_c_api.cpp
)

add_executable(arenaalloc_tests ${ARENAALLOC_TEST_SOURCES})
target_link_libraries(arenaalloc_tests PRIVATE arenaalloc GTest::gtest_main)
gtest_discover_tests(arenaalloc_tests DISCOVERY_TIMEOUT 60)
