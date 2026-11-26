cmake_minimum_required(VERSION 3.28)
project(nuq VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(NUQ_SYSTEM_NNG "Link an installed NNG 2.x instead of building the pinned copy" OFF)
option(NUQ_SANITIZE "Build nuq with AddressSanitizer and UBSan" OFF)
option(NUQ_TESTS "Build the test suite" ON)
# The release workflow passes the version it is about to tag. Anything else
# reports the project version above.
set(NUQ_VERSION "" CACHE STRING "Version nuq reports instead of the project version")
if(NOT NUQ_VERSION)
  set(NUQ_VERSION "${PROJECT_VERSION}")
endif()

# Windows: link the C runtime statically, so nuq.exe runs on machines without
# the Visual C++ redistributable and inside panes whatever their PATH.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(cmake/Nng.cmake)

if(WIN32)
  # Native Windows has no curses; PDCursesMod (wincon port) stands in.
  include(cmake/PDCursesMod.cmake)
  set(CURSES_LIBRARIES nuq_pdcurses)
  set(CURSES_INCLUDE_DIRS "")
else()
  set(CURSES_NEED_NCURSES TRUE)
  # macOS ncurses already renders multibyte text. Linux ships a separate
  # wide-character library for the dashboard's UTF-8 text.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CURSES_NEED_WIDE TRUE)
  endif()
  find_package(Curses REQUIRED)
endif()

# Warning flags for nuq's own targets (third-party code keeps its defaults).
if(MSVC)
  # C4244/C4267: implicit integer narrowing, which -Wall -Wextra does not
  # report either (that is -Wconversion). C4706: assignment used as a
  # condition, written deliberately in parenthesised loop conditions.
  # The _CRT_* defines silence "use the _s / _-prefixed variant" advice for
  # the standard and POSIX names the code base uses on purpose.
  set(NQ_WARN_FLAGS /W4 /wd4244 /wd4267 /wd4706)
  set(NQ_PLATFORM_DEFS _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
else()
  set(NQ_WARN_FLAGS -Wall -Wextra -Wpedantic -Wshadow)
  set(NQ_PLATFORM_DEFS _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE _DARWIN_C_SOURCE)
endif()

add_library(cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(cjson SYSTEM PUBLIC third_party/cjson)

# The agent skill is compiled in, so `nuq skills install` works from an
# installed binary with no source tree around.
set(NQ_SKILL_MD ${CMAKE_CURRENT_SOURCE_DIR}/skills/nuq/SKILL.md)
set(NQ_SKILL_C ${CMAKE_CURRENT_BINARY_DIR}/skill_data.c)
add_custom_command(
  OUTPUT ${NQ_SKILL_C}
  COMMAND ${CMAKE_COMMAND} -DINPUT=${NQ_SKILL_MD} -DOUTPUT=${NQ_SKILL_C} -DSYMBOL=nq_skill_md
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
  DEPENDS ${NQ_SKILL_MD} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
  COMMENT "Embedding skills/nuq/SKILL.md"
  VERBATIM)

if(WIN32)
  set(NQ_COMPAT_C src/compat_win.c)
else()
  set(NQ_COMPAT_C src/compat_posix.c)
endif()

# Every Windows executable carries a manifest that makes the process code
# page UTF-8, so argv, getenv, fopen and CreateProcessA all speak UTF-8.
# Interface sources reach every executable that links nuq_core.
add_library(nuq_manifest INTERFACE)
if(WIN32)
  target_sources(nuq_manifest INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/nuq.manifest)
endif()

add_library(nuq_core STATIC
  ${NQ_SKILL_C}
  ${NQ_COMPAT_C}
  src/util.c
  src/paths.c
  src/bus.c
  src/proto.c
  src/store.c
  src/daemon.c
  src/tmux.c
  src/inject.c
  src/adapters.c
  src/launch.c
  src/pane.c
  src/hook.c
  src/cli.c
  src/extras.c
  src/dash.c
)
target_include_directories(nuq_core PUBLIC src)
target_compile_definitions(nuq_core PUBLIC
  ${NQ_PLATFORM_DEFS}
  NQ_VERSION="${NUQ_VERSION}")
target_compile_options(nuq_core PRIVATE ${NQ_WARN_FLAGS})
target_link_libraries(nuq_core PUBLIC cjson ${CURSES_LIBRARIES} nuq_manifest PRIVATE nuq_nng)
if(CURSES_INCLUDE_DIRS)
  target_include_directories(nuq_core SYSTEM PRIVATE ${CURSES_INCLUDE_DIRS})
endif()

add_executable(nuq src/main.c)
target_compile_options(nuq PRIVATE ${NQ_WARN_FLAGS})
target_link_libraries(nuq PRIVATE nuq_core)

if(NUQ_SANITIZE)
  foreach(t nuq_core nuq)
    if(MSVC)
      # MSVC has AddressSanitizer but no UBSan.
      target_compile_options(${t} PUBLIC /fsanitize=address)
    else()
      target_compile_options(${t} PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${t} PUBLIC -fsanitize=address,undefined)
    endif()
  endforeach()
endif()

include(GNUInstallDirs)
install(TARGETS nuq RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(NUQ_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
