#pragma once

/**
 * @file FrameFormat.hpp
 * @brief What an NDI frame_format_type means, as a pure decision.
 *
 * The four types, from Processing.NDI.structs.h:
 *
 *   progressive   a frame, no fields involved
 *   interleaved   a frame with both fields already woven into it
 *   field_0       half-height, the EVEN lines of the picture
 *   field_1       half-height, the ODD lines
 *
 * Only field_0/field_1 need the GPU to resolve anything. Interleaved is
 * already a picture and must be shown, not dropped -- it reaches us whenever
 * the input asks for fields, which the 16-bit format does.
 *
 * Pure so that the interleaved case is reachable from a test; inside
 * ndi_video_to_avframe it needed the SDK, a receiver and a live frame.
 */

#include <Video/VideoEnums.hpp>

// Processing.NDI.structs.h uses NULL without including anything that defines
// it; it compiles elsewhere only because other headers get there first.
#include <cstddef>

#include <Processing.NDI.Lib.h>

namespace Ndi
{

/// What to do with a frame of this frame_format_type.
struct FrameFormatDecision
{
  /// No frame_format_type sets this false: an unexpected value is not a
  /// reason to drop a frame whose pixels are fine.
  bool accept{true};

  /// What the GPU side has to do about fields.
  Video::Interlacing interlacing{Video::Interlacing::None};

  /// Whether to set AV_FRAME_FLAG_INTERLACED.
  bool interlaced{false};

  /// Which field this is: true = field_0 (even lines). Carried on
  /// AV_FRAME_FLAG_TOP_FIELD_FIRST, which planeRows() reads to pick the half
  /// of the texture to fill -- a parity marker, not field ORDER in the stream.
  bool topField{false};
};

inline constexpr FrameFormatDecision
decodeFrameFormat(NDIlib_frame_format_type_e t) noexcept
{
  switch(t)
  {
    case NDIlib_frame_format_type_progressive:
      return {true, Video::Interlacing::None, false, false};

    case NDIlib_frame_format_type_interleaved:
      // Both fields already in one picture. Interlaced, but nothing to resolve.
      return {true, Video::Interlacing::Woven, true, false};

    case NDIlib_frame_format_type_field_0:
      return {true, Video::Interlacing::Fields, true, true};

    case NDIlib_frame_format_type_field_1:
      return {true, Video::Interlacing::Fields, true, false};

    default:
      // An unknown value is not a reason to throw away a frame whose pixels
      // are perfectly good. Treat it as progressive.
      return {true, Video::Interlacing::None, false, false};
  }
}

}
