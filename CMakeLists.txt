cmake_minimum_required(VERSION 3.16)
project(ArenaAlloc VERSION 0.1.0 LANGUAGES CXX)

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
)
target_include_directories(arenaalloc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(arenaalloc PUBLIC Threads::Threads)
target_compile_options(arenaalloc PRIVATE -Wall -Wextra -Wpedantic)
