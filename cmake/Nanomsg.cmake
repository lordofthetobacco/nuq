# Provides the INTERFACE target `nuq_nanomsg`.
#
# Default: build nanomsg 1.3.0 from the upstream git repository as a static
# library. Distro packages are too old to rely on (Debian/Ubuntu ship 1.1.5).

add_library(nuq_nanomsg INTERFACE)

if(NUQ_SYSTEM_NANOMSG)
  find_package(nanomsg CONFIG REQUIRED)
  target_link_libraries(nuq_nanomsg INTERFACE nanomsg)
  # Installed headers live under <nanomsg/...>.
  target_compile_definitions(nuq_nanomsg INTERFACE NQ_NN_INSTALLED_HEADERS)
else()
  include(FetchContent)
  set(NN_STATIC_LIB ON CACHE BOOL "" FORCE)
  set(NN_TESTS OFF CACHE BOOL "" FORCE)
  set(NN_TOOLS OFF CACHE BOOL "" FORCE)
  set(NN_ENABLE_NANOCAT OFF CACHE BOOL "" FORCE)
  set(NN_ENABLE_DOC OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(nanomsg
    GIT_REPOSITORY https://github.com/nanomsg/nanomsg.git
    GIT_TAG 1.3.0
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(nanomsg)
  target_link_libraries(nuq_nanomsg INTERFACE nanomsg)
  # The in-tree target exports no include path; headers are src/nn.h etc.
  target_include_directories(nuq_nanomsg SYSTEM INTERFACE ${nanomsg_SOURCE_DIR}/src)
endif()
