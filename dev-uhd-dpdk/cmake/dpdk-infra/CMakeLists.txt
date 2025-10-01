cmake_minimum_required(VERSION 3.18)
project(dpdk_infra_sidecar CXX)

# ---------- Repo root ----------
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
message(STATUS "Repo root (for sidecar): ${REPO_ROOT}")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------- Dependencies ----------
find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk)   # change to 'dpdk' if your pkg-config name differs

# yaml-cpp: support imported target or plain library
set(YAMLCPP_LIB "")
find_package(yaml-cpp QUIET)  # may define yaml-cpp::yaml-cpp
if(TARGET yaml-cpp::yaml-cpp)
  set(YAMLCPP_LIB yaml-cpp::yaml-cpp)
else()
  pkg_check_modules(YAMLCPP_PC QUIET yaml-cpp)
  if(YAMLCPP_PC_FOUND)
    include_directories(${YAMLCPP_PC_INCLUDE_DIRS})
    link_directories(${YAMLCPP_PC_LIBRARY_DIRS})
    set(YAMLCPP_LIB ${YAMLCPP_PC_LIBRARIES})
  else()
    find_library(YAMLCPP_LIB NAMES yaml-cpp)
    if(NOT YAMLCPP_LIB)
      message(FATAL_ERROR "yaml-cpp not found (no imported target, no pkg-config, no libyaml-cpp)")
    endif()
  endif()
endif()

find_package(Threads)

# ---------- Compile flags ----------
add_library(flexsdr_compile_options INTERFACE)
target_compile_features(flexsdr_compile_options INTERFACE cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(flexsdr_compile_options INTERFACE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
  )
endif()

# SIMD (fix for rte_memcpy SSSE3) + propagate DPDK pkg-config CFLAGS
set(SIMD_FLAGS "")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i.86")
    list(APPEND SIMD_FLAGS -mssse3)   # or -march=native
  endif()
endif()
if(DPDK_CFLAGS_OTHER)
  target_compile_options(flexsdr_compile_options INTERFACE ${DPDK_CFLAGS_OTHER})
endif()
if(SIMD_FLAGS)
  target_compile_options(flexsdr_compile_options INTERFACE ${SIMD_FLAGS})
endif()

# ---------- Includes & link dirs ----------
set(FLEXSDR_INC_DIR "${REPO_ROOT}/include")
include_directories(${FLEXSDR_INC_DIR})
include_directories(${DPDK_INCLUDE_DIRS})
link_directories(${DPDK_LIBRARY_DIRS})

# ---------- Robust file finder ----------
function(fsdr_find_one OUT_VAR)
  set(options)
  set(oneValueArgs NAME)
  cmake_parse_arguments(FS "${options}" "${oneValueArgs}" "" ${ARGN})
  if(NOT FS_NAME)
    message(FATAL_ERROR "fsdr_find_one(NAME <filename>) requires NAME")
  endif()

  set(_cands "${REPO_ROOT}/${FS_NAME}")

  file(GLOB _d1 "${REPO_ROOT}/*/${FS_NAME}")
  file(GLOB _d2 "${REPO_ROOT}/*/*/${FS_NAME}")
  file(GLOB _d3 "${REPO_ROOT}/*/*/*/${FS_NAME}")
  file(GLOB _d4 "${REPO_ROOT}/*/*/*/*/${FS_NAME}")
  file(GLOB _d5 "${REPO_ROOT}/*/*/*/*/*/${FS_NAME}")
  file(GLOB _d6 "${REPO_ROOT}/*/*/*/*/*/*/${FS_NAME}")

  list(APPEND _cands ${_d1} ${_d2} ${_d3} ${_d4} ${_d5} ${_d6})

  set(_hit "")
  foreach(path IN LISTS _cands)
    if(EXISTS "${path}")
      set(_hit "${path}")
      break()
    endif()
  endforeach()

  set(${OUT_VAR} "${_hit}" PARENT_SCOPE)
endfunction()

# ---------- Locate sources ----------
fsdr_find_one(CONF_CPP NAME "config_params.cpp")
fsdr_find_one(EAL_CPP NAME "eal_bootstrap.cpp")
fsdr_find_one(PRIMARY_CPP NAME "flexsdr_primary.cpp")
if(NOT PRIMARY_CPP)
  fsdr_find_one(PRIMARY_CPP NAME "flxesdr_primary.cpp")  # tolerate earlier typo
endif()
fsdr_find_one(SECONDARY_CPP NAME "flexsdr_secondary.cpp")

if(NOT CONF_CPP)
  message(FATAL_ERROR "config_params.cpp not found under ${REPO_ROOT}")
endif()
if(NOT EAL_CPP)
  message(FATAL_ERROR "eal_bootstrap.cpp not found under ${REPO_ROOT}")
endif()
if(NOT PRIMARY_CPP)
  message(FATAL_ERROR "flexsdr_primary.cpp (or flxesdr_primary.cpp) not found under ${REPO_ROOT}")
endif()
if(NOT SECONDARY_CPP)
  message(FATAL_ERROR "flexsdr_secondary.cpp not found under ${REPO_ROOT}")
endif()

# Test file: repo-level or sidecar-local
set(TEST_INFRA_CPP "${REPO_ROOT}/tests/test_dpdk_infra.cpp")
if(NOT EXISTS "${TEST_INFRA_CPP}")
  set(TEST_INFRA_CPP "${CMAKE_CURRENT_LIST_DIR}/test_dpdk_infra.cpp")
endif()

set(BUILD_TEST_INFRA OFF)
if(EXISTS "${TEST_INFRA_CPP}")
  set(BUILD_TEST_INFRA ON)
endif()

message(STATUS "Found sources:")
message(STATUS "  config_params.cpp   : ${CONF_CPP}")
message(STATUS "  eal_bootstrap.cpp   : ${EAL_CPP}")
message(STATUS "  primary impl        : ${PRIMARY_CPP}")
message(STATUS "  secondary impl      : ${SECONDARY_CPP}")
message(STATUS "  test_dpdk_infra.cpp : ${TEST_INFRA_CPP} (build=${BUILD_TEST_INFRA})")

# ---------- Libraries ----------
add_library(flexsdr_conf STATIC "${CONF_CPP}")
target_link_libraries(flexsdr_conf PUBLIC ${YAMLCPP_LIB} flexsdr_compile_options)
target_include_directories(flexsdr_conf PUBLIC ${FLEXSDR_INC_DIR})

add_library(flexsdr_eal STATIC "${EAL_CPP}")
target_link_libraries(flexsdr_eal PUBLIC ${DPDK_LIBRARIES} flexsdr_compile_options)
target_include_directories(flexsdr_eal PUBLIC ${FLEXSDR_INC_DIR} ${DPDK_INCLUDE_DIRS})

add_library(flexsdr_primary STATIC "${PRIMARY_CPP}")
target_link_libraries(flexsdr_primary
  PUBLIC flexsdr_conf flexsdr_eal ${DPDK_LIBRARIES} flexsdr_compile_options
)
target_include_directories(flexsdr_primary PUBLIC ${FLEXSDR_INC_DIR} ${DPDK_INCLUDE_DIRS})

add_library(flexsdr_secondary STATIC "${SECONDARY_CPP}")
target_link_libraries(flexsdr_secondary
  PUBLIC flexsdr_conf flexsdr_eal ${DPDK_LIBRARIES} flexsdr_compile_options
)
target_include_directories(flexsdr_secondary PUBLIC ${FLEXSDR_INC_DIR} ${DPDK_INCLUDE_DIRS})

# ---------- Test executable ----------
# ---------- Test executable ----------
if(BUILD_TEST_INFRA)
  add_executable(test_dpdk_infra "${TEST_INFRA_CPP}")
  target_include_directories(test_dpdk_infra PRIVATE ${FLEXSDR_INC_DIR} ${DPDK_INCLUDE_DIRS})

  # Prefer shared libdpdk if available
  find_library(DPDK_SHARED dpdk)
  if (DPDK_SHARED)
    message(STATUS "Linking test_dpdk_infra against shared ${DPDK_SHARED}")
    target_link_libraries(test_dpdk_infra
      PRIVATE flexsdr_primary flexsdr_secondary flexsdr_conf flexsdr_eal
              ${YAMLCPP_LIB} ${DPDK_SHARED})
  else()
    message(STATUS "Shared libdpdk not found; falling back to static libs list")
    # Keep constructors (tailqs) when falling back to static list
    target_link_options(test_dpdk_infra PRIVATE -Wl,--no-as-needed -Wl,--whole-archive)
    target_link_libraries(test_dpdk_infra
      PRIVATE flexsdr_primary flexsdr_secondary flexsdr_conf flexsdr_eal
              ${YAMLCPP_LIB} ${DPDK_LIBRARIES})
    target_link_options(test_dpdk_infra PRIVATE -Wl,--no-whole-archive)
  endif()

  if(Threads_FOUND)
    target_link_libraries(test_dpdk_infra PRIVATE Threads::Threads)
  endif()
  find_library(LIBDL dl)   
    if(LIBDL)
      target_link_libraries(test_dpdk_infra PRIVATE ${LIBDL})
    endif()
  find_library(LIBNUMA numa)
    if(LIBNUMA)
      target_link_libraries(test_dpdk_infra PRIVATE ${LIBNUMA})
    endif()
endif()

message(STATUS "DPDK include dirs: ${DPDK_INCLUDE_DIRS}")
message(STATUS "DPDK libraries   : ${DPDK_LIBRARIES}")
message(STATUS "Sidecar build type: ${CMAKE_BUILD_TYPE}")
