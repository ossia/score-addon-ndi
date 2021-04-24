project(score_addon_ndi LANGUAGES CXX)

# General initialization
score_common_setup()

# Source files
set(HDRS
  Ndi/InputNode.hpp
  Ndi/InputFactory.hpp
  Ndi/OutputNode.hpp
  Ndi/OutputFactory.hpp
  Ndi/Loader.hpp

  score_addon_ndi.hpp
)
set(SRCS
  Ndi/InputNode.cpp
  Ndi/InputFactory.cpp
  Ndi/OutputNode.cpp
  Ndi/OutputFactory.cpp
  Ndi/Loader.cpp

  score_addon_ndi.cpp
)


# Creation of the library
add_library(${PROJECT_NAME} ${SRCS} ${HDRS})
target_include_directories(${PROJECT_NAME} PRIVATE 3rdparty/NDI/include)

# Code generation
score_generate_command_list_file(${PROJECT_NAME} "${HDRS}")

# Link
target_link_libraries(${PROJECT_NAME}
  PUBLIC
    score_plugin_engine score_plugin_media score_plugin_gfx
    avcodec avformat avdevice swresample swscale postproc avutil
)
# Target-specific options
setup_score_plugin(${PROJECT_NAME})
