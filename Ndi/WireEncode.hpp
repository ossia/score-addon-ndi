#pragma once

/**
 * @file WireEncode.hpp
 * @brief Which score::gfx encoder produces each NDI wire format.
 *
 * The GPU side of Ndi/VideoFrameFormat.hpp: that one says what the bytes look
 * like to the SDK, this one says which existing encoder writes them. Nothing
 * converts anything here -- makeWireEncoder maps a neutral Video::VideoPixelFormat to
 * the encoder that emits exactly those bytes, and it is the same table the AJA
 * and DeckLink playout paths use, so an NDI frame and an SDI frame of the same
 * format come out byte-identical.
 *
 * Every format is asked for with contiguousFramestore, so every encoder
 * produces one texture whose readback IS the framestore: the renderer reads it
 * straight into the send buffer and nothing is copied between GPU and wire.
 *
 * RGBA/RGBX are not in the table: the scene texture is already those bytes, so
 * that path runs InvertYRenderer and sends the scene's own readback.
 */

#include <Gfx/Graph/encoders/WireEncoderFactory.hpp>
#include <Video/VideoPixelFormat.hpp>
#include <Ndi/NdiColorSpace.hpp>
#include <Ndi/VideoFrameFormat.hpp>

#include <memory>

namespace Ndi
{

/// How to produce one NDI wire format on the GPU.
struct NdiEncoding
{
  /// Unknown means no encoder: the scene texture is already the wire format
  /// (RGBA), and the output node reads it back through InvertYRenderer.
  Video::VideoPixelFormat gfx{
      Video::VideoPixelFormat::Unknown};

  /// Render the scene into RGBA16F instead of RGBA8. Only worth it above 8
  /// bits: from an 8-bit scene a 16-bit encoder produces valid bytes whose
  /// values are 8-bit quantities scaled up, and no receiver can tell -- but the
  /// precision has to come from somewhere, and this is where.
  bool floatRender{false};

  bool hasEncoder() const noexcept
  {
    return gfx != Video::VideoPixelFormat::Unknown;
  }
};

/// The encoding for @p name, or a default-constructed one (no encoder) for
/// RGBA and for anything this addon cannot send.
inline NdiEncoding ndiEncoding(std::string_view name) noexcept
{
  using F = Video::VideoPixelFormat;

  // RGBA: the scene is already these bytes. RGBX is the same bytes with a hint
  // that the alpha is all 255 -- NDI's own words -- so it is sent the same way.
  // Choosing it is a statement about the content, not a conversion.
  if(name == "RGBA" || name == "RGBX")
    return {};

  if(name == "BGRA" || name == "BGRX")
    return {.gfx = F::BGRA8, .floatRender = false};

  if(name == "UYVY")
    return {.gfx = F::UYVY422, .floatRender = false};

  // 16-bit 4:2:2. score::gfx has two encoders for it: two plane textures, or
  // one texture that IS the framestore -- luma rows above chroma rows, which is
  // exactly NDI's layout at p_data. We ask for the second, so P216 sends with
  // no assembly step and no copy, like the packed formats.
  if(name == "P216")
    return {.gfx = F::P216, .floatRender = true};

  // 8-bit 4:2:0. YV12 asks for YVU420P rather than reordering YUV420P's
  // planes: a framestore has one fixed plane order, and only the encoder can
  // put Cr first.
  if(name == "NV12")
    return {.gfx = F::NV12, .floatRender = false};
  if(name == "I420")
    return {.gfx = F::YUV420P, .floatRender = false};
  if(name == "YV12")
    return {.gfx = F::YVU420P, .floatRender = false};

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
