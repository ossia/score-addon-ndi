#pragma once

/**
 * @file ReceiveLayout.hpp
 * @brief Where each plane of a received NDI frame is, and how big it is.
 *
 * An NDI frame is one pointer and one stride; the planes are found by walking
 * from p_data. Getting an offset wrong shows chroma from the wrong place --
 * a picture, just not the right one -- and getting `total` wrong describes
 * memory to libavutil that the SDK does not own.
 *
 * `stride` is the frame's line_stride_in_bytes, which describes the PRIMARY
 * plane; the chroma strides are derived from it.
 *
 * Alpha is dropped for UYVA and PA16 -- ffmpeg has no zero-copy pixel format
 * for either -- but it is still part of the SDK's allocation, so `total`
 * counts it.
 */

#include <Video/VideoEnums.hpp>

// Processing.NDI.structs.h uses NULL without including anything that defines
// it; it compiles elsewhere only because other headers get there first.
#include <cstddef>

#include <Processing.NDI.Lib.h>

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace Ndi
{

struct ReceiveLayout
{
  bool supported{false};
  AVPixelFormat format{AV_PIX_FMT_NONE};

  /// Planes handed to the AVFrame, which is not always what the wire carries:
  /// the alpha of UYVA and PA16 is present in memory and not referenced here.
  int planeCount{0};
  size_t offset[3]{};
  int stride[3]{};

  /// Every byte the SDK's frame occupies, alpha included.
  size_t total{0};
};

inline ReceiveLayout
receiveLayout(NDIlib_FourCC_video_type_e fourcc, int s, int h) noexcept
{
  if(s <= 0 || h <= 0)
    return {};

  const size_t luma = size_t(s) * h;
  const int cs = s / 2;
  const int ch = h / 2;
  const size_t chroma420 = size_t(cs) * ch;

  switch(fourcc)
  {
    case NDIlib_FourCC_video_type_UYVY:
      return {true, AV_PIX_FMT_UYVY422, 1, {0}, {s}, luma};

    // UYVY followed by a full-resolution 8-bit alpha plane (4:2:2:4), so the
    // alpha is half the size of the UYVY part rather than equal to it.
    case NDIlib_FourCC_video_type_UYVA:
      return {true, AV_PIX_FMT_UYVY422, 1, {0}, {s}, luma + size_t(cs) * h};

    case NDIlib_FourCC_video_type_BGRA:
      return {true, AV_PIX_FMT_BGRA, 1, {0}, {s}, luma};
    case NDIlib_FourCC_video_type_BGRX:
      return {true, AV_PIX_FMT_BGR0, 1, {0}, {s}, luma};
    case NDIlib_FourCC_video_type_RGBA:
      return {true, AV_PIX_FMT_RGBA, 1, {0}, {s}, luma};
    case NDIlib_FourCC_video_type_RGBX:
      return {true, AV_PIX_FMT_RGB0, 1, {0}, {s}, luma};

    case NDIlib_FourCC_video_type_NV12:
      return {
          true, AV_PIX_FMT_NV12, 2, {0, luma}, {s, s}, luma + size_t(s) * ch};

    case NDIlib_FourCC_video_type_I420:
      return {
          true,          AV_PIX_FMT_YUV420P,        3,
          {0, luma, luma + chroma420},              {s, cs, cs},
          luma + 2 * chroma420};

    // YV12 is I420 with the chroma planes exchanged on the wire: Cr first.
    // data[1] is always Cb to libavutil, so it points at the SECOND one.
    case NDIlib_FourCC_video_type_YV12:
      return {
          true,          AV_PIX_FMT_YUV420P,        3,
          {0, luma + chroma420, luma},              {s, cs, cs},
          luma + 2 * chroma420};

    case NDIlib_FourCC_video_type_P216:
      return {true, AV_PIX_FMT_P216LE, 2, {0, luma}, {s, s}, 2 * luma};

    // P216 followed by a 16-bit full-resolution alpha plane.
    case NDIlib_FourCC_video_type_PA16:
      return {true, AV_PIX_FMT_P216LE, 2, {0, luma}, {s, s}, 3 * luma};

    default:
      return {};
  }
}

}
