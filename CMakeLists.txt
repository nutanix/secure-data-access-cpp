# Copyright (c) 2026 Nutanix Inc. All rights reserved.
#
# Build for the Secure Data Access C++ SDK. gRPC and protobuf are expected to
# be provided by the environment (system, vcpkg, or Conan) and discovered via
# find_package. Point CMake at them with, for example:
#   cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix)"

cmake_minimum_required(VERSION 3.16)

project(secure_data_access_cpp VERSION 1.0.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Dependencies provided by the environment.
find_package(Threads REQUIRED)
# Prefer protobuf config packages when present, but fall back to CMake's
# FindProtobuf module on environments where CONFIG packages are unavailable.
find_package(protobuf CONFIG QUIET)
if(protobuf_FOUND)
  set(SDA_PROTOBUF_TARGET protobuf::libprotobuf)
else()
  find_package(Protobuf REQUIRED)
  if(TARGET protobuf::libprotobuf)
    set(SDA_PROTOBUF_TARGET protobuf::libprotobuf)
  elseif(TARGET Protobuf::libprotobuf)
    set(SDA_PROTOBUF_TARGET Protobuf::libprotobuf)
  else()
    message(FATAL_ERROR "Protobuf found but no libprotobuf imported target")
  endif()
endif()
find_package(gRPC CONFIG REQUIRED)

# Strict flags for the SDK's own code. Not applied to the vendored proto stubs,
# which are generated and may trip individual warnings across toolchains.
set(SDK_STRICT_FLAGS -Wall -Wextra -Werror)

# Build examples/tests by default only when this project is top-level.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(SDA_BUILD_BY_DEFAULT ON)
else()
  set(SDA_BUILD_BY_DEFAULT OFF)
endif()

# ---------------------------------------------------------------------------
# Vendored gRPC/protobuf stubs (checked in under generated/).
# ---------------------------------------------------------------------------
add_library(vdisk_proto STATIC
  generated/vdisk.pb.cc
  generated/vdisk.grpc.pb.cc)
target_include_directories(vdisk_proto PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/generated>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/secure_data_access/generated>)
target_link_libraries(vdisk_proto PUBLIC
  gRPC::grpc++
  ${SDA_PROTOBUF_TARGET})
target_compile_options(vdisk_proto PRIVATE
  $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-fvisibility=hidden>)
set_target_properties(vdisk_proto PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION 0)

# ---------------------------------------------------------------------------
# The SDK library.
# ---------------------------------------------------------------------------
add_library(secure_data_access STATIC
  src/secure_data_access.cc
  src/api/error_code.cc
  src/api/read_region.cc
  src/api/read_regions_request.cc
  src/api/write_region.cc
  src/api/write_regions_request.cc
  src/api/vdisk_client_config.cc
  src/api/vdisk_clients.cc
  src/internal/grpc_vdisk_client.cc
  src/internal/grpc_vdisk_client_support.cc)
target_include_directories(secure_data_access
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(secure_data_access PUBLIC
  vdisk_proto
  gRPC::grpc++
  ${SDA_PROTOBUF_TARGET}
  Threads::Threads)
target_compile_options(secure_data_access PRIVATE ${SDK_STRICT_FLAGS})
target_compile_options(secure_data_access PRIVATE
  $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-fvisibility=hidden>)
set_target_properties(secure_data_access PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION 0)

# ---------------------------------------------------------------------------
# Examples.
# ---------------------------------------------------------------------------
option(SDA_BUILD_EXAMPLES "Build the bundled examples" ${SDA_BUILD_BY_DEFAULT})
if(SDA_BUILD_EXAMPLES)
  add_executable(backup_read_example examples/backup_read_example.cc)
  target_link_libraries(backup_read_example PRIVATE secure_data_access)
  target_compile_options(backup_read_example PRIVATE ${SDK_STRICT_FLAGS})

  add_executable(restore_write_example examples/restore_write_example.cc)
  target_link_libraries(restore_write_example PRIVATE secure_data_access)
  target_compile_options(restore_write_example PRIVATE ${SDK_STRICT_FLAGS})
endif()

# ---------------------------------------------------------------------------
# Tests (GoogleTest).
# ---------------------------------------------------------------------------
option(SDA_BUILD_TESTS "Build the smoke tests" ${SDA_BUILD_BY_DEFAULT})
if(SDA_BUILD_TESTS)
  find_package(GTest CONFIG REQUIRED)
  enable_testing()
  add_executable(sda_smoke_tests
    test/request_and_config_validation_test.cc
    test/error_code_mapping_test.cc
    test/byte_budget_test.cc
    test/recovered_chunk_payload_ownership_test.cc
    test/write_reader_start_failure_test.cc
    test/build_write_arg_ownership_test.cc
    test/read_reactor_test.cc
    test/grpc_vdisk_client_test_hooks.cc)
  target_include_directories(sda_smoke_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(sda_smoke_tests PRIVATE
    secure_data_access
    vdisk_proto
    GTest::gtest_main)
  target_compile_options(sda_smoke_tests PRIVATE ${SDK_STRICT_FLAGS})

  # read_reactor_test.cc starts an in-process TLS server. That needs a POSIX
  # host (mkdtemp, /tmp) and the openssl executable. The other smoke tests
  # do not. When either is missing the TLS cases skip; they do not fail
  # configuration.
  if(UNIX)
    find_program(SDA_OPENSSL_EXECUTABLE openssl)
  endif()
  if(UNIX AND SDA_OPENSSL_EXECUTABLE)
    target_compile_definitions(sda_smoke_tests PRIVATE
      "SDA_OPENSSL_EXECUTABLE=\"${SDA_OPENSSL_EXECUTABLE}\"")
    message(STATUS
      "Read reactor TLS tests enabled (${SDA_OPENSSL_EXECUTABLE})")
  else()
    message(STATUS
      "Read reactor TLS tests skipped: need a POSIX host and openssl")
  endif()

  # Register the whole suite as one ctest target. Deliberately not using
  # gtest_discover_tests: that runs the binary at build time to enumerate
  # cases, which needlessly couples the build to gRPC static initialization.
  add_test(NAME sda_smoke_tests COMMAND sda_smoke_tests)
endif()

# ---------------------------------------------------------------------------
# Install and package export.
# ---------------------------------------------------------------------------
install(TARGETS secure_data_access vdisk_proto
  EXPORT secure_data_accessTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(FILES
  generated/vdisk.pb.h
  generated/vdisk.grpc.pb.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/secure_data_access/generated)

install(EXPORT secure_data_accessTargets
  FILE secure_data_accessTargets.cmake
  NAMESPACE secure_data_access::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/secure_data_access)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/secure_data_accessConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/secure_data_accessConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/secure_data_access)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/secure_data_accessConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/secure_data_accessConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/secure_data_accessConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/secure_data_access)
