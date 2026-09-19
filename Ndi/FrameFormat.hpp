#pragma once

/**
 * @file FrameFormat.hpp
 * @brief What an NDI frame_format_type means, as a pure decision.
 *
 * TESTABILITY SEAM. This lived as two switch statements inside
 * ndi_video_to_avframe, which needs the SDK, a receiver and a live frame to
 * reach -- so the one case that mattered was never covered, and the way it
 * failed was invisible:
 *
 *   NDIlib_frame_format_type_interleaved fell into a `default:` that freed the
 *   AVFrame and returned nullptr. An interleaved source therefore produced no
 *   picture at all on libavutil >= 58, i.e. on every FFmpeg from 6.0 onwards.
 *
 * Nothing logged, nothing crashed, no frame arrived. And the receiver asks for
 * fields whenever the input is set to the 16-bit "Best" format
 * (allow_video_fields = wantsBest), which is exactly when a source stops
 * pre-weaving and starts saying "interleaved" -- so choosing 16-bit on an
 * interlaced source turned the picture black. NDI Signal Generator in its
 * Interlaced mode reproduces it: 100 frames out of 100 arrive as interleaved.
 *
 * The four types, from the SDK (Processing.NDI.structs.h):
 *
 *   progressive   a frame, no fields involved
 *   interleaved   a frame with both fields already woven into it
 *   field_0       half-height, the EVEN lines of the picture
 *   field_1       half-height, the ODD lines
 *
 * Only field_0/field_1 need the GPU to resolve anything; interleaved is
 * already a picture and must simply be shown.
 */

#include <Video/VideoEnums.hpp>

// Processing.NDI.structs.h uses NULL in its default arguments without
// including anything that defines it; it only compiles elsewhere because other
// headers get there first. Keep this one self-contained, as
// Ndi/VideoFrameFormat.hpp does for the same reason.
#include <cstddef>

#include <Processing.NDI.Lib.h>

namespace Ndi
{

/// What to do with a frame of this frame_format_type.
struct FrameFormatDecision
{
  /// False only for a frame that genuinely cannot be turned into a picture.
  /// No NDI frame_format_type qualifies, which is the point: dropping the
  /// frame is never the right answer to an unexpected value here, because the
  /// pixels are fine either way.
  bool accept{true};

  /// What the GPU side has to do about fields.
  Video::Interlacing interlacing{Video::Interlacing::None};

  /// Whether to set AV_FRAME_FLAG_INTERLACED.
  bool interlaced{false};

  /// For the fielded cases, which field this is: true = field_0 (even lines).
  /// Carried on AV_FRAME_FLAG_TOP_FIELD_FIRST, which the decoder reads back
  /// through GPUVideoDecoder::planeRows to decide which half of the stacked
  /// texture to fill. It is a field-parity marker here, not a statement about
  /// field order in the stream.
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
