cmake_minimum_required(VERSION 3.14)
project(rdma_framework VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build options
option(RDMA_ENABLE_LOGGING "Enable logging (disabled for benchmarks)" ON)
option(RDMA_BUILD_EXAMPLES "Build examples" ON)
option(RDMA_BUILD_TESTS "Build tests" ON)
option(RDMA_USE_SOFTROCE "Enable SoftRoCE support for development" ON)

# Find required packages
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(IBVerbs REQUIRED)
find_package(RDMACM REQUIRED)
find_package(Threads REQUIRED)

# Optional NUMA support
find_library(NUMA_LIBRARY numa)
if(NUMA_LIBRARY)
    add_definitions(-DRDMA_HAS_NUMA)
    message(STATUS "NUMA support enabled")
else()
    message(STATUS "NUMA support disabled (libnuma not found)")
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG -march=native")

if(RDMA_ENABLE_LOGGING)
    add_definitions(-DRDMA_ENABLE_LOGGING)
endif()

# Main library
add_library(rdma_framework STATIC
    src/common/status.cpp
    src/common/logging.cpp
    src/core/device.cpp
    src/core/protection_domain.cpp
    src/core/memory_region.cpp
    src/core/completion_queue.cpp
    src/core/queue_pair.cpp
    src/memory/buffer_pool.cpp
    src/memory/hugepage_allocator.cpp
    src/transport/connection.cpp
    src/transport/connection_manager.cpp
    src/transport/tcp_handshake.cpp
    src/transport/flow_control.cpp
    src/rpc/rpc_slot.cpp
    src/rpc/rpc_endpoint.cpp
    src/rpc/completion_dispatcher.cpp
    src/reactor/reactor.cpp
)

target_include_directories(rdma_framework PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rdma_framework PUBLIC
    IBVerbs::IBVerbs
    RDMACM::RDMACM
    Threads::Threads
)

if(NUMA_LIBRARY)
    target_link_libraries(rdma_framework PUBLIC ${NUMA_LIBRARY})
endif()

# Examples
if(RDMA_BUILD_EXAMPLES)
    add_executable(rdma_server examples/server.cpp)
    target_link_libraries(rdma_server rdma_framework)

    add_executable(rdma_client examples/client.cpp)
    target_link_libraries(rdma_client rdma_framework)

    add_executable(kv_server examples/kv_server.cpp)
    target_link_libraries(kv_server rdma_framework)

    add_executable(kv_client examples/kv_client.cpp)
    target_link_libraries(kv_client rdma_framework)
endif()

# Tests
if(RDMA_BUILD_TESTS)
    enable_testing()
    # TODO: Add test targets
endif()
