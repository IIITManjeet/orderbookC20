cmake_minimum_required(VERSION 3.20)
project(orderBookCpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(OB_COMPILE_OPTS
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-march=native>
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Debug>:-g3>
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)
set(OB_LINK_OPTS
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

add_library(order_book STATIC src/order_book.cpp)
target_include_directories(order_book PUBLIC include)
target_compile_options(order_book PRIVATE ${OB_COMPILE_OPTS})
target_link_options(order_book PRIVATE ${OB_LINK_OPTS})

add_executable(demo src/main.cpp)
target_link_libraries(demo PRIVATE order_book)
target_compile_options(demo PRIVATE ${OB_COMPILE_OPTS})
target_link_options(demo PRIVATE ${OB_LINK_OPTS})

option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_BENCH "Build benchmarks" ON)
option(BUILD_LIVE  "Build live Binance paper-trader" ON)

include(FetchContent)

if(BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(test_order_book tests/test_order_book.cpp)
    target_link_libraries(test_order_book PRIVATE order_book gtest_main)
    target_compile_options(test_order_book PRIVATE ${OB_COMPILE_OPTS})
    target_link_options(test_order_book PRIVATE ${OB_LINK_OPTS})

    include(GoogleTest)
    gtest_discover_tests(test_order_book)
endif()

if(BUILD_BENCH)
    FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.9.0
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)

    add_executable(bench_throughput bench/bench_throughput.cpp)
    target_link_libraries(bench_throughput PRIVATE order_book benchmark::benchmark)
    target_compile_options(bench_throughput PRIVATE ${OB_COMPILE_OPTS})
    target_link_options(bench_throughput PRIVATE ${OB_LINK_OPTS})
endif()

if(BUILD_LIVE)
    find_package(CURL REQUIRED)

    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(nlohmann_json)

    add_library(live_core STATIC
        src/feed.cpp
        src/trading_engine.cpp
    )
    target_include_directories(live_core PUBLIC include)
    target_link_libraries(live_core
        PUBLIC  order_book
        PRIVATE CURL::libcurl nlohmann_json::nlohmann_json
    )
    target_compile_options(live_core PRIVATE ${OB_COMPILE_OPTS})
    target_link_options(live_core PRIVATE ${OB_LINK_OPTS})

    add_executable(live_trade src/live_trade.cpp)
    target_link_libraries(live_trade PRIVATE live_core)
    target_compile_options(live_trade PRIVATE ${OB_COMPILE_OPTS})
    target_link_options(live_trade PRIVATE ${OB_LINK_OPTS})

    if(BUILD_TESTS)
        add_executable(test_live tests/test_live.cpp)
        target_link_libraries(test_live PRIVATE live_core gtest_main)
        target_compile_options(test_live PRIVATE ${OB_COMPILE_OPTS})
        target_link_options(test_live PRIVATE ${OB_LINK_OPTS})
        gtest_discover_tests(test_live)
    endif()
endif()
