# Provides the target `nuq_pdcurses` on Windows: PDCursesMod's console
# (wincon) port as a static library, built wide with UTF-8 forced so the
# dashboard's multibyte text goes through the same calls as with ncursesw.
# Its own CMake project builds demos and DLLs, so the sources are listed here.

include(FetchContent)
FetchContent_Declare(pdcursesmod
  GIT_REPOSITORY https://github.com/Bill-Gray/PDCursesMod.git
  GIT_TAG v4.5.4
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR do-not-add-subdirectory
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(pdcursesmod)

file(GLOB NQ_PDC_CORE ${pdcursesmod_SOURCE_DIR}/pdcurses/*.c)
file(GLOB NQ_PDC_PORT ${pdcursesmod_SOURCE_DIR}/wincon/*.c)
add_library(nuq_pdcurses STATIC ${NQ_PDC_CORE} ${NQ_PDC_PORT})
# These change the layout of chtype and cchar_t, so users of curses.h must
# see them too.
target_compile_definitions(nuq_pdcurses PUBLIC PDC_WIDE PDC_FORCE_UTF8)
target_compile_definitions(nuq_pdcurses PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
target_include_directories(nuq_pdcurses PRIVATE ${pdcursesmod_SOURCE_DIR}/wincon)
target_include_directories(nuq_pdcurses SYSTEM PUBLIC ${pdcursesmod_SOURCE_DIR})
target_link_libraries(nuq_pdcurses PRIVATE user32 advapi32 winmm)
