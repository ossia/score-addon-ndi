#pragma once
#include <Ndi/Loader.hpp>

namespace Ndi
{

struct HxDecoderLibs
{
  const char* avcodec{};
  const char* avutil{};

  constexpr bool known() const noexcept { return avcodec && avutil; }
};

/**
 * @brief The FFmpeg sonames an NDI runtime dlopens to decode NDI|HX.
 *
 * NDI|HX is H.264/HEVC and the runtime ships no decoder: it dlopens FFmpeg by
 * exact soname and dlsyms the entry points, so a system without that exact
 * generation gets a "Video decoder not found" placeholder frame instead of a
 * picture, with no error reported anywhere.
 *
 * Traced from the runtimes' own dlopen attempts: 5.6.1 asks for
 * libavcodec.so.58 with libavutil.so.56, 6.2.0.3 for libavcodec.so.61 with
 * libavutil.so.59. Each is tried with a "-ndi" infix first, which is how a
 * private build can be placed next to libndi without touching the system
 * FFmpeg.
 */
constexpr HxDecoderLibs hxDecoderLibs(int ndiMajor) noexcept
{
  switch(ndiMajor)
  {
    case 5:
      return {"libavcodec.so.58", "libavutil.so.56"};
    case 6:
      return {"libavcodec.so.61", "libavutil.so.59"};
    default:
      return {};
  }
}

/**
 * @brief Whether NDI|HX sources can be decoded by this runtime, on this system.
 *
 * Only ever false on Linux and the BSDs: elsewhere the runtime carries its own
 * decoders. An unknown runtime version answers true -- a warning about a
 * dependency we cannot name is worse than none.
 */
bool hxDecoderAvailable(const Loader& ndi) noexcept;
}
