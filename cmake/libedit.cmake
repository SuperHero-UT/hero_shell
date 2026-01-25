
include(ncurses)

function(superhero_resolve_readline)
  superhero_resolve_ncurses()
  if(TARGET superhero_readline)
    return()
  endif()

  set(_src "${CMAKE_SOURCE_DIR}/external/readline")
  if(NOT EXISTS "${_src}/configure")
    message(FATAL_ERROR "Bundled readline source not found: ${_src}")
  endif()

  message(STATUS "Using bundled Readline from ${_src}")

  include(ExternalProject)
  # find_package(Curses REQUIRED)

  set(_build "${CMAKE_BINARY_DIR}/_deps_build/readline")
  set(_prefix "${CMAKE_BINARY_DIR}/_deps_install/readline")
  set(_incdir "${_prefix}/include")
  set(_libdir "${_prefix}/lib")
  set(_dep_make "make")
  if(SUPERHERO_DEPS_PARALLELISM)
    set(_dep_make_parallel "-j${SUPERHERO_DEPS_PARALLELISM}")
  else()
    set(_dep_make_parallel "-j")
  endif()

  # configure 時点で空でも作っておく（存在チェック回避）
  file(MAKE_DIRECTORY "${_incdir}" "${_libdir}")

  ExternalProject_Add(
    superhero_ext_readline
    SOURCE_DIR "${_src}"
    BINARY_DIR "${_build}"
    INSTALL_DIR "${_prefix}"
    UPDATE_COMMAND ""
    CONFIGURE_COMMAND "${_src}/configure" --prefix=${_prefix}
    BUILD_COMMAND ${_dep_make} ${_dep_make_parallel} static
    INSTALL_COMMAND
      make install-static
    INSTALL_BYPRODUCTS "${_libdir}/libreadline.a"
    USES_TERMINAL_CONFIGURE ON
    USES_TERMINAL_BUILD ON
    USES_TERMINAL_INSTALL ON)

  # ---- 利用側が見る唯一のターゲット ----
  add_library(superhero_readline INTERFACE)

  target_include_directories(superhero_readline SYSTEM INTERFACE "${_incdir}")

  target_link_libraries(superhero_readline INTERFACE "${_libdir}/libreadline.a"
                                                     superhero_ncurses)


  # readline を使うターゲットが先に ExternalProject を実行するよう保証
  add_dependencies(superhero_ext_readline superhero_ext_ncurses)
  add_dependencies(superhero_readline superhero_ext_readline)
endfunction()

