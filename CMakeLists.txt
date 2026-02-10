cmake_minimum_required(VERSION 3.20)
project(StrikeMQ VERSION 0.1.4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(WITH_DPDK "Enable DPDK kernel bypass networking" OFF)
option(WITH_IOURING "Enable io_uring for storage I/O" OFF)
option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_BENCHMARKS "Build benchmarks" ON)
option(PORTABLE "Build portable binary (skip -march=native)" OFF)

# Platform detection
if(APPLE)
    message(STATUS "Building on macOS — DPDK disabled, using kqueue fallback")
    set(WITH_DPDK OFF CACHE BOOL "" FORCE)
    set(WITH_IOURING OFF CACHE BOOL "" FORCE)
    add_definitions(-DSTRIKE_PLATFORM_MACOS)
elseif(UNIX)
    message(STATUS "Building on Linux")
    add_definitions(-DSTRIKE_PLATFORM_LINUX)
endif()

# Compiler flags
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(APPLE)
        set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
    elseif(PORTABLE)
        set(CMAKE_CXX_FLAGS_RELEASE "-O3 -flto -DNDEBUG")
    else()
        set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -flto -DNDEBUG")
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -falign-functions=64")
    endif()
endif()

# Find DPDK (Linux only)
if(WITH_DPDK)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DPDK libdpdk)
    if(DPDK_FOUND)
        add_definitions(-DSTRIKE_WITH_DPDK)
        message(STATUS "DPDK found: ${DPDK_VERSION}")
    else()
        message(WARNING "DPDK not found, using fallback networking")
        set(WITH_DPDK OFF)
    endif()
endif()

# Find io_uring (Linux only)
if(WITH_IOURING)
    find_library(URING_LIB uring)
    if(URING_LIB)
        add_definitions(-DSTRIKE_WITH_IOURING)
    else()
        set(WITH_IOURING OFF)
    endif()
endif()

find_package(Threads REQUIRED)

# ─── Core Library ───
file(GLOB_RECURSE CORE_SOURCES
    src/core/*.cpp
    src/protocol/*.cpp
    src/storage/*.cpp
    src/utils/*.cpp
)

add_library(strikemq_core STATIC ${CORE_SOURCES})
target_include_directories(strikemq_core PUBLIC include)
target_link_libraries(strikemq_core PUBLIC Threads::Threads)

if(NOT APPLE)
    target_link_libraries(strikemq_core PUBLIC atomic)
endif()

# ─── Network Library ───
file(GLOB_RECURSE NETWORK_SOURCES src/network/*.cpp)
add_library(strikemq_network STATIC ${NETWORK_SOURCES})
target_include_directories(strikemq_network PUBLIC include)
target_link_libraries(strikemq_network PUBLIC strikemq_core)

if(WITH_DPDK AND DPDK_FOUND)
    target_include_directories(strikemq_network PUBLIC ${DPDK_INCLUDE_DIRS})
    target_link_libraries(strikemq_network PUBLIC ${DPDK_LIBRARIES})
endif()

if(WITH_IOURING AND URING_LIB)
    target_link_libraries(strikemq_network PUBLIC ${URING_LIB})
endif()

# ─── HTTP Library ───
file(GLOB_RECURSE HTTP_SOURCES src/http/*.cpp)
add_library(strikemq_http STATIC ${HTTP_SOURCES})
target_include_directories(strikemq_http PUBLIC include)
target_link_libraries(strikemq_http PUBLIC strikemq_core)

# ─── Main Executable ───
add_executable(strikemq src/main.cpp)
target_link_libraries(strikemq PRIVATE strikemq_network strikemq_http strikemq_core)

# ─── Tests ───
if(BUILD_TESTS)
    enable_testing()
    
    add_executable(strikemq_test_ring tests/ring_buffer_test.cpp)
    target_link_libraries(strikemq_test_ring PRIVATE strikemq_core)
    add_test(NAME ring_buffer_test COMMAND strikemq_test_ring)
    
    add_executable(strikemq_test_pool tests/memory_pool_test.cpp)
    target_link_libraries(strikemq_test_pool PRIVATE strikemq_core)
    add_test(NAME memory_pool_test COMMAND strikemq_test_pool)

    add_executable(strikemq_test_codec tests/kafka_codec_test.cpp)
    target_link_libraries(strikemq_test_codec PRIVATE strikemq_core)
    add_test(NAME kafka_codec_test COMMAND strikemq_test_codec)
endif()

# ─── Benchmarks ───
if(BUILD_BENCHMARKS)
    add_executable(strikemq_bench benchmarks/latency_bench.cpp)
    target_link_libraries(strikemq_bench PRIVATE strikemq_core)
endif()

# ─── Install ───
install(TARGETS strikemq RUNTIME DESTINATION bin)
install(DIRECTORY config/ DESTINATION etc/strikemq)
