#pragma once

/**
 * @file VideoFrameFormat.hpp
 * @brief Turning a readback into an NDI video frame, without score or a GPU.
 *
 * The conversion happens on the GPU: the output node renders through
 * score::gfx::UYVYEncoder for UYVY, and straight to RGBA otherwise, so by the
 * time the bytes reach the CPU they are already the wire format. Nothing here
 * converts, copies or allocates -- it fills in the five fields that describe
 * those bytes to the SDK, and refuses anything it cannot describe truthfully.
 *
 * That refusal is the point of the function. NDIlib_video_frame_v2_t defaults
 * its FourCC to UYVY, so a frame that was never filled in does not look invalid
 * to the SDK: it looks like a UYVY frame with a null data pointer. Every path
 * that cannot produce a real description must return false rather than leave a
 * plausible-looking frame behind.
 */

// Processing.NDI.structs.h uses NULL in its default arguments without
// including anything that defines it; it only compiles elsewhere because other
// headers get there first. Keep this one self-contained.
#include <cstddef>

#include <Processing.NDI.Lib.h>

#include <cstdint>
#include <string_view>

namespace Ndi
{
/**
 * @brief Describe already-encoded readback bytes as an NDI video frame.
 *
 * @param format       the wire format the bytes are in: "RGBA" or "UYVY".
 * @param bytes        the readback, which is what the SDK will read. It must
 *                     stay valid and unmodified until the next synchronising
 *                     call -- see ReadbackPool, which is what enforces that.
 * @param width,height the picture size in pixels.
 * @param strideBytes  bytes per row in @p bytes. The GPU readback decides this,
 *                     so it is passed in rather than assumed: it must be at
 *                     least the packed size for the format, and may be larger
 *                     if the backend padded rows.
 * @return false when the description would not be truthful, in which case the
 *         frame must not be sent.
 */
inline bool describeVideoFrame(
    std::string_view format, const uint8_t* bytes, int width, int height,
    int strideBytes, NDIlib_video_frame_v2_t& out) noexcept
{
  if(!bytes || width <= 0 || height <= 0 || strideBytes <= 0)
    return false;

  if(format == "UYVY")
  {
    // Two pixels per four-byte macropixel, so an odd width cannot be expressed.
    if((width % 2) != 0)
      return false;
    if(strideBytes < 2 * width)
      return false;

    out.xres = width;
    out.yres = height;
    out.frame_format_type = NDIlib_frame_format_type_progressive;
    out.FourCC = NDIlib_FourCC_video_type_UYVY;
    out.p_data = const_cast<uint8_t*>(bytes);
    out.line_stride_in_bytes = strideBytes;
    return true;
  }

  if(format == "RGBA")
  {
    if(strideBytes < 4 * width)
      return false;

    out.xres = width;
    out.yres = height;
    out.frame_format_type = NDIlib_frame_format_type_progressive;
    out.FourCC = NDIlib_FourCC_video_type_RGBA;
    out.p_data = const_cast<uint8_t*>(bytes);
    out.line_stride_in_bytes = strideBytes;
    return true;
  }

  return false;
}

/**
 * @brief Bytes per row of a readback, from what actually came back.
 *
 * The backend may pad rows, so the stride is derived from the readback's own
 * size rather than from the format's packed width. Returns 0 when the readback
 * cannot be interpreted, which describeVideoFrame then refuses.
 */
inline int readbackStride(int dataSize, int height) noexcept
{
  if(dataSize <= 0 || height <= 0)
    return 0;
  return dataSize / height;
}
}
