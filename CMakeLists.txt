cmake_minimum_required(VERSION 3.28)
project(nuq VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(NUQ_SYSTEM_NNG "Link an installed NNG 2.x instead of building the pinned copy" OFF)
option(NUQ_SANITIZE "Build nuq with AddressSanitizer and UBSan" OFF)
option(NUQ_TESTS "Build the test suite" ON)

include(cmake/Nng.cmake)

set(CURSES_NEED_NCURSES TRUE)
# macOS ncurses already renders multibyte text. Linux ships a separate
# wide-character library for the dashboard's UTF-8 text.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(CURSES_NEED_WIDE TRUE)
endif()
find_package(Curses REQUIRED)

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

add_library(nuq_core STATIC
  ${NQ_SKILL_C}
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
  _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE _DARWIN_C_SOURCE
  NQ_VERSION="${PROJECT_VERSION}")
target_compile_options(nuq_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
target_link_libraries(nuq_core PUBLIC cjson ${CURSES_LIBRARIES} PRIVATE nuq_nng)
target_include_directories(nuq_core SYSTEM PRIVATE ${CURSES_INCLUDE_DIRS})

add_executable(nuq src/main.c)
target_compile_options(nuq PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
target_link_libraries(nuq PRIVATE nuq_core)

if(NUQ_SANITIZE)
  foreach(t nuq_core nuq)
    target_compile_options(${t} PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${t} PUBLIC -fsanitize=address,undefined)
  endforeach()
endif()

include(GNUInstallDirs)
install(TARGETS nuq RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(NUQ_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
