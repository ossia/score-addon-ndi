#pragma once

/**
 * @file NdiColorSpace.hpp
 * @brief Which YUV standard an NDI frame is in, and who gets to decide.
 *
 * ## What the specification says
 *
 * NDI does not negotiate the colour space of an SDR YUV frame and carries no
 * field that could signal it. The standard is fixed by the resolution, and both
 * ends are expected to derive it the same way. NDI SDK Documentation v6.2,
 * "Video Frames", p.54:
 *
 *     When running in a YUV color space, the following standards are applied:
 *
 *       Resolution                            Standard
 *       SD resolutions                        BT.601
 *       HD resolutions  xres>720 || yres>576   Rec.709
 *       UHD resolutions xres>1920 || yres>1080 Rec.2020
 *       Alpha channel                         Full range for data type
 *
 * ## What actually happens
 *
 * No implementation follows that table, including NDI's own. Measured on this
 * network, September 2026:
 *
 *   - libndi 5.6.1 and 6.2.0.3 both convert YUV to RGB with Rec.709 at EVERY
 *     resolution -- 64x32, 720x576, 1280x720, 2560x1440 and 3840x2160, in
 *     UYVY, P216, NV12 and I420. Sending the same green in three encodings and
 *     reading it back, only the Rec.709 one returns as green.
 *   - NDI Test Patterns set to 601 emits 75% bars encoded BT.601 at 1920x1080,
 *     an HD size the table assigns to Rec.709.
 *   - That same frame declares matrix="bt_709" in its ndi_color_info. The
 *     metadata contradicts its own pixels, so it cannot be trusted for the SDR
 *     matrix.
 *   - DistroAV, the OBS plugin, defaults every NDI source to BT.709 limited and
 *     offers a manual override; it has no resolution logic at all. GStreamer's
 *     NDI plugin sets no colorimetry whatsoever.
 *
 * So the matrix is a policy, not a fact that can be derived. Rec.709 is the
 * default because it is what every receiver measured actually applies;
 * everything else is offered because real equipment does emit it.
 *
 * Range is limited in every case. The table's only mention of full range is the
 * alpha channel, and the HDR section says outright that "full-range signals are
 * not supported by NDI Tools at this point".
 *
 * The rule applies to the PICTURE size, which for UYVY is not the size of the
 * texture the encoder writes -- that one is half as wide.
 */

#include <Gfx/Graph/encoders/ColorSpaceOut.hpp>

#include <QString>

#include <optional>
#include <string_view>

namespace Ndi
{

enum class YuvStandard
{
  BT601,
  BT709,
  BT2020
};

constexpr const char* ndiYuvStandardName(YuvStandard s) noexcept
{
  switch(s)
  {
    case YuvStandard::BT2020:
      return "Rec.2020";
    case YuvStandard::BT709:
      return "Rec.709";
    case YuvStandard::BT601:
      return "BT.601";
  }
  return "Rec.709";
}

/// The standard the SDK documentation assigns to this picture size.
/// Reference only: see the file header for why it is not the default.
constexpr YuvStandard documentedYuvStandard(int xres, int yres) noexcept
{
  if(xres > 1920 || yres > 1080)
    return YuvStandard::BT2020;
  if(xres > 720 || yres > 576)
    return YuvStandard::BT709;
  return YuvStandard::BT601;
}

/// The guess score's own video decoder makes for a frame whose colour space is
/// unspecified (Gfx/Graph/decoders/ColorSpace.hpp). Offered so an NDI device can
/// be made to match the rest of score rather than the rest of the network.
constexpr YuvStandard heuristicYuvStandard(int xres, int) noexcept
{
  return xres >= 1280 ? YuvStandard::BT709 : YuvStandard::BT601;
}

/**
 * @brief What the user chose, for one direction of one device.
 *
 * The three explicit values mean exactly themselves. The three automatic ones
 * differ in what they consult, and they exist because the three possible
 * authorities disagree with each other in practice:
 *
 *   - AutoHeuristic    what score would assume for an unlabelled video frame;
 *   - AutoMetadata     the sender's own ndi_color_info, where it has one;
 *   - AutoNdiRules     the resolution table in the SDK documentation.
 *
 * AutoMetadata is receive-only -- there is nothing to read on the way out.
 */
enum class ColorSpaceSetting
{
  Rec709,  ///< default, both directions
  BT601,
  Rec2020,
  AutoHeuristic,
  AutoMetadata,
  AutoNdiRules
};

/// The stored and displayed name. These strings go into save files: changing
/// one silently resets every device that used it to the default.
constexpr const char* colorSpaceSettingName(ColorSpaceSetting s) noexcept
{
  switch(s)
  {
    case ColorSpaceSetting::Rec709:
      return "Rec.709";
    case ColorSpaceSetting::BT601:
      return "BT.601";
    case ColorSpaceSetting::Rec2020:
      return "Rec.2020";
    case ColorSpaceSetting::AutoHeuristic:
      return "Auto (heuristic)";
    case ColorSpaceSetting::AutoMetadata:
      return "Auto (metadata priority)";
    case ColorSpaceSetting::AutoNdiRules:
      return "Auto (NDI rules priority)";
  }
  return "Rec.709";
}

/// Every setting an OUTPUT may take: there is no metadata to prioritise on the
/// way out, so AutoMetadata is absent.
inline constexpr ColorSpaceSetting outputColorSpaceSettings[] = {
    ColorSpaceSetting::Rec709, ColorSpaceSetting::BT601, ColorSpaceSetting::Rec2020,
    ColorSpaceSetting::AutoNdiRules, ColorSpaceSetting::AutoHeuristic};

/// Every setting an INPUT may take.
inline constexpr ColorSpaceSetting inputColorSpaceSettings[] = {
    ColorSpaceSetting::Rec709,       ColorSpaceSetting::BT601,
    ColorSpaceSetting::Rec2020,      ColorSpaceSetting::AutoHeuristic,
    ColorSpaceSetting::AutoMetadata, ColorSpaceSetting::AutoNdiRules};

/// Parse a stored name. Anything unrecognised -- an empty settings blob from
/// before this field existed, a hand-edited file -- becomes the default rather
/// than an error: the alternative is a device that will not open.
inline ColorSpaceSetting colorSpaceSettingFromName(std::string_view name) noexcept
{
  for(auto s :
      {ColorSpaceSetting::Rec709, ColorSpaceSetting::BT601, ColorSpaceSetting::Rec2020,
       ColorSpaceSetting::AutoHeuristic, ColorSpaceSetting::AutoMetadata,
       ColorSpaceSetting::AutoNdiRules})
    if(name == colorSpaceSettingName(s))
      return s;
  return ColorSpaceSetting::Rec709;
}

inline ColorSpaceSetting colorSpaceSettingFromName(const QString& name)
{
  // Named, not inlined into the call: a view into the temporary would be
  // legal here and a dangling reference the moment anyone refactors it.
  const auto utf8 = name.toStdString();
  return colorSpaceSettingFromName(std::string_view{utf8});
}

/// A literal converts to both of the above, which is ambiguous without this.
inline ColorSpaceSetting colorSpaceSettingFromName(const char* name) noexcept
{
  return colorSpaceSettingFromName(name ? std::string_view{name} : std::string_view{});
}

/// What a sender declared about a frame, where it declared anything. Only the
/// receive path fills this in; see Ndi/ColorInfo.hpp.
struct MetadataColor
{
  std::optional<YuvStandard> matrix;
  std::optional<AVColorTransferCharacteristic> transfer;
  std::optional<AVColorPrimaries> primaries;
};

/**
 * @brief The standard to use, given the setting, the picture and the metadata.
 *
 * @p md is empty on the send side and for a frame that declared nothing. Note
 * that AutoMetadata falls back to the default rather than to the SDK rule: a
 * sender that says nothing is overwhelmingly likely to be one of the many that
 * simply encode Rec.709.
 */
constexpr YuvStandard resolveYuvStandard(
    ColorSpaceSetting setting, int xres, int yres, const MetadataColor& md = {}) noexcept
{
  switch(setting)
  {
    case ColorSpaceSetting::Rec709:
      return YuvStandard::BT709;
    case ColorSpaceSetting::BT601:
      return YuvStandard::BT601;
    case ColorSpaceSetting::Rec2020:
      return YuvStandard::BT2020;
    case ColorSpaceSetting::AutoHeuristic:
      return heuristicYuvStandard(xres, yres);
    case ColorSpaceSetting::AutoNdiRules:
      return documentedYuvStandard(xres, yres);
    case ColorSpaceSetting::AutoMetadata:
      return md.matrix ? *md.matrix : YuvStandard::BT709;
  }
  return YuvStandard::BT709;
}

/// The AVColorSpace that names @p s, for the decode side.
constexpr AVColorSpace avColorSpace(YuvStandard s) noexcept
{
  switch(s)
  {
    case YuvStandard::BT601:
      return AVCOL_SPC_SMPTE170M;
    case YuvStandard::BT709:
      return AVCOL_SPC_BT709;
    case YuvStandard::BT2020:
      return AVCOL_SPC_BT2020_NCL;
  }
  return AVCOL_SPC_BT709;
}

/**
 * @brief GLSL `convert_from_rgb()` for @p standard, limited range.
 *
 * Feed it to score::gfx::UYVYEncoder::init and friends as the colour
 * conversion.
 *
 * Two things about the arguments are worth knowing before changing them:
 *
 *  - on the BT.601 and Rec.709 paths, colorMatrixOut ignores the transfer
 *    characteristic and the primaries entirely: only the matrix and the range
 *    reach the shader. The values passed here are the honest description of the
 *    output, not switches -- changing them changes nothing;
 *
 *  - on the Rec.2020 path they are not inert, and the INPUT transfer is set to
 *    AVCOL_TRC_UNSPECIFIED on purpose. That selects the matrix-only branch of
 *    bt2020SdrOutShader. Left at its sRGB default, the shader would also map the
 *    gamut, BT.709 primaries -> BT.2020 primaries, and desaturate the picture
 *    against every receiver that does not mirror that step. NDI's rule is a
 *    statement about matrix coefficients, and an SDR NDI frame carries no
 *    primaries signal for a receiver to act on, so matrix-only is what
 *    round-trips.
 */
inline QString ndiColorMatrixOut(YuvStandard standard)
{
  switch(standard)
  {
    case YuvStandard::BT2020:
      return score::gfx::colorMatrixOut(
          AVCOL_SPC_BT2020_NCL, AVCOL_TRC_BT709, AVCOL_RANGE_MPEG, AVCOL_PRI_BT2020,
          AVCOL_TRC_UNSPECIFIED);
    case YuvStandard::BT709:
      return score::gfx::colorMatrixOut(
          AVCOL_SPC_BT709, AVCOL_TRC_BT709, AVCOL_RANGE_MPEG, AVCOL_PRI_BT709);
    case YuvStandard::BT601:
      break;
  }
  return score::gfx::colorMatrixOut(
      AVCOL_SPC_SMPTE170M, AVCOL_TRC_SMPTE170M, AVCOL_RANGE_MPEG, AVCOL_PRI_SMPTE170M);
}

/// Convenience for the send path: resolve, then build the shader.
inline QString ndiColorMatrixOut(ColorSpaceSetting setting, int xres, int yres)
{
  return ndiColorMatrixOut(resolveYuvStandard(setting, xres, yres));
}

}
