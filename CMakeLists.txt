cmake_minimum_required(VERSION 3.15)
project(LockFreeBPlusTree CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
if(ENABLE_TSAN)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread -g")
endif()

# Output binaries to build/bin
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---- Dependencies ----
find_package(GTest REQUIRED)
find_package(benchmark QUIET)

# ---- Library target (shared between tests and benchmarks) ----
add_library(btree_lib STATIC
    src/btree.cpp
    src/version_chain.cpp
    src/mvcc.cpp
    src/epoch_reclamation.cpp
)
target_include_directories(btree_lib PUBLIC include)

# ---- Week 1: single-threaded B+Tree tests ----
add_executable(test_btree_basic
    tests/test_btree_basic.cpp
)
target_include_directories(test_btree_basic PRIVATE include)
target_link_libraries(test_btree_basic PRIVATE btree_lib GTest::GTest GTest::Main)

# ---- Week 2: MVCC layer tests ----
add_executable(test_mvcc
    tests/test_mvcc.cpp
)
target_include_directories(test_mvcc PRIVATE include)
target_link_libraries(test_mvcc PRIVATE btree_lib GTest::GTest GTest::Main)

# ---- Benchmark target (only built if Google Benchmark is found) ----
if(benchmark_FOUND)
    add_executable(benchmark_btree
        benchmarks/benchmark_btree.cpp
    )
    target_include_directories(benchmark_btree PRIVATE include)
    target_link_libraries(benchmark_btree PRIVATE btree_lib benchmark::benchmark pthread)
endif()

# ---- Week 3: concurrent / EBR tests ----
find_package(Threads REQUIRED)
add_executable(test_concurrent
    tests/test_concurrent.cpp
)
target_include_directories(test_concurrent PRIVATE include)
target_link_libraries(test_concurrent PRIVATE
    btree_lib GTest::GTest GTest::Main Threads::Threads)

# Enable CTest integration
include(GoogleTest)
gtest_discover_tests(test_btree_basic)
gtest_discover_tests(test_mvcc)
gtest_discover_tests(test_concurrent)
