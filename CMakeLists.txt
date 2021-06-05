cmake_minimum_required(VERSION 3.13 FATAL_ERROR)
project(score_addon_ndi LANGUAGES CXX)

if(NOT TARGET score_lib_base)
  include("${SCORE_SOURCE_DIR}/cmake/ScoreExternalAddon.cmake")
else()
  if(NOT TARGET score_plugin_gfx)
    return()
  endif()
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
    avcodec avformat avdevice swresample swscale postproc avutil
)

# Target-specific options
setup_score_plugin(score_addon_ndi)
