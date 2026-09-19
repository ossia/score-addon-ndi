#pragma once
#include <Gfx/SharedInputSettings.hpp>

#include <Video/VideoEnums.hpp>

#include <QString>

#include <string_view>

namespace Ndi
{

/**
 * @brief What to ask the SDK to deliver, and what that costs.
 *
 * Measured on libndi 5.6.1 and 6.2.0.3 against a 1080i50 source:
 *
 *   UYVY_RGBA + allow_video_fields=false -> 1920x1080 progressive at 25/s,
 *       the SDK having de-interlaced it for about 0.14 ms per frame.
 *   best                                 -> 1920x540 FIELDS at 50/s, 16-bit
 *       P216, whatever allow_video_fields is set to.
 *
 * The SDK documents that second behaviour ("you should consider that
 * allow_video_fields is true, and individual fields will always be delivered"),
 * and it is the whole reason the deinterlace setting exists. Asking for 16-bit
 * is therefore also asking score to handle fields.
 */
enum class ReceiveFormat
{
  EightBit,  ///< default: UYVY/RGBA, the SDK de-interlaces
  Best       ///< 16-bit where the source has it, fields delivered separately
};

constexpr const char* receiveFormatName(ReceiveFormat f) noexcept
{
  switch(f)
  {
    case ReceiveFormat::Best:
      return "Best available (16-bit, fields)";
    case ReceiveFormat::EightBit:
      return "8-bit (SDK de-interlaces)";
  }
  return "8-bit (SDK de-interlaces)";
}

inline constexpr ReceiveFormat receiveFormats[]
    = {ReceiveFormat::EightBit, ReceiveFormat::Best};

/// Anything unrecognised -- including the empty string a device saved before
/// this field existed -- is the 8-bit default, which is what those devices did.
inline ReceiveFormat receiveFormatFromName(std::string_view name) noexcept
{
  for(auto f : receiveFormats)
    if(name == receiveFormatName(f))
      return f;
  return ReceiveFormat::EightBit;
}

inline ReceiveFormat receiveFormatFromName(const QString& name)
{
  const auto utf8 = name.toStdString();
  return receiveFormatFromName(std::string_view{utf8});
}

/// A literal converts to both of the above, which is ambiguous without this.
inline ReceiveFormat receiveFormatFromName(const char* name) noexcept
{
  return receiveFormatFromName(name ? std::string_view{name} : std::string_view{});
}

/**
 * @brief Per-device receive settings.
 *
 * Gfx::SharedInputSettings carries the source name; the rest is ours, because
 * NDI does not signal any of it. Ndi/NdiColorSpace.hpp has the measurements
 * behind the colour choice.
 *
 * Empty strings mean the defaults, which is what a device saved before these
 * fields existed deserializes to -- and those defaults are today's behaviour.
 */
/**
 * @brief What to do with fields, when a source delivers them separately.
 *
 * Only reachable through the 16-bit receive format: the 8-bit path asks the SDK
 * to de-interlace and never sees a field. Weave keeps full vertical resolution
 * and combs on motion; bob halves the vertical resolution and does not.
 */
constexpr const char* deinterlaceName(Video::Deinterlace d) noexcept
{
  switch(d)
  {
    case Video::Deinterlace::Bob:
      return "Bob (smooth motion, half vertical resolution)";
    case Video::Deinterlace::Weave:
      return "Weave (full resolution, combs on motion)";
  }
  return "Weave (full resolution, combs on motion)";
}

inline constexpr Video::Deinterlace deinterlaceModes[]
    = {Video::Deinterlace::Weave, Video::Deinterlace::Bob};

inline Video::Deinterlace deinterlaceFromName(std::string_view name) noexcept
{
  for(auto d : deinterlaceModes)
    if(name == deinterlaceName(d))
      return d;
  return Video::Deinterlace::Weave;
}

inline Video::Deinterlace deinterlaceFromName(const QString& name)
{
  const auto utf8 = name.toStdString();
  return deinterlaceFromName(std::string_view{utf8});
}

inline Video::Deinterlace deinterlaceFromName(const char* name) noexcept
{
  return deinterlaceFromName(name ? std::string_view{name} : std::string_view{});
}

struct InputSettings : Gfx::SharedInputSettings
{
  QString colorSpace;
  QString receiveFormat;
  QString deinterlace;
};
}
