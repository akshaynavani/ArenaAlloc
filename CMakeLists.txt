cmake_minimum_required(VERSION 3.16)
project(ArenaAlloc VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ARENAALLOC_BUILD_TESTS "Build the GoogleTest suite" ON)
option(ARENAALLOC_BUILD_BENCH "Build the benchmark harness" ON)

find_package(Threads REQUIRED)

add_library(arenaalloc STATIC
  src/os_memory.cpp
  src/heap.cpp
  src/arena_pool.cpp
  src/c_api.cpp
)
target_include_directories(arenaalloc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(arenaalloc PUBLIC Threads::Threads)
target_compile_options(arenaalloc PRIVATE -Wall -Wextra -Wpedantic)

# LD_PRELOAD-able malloc replacement (glibc/Linux only).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(arenaalloc_preload SHARED src/malloc_shim.c)
  target_link_libraries(arenaalloc_preload PRIVATE arenaalloc)
  set_target_properties(arenaalloc_preload PROPERTIES C_VISIBILITY_PRESET hidden)
  # The C++ runtime must be linked in; exceptions are never thrown on the hot path.
  set_target_properties(arenaalloc_preload PROPERTIES LINKER_LANGUAGE CXX)
endif()

if(ARENAALLOC_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
  add_subdirectory(tests)
endif()
