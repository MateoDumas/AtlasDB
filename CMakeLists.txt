cmake_minimum_required(VERSION 3.20)
project(AtlasDB VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ATLAS_BUILD_TESTS "Build the Catch2 test suite" ON)
option(ATLAS_BUILD_BENCHMARKS "Build the Google Benchmark suite" ON)
option(ATLAS_BUILD_EXAMPLES "Build example client applications" ON)
option(ATLAS_BUILD_FUZZERS "Build libFuzzer targets (Clang only)" OFF)

set(ATLAS_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. 'address,undefined' or 'thread' (GCC/Clang)")
if(ATLAS_SANITIZE)
  add_compile_options(-fsanitize=${ATLAS_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${ATLAS_SANITIZE})
endif()

if(ATLAS_BUILD_FUZZERS)
  # Coverage instrumentation for everything; the fuzz targets link the driver.
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(atlas_core STATIC
  src/common/value.cpp
  src/common/bytes.cpp
  src/common/result.cpp
  src/sql/lexer.cpp
  src/sql/ast.cpp
  src/sql/parser.cpp
  src/catalog/schema.cpp
  src/storage/file.cpp
  src/storage/page.cpp
  src/storage/pager.cpp
  src/storage/table_heap.cpp
  src/storage/row_codec.cpp
  src/storage/wal.cpp
  src/index/node_store.cpp
  src/txn/lock.cpp
  src/engine/expr.cpp
  src/engine/operators.cpp
  src/engine/planner.cpp
  src/engine/database.cpp
  src/engine/session.cpp
  src/net/socket.cpp
  src/net/protocol.cpp
  src/net/server.cpp
  src/net/client.cpp
)
target_include_directories(atlas_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(atlas_core PUBLIC ATLAS_VERSION="${PROJECT_VERSION}")
target_link_libraries(atlas_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(atlas_core PUBLIC ws2_32)
endif()

if(MSVC)
  target_compile_options(atlas_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(atlas_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Produce self-contained executables with MinGW so they run outside MSYS2.
function(atlas_static_link target)
  if(MINGW)
    target_link_options(${target} PRIVATE -static)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------
add_executable(atlasdb apps/cli.cpp)
target_link_libraries(atlasdb PRIVATE atlas_core)
atlas_static_link(atlasdb)

add_executable(atlasdb-server apps/server_main.cpp)
target_link_libraries(atlasdb-server PRIVATE atlas_core)
atlas_static_link(atlasdb-server)

if(ATLAS_BUILD_EXAMPLES)
  add_executable(client_example examples/client_example.cpp)
  target_link_libraries(client_example PRIVATE atlas_core)
  atlas_static_link(client_example)
endif()

# ---------------------------------------------------------------------------
# Third-party dependencies (fetched on demand)
# ---------------------------------------------------------------------------
if(ATLAS_BUILD_TESTS OR ATLAS_BUILD_BENCHMARKS)
  include(FetchContent)
endif()

if(ATLAS_BUILD_TESTS)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(Catch2)

  add_executable(atlas_tests
    tests/test_lexer_parser.cpp
    tests/test_btree.cpp
    tests/test_storage.cpp
    tests/test_sql.cpp
    tests/test_constraints.cpp
    tests/test_transactions.cpp
    tests/test_recovery.cpp
    tests/test_protocol.cpp
    tests/test_fuzz.cpp
  )
  target_link_libraries(atlas_tests PRIVATE atlas_core Catch2::Catch2WithMain)
  atlas_static_link(atlas_tests)

  enable_testing()
  add_test(NAME atlas_tests COMMAND atlas_tests)
endif()

if(ATLAS_BUILD_BENCHMARKS)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(benchmark)

  add_executable(atlas_bench bench/bench_main.cpp)
  target_link_libraries(atlas_bench PRIVATE atlas_core benchmark::benchmark_main)
  atlas_static_link(atlas_bench)
endif()

if(ATLAS_BUILD_FUZZERS)
  foreach(target fuzz_sql fuzz_protocol)
    add_executable(${target} fuzz/${target}.cpp)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(${target} PRIVATE atlas_core)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer)
  endforeach()
endif()
