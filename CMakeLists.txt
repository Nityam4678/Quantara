cmake_minimum_required(VERSION 3.20)

project(LimitOrderBook
        VERSION 1.0.0
        DESCRIPTION "Low-latency limit order book matching engine"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Tuning for the host CPU. On by default because the benchmark numbers in the
# README were produced with it; turn it off for portable binaries.
option(LOB_NATIVE_ARCH "Build with -march=native" ON)

# ---------------------------------------------------------------------------
# Engine
# ---------------------------------------------------------------------------
add_library(lob STATIC
    src/order_pool.cpp
    src/price_level.cpp
    src/order_book.cpp)

target_include_directories(lob PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(MSVC)
    target_compile_options(lob PUBLIC /W4)
else()
    target_compile_options(lob PUBLIC -Wall -Wextra -Wpedantic)
    if(LOB_NATIVE_ARCH)
        target_compile_options(lob PUBLIC -march=native)
    endif()
endif()

# ---------------------------------------------------------------------------
# Benchmark
# ---------------------------------------------------------------------------
add_executable(lob_bench src/main.cpp)
target_link_libraries(lob_bench PRIVATE lob)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

foreach(suite pool_smoke level_smoke book_smoke)
    add_executable(${suite} tests/${suite}.cpp)
    target_link_libraries(${suite} PRIVATE lob)
    add_test(NAME ${suite} COMMAND ${suite})
endforeach()
