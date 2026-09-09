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
endif()
