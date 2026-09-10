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
  Ndi/ReadbackPool.hpp

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
  target_link_libraries(score_addon_ndi_pixel_format_test PRIVATE)
  target_compile_features(score_addon_ndi_pixel_format_test PRIVATE cxx_std_20)

  if(COMMAND add_test)
    add_test(NAME score_addon_ndi_pixel_format
             COMMAND score_addon_ndi_pixel_format_test)
  endif()

  # NOTE ON RED TESTS. Three of the tests below are RED ON PURPOSE against
  # 0746bcb: score_addon_ndi_readback_pool (the dropped notify in
  # requestStop()), score_addon_ndi_frame_description (descriptions returned as
  # valid with a null p_data, degenerate dimensions, and sws_scale's failure
  # ignored) and score_addon_ndi_async_send (a buffer freed while the SDK still
  # owns it). They are not marked WILL_FAIL: each binary pins several separate
  # defects, so a WILL_FAIL that stays satisfied while only some of them are
  # fixed would hide the rest. If they need to be green before the fixes land,
  # declare them in cmake/ScoreExpectedRedGuard.cmake rather than weakening the
  # assertions.
  #
  # The ownership state machine needs no GPU and no SDK either, so the same
  # applies. It interposes pthread_cond_wait to place the sender thread in the
  # window where requestStop()'s notify can be dropped, hence the dl link.
  add_executable(score_addon_ndi_readback_pool_test tests/ReadbackPoolTest.cpp)
  target_include_directories(score_addon_ndi_readback_pool_test
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(score_addon_ndi_readback_pool_test PRIVATE ${CMAKE_DL_LIBS})
  target_compile_features(score_addon_ndi_readback_pool_test PRIVATE cxx_std_20)
  find_package(Threads REQUIRED)
  target_link_libraries(score_addon_ndi_readback_pool_test PRIVATE Threads::Threads)

  # Adversarial inputs to the frame description. Some of the overruns it finds are
  # only visible under -fsanitize=address; the test says so where that is the case.
  add_executable(score_addon_ndi_frame_description_test tests/FrameDescriptionTest.cpp)
  target_include_directories(score_addon_ndi_frame_description_test
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/NDI/include")
  target_link_libraries(score_addon_ndi_frame_description_test PRIVATE)
  target_compile_features(score_addon_ndi_frame_description_test PRIVATE cxx_std_20)

  if(COMMAND add_test)
    add_test(NAME score_addon_ndi_readback_pool
             COMMAND score_addon_ndi_readback_pool_test)
    set_tests_properties(score_addon_ndi_readback_pool PROPERTIES TIMEOUT 300)
    add_test(NAME score_addon_ndi_frame_description
             COMMAND score_addon_ndi_frame_description_test)
  endif()

  # Per-frame cost of the send path. Not a pass/fail test of anything but its own
  # invariants -- it is there to be read.
  add_executable(score_addon_ndi_send_path_bench tests/SendPathBench.cpp)
  target_include_directories(score_addon_ndi_send_path_bench
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/NDI/include")
  target_link_libraries(score_addon_ndi_send_path_bench PRIVATE avutil swscale Qt6::Core)
  target_compile_features(score_addon_ndi_send_path_bench PRIVATE cxx_std_20)
  if(COMMAND add_test)
    add_test(NAME score_addon_ndi_send_path_bench
             COMMAND score_addon_ndi_send_path_bench)
    set_tests_properties(score_addon_ndi_send_path_bench PROPERTIES TIMEOUT 300)
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
      PRIVATE "${NDI_RUNTIME_LIBRARY}")
    target_compile_features(score_addon_ndi_loopback_test PRIVATE cxx_std_20)

    # The loopback test sends synchronously; production sends asynchronously,
    # which is a different ownership contract. This one exercises that contract.
    add_executable(score_addon_ndi_async_send_test tests/AsyncSendTest.cpp)
    target_include_directories(score_addon_ndi_async_send_test
      PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/NDI/include")
    target_link_libraries(score_addon_ndi_async_send_test
      PRIVATE "${NDI_RUNTIME_LIBRARY}")
    target_compile_features(score_addon_ndi_async_send_test PRIVATE cxx_std_20)

    if(COMMAND add_test)
      add_test(NAME score_addon_ndi_loopback COMMAND score_addon_ndi_loopback_test)
      set_tests_properties(score_addon_ndi_loopback PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 120
        RUN_SERIAL TRUE
        LABELS "ndi;hardware")
      add_test(NAME score_addon_ndi_async_send COMMAND score_addon_ndi_async_send_test)
      set_tests_properties(score_addon_ndi_async_send PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 300
        RUN_SERIAL TRUE
        LABELS "ndi;hardware")
    endif()
  else()
    message(STATUS "score-addon-ndi: no NDI runtime found, loopback test skipped")
  endif()
endif()
