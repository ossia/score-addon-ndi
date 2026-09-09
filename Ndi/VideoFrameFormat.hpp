#pragma once

/**
 * @file VideoFrameFormat.hpp
 * @brief Turning a readback into an NDI video frame, without score or a GPU.
 *
 * The output node reads back RGBA from the renderer and has to describe it to
 * the NDI SDK: a FourCC, a data pointer and a line stride, plus a colour
 * conversion when the setting asks for something other than RGBA. That
 * description is the part most likely to be wrong and the part that needs no
 * device to exercise, so it lives here and tests/PixelFormatTest.cpp drives it
 * directly.
 */

// Processing.NDI.structs.h uses NULL in its default arguments without
// including anything that defines it; it only compiles elsewhere because other
// headers get there first. Keep this one self-contained.
#include <cstddef>

#include <Processing.NDI.Lib.h>

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <cstdint>
#include <string_view>

namespace Ndi
{
/**
 * @brief Describe an RGBA readback as an NDI frame.
 *
 * @param staging  UYVY422 frame to convert into; must be distinct from the one
 *                 handed to the previous send_video_async, which the SDK reads
 *                 until the next one.
 * @return false when the format is not one this addon writes, in which case the
 *         frame must not be sent: NDIlib_video_frame_v2_t defaults its FourCC
 *         to UYVY, so sending it would hand the SDK a null pointer labelled as
 *         a valid format.
 */
inline bool describeVideoFrame(
    std::string_view format, const uint8_t* rgba, int width, int height,
    SwsContext* sws, AVFrame* staging, NDIlib_video_frame_v2_t& out) noexcept
{
  out.xres = width;
  out.yres = height;
  out.frame_format_type = NDIlib_frame_format_type_progressive;

  if(format == "UYVY")
  {
    if(!sws || !staging)
      return false;

    const uint8_t* inData[1] = {rgba};
    const int inLinesize[1] = {4 * width};
    sws_scale(sws, inData, inLinesize, 0, height, staging->data, staging->linesize);

    out.FourCC = NDIlib_FourCC_video_type_UYVY;
    out.p_data = staging->data[0];
    out.line_stride_in_bytes = staging->linesize[0];
    return true;
  }

  if(format == "RGBA")
  {
    out.FourCC = NDIlib_FourCC_video_type_RGBA;
    out.p_data = const_cast<uint8_t*>(rgba);
    out.line_stride_in_bytes = 4 * width;
    return true;
  }

  return false;
}
}
