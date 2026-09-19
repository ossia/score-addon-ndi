#pragma once

/**
 * @file WireEncode.hpp
 * @brief Which score::gfx encoder produces each NDI wire format.
 *
 * The GPU side of Ndi/VideoFrameFormat.hpp: that one says what the bytes look
 * like to the SDK, this one says which existing encoder writes them. Nothing
 * here converts anything itself -- score::gfx::makeWireEncoder already maps a
 * neutral VideoPixelFormat to the encoder that emits exactly those bytes, and
 * it is the same table the AJA and DeckLink playout paths use, so an NDI frame
 * and an SDI frame of the same format come out byte-identical.
 *
 * Two shapes of output come back from those encoders:
 *
 *   - single-plane (UYVY, the RGB orders): one texture whose readback IS the
 *     framestore. The renderer reads it straight into the send buffer and
 *     nothing is copied between the GPU and the wire.
 *
 *   - multi-plane (P216, NV12, I420, YV12): one readback per plane, in separate
 *     allocations. NDI wants them adjacent in one buffer, so the renderer
 *     assembles them with Ndi::assembleFramestore -- one copy per frame, which
 *     is the price of a planar format and still nothing like the 21.7 ms
 *     sws_scale used to cost.
 *
 * RGBA is not in the table at all: the scene texture is already RGBA8, so that
 * path runs InvertYRenderer and sends the scene's own readback.
 */

#include <Gfx/Graph/encoders/WireEncoderFactory.hpp>
#include <Gfx/Graph/interop/VideoPixelFormat.hpp>
#include <Ndi/NdiColorSpace.hpp>
#include <Ndi/VideoFrameFormat.hpp>

#include <memory>

namespace Ndi
{

/// One plane of an encoder's output, and where it goes in the framestore.
struct PlaneSpec
{
  /// The encoder's plane index. Not always the framestore order: YV12 is I420
  /// with the chroma planes exchanged, so it takes the same encoder and lists
  /// its planes as 0, 2, 1. FFmpeg has no pixel format for the swapped order
  /// either -- see score::gfx::interop::chromaSwappedTwin, which says the same
  /// thing about naming it.
  int encoderPlane;
  int widthDiv;       ///< plane width  = picture width  / widthDiv
  int heightDiv;      ///< plane height = picture height / heightDiv
  int bytesPerTexel;  ///< of the plane's readback: R8=1, RG8=2, R16=2, RG16=4
};

/// How to produce one NDI wire format on the GPU.
struct NdiEncoding
{
  /// Unknown means no encoder: the scene texture is already the wire format
  /// (RGBA), and the output node reads it back through InvertYRenderer.
  score::gfx::interop::VideoPixelFormat gfx{
      score::gfx::interop::VideoPixelFormat::Unknown};

  /// Render the scene into RGBA16F instead of RGBA8. Only worth it above 8
  /// bits: from an 8-bit scene a 16-bit encoder produces valid bytes whose
  /// values are 8-bit quantities scaled up, and no receiver can tell -- but the
  /// precision has to come from somewhere, and this is where.
  bool floatRender{false};

  int planeCount{0};  ///< 0 or 1 = nothing to assemble
  PlaneSpec planes[3]{};

  bool needsAssembly() const noexcept { return planeCount > 1; }
  bool hasEncoder() const noexcept
  {
    return gfx != score::gfx::interop::VideoPixelFormat::Unknown;
  }
};

/// The encoding for @p name, or a default-constructed one (no encoder) for
/// RGBA and for anything this addon cannot send.
inline NdiEncoding ndiEncoding(std::string_view name) noexcept
{
  using F = score::gfx::interop::VideoPixelFormat;

  // RGBA: the scene is already these bytes. RGBX is the same bytes with a hint
  // that the alpha is all 255 -- NDI's own words -- so it is sent the same way.
  // Choosing it is a statement about the content, not a conversion.
  if(name == "RGBA" || name == "RGBX")
    return {};

  if(name == "BGRA" || name == "BGRX")
    return {.gfx = F::BGRA8, .floatRender = false, .planeCount = 1};

  if(name == "UYVY")
    return {.gfx = F::UYVY422, .floatRender = false, .planeCount = 1};

  // 16-bit 4:2:2. score::gfx has two encoders for it: two plane textures, or
  // one texture that IS the framestore -- luma rows above chroma rows, which is
  // exactly NDI's layout at p_data. We ask for the second, so P216 sends with
  // no assembly step and no copy, like the packed formats.
  if(name == "P216")
    return {.gfx = F::P216, .floatRender = true, .planeCount = 1};

  // 8-bit 4:2:0. All three have a contiguous-framestore encoder
  // (score::gfx::Yuv420PackedEncoder), so like P216 they send with no assembly
  // step, no concatenation, and ONE readback instead of two or three. The
  // plane geometry below is therefore unused -- it is kept only so the
  // arithmetic stays visible next to the formats it describes.
  //
  // YV12 asks for YVU420P rather than reordering YUV420P's planes, because a
  // framestore has one fixed plane order and only the encoder can put Cr
  // first. That reordering is what planeCount = 3 used to be for.
  if(name == "NV12")
    return {.gfx = F::NV12, .floatRender = false, .planeCount = 1};
  if(name == "I420")
    return {.gfx = F::YUV420P, .floatRender = false, .planeCount = 1};
  if(name == "YV12")
    return {.gfx = F::YVU420P, .floatRender = false, .planeCount = 1};

  return {};
}

/// The encoder for @p name, or nullptr when the format needs none (RGBA) or
/// score::gfx has none for it.
inline std::unique_ptr<score::gfx::GPUVideoEncoder>
makeNdiEncoder(std::string_view name)
{
  const auto enc = ndiEncoding(name);
  if(!enc.hasEncoder())
    return {};
  // NDI always wants one pointer with the planes adjacent, so ask for the
  // contiguous variant wherever score::gfx has one.
  return score::gfx::makeWireEncoder(enc.gfx, /* contiguousFramestore */ true);
}

/// Bytes the assembled framestore needs, which must agree with the NDI-side
/// arithmetic in VideoFrameFormat.hpp -- the encode renderer checks that it
/// does before sending anything.
inline size_t framestoreBytes(std::string_view name, int width, int height) noexcept
{
  return size_t(packedRowBytes(name, width)) * framestoreRows(name, height);
}

}
