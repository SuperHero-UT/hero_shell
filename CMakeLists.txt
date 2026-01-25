cmake_minimum_required(VERSION 3.15)
project(hero_shell VERSION 0.1.0 LANGUAGES CXX)


set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(ExternalProject)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-attributes>
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-return-type>
    $<$<COMPILE_LANGUAGE:C>:-Wno-array-bounds>
    $<$<COMPILE_LANGUAGE:C>:-Wno-unused-result>
  )
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wno-stringop-overflow>)
endif()

cmake_host_system_information(
  RESULT _nproc
  QUERY NUMBER_OF_LOGICAL_CORES
)

################################################################################
# gRPC and Protobuf
################################################################################
# GRPC
set(_grpc_src "${CMAKE_SOURCE_DIR}/external/grpc")
set(_grpc_bindir "${CMAKE_BINARY_DIR}/_deps/grpc-build")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(gRPC_INSTALL OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_CODEGEN ON CACHE BOOL "" FORCE)
set(gRPC_PROTOBUF_PROVIDER "module" CACHE STRING "" FORCE)
set(gRPC_ABSL_PROVIDER "module" CACHE STRING "" FORCE)
set(gRPC_RE2_PROVIDER "module" CACHE STRING "" FORCE)
set(gRPC_CARES_PROVIDER "module" CACHE STRING "" FORCE)
set(gRPC_ZLIB_PROVIDER "module" CACHE STRING "" FORCE)
set(gRPC_SSL_PROVIDER "module" CACHE STRING "" FORCE)
add_compile_definitions(CARES_NO_DEPRECATED)
set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(utf8_range_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(utf8_range_ENABLE_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory("${_grpc_src}" "${_grpc_bindir}" EXCLUDE_FROM_ALL)

set(SUPERHERO_PROTOC
    "$<TARGET_FILE:protobuf::protoc>"
    CACHE FILEPATH "vendored protoc" FORCE)
set(SUPERHERO_GRPC_CPP_PLUGIN
    "$<TARGET_FILE:grpc_cpp_plugin>"
    CACHE FILEPATH "vendored grpc_cpp_plugin" FORCE)

# Proto
add_subdirectory(proto)
set(_SUPERHERO_PROTO_TARGET "${PROJECT_NAME}_proto")

################################################################################
# NCURSES
################################################################################
set(_nc_src "${CMAKE_SOURCE_DIR}/external/ncurses")

if(NOT EXISTS "${_nc_src}/configure")
  message(FATAL_ERROR "Bundled ncurses source not found: ${_nc_src}")
  message(FATAL_ERROR "Please run 'git submodule update --init --recursive'")
endif()

message(STATUS "Using bundled Readline from ${_nc_src}")


set(_nc_build "${CMAKE_BINARY_DIR}/_deps_build/ncurses")
set(_nc_prefix "${CMAKE_BINARY_DIR}/_deps_install/ncurses")
set(_nc_incdir "${_nc_prefix}/include")
set(_nc_libdir "${_nc_prefix}/lib")

file(MAKE_DIRECTORY "${_nc_incdir}" "${_nc_libdir}")

ExternalProject_Add(
  superhero_ext_ncurses
  SOURCE_DIR "${_nc_src}"
  BINARY_DIR "${_nc_build}"
  INSTALL_DIR "${_nc_prefix}"
  UPDATE_COMMAND ""
  CONFIGURE_COMMAND
    "${_nc_src}/configure" --prefix=${_nc_prefix} --without-cxx --without-cxx-binding
  BUILD_COMMAND make -j8
  INSTALL_COMMAND
    make install
  INSTALL_BYPRODUCTS "${_nc_libdir}/libncurses.a"
  USES_TERMINAL_CONFIGURE ON
  USES_TERMINAL_BUILD ON
  USES_TERMINAL_INSTALL ON)

add_library(superhero_ncurses INTERFACE)
target_include_directories(superhero_ncurses SYSTEM INTERFACE "${_nc_incdir}")
target_link_libraries(superhero_ncurses INTERFACE "${_nc_libdir}/libncurses.a")
add_dependencies(superhero_ncurses superhero_ext_ncurses)

################################################################################
# Readline
################################################################################
set(_rl_src "${CMAKE_SOURCE_DIR}/external/libedit")

if(NOT EXISTS "${_rl_src}/configure")
  message(FATAL_ERROR "Bundled readline source not found: ${_rl_src}")
  message(FATAL_ERROR "Please run 'git submodule update --init --recursive'")
endif()
message(STATUS "Using bundled Readline from ${_rl_src}")

set(_rl_build "${CMAKE_BINARY_DIR}/_deps_build/readline")
set(_rl_prefix "${CMAKE_BINARY_DIR}/_deps_install/readline")
set(_rl_incdir "${_rl_prefix}/include")
set(_rl_libdir "${_rl_prefix}/lib")
file(MAKE_DIRECTORY "${_rl_incdir}" "${_rl_libdir}")

ExternalProject_Add(
  superhero_ext_readline
  SOURCE_DIR "${_rl_src}"
  BINARY_DIR "${_rl_build}"
  INSTALL_DIR "${_rl_prefix}"
  UPDATE_COMMAND ""
  CONFIGURE_COMMAND
    "${CMAKE_COMMAND}" -E env
      "CPPFLAGS=-I${_nc_incdir} -I${_nc_incdir}/ncurses"
      "LDFLAGS=-L${_nc_libdir}"
      "LIBS=-lncurses"
      "CFLAGS=${CMAKE_C_FLAGS} -Wno-unused-result"
    "${_rl_src}/configure" --prefix=${_rl_prefix} --enable-shared=no --enable-static=yes
  BUILD_COMMAND make -j8
  INSTALL_COMMAND
    make install
  INSTALL_BYPRODUCTS "${_rl_libdir}/libedit.a"
  USES_TERMINAL_CONFIGURE ON
  USES_TERMINAL_BUILD ON
  USES_TERMINAL_INSTALL ON)

add_library(superhero_readline INTERFACE)
target_include_directories(superhero_readline SYSTEM INTERFACE "${_rl_incdir}")
target_link_libraries(superhero_readline INTERFACE "${_rl_libdir}/libedit.a"
                                                   superhero_ncurses)
add_dependencies(superhero_ext_readline superhero_ext_ncurses)
add_dependencies(superhero_readline superhero_ext_readline)


################################################################################
# hero_shell
################################################################################
add_executable(hero_shell src/hero_shell.cc src/commands.cc src/completions.cc)
target_compile_features(hero_shell PRIVATE cxx_std_17)
# target_compile_options(hero_shell PRIVATE -Wall -Wextra -Wpedantic -Werror
#                                           -Wno-deprecated-declarations
#                                           -Wno-gcc-compat
#                                           -Wno-variadic-macro-arguments-omitted)
target_include_directories(hero_shell
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(
  hero_shell PRIVATE ${_SUPERHERO_PROTO_TARGET} superhero_readline)
if(APPLE)
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  target_link_libraries(hero_shell PUBLIC "${COREFOUNDATION_FRAMEWORK}")
endif()
if(UNIX AND NOT APPLE)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(hero_shell PRIVATE -static-libgcc -static-libstdc++)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_link_options(hero_shell PRIVATE -static-libstdc++)
  endif()
  target_link_options(hero_shell PRIVATE -static)
endif()

install(TARGETS hero_shell RUNTIME DESTINATION bin)
