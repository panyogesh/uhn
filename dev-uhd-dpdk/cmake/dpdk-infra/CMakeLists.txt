cmake_minimum_required(VERSION 3.18)
project(dpdk_infra_sidecar CXX)

# ---------- Basics ----------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)

# ---------- Repo root (we're configuring from dev-uhd-dpdk/cmake/dpdk-infra) ----------
get_filename_component(REPO_ROOT "${CMAKE_SOURCE_DIR}/../.." ABSOLUTE)
message(STATUS "Repo root (for sidecar): ${REPO_ROOT}")

# ---------- Dependencies ----------
find_package(PkgConfig REQUIRED)
# Your system exports 'libdpdk' (public) and 'libdpdk-libs' (internal). Use libdpdk.
pkg_check_modules(DPDK REQUIRED libdpdk)
find_package(yaml-cpp REQUIRED)
find_package(Threads REQUIRED)

# DPDK includes/libs from pkg-config
include_directories(${DPDK_INCLUDE_DIRS})
link_directories(${DPDK_LIBRARY_DIRS})

# Sanitize libs: pkg-config may inject -Wl,--as-needed; drop it for our test target
set(DPDK_LIBS_SANITIZED ${DPDK_LIBRARIES})
list(FILTER DPDK_LIBS_SANITIZED EXCLUDE REGEX "^-Wl,--as-needed$")

message(STATUS "DPDK include dirs: ${DPDK_INCLUDE_DIRS}")
message(STATUS "DPDK library dirs: ${DPDK_LIBRARY_DIRS}")
message(STATUS "DPDK libs (raw)  : ${DPDK_LIBRARIES}")

# ---------- ISA helper to satisfy DPDK intrinsics on GCC/Clang ----------
include(CheckCXXCompilerFlag)
function(apply_dpdk_isa target)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    return()
  endif()
  if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i.86")
    return()
  endif()
  set(_isa "")
  check_cxx_compiler_flag("-march=x86-64-v2" HAS_X86V2)
  if(HAS_X86V2)
    set(_isa "-march=x86-64-v2")
  else()
    check_cxx_compiler_flag("-mssse3" HAS_SSSE3)
    if(HAS_SSSE3)
      set(_isa "-mssse3")
    endif()
  endif()
  if(_isa)
    target_compile_options(${target} PRIVATE ${_isa})
    message(STATUS "Applied ${_isa} to ${target}")
  endif()
endfunction()

# ---------- Source files (exact per your tree) ----------
set(CONF_CPP      "${REPO_ROOT}/src/conf/config_params.cpp")
set(EAL_CPP       "${REPO_ROOT}/src/transport/eal_bootstrap.cpp")
set(PRIMARY_CPP   "${REPO_ROOT}/src/transport/flexsdr_primary.cpp")
set(SECONDARY_CPP "${REPO_ROOT}/src/transport/flexsdr_secondary.cpp")

# Per-file existence checks (clear error messages)
if(NOT EXISTS "${CONF_CPP}")
  message(FATAL_ERROR "Missing required source: ${CONF_CPP}")
endif()
if(NOT EXISTS "${EAL_CPP}")
  message(FATAL_ERROR "Missing required source: ${EAL_CPP}")
endif()
if(NOT EXISTS "${PRIMARY_CPP}")
  message(FATAL_ERROR "Missing required source: ${PRIMARY_CPP}")
endif()
if(NOT EXISTS "${SECONDARY_CPP}")
  message(FATAL_ERROR "Missing required source: ${SECONDARY_CPP}")
endif()

# Test source: if repo has tests/test_dpdk_infra.cpp use it, else use sidecar copy
set(TEST_INFRA_CPP "${REPO_ROOT}/test/test_dpdk_infra.cpp")
if(NOT EXISTS "${TEST_INFRA_CPP}")
  set(TEST_INFRA_CPP "${CMAKE_SOURCE_DIR}/test_dpdk_infra.cpp")
endif()
set(BUILD_TEST_INFRA OFF)
if(EXISTS "${TEST_INFRA_CPP}")
  set(BUILD_TEST_INFRA ON)
endif()

message(STATUS "Found sources:")
message(STATUS "  config_params.cpp : ${CONF_CPP}")
message(STATUS "  eal_bootstrap.cpp : ${EAL_CPP}")
message(STATUS "  primary           : ${PRIMARY_CPP}")
message(STATUS "  secondary         : ${SECONDARY_CPP}")
message(STATUS "  test_dpdk_infra   : ${TEST_INFRA_CPP} (build=${BUILD_TEST_INFRA})")

# ---------- Public include root ----------
set(FLEXSDR_INC "${REPO_ROOT}/include")

# ---------- Libraries ----------
add_library(flexsdr_conf STATIC "${CONF_CPP}")
target_include_directories(flexsdr_conf PUBLIC "${FLEXSDR_INC}")
target_link_libraries(flexsdr_conf PUBLIC yaml-cpp Threads::Threads)
apply_dpdk_isa(flexsdr_conf)

add_library(flexsdr_eal STATIC "${EAL_CPP}")
target_include_directories(flexsdr_eal PUBLIC "${FLEXSDR_INC}" ${DPDK_INCLUDE_DIRS})
target_link_libraries(flexsdr_eal PUBLIC ${DPDK_LIBS_SANITIZED} Threads::Threads)
apply_dpdk_isa(flexsdr_eal)

add_library(flexsdr_primary STATIC "${PRIMARY_CPP}")
target_include_directories(flexsdr_primary PUBLIC "${FLEXSDR_INC}" ${DPDK_INCLUDE_DIRS})
target_link_libraries(flexsdr_primary PUBLIC flexsdr_conf flexsdr_eal ${DPDK_LIBS_SANITIZED} Threads::Threads)
apply_dpdk_isa(flexsdr_primary)

add_library(flexsdr_secondary STATIC "${SECONDARY_CPP}")
target_include_directories(flexsdr_secondary PUBLIC "${FLEXSDR_INC}" ${DPDK_INCLUDE_DIRS})
target_link_libraries(flexsdr_secondary PUBLIC flexsdr_conf flexsdr_eal ${DPDK_LIBS_SANITIZED} Threads::Threads)
apply_dpdk_isa(flexsdr_secondary)

# ---------- Test executable (debug-leaning flags; loud logs) ----------
add_executable(test_dpdk_infra
  "${CMAKE_SOURCE_DIR}/test_dpdk_infra.cpp"   # sidecar copy
)

target_include_directories(test_dpdk_infra PRIVATE
  "${REPO_ROOT}/include"
  ${DPDK_INCLUDE_DIRS}
)

# Keep DPDK DSOs live (so constructors run & tailqs register)
target_link_options(test_dpdk_infra PRIVATE -Wl,--no-as-needed)

# Export symbols so backtrace has names
target_link_options(test_dpdk_infra PRIVATE -rdynamic)

# Debug-leaning flags for the test utility
target_compile_options(test_dpdk_infra PRIVATE
  -Wall -Wextra -Wno-pedantic -Wno-unused-parameter
  -g -O1 -fno-omit-frame-pointer
)

# Optional: AddressSanitizer for fast catches (ON by default for test)
option(ENABLE_ASAN "Enable AddressSanitizer for test_dpdk_infra" ON)
if(ENABLE_ASAN)
  target_compile_options(test_dpdk_infra PRIVATE -fsanitize=address)
  target_link_options(test_dpdk_infra PRIVATE -fsanitize=address)
endif()

target_link_libraries(test_dpdk_infra
  PRIVATE
    flexsdr_eal
    flexsdr_primary
    flexsdr_secondary
    flexsdr_conf
    yaml-cpp
    ${DPDK_LIBS_SANITIZED}
    Threads::Threads
)

# Let the loader find librte_*.so without env tweaks
set_target_properties(test_dpdk_infra PROPERTIES
  BUILD_RPATH   "${DPDK_LIBRARY_DIRS}"
  INSTALL_RPATH "${DPDK_LIBRARY_DIRS}"
)

# Apply ISA flags for DPDK intrinsics
apply_dpdk_isa(test_dpdk_infra)

# ---------- Warnings ----------
foreach(tgt IN ITEMS flexsdr_conf flexsdr_eal flexsdr_primary flexsdr_secondary test_dpdk_infra)
  if(TARGET ${tgt})
    target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wno-pedantic -Wno-unused-parameter)
  endif()
endforeach()

message(STATUS "Sidecar build type: ${CMAKE_BUILD_TYPE}")
