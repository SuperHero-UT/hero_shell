function(superhero_resolve_ncurses)
  if(TARGET superhero_ncurses)
    return()
  endif()

  set(_src "${CMAKE_SOURCE_DIR}/external/ncurses")

  if(NOT EXISTS "${_src}/configure")
    message(FATAL_ERROR "Bundled ncurses source not found: ${_src}")
    message(FATAL_ERROR "Please run 'git submodule update --init --recursive'")
  endif()

  message(STATUS "Using bundled Readline from ${_src}")

  include(ExternalProject)

  set(_build "${CMAKE_BINARY_DIR}/_deps_build/ncurses")
  set(_prefix "${CMAKE_BINARY_DIR}/_deps_install/ncurses")
  set(_incdir "${_prefix}/include")
  set(_libdir "${_prefix}/lib")
  file(MAKE_DIRECTORY "${_incdir}" "${_libdir}")

  set(_dep_make "make")
  if(SUPERHERO_DEPS_PARALLELISM)
    set(_dep_make_parallel "-j${SUPERHERO_DEPS_PARALLELISM}")
  else()
    set(_dep_make_parallel "-j")
  endif()

  ExternalProject_Add(
    superhero_ext_ncurses
    SOURCE_DIR "${_src}"
    BINARY_DIR "${_build}"
    INSTALL_DIR "${_prefix}"
    UPDATE_COMMAND ""
    CONFIGURE_COMMAND "${_src}/configure" --prefix=${_prefix}
    BUILD_COMMAND ${_dep_make} ${_dep_make_parallel}
    INSTALL_COMMAND
      make install
    INSTALL_BYPRODUCTS "${_libdir}/libncurses.a"
    USES_TERMINAL_CONFIGURE ON
    USES_TERMINAL_BUILD ON
    USES_TERMINAL_INSTALL ON)

  add_library(superhero_ncurses INTERFACE)

  target_include_directories(superhero_ncurses SYSTEM INTERFACE "${_incdir}")

  target_link_libraries(superhero_ncurses INTERFACE "${_libdir}/libncurses.a")

  add_dependencies(superhero_ncurses superhero_ext_ncurses)
endfunction()
