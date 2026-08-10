# Copyright (c) 2026 Timothy Palpant
#
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Host tests for the Zephyr-independent parts of Zest.
#
# These build with a plain host compiler, so the numeric, timing, control and
# error layers can be exercised without a west workspace:
#
#     cmake -S tests/host -B build/host && cmake --build build/host
#     ctest --test-dir build/host --output-on-failure

cmake_minimum_required(VERSION 3.20)
project(zest_host_tests CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_library(zest_host STATIC ${CMAKE_CURRENT_SOURCE_DIR}/../../src/error.cpp)
target_include_directories(zest_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
target_compile_options(zest_host PRIVATE -Wall -Wextra -Wpedantic)

# Exceptions and RTTI are off in the Zephyr configuration Zest targets, so the
# host build holds the headers to the same constraint.
target_compile_options(zest_host PUBLIC -fno-exceptions -fno-rtti)

set(ZEST_HOST_TESTS
  error
  filters
  transforms
  timing
  retry
  battery_curve
  units
  control
  ring_buffer
  function
  sample_store
  cbor
)

foreach(name IN LISTS ZEST_HOST_TESTS)
  set(source ${CMAKE_CURRENT_SOURCE_DIR}/test_${name}.cpp)
  if(NOT EXISTS ${source})
    continue()
  endif()
  add_executable(test_${name} ${source})
  target_link_libraries(test_${name} PRIVATE zest_host)
  target_compile_options(test_${name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME ${name} COMMAND test_${name})
endforeach()
