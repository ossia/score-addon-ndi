cmake_minimum_required(VERSION 3.13 FATAL_ERROR)
project(score_addon_ndi LANGUAGES CXX)
if(NOT TARGET score_lib_base)
  include("${SCORE_SOURCE_DIR}/cmake/ScoreExternalAddon.cmake")
else()
  if(NOT TARGET score_plugin_gfx)
    return()
  endif()
  if(NOT TARGET score_plugin_media)
    return()
  endif()
endif()

if(EMSCRIPTEN)
  return()
endif()

# General initialization
score_common_setup()

# Creation of the library
add_library(score_addon_ndi
  Ndi/InputNode.hpp
  Ndi/InputFactory.hpp
  Ndi/OutputNode.hpp
  Ndi/OutputFactory.hpp
  Ndi/Loader.hpp

  Ndi/InputNode.cpp
  Ndi/InputFactory.cpp
  Ndi/OutputNode.cpp
  Ndi/OutputFactory.cpp
  Ndi/Loader.cpp

  score_addon_ndi.hpp
  score_addon_ndi.cpp
)

target_include_directories(score_addon_ndi PRIVATE 3rdparty/NDI/include)

# Link
target_link_libraries(score_addon_ndi
  PUBLIC
    score_plugin_engine score_plugin_media score_plugin_gfx
    avformat avutil swscale
)

# Target-specific options
setup_score_plugin(score_addon_ndi)

# --- tests --------------------------------------------------------------------
# The frame description needs no GPU, no NDI runtime and no score, so it is
# testable on its own. Gated on SCORE_TESTING, which only the coverage job sets,
# so packaging builds do not carry it.
if(SCORE_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/PixelFormatTest.cpp")
  add_executable(score_addon_ndi_pixel_format_test tests/PixelFormatTest.cpp)
  target_include_directories(score_addon_ndi_pixel_format_test
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/NDI/include")
  target_link_libraries(score_addon_ndi_pixel_format_test PRIVATE avutil swscale)
  target_compile_features(score_addon_ndi_pixel_format_test PRIVATE cxx_std_20)

  if(COMMAND add_test)
    add_test(NAME score_addon_ndi_pixel_format
             COMMAND score_addon_ndi_pixel_format_test)
  endif()

  # The loopback test sends through the real SDK and reads the frame back, so it
  # needs the NDI runtime at link time rather than the dlopen the addon itself
  # uses. The runtime is not redistributable and is absent from the CI images, so
  # look for it and only wire the test up where it exists -- self-hosted runners
  # and developer machines have it. It also exits 77 when the runtime refuses to
  # initialise, so a machine that has the library but cannot use it skips rather
  # than fails.
  find_library(NDI_RUNTIME_LIBRARY
    NAMES ndi libndi
    HINTS
      ENV NDI_RUNTIME_DIR_V6
      ENV NDI_RUNTIME_DIR_V5
      ENV NDI_SDK_DIR
    PATH_SUFFIXES lib lib/x86_64-linux-gnu lib64
    PATHS /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu /opt/ossia-sdk-x86_64/ndi/lib
  )

  if(NDI_RUNTIME_LIBRARY AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/LoopbackTest.cpp")
    message(STATUS "score-addon-ndi: NDI runtime found (${NDI_RUNTIME_LIBRARY}), loopback test enabled")
    add_executable(score_addon_ndi_loopback_test tests/LoopbackTest.cpp)
    target_include_directories(score_addon_ndi_loopback_test
      PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/NDI/include")
    target_link_libraries(score_addon_ndi_loopback_test
      PRIVATE avutil swscale "${NDI_RUNTIME_LIBRARY}")
    target_compile_features(score_addon_ndi_loopback_test PRIVATE cxx_std_20)

    if(COMMAND add_test)
      add_test(NAME score_addon_ndi_loopback COMMAND score_addon_ndi_loopback_test)
      set_tests_properties(score_addon_ndi_loopback PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 120
        RUN_SERIAL TRUE
        LABELS "ndi;hardware")
    endif()
  else()
    message(STATUS "score-addon-ndi: no NDI runtime found, loopback test skipped")
  endif()
endif()
