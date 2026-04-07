cmake_minimum_required(VERSION 3.16)
project(FlexQL VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native -funroll-loops")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

include_directories(
    ${CMAKE_SOURCE_DIR}/include/common
    ${CMAKE_SOURCE_DIR}/include/network
    ${CMAKE_SOURCE_DIR}/include/parser
    ${CMAKE_SOURCE_DIR}/include/query
    ${CMAKE_SOURCE_DIR}/include/storage
    ${CMAKE_SOURCE_DIR}/include/index
    ${CMAKE_SOURCE_DIR}/include/cache
    ${CMAKE_SOURCE_DIR}/include/server
)

find_package(Threads REQUIRED)

add_library(flexql_engine STATIC
    src/storage/storage.cpp
    src/storage/durable_log.cpp
    src/storage/persistence.cpp
    src/index/hash_index.cpp
    src/cache/lru_cache.cpp
    src/parser/parser.cpp
    src/query/executor.cpp
    src/network/network.cpp
    src/server/session.cpp
)
target_link_libraries(flexql_engine Threads::Threads)

add_library(flexql_client_lib STATIC
    src/client/flexql_client.cpp
    src/network/network.cpp
)
target_link_libraries(flexql_client_lib Threads::Threads)
set_target_properties(
    flexql_client_lib
    PROPERTIES
    OUTPUT_NAME flexql
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)

add_library(flexql_client_shared SHARED
    src/client/flexql_client.cpp
    src/network/network.cpp
)
target_link_libraries(flexql_client_shared Threads::Threads)
set_target_properties(
    flexql_client_shared
    PROPERTIES
    OUTPUT_NAME flexql
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
    POSITION_INDEPENDENT_CODE ON
)

add_executable(flexql-server
    src/server/server.cpp
)
target_link_libraries(flexql-server flexql_engine Threads::Threads)

add_executable(flexql-client
    src/client/repl.c
)
target_link_libraries(flexql-client flexql_client_lib Threads::Threads stdc++)

add_executable(flexql-test
    tests/test_main.cpp
)
target_link_libraries(flexql-test flexql_engine flexql_client_lib Threads::Threads)

add_executable(flexql-test-all
    tests/test_all_cases.cpp
)
target_link_libraries(flexql-test-all flexql_engine flexql_client_lib Threads::Threads)

add_executable(flexql-benchmark
    benchmarks/benchmark_flexql.cpp
)
target_link_libraries(flexql-benchmark flexql_client_lib Threads::Threads stdc++)

set_target_properties(
    flexql-server
    flexql-client
    flexql-test
    flexql-test-all
    flexql-benchmark
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
