add_executable(test_core test_core.c)
target_link_libraries(test_core PRIVATE nuq_core)
target_compile_options(test_core PRIVATE -Wall -Wextra)
add_test(NAME core COMMAND test_core ${CMAKE_CURRENT_BINARY_DIR})

# Stand-in for a full-screen agent; see fake_tui.c.
add_executable(fake_tui fake_tui.c)
# cfmakeraw() is hidden by -std=c11 on glibc without this.
target_compile_definitions(fake_tui PRIVATE _DEFAULT_SOURCE _DARWIN_C_SOURCE)

set(NQ_TEST_ENV
  "NQ=$<TARGET_FILE:nuq>"
  "FAKE_TUI=$<TARGET_FILE:fake_tui>"
  "FAKE_AGENT=${CMAKE_CURRENT_SOURCE_DIR}/fake_agent.sh"
  # Leak reports from short-lived commands would fail every test on Linux.
  "ASAN_OPTIONS=detect_leaks=0")

# nuq is used from zsh and from bash, so every integration script is run
# by each of them (and by plain sh where one is installed).
foreach(shell sh bash zsh)
  find_program(NQ_SHELL_${shell} ${shell})
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
  endforeach()
endforeach()
