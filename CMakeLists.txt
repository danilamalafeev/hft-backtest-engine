cmake_minimum_required(VERSION 3.20)

project(
    lob_backtester
    VERSION 0.1.0
    DESCRIPTION "Institutional-grade LOB backtesting engine scaffold"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_Declare(
    googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
FetchContent_MakeAvailable(googlebenchmark)

add_library(lob_core STATIC
    src/csv_parser.cpp
    src/order_book.cpp
)
target_include_directories(
    lob_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)
target_compile_features(lob_core PUBLIC cxx_std_20)

enable_testing()

add_executable(lob_tests
    tests/test_lob.cpp
)

target_link_libraries(
    lob_tests
    PRIVATE
        lob_core
        GTest::gtest_main
)

add_executable(lob_benchmarks
    benchmarks/bench_order_book.cpp
)

target_link_libraries(
    lob_benchmarks
    PRIVATE
        lob_core
        benchmark::benchmark
        benchmark::benchmark_main
)

if(APPLE)
    target_link_options(lob_benchmarks PRIVATE LINKER:-no_warn_duplicate_libraries)
endif()

add_executable(lob_backtest
    src/main_backtest.cpp
)

target_link_libraries(
    lob_backtest
    PRIVATE
        lob_core
)

include(GoogleTest)
gtest_discover_tests(lob_tests)

add_custom_target(
    tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS lob_tests
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
