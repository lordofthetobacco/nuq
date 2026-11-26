# Provides the INTERFACE target `nuq_nng`.
#
# Default: build NNG 2.1.0 from the upstream git repository as a static
# library. Distro packages ship NNG 1.x, whose API differs from 2.x.

add_library(nuq_nng INTERFACE)

if(NUQ_SYSTEM_NNG)
  find_package(nng 2 CONFIG REQUIRED)
  target_link_libraries(nuq_nng INTERFACE nng::nng)
else()
  include(FetchContent)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(NNG_TESTS OFF CACHE BOOL "" FORCE)
  set(NNG_TOOLS OFF CACHE BOOL "" FORCE)
  set(NNG_ENABLE_NNGCAT OFF CACHE BOOL "" FORCE)
  set(NNG_ENABLE_TLS OFF CACHE BOOL "" FORCE)
  # nuq only speaks ipc://. Leaving the rest out also drops NNG's HTTP code,
  # which is the only part of it that warns with current compilers.
  set(NNG_ENABLE_HTTP OFF CACHE BOOL "" FORCE)
  foreach(t INPROC TCP WS FDC UDP)
    set(NNG_TRANSPORT_${t} OFF CACHE BOOL "" FORCE)
  endforeach()
  FetchContent_Declare(nng
    GIT_REPOSITORY https://github.com/nanomsg/nng.git
    GIT_TAG v2.1.0
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
    SYSTEM)
  FetchContent_MakeAvailable(nng)
  # The in-tree target exports include/ (<nng/nng.h>) and NNG_STATIC_LIB.
  target_link_libraries(nuq_nng INTERFACE nng)
endif()
