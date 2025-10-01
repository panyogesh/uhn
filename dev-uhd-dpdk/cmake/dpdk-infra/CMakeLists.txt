cmake_minimum_required(VERSION 3.18)
project(dpdk_infra_sidecar CXX)

# Resolve repo root relative to this file: cmake/dpdk-infra/CMakeLists.txt -> repo root = ../..
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
message(STATUS "Repo root (for sidecar): ${REPO_ROOT}")

# Build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk)
find_package(yaml-cpp REQUIRED)           # provides yaml-cpp::yaml-cpp
find_package(Threads)

# Common flags
add_library(flexsdr_compile_options INTERFACE)
target_compile_features(flexsdr_compile_options INTERFACE cxx_std_17)
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(flexsdr_compile_options INTERFACE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# Include dirs
set(FLEXSDR_INC_DIR "${REPO_ROOT}/include")
include_directories(${FLEXSDR_INC_DIR})
include_directories(${DPDK_INCLUDE_DIRS})
link_directories(${DPDK_LIBRARY_DIRS})

# ---------- Libraries from your repo ----------

# Config loader
add_library(flexsdr_conf STATIC
  ${REPO_ROOT}/src/conf/config_params.cpp
)
target_link_libraries(flexsdr_conf
  PUBLIC yaml-cpp::yaml-cpp flexsdr_compile_options
)
target_include_directories(flexsdr_conf PUBLIC ${FLEXSDR_INC_DIR})

# EAL bootstrapper
add_library(flexsdr_eal STATIC
  ${REPO_ROOT}/src/eal_bootstrap.cpp
)
target_link_libraries(flexsdr_eal
  PUBLIC ${DPDK_LIBRARIES} flexsdr_compile_options
)
target_include_directories(flexsdr_eal PUBLIC ${FLEXSDR_INC_DIR} ${DPDK_INCLUDE_DIRS})

# Primary app infra
add_library(flexsdr_primary STATIC
  ${REPO_ROOT}/src/flxesdr_primary.cpp
)
target_link_libraries(flexsdr_primary
  PUBLIC flexsdr_conf flexsdr_eal ${DPDK_LIBRARIES} flexsdr_compile_options
)
target_include_directories(flexsdr_primary PUBLIC ${FLEXSDR_INC_DIR} ${DPDK_INCLUDE_DIRS})

# Secondary app infra
add_library(flexsdr_secondary STATIC
  ${REPO_ROOT}/src/flexsdr_secondary.cpp
)
target_link_libraries(flexsdr_secondary
  PUBLIC flexsdr_conf flexsdr_eal ${DPDK_LIBRARIES} flexsdr_compile_options
)
target_include_directories(flexsdr_secondary PUBLIC ${FLEXSDR_INC_DIR} ${DPDK_INCLUDE_DIRS})

# ---------- Test executable ----------
add_executable(test_dpdk_infra
  ${REPO_ROOT}/tests/test_dpdk_infra.cpp
)
target_link_libraries(test_dpdk_infra
  PRIVATE flexsdr_primary flexsdr_secondary flexsdr_conf flexsdr_eal
          yaml-cpp::yaml-cpp ${DPDK_LIBRARIES}
)
target_include_directories(test_dpdk_infra PRIVATE ${FLEXSDR_INC_DIR} ${DPDK_INCLUDE_DIRS})

# Extras some distros need explicitly
if (Threads_FOUND)
  target_link_libraries(test_dpdk_infra PRIVATE Threads::Threads)
endif()
find_library(LIBDL dl)
if (LIBDL)
  target_link_libraries(test_dpdk_infra PRIVATE ${LIBDL})
endif()
find_library(LIBNUMA numa)
if (LIBNUMA)
  target_link_libraries(test_dpdk_infra PRIVATE ${LIBNUMA})
endif()

message(STATUS "DPDK include dirs: ${DPDK_INCLUDE_DIRS}")
message(STATUS "DPDK libraries   : ${DPDK_LIBRARIES}")
message(STATUS "Sidecar build type: ${CMAKE_BUILD_TYPE}")
