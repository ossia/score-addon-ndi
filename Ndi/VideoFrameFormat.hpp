#pragma once

/**
 * @file VideoFrameFormat.hpp
 * @brief Turning a readback into an NDI video frame, without score or a GPU.
 *
 * The conversion happens on the GPU: the output node renders through one of
 * score::gfx's wire encoders (see Ndi/WireEncode.hpp) and straight to RGBA when
 * the scene is already the wire format, so by the time the bytes reach the CPU
 * they are what goes out. Nothing here converts, copies or allocates -- it
 * fills in the five fields that describe those bytes to the SDK, and refuses
 * anything it cannot describe truthfully.
 *
 * That refusal is the point of the function. NDIlib_video_frame_v2_t defaults
 * its FourCC to UYVY, so a frame that was never filled in does not look invalid
 * to the SDK: it looks like a UYVY frame with a null data pointer. Every path
 * that cannot produce a real description must return false rather than leave a
 * plausible-looking frame behind.
 *
 * ## One buffer, several planes
 *
 * An NDI frame is a single pointer plus one `line_stride_in_bytes`, even for
 * the planar formats: the SDK finds the other planes by walking from p_data
 * (SDK Documentation v6.2, "Video Frames"):
 *
 *   NV12 / P216:  p_y = p_data;  p_uv = p_y + stride * yres
 *   I420 / YV12:  p_y = p_data;  p_u  = p_y + stride * yres;
 *                                p_v  = p_u + (stride / 2) * (yres / 2)
 *
 * So the framestore is taller than the picture, and how much taller depends on
 * the format -- that is what framestoreRows() is for. Get it wrong and
 * readbackStride() divides by the wrong number of rows, which silently yields a
 * stride that is a multiple or a fraction of the real one; the SDK then reads
 * every row from the wrong offset.
 */

// Processing.NDI.structs.h uses NULL in its default arguments without
// including anything that defines it; it only compiles elsewhere because other
// headers get there first. Keep this one self-contained.
#include <cstddef>

#include <Processing.NDI.Lib.h>

#include <cstdint>
#include <cstring>
#include <string_view>

namespace Ndi
{

/**
 * @brief What one NDI wire format looks like in memory, by name.
 *
 * NDI-side knowledge only: no QRhi, no score::gfx, no libav, so the host tests
 * and the sender thread can both use it. Ndi/WireEncode.hpp maps the same names
 * onto the GPU encoders that produce these bytes.
 */
struct NdiWireFormat
{
  std::string_view name;
  NDIlib_FourCC_video_type_e fourcc;

  /// Bytes per pixel of the PRIMARY plane, which is what line_stride_in_bytes
  /// describes: 4 for the RGB orders, 2 for UYVY and P216's luma, 1 for the
  /// 8-bit planar lumas.
  int primaryBytesPerPixel;

  /// Framestore rows = height * rowsNum / rowsDen. 1/1 for the packed formats,
  /// 2/1 for P216 (luma and chroma planes are the same size), 3/2 for the 4:2:0
  /// planar ones.
  int rowsNum, rowsDen;

  /// Subsampled formats cannot express an odd picture size.
  bool widthMustBeEven;
  bool heightMustBeEven;
};

/// Every format this addon can send, by the name stored in OutputSettings.
/// UYVA and PA16 are absent on purpose: both carry an alpha plane, and no GPU
/// encoder produces one yet, so there is nothing to describe.
inline constexpr NdiWireFormat wireFormats[] = {
    // name     FourCC                                bpp  rows   evenW  evenH
    {"RGBA", NDIlib_FourCC_video_type_RGBA, 4, 1, 1, false, false},
    {"RGBX", NDIlib_FourCC_video_type_RGBX, 4, 1, 1, false, false},
    {"BGRA", NDIlib_FourCC_video_type_BGRA, 4, 1, 1, false, false},
    {"BGRX", NDIlib_FourCC_video_type_BGRX, 4, 1, 1, false, false},
    {"UYVY", NDIlib_FourCC_video_type_UYVY, 2, 1, 1, true, false},
    {"P216", NDIlib_FourCC_video_type_P216, 2, 2, 1, true, false},
    {"NV12", NDIlib_FourCC_video_type_NV12, 1, 3, 2, true, true},
    {"I420", NDIlib_FourCC_video_type_I420, 1, 3, 2, true, true},
    {"YV12", NDIlib_FourCC_video_type_YV12, 1, 3, 2, true, true},
};

/// The format called @p name, or nullptr when this addon cannot send it.
inline constexpr const NdiWireFormat* findWireFormat(std::string_view name) noexcept
{
  for(const auto& f : wireFormats)
    if(f.name == name)
      return &f;
  return nullptr;
}

/// Bytes in one tightly packed row of the primary plane -- the smallest stride
/// that can be truthful. Returns 0 for an unknown format or width.
inline constexpr int packedRowBytes(std::string_view format, int width) noexcept
{
  const auto* f = findWireFormat(format);
  if(!f || width <= 0)
    return 0;
  return width * f->primaryBytesPerPixel;
}

/**
 * @brief Rows in the whole framestore, which is not the picture height.
 *
 * P216 has a chroma plane the same size as its luma plane (2x the rows), the
 * 4:2:0 planar formats have half a picture's worth of chroma (1.5x), and the
 * packed formats are just the picture. Pass this to readbackStride() rather
 * than the picture height.
 */
inline constexpr int framestoreRows(std::string_view format, int height) noexcept
{
  const auto* f = findWireFormat(format);
  if(!f || height <= 0)
    return 0;
  return height * f->rowsNum / f->rowsDen;
}

/**
 * @brief Describe already-encoded readback bytes as an NDI video frame.
 *
 * @param format       the wire format the bytes are in, as named in
 *                     `wireFormats` above.
 * @param bytes        the readback, which is what the SDK will read. It must
 *                     stay valid and unmodified until the next synchronising
 *                     call -- see ReadbackPool, which is what enforces that.
 * @param width,height the PICTURE size in pixels, not the framestore's.
 * @param strideBytes  bytes per row of the primary plane. The GPU readback
 *                     decides this, so it is passed in rather than assumed: it
 *                     must be at least the packed size for the format, and may
 *                     be larger if the backend padded rows.
 * @return false when the description would not be truthful, in which case the
 *         frame must not be sent.
 */
inline bool describeVideoFrame(
    std::string_view format, const uint8_t* bytes, int width, int height,
    int strideBytes, NDIlib_video_frame_v2_t& out) noexcept
{
  if(!bytes || width <= 0 || height <= 0 || strideBytes <= 0)
    return false;

  const auto* f = findWireFormat(format);
  if(!f)
    return false;

  // Two pixels per macropixel or per chroma site: an odd size cannot be
  // expressed, and the planar formats' half-height chroma needs an even height.
  if(f->widthMustBeEven && (width % 2) != 0)
    return false;
  if(f->heightMustBeEven && (height % 2) != 0)
    return false;

  // A stride below the packed row size would have the SDK read each row short
  // and walk into the next one.
  if(strideBytes < width * f->primaryBytesPerPixel)
    return false;

  out.xres = width;
  out.yres = height;
  out.frame_format_type = NDIlib_frame_format_type_progressive;
  out.FourCC = f->fourcc;
  out.p_data = const_cast<uint8_t*>(bytes);
  out.line_stride_in_bytes = strideBytes;
  return true;
}

/**
 * @brief One plane as the GPU handed it back, for assembly into a framestore.
 *
 * @p srcStride is what the readback actually has, which may be padded;
 * @p rowBytes is what the wire wants, which never is. The two differ often
 * enough that copying @p srcStride bytes per row would be a real bug: it would
 * shift every row after the first by the padding.
 */
struct PlaneSource
{
  const uint8_t* data{};
  int srcStride{};
  int rowBytes{};
  int rows{};
};

/**
 * @brief Copy planes into one tightly packed framestore, in order.
 *
 * Every format the output sends has a contiguous-framestore encoder, so
 * nothing in the send path needs this. It stays as the reference the encoder
 * tests measure the packed encoders against.
 *
 * @return bytes written, or 0 if anything did not add up.
 */
inline size_t assembleFramestore(
    uint8_t* dst, size_t dstCapacity, const PlaneSource* planes, int count) noexcept
{
  if(!dst || !planes || count <= 0)
    return 0;

  size_t written = 0;
  for(int i = 0; i < count; i++)
  {
    const auto& p = planes[i];
    if(!p.data || p.rows <= 0 || p.rowBytes <= 0 || p.srcStride < p.rowBytes)
      return 0;
    if(written + size_t(p.rowBytes) * p.rows > dstCapacity)
      return 0;

    const uint8_t* src = p.data;
    for(int r = 0; r < p.rows; r++)
    {
      std::memcpy(dst + written, src, size_t(p.rowBytes));
      written += size_t(p.rowBytes);
      src += p.srcStride;
    }
  }
  return written;
}

/**
 * @brief Bytes per row of a readback, from what actually came back.
 *
 * The backend may pad rows, so the stride is derived from the readback's own
 * size rather than from the format's packed width. @p rows is the number of
 * rows in the buffer -- framestoreRows(), NOT the picture height, for any
 * format whose chroma lives below its luma. Returns 0 when the readback cannot
 * be interpreted, which describeVideoFrame then refuses.
 */
inline int readbackStride(int dataSize, int rows) noexcept
{
  if(dataSize <= 0 || rows <= 0)
    return 0;
  return dataSize / rows;
}
}
