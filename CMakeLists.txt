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

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_library(lob_core INTERFACE)
target_include_directories(
    lob_core
    INTERFACE
        ${PROJECT_SOURCE_DIR}/include
)
target_compile_features(lob_core INTERFACE cxx_std_20)

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

include(GoogleTest)
gtest_discover_tests(lob_tests)
