# ---- Basics ---------------------------------------------------------------
cmake_minimum_required(VERSION 3.18)
project(flexsdr LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Prefer /usr/local at runtime (you already ran ldconfig, but this helps dev runs)
list(APPEND CMAKE_INSTALL_RPATH "/usr/local/lib" "/usr/local/lib/x86_64-linux-gnu")
list(APPEND CMAKE_BUILD_RPATH   "/usr/local/lib" "/usr/local/lib/x86_64-linux-gnu")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)

# ---- Dependencies ---------------------------------------------------------
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(yaml-cpp REQUIRED)              # system yaml-cpp

# DPDK via pkg-config (imports proper usage requirements, including -include rte_config.h)
# DPDK via pkg-config (portable across CMake versions)
pkg_check_modules(DPDK REQUIRED libdpdk)

if(NOT TARGET PkgConfig::libdpdk)
  add_library(PkgConfig::libdpdk INTERFACE IMPORTED)

  # Include & lib dirs
  target_include_directories(PkgConfig::libdpdk INTERFACE ${DPDK_INCLUDE_DIRS})
  target_link_directories(PkgConfig::libdpdk  INTERFACE ${DPDK_LIBRARY_DIRS})

  # Actual DPDK libs (rte_*)
  target_link_libraries(PkgConfig::libdpdk INTERFACE ${DPDK_LIBRARIES})

  # Extra cflags from pkg-config (e.g., -include rte_config.h)
  if(DEFINED DPDK_CFLAGS_OTHER AND NOT DPDK_CFLAGS_OTHER STREQUAL "")
    separate_arguments(_DPDK_CFLAGS_OTHER NATIVE_COMMAND "${DPDK_CFLAGS_OTHER}")
    target_compile_options(PkgConfig::libdpdk INTERFACE ${_DPDK_CFLAGS_OTHER})
  endif()

  # Extra link flags if any (e.g., --as-needed)
  if(DEFINED DPDK_LDFLAGS_OTHER AND NOT DPDK_LDFLAGS_OTHER STREQUAL "")
    separate_arguments(_DPDK_LDFLAGS_OTHER NATIVE_COMMAND "${DPDK_LDFLAGS_OTHER}")
    target_link_options(PkgConfig::libdpdk INTERFACE ${_DPDK_LDFLAGS_OTHER})
  endif()
endif()

# UHD: prefer CMake config (UHD::UHD); fallback to pkg-config
set(HAVE_UHD_TARGET FALSE)
find_package(UHD QUIET CONFIG)
if(TARGET UHD::UHD)
  set(HAVE_UHD_TARGET TRUE)
else()
  pkg_check_modules(UHD_PC QUIET uhd)
  if(UHD_PC_FOUND)
    add_library(UHD::UHD INTERFACE IMPORTED)
    target_include_directories(UHD::UHD INTERFACE ${UHD_PC_INCLUDE_DIRS})
    target_link_directories(UHD::UHD INTERFACE   ${UHD_PC_LIBRARY_DIRS})
    target_link_libraries(UHD::UHD INTERFACE     ${UHD_PC_LIBRARIES})
    set(HAVE_UHD_TARGET TRUE)
  endif()
endif()
if(NOT HAVE_UHD_TARGET)
  message(FATAL_ERROR "UHD not found (neither CMake config nor pkg-config). Install to /usr/local.")
endif()

# Protobuf/gRPC (you already generate sources; just link libs)
# --- Protobuf (portable across old/new CMake/protobuf) ---------------------
find_package(Protobuf QUIET)  # try CMake package first

unset(PROTOBUF_TARGET)

# If any canonical target exists, use it.
if(TARGET protobuf::libprotobuf)
  set(PROTOBUF_TARGET protobuf::libprotobuf)
elseif(TARGET Protobuf::libprotobuf)
  set(PROTOBUF_TARGET Protobuf::libprotobuf)
elseif(TARGET Protobuf::protobuf)
  set(PROTOBUF_TARGET Protobuf::protobuf)
else()
  # Fallback to pkg-config (works with system protobuf 3.6.1)
  pkg_check_modules(PB REQUIRED protobuf)
  add_library(Protobuf::libprotobuf INTERFACE IMPORTED)
  target_include_directories(Protobuf::libprotobuf INTERFACE ${PB_INCLUDE_DIRS})
  target_link_directories(Protobuf::libprotobuf  INTERFACE ${PB_LIBRARY_DIRS})
  target_link_libraries(Protobuf::libprotobuf    INTERFACE ${PB_LIBRARIES})

  # pass-thru extra flags if provided by pkg-config
  if(PB_CFLAGS_OTHER)
    separate_arguments(_PB_CFLAGS_OTHER NATIVE_COMMAND "${PB_CFLAGS_OTHER}")
    target_compile_options(Protobuf::libprotobuf INTERFACE ${_PB_CFLAGS_OTHER})
  endif()
  if(PB_LDFLAGS_OTHER)
    separate_arguments(_PB_LDFLAGS_OTHER NATIVE_COMMAND "${PB_LDFLAGS_OTHER}")
    target_link_options(Protobuf::libprotobuf INTERFACE ${_PB_LDFLAGS_OTHER})
  endif()

  set(PROTOBUF_TARGET Protobuf::libprotobuf)
endif()

message(STATUS "Using protobuf target: ${PROTOBUF_TARGET}")
# --- gRPC (portable) -------------------------------------------------------
find_package(gRPC QUIET)
unset(GRPCPP_TARGET)

if(TARGET gRPC::grpc++)
  set(GRPCPP_TARGET gRPC::grpc++)
else()
  # pull both C++ and C core via pkg-config
  pkg_check_modules(GRPCPP REQUIRED grpc++)
  pkg_check_modules(GRPCCORE REQUIRED grpc)

  add_library(gRPC::grpc++ INTERFACE IMPORTED)
  target_include_directories(gRPC::grpc++ INTERFACE
    ${GRPCPP_INCLUDE_DIRS} ${GRPCCORE_INCLUDE_DIRS})
  target_link_directories(gRPC::grpc++ INTERFACE
    ${GRPCPP_LIBRARY_DIRS} ${GRPCCORE_LIBRARY_DIRS})
  # include BOTH libgrpc++ and libgrpc (+ any deps pkg-config lists)
  target_link_libraries(gRPC::grpc++ INTERFACE
    ${GRPCPP_LIBRARIES} ${GRPCCORE_LIBRARIES})

  # pass-thru any extra flags
  if(GRPCPP_CFLAGS_OTHER)
    separate_arguments(_GRPCPP_CFLAGS_OTHER NATIVE_COMMAND "${GRPCPP_CFLAGS_OTHER}")
    target_compile_options(gRPC::grpc++ INTERFACE ${_GRPCPP_CFLAGS_OTHER})
  endif()
  if(GRPCPP_LDFLAGS_OTHER)
    separate_arguments(_GRPCPP_LDFLAGS_OTHER NATIVE_COMMAND "${GRPCPP_LDFLAGS_OTHER}")
    target_link_options(gRPC::grpc++ INTERFACE ${_GRPCPP_LDFLAGS_OTHER})
  endif()
endif()

set(GRPCPP_TARGET gRPC::grpc++)
message(STATUS "Using gRPC++ target: ${GRPCPP_TARGET}")

# ---- Project include roots ------------------------------------------------
set(PROJ_INCLUDE_DIR      ${CMAKE_SOURCE_DIR}/include)
set(PROJ_INCLUDE_CONF     ${CMAKE_SOURCE_DIR}/include/conf)
set(PROJ_INCLUDE_TRAN     ${CMAKE_SOURCE_DIR}/include/transport)
set(PROJ_INCLUDE_DEV      ${CMAKE_SOURCE_DIR}/include/device)
set(PROJ_INCLUDE_RUNTIME  ${CMAKE_SOURCE_DIR}/include/runtime)
set(PROJ_INCLUDE_WORKERS  ${CMAKE_SOURCE_DIR}/include/workers)
set(GENERATED_PROTO_DIR   ${CMAKE_SOURCE_DIR}/src/generated_proto)
set(GRPC_CLIENT_INC       ${CMAKE_SOURCE_DIR}/src/device/grpc_client)

# ---- Libraries ------------------------------------------------------------
add_library(flexsdr_cfg
  src/conf/config_params.cpp
)
target_include_directories(flexsdr_cfg PUBLIC
  ${PROJ_INCLUDE_DIR} ${PROJ_INCLUDE_CONF} ${PROJ_INCLUDE_TRAN} ${PROJ_INCLUDE_DEV}
)
target_link_libraries(flexsdr_cfg PUBLIC yaml-cpp PkgConfig::libdpdk Threads::Threads)

add_library(flexsdr_eal
  src/transport/eal_bootstrap.cpp
)
target_include_directories(flexsdr_eal PUBLIC
  ${PROJ_INCLUDE_DIR} ${PROJ_INCLUDE_TRAN}
)
target_link_libraries(flexsdr_eal PUBLIC PkgConfig::libdpdk Threads::Threads)

add_library(flexsdr_primary
  src/transport/flexsdr_primary.cpp
)
target_include_directories(flexsdr_primary PUBLIC
  ${PROJ_INCLUDE_DIR} ${PROJ_INCLUDE_TRAN}
)
target_link_libraries(flexsdr_primary PUBLIC flexsdr_cfg PkgConfig::libdpdk Threads::Threads)

add_library(flexsdr_secondary
  src/transport/flexsdr_secondary.cpp
)
target_include_directories(flexsdr_secondary PUBLIC
  ${PROJ_INCLUDE_DIR} ${PROJ_INCLUDE_TRAN}
)
target_link_libraries(flexsdr_secondary PUBLIC flexsdr_cfg PkgConfig::libdpdk Threads::Threads)

add_library(flexsdr_grpc
  ${GENERATED_PROTO_DIR}/flexsdr.pb.cc
  ${GENERATED_PROTO_DIR}/flexsdr.grpc.pb.cc
  src/device/grpc_client/flexsdr_client_impl.cpp
)
target_include_directories(flexsdr_grpc PUBLIC
   ${GENERATED_PROTO_DIR}
   ${GRPC_CLIENT_INC}
   ${PROJ_INCLUDE_DIR}
)

target_link_libraries(flexsdr_grpc
  PUBLIC
    ${PROTOBUF_TARGET}       # e.g. protobuf::libprotobuf
    ${GRPCPP_TARGET}         # e.g. gRPC::grpc++
)

add_library(flexsdr_device
  src/device/flexsdr_device.cpp
  src/device/flexsdr_rx_streamer.cpp
  src/device/flexsdr_tx_streamer.cpp
  src/device/registry.cpp
)
target_include_directories(flexsdr_device PUBLIC
  ${PROJ_INCLUDE_DIR} ${PROJ_INCLUDE_DEV} ${PROJ_INCLUDE_TRAN} ${PROJ_INCLUDE_CONF}
  ${GENERATED_PROTO_DIR} ${GRPC_CLIENT_INC}
)
target_link_libraries(flexsdr_device PUBLIC
  UHD::UHD
  flexsdr_cfg
  flexsdr_grpc
  PkgConfig::libdpdk
  Threads::Threads
)

# ---- Executables ----------------------------------------------------------
add_executable(test_flexsdr_factory test/test_flexsdr_factory.cpp)
target_include_directories(test_flexsdr_factory PRIVATE
  ${PROJ_INCLUDE_DIR} ${PROJ_INCLUDE_DEV}
  ${GENERATED_PROTO_DIR}
)
target_link_libraries(test_flexsdr_factory PRIVATE
  flexsdr_device
  flexsdr_eal
  flexsdr_secondary
  UHD::UHD
  Threads::Threads
)

# FlexSDR Library Test (OAI integration test)
add_executable(test_flexsdr_lib
  test/test_flexsdr_lib.cpp
  test/flexsdr_lib.cpp
)
target_include_directories(test_flexsdr_lib PRIVATE
  ${CMAKE_SOURCE_DIR}/test              # For common_lib.h
  ${PROJ_INCLUDE_DIR}
  ${PROJ_INCLUDE_DEV}
  ${PROJ_INCLUDE_TRAN}
  ${PROJ_INCLUDE_CONF}
  ${GENERATED_PROTO_DIR}
)
target_link_libraries(test_flexsdr_lib PRIVATE
  flexsdr_device
  flexsdr_eal
  flexsdr_secondary
  flexsdr_cfg
  UHD::UHD
  PkgConfig::libdpdk
  Threads::Threads
)

# ---- Warnings & (optional) ISA tweaks -------------------------------------
foreach(tgt IN ITEMS
  flexsdr_cfg flexsdr_eal flexsdr_primary flexsdr_secondary
  flexsdr_grpc flexsdr_device
  test_flexsdr_factory test_flexsdr_lib)
  target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wno-pedantic)
endforeach()
