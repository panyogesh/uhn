# CMakeLists.txt for FlexSDR Library Test
cmake_minimum_required(VERSION 3.10)

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk)

# Find UHD
find_package(UHD REQUIRED)

# Test executable
add_executable(test_flexsdr_lib
    test_flexsdr_lib.cpp
    flexsdr_lib.cpp
)

# Include directories
target_include_directories(test_flexsdr_lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}           # For common_lib.h
    ${CMAKE_SOURCE_DIR}/include           # For FlexSDR headers
    ${CMAKE_SOURCE_DIR}/src/generated_proto  # For protobuf headers
    ${UHD_INCLUDE_DIRS}
    ${DPDK_INCLUDE_DIRS}
)

# Compile definitions
target_compile_definitions(test_flexsdr_lib PRIVATE
    ${DPDK_CFLAGS_OTHER}
)

# Compiler options
target_compile_options(test_flexsdr_lib PRIVATE
    -std=c++17
    -Wall
    -Wextra
)

# Link libraries
target_link_libraries(test_flexsdr_lib PRIVATE
    flexsdr_device
    flexsdr_transport
    flexsdr_config
    ${UHD_LIBRARIES}
    ${DPDK_LIBRARIES}
    pthread
)

# Installation
install(TARGETS test_flexsdr_lib
    RUNTIME DESTINATION bin
)

# Print build information
message(STATUS "FlexSDR Library Test Configuration:")
message(STATUS "  UHD Include: ${UHD_INCLUDE_DIRS}")
message(STATUS "  UHD Libraries: ${UHD_LIBRARIES}")
message(STATUS "  DPDK Include: ${DPDK_INCLUDE_DIRS}")
message(STATUS "  DPDK Libraries: ${DPDK_LIBRARIES}")
