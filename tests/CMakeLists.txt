add_executable(test_core test_core.c)
target_link_libraries(test_core PRIVATE nuq_core)
if(MSVC)
  target_compile_options(test_core PRIVATE ${NQ_WARN_FLAGS})
else()
  target_compile_options(test_core PRIVATE -Wall -Wextra)
endif()
add_test(NAME core COMMAND test_core ${CMAKE_CURRENT_BINARY_DIR})

# Stand-in for a full-screen agent; see fake_tui.c.
add_executable(fake_tui fake_tui.c)
# cfmakeraw() is hidden by -std=c11 on glibc without this.
target_compile_definitions(fake_tui PRIVATE _DEFAULT_SOURCE _DARWIN_C_SOURCE)

if(WIN32)
  # The manifest makes the fakes UTF-8 processes like nuq itself.
  target_compile_options(fake_tui PRIVATE ${NQ_WARN_FLAGS})
  target_compile_definitions(fake_tui PRIVATE ${NQ_PLATFORM_DEFS})
  target_link_libraries(fake_tui PRIVATE nuq_manifest)

  # A pane cannot run a shell script on Windows, so the line-mode agent is a
  # program there. Named fake_agent.sh.exe so its agent kind (the file name
  # without .exe) and its config key are the same as on POSIX.
  add_executable(fake_agent fake_agent.c)
  set_target_properties(fake_agent PROPERTIES OUTPUT_NAME fake_agent.sh)
  target_compile_options(fake_agent PRIVATE ${NQ_WARN_FLAGS})
  target_compile_definitions(fake_agent PRIVATE ${NQ_PLATFORM_DEFS})
  target_link_libraries(fake_agent PRIVATE nuq_manifest)
  set(NQ_FAKE_AGENT $<TARGET_FILE:fake_agent>)
else()
  set(NQ_FAKE_AGENT ${CMAKE_CURRENT_SOURCE_DIR}/fake_agent.sh)
endif()

set(NQ_TEST_ENV
  "NQ=$<TARGET_FILE:nuq>"
  "FAKE_TUI=$<TARGET_FILE:fake_tui>"
  "FAKE_AGENT=${NQ_FAKE_AGENT}"
  # Leak reports from short-lived commands would fail every test on Linux.
  "ASAN_OPTIONS=detect_leaks=0")

if(WIN32)
  # The scripts run under Git Bash. find_program(bash) could return the WSL
  # launcher in System32, which runs a Linux shell that cannot start nuq.exe,
  # so only Git's own bash is accepted. Git's sh.exe is the same bash, and
  # zsh does not exist natively, so bash is the only shell on Windows.
  find_program(NQ_GIT git)
  set(_nq_git_bins "$ENV{ProgramW6432}/Git/bin" "$ENV{ProgramFiles}/Git/bin")
  if(NQ_GIT)
    get_filename_component(_nq_git_dir "${NQ_GIT}" DIRECTORY)
    list(PREPEND _nq_git_bins "${_nq_git_dir}/../bin")
  endif()
  find_program(NQ_SHELL_bash bash PATHS ${_nq_git_bins} NO_DEFAULT_PATH)
  set(NQ_SHELLS bash)

  # psmux supplies tmux.exe. winget puts it under a per-package directory
  # that a shell opened before the install does not have on PATH.
  file(GLOB _nq_psmux_dirs "$ENV{LOCALAPPDATA}/Microsoft/WinGet/Packages/marlocarlo.psmux_*")
  find_program(NQ_TMUX tmux PATHS ${_nq_psmux_dirs} "$ENV{LOCALAPPDATA}/Microsoft/WinGet/Links")
  if(NQ_TMUX)
    get_filename_component(_nq_tmux_dir "${NQ_TMUX}" DIRECTORY)
    file(TO_NATIVE_PATH "${_nq_tmux_dir}" _nq_tmux_dir)
    set(NQ_TEST_PATH_MOD "PATH=path_list_append:${_nq_tmux_dir}")
  endif()
else()
  # nuq is used from zsh and from bash, so every integration script is run
  # by each of them (and by plain sh where one is installed).
  set(NQ_SHELLS sh bash zsh)
  foreach(shell ${NQ_SHELLS})
    find_program(NQ_SHELL_${shell} ${shell})
  endforeach()
endif()

foreach(shell ${NQ_SHELLS})
  if(NOT NQ_SHELL_${shell})
    continue()
  endif()
  foreach(script bus grid hooks)
    add_test(NAME ${script}-${shell}
      COMMAND ${NQ_SHELL_${shell}} ${CMAKE_CURRENT_SOURCE_DIR}/${script}.sh)
    set_tests_properties(${script}-${shell} PROPERTIES
      ENVIRONMENT "${NQ_TEST_ENV}"
      SKIP_RETURN_CODE 77
      TIMEOUT 180)
    if(NQ_TEST_PATH_MOD)
      set_tests_properties(${script}-${shell} PROPERTIES
        ENVIRONMENT_MODIFICATION "${NQ_TEST_PATH_MOD}")
    endif()
  endforeach()
endforeach()
