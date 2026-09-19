#pragma once

/**
 * @file ColorInfo.hpp
 * @brief Reading what a sender says about its own colour, where it says it.
 *
 * NDI frames may carry an XML string in p_metadata. The SDK documents one
 * element for colour (v6.2 section 18.1.2, in the HDR chapter):
 *
 *     <ndi_color_info primaries="bt_2020" transfer="bt_2100_hlg" matrix="bt_2020" />
 *
 * with primaries and matrix in {bt_601, bt_709, bt_2020, bt_2100} and transfer
 * additionally allowing bt_2100_pq and bt_2100_hlg. Several elements may be
 * wrapped in an <ndi_metadata_group>.
 *
 * Two things measured on real senders shape how this is used:
 *
 *   - It is NOT HDR-only in practice. NDI Test Patterns attaches it to plain
 *     SDR frames, and an HDR source attaches bt_2100_hlg to 16-bit ones.
 *   - It can be FALSE. The same Test Patterns app, set to 601, sends 75% bars
 *     encoded BT.601 while declaring matrix="bt_709" on the very same frame.
 *
 * So the transfer and the primaries are worth believing -- nothing else can
 * tell us, and they were right on the HDR source -- while the matrix is only
 * consulted when the user asks for it by choosing "Auto (metadata priority)".
 *
 * The parse is deliberately a scan for attributes rather than a real XML parse:
 * this runs per frame on the receive thread, the grammar is three attributes,
 * and pulling in a DOM to read them would cost an allocation per frame.
 */

#include <Ndi/NdiColorSpace.hpp>

#include <cstring>
#include <string_view>

namespace Ndi
{

/// The value of `attr="..."` inside @p xml, or an empty view when absent.
inline std::string_view
xmlAttribute(std::string_view xml, std::string_view attr) noexcept
{
  for(size_t from = 0;;)
  {
    const auto at = xml.find(attr, from);
    if(at == std::string_view::npos)
      return {};

    // The name must stand alone: "matrix" must not match inside a longer
    // attribute name, and must be followed by = and a quote.
    size_t i = at + attr.size();
    while(i < xml.size() && (xml[i] == ' ' || xml[i] == '\t'))
      ++i;
    const bool boundedLeft = at == 0 || xml[at - 1] == ' ' || xml[at - 1] == '\t'
                             || xml[at - 1] == '<';
    if(!boundedLeft || i >= xml.size() || xml[i] != '=')
    {
      from = at + 1;
      continue;
    }

    ++i;
    while(i < xml.size() && (xml[i] == ' ' || xml[i] == '\t'))
      ++i;
    if(i >= xml.size() || (xml[i] != '"' && xml[i] != '\''))
      return {};

    const char quote = xml[i++];
    const auto end = xml.find(quote, i);
    if(end == std::string_view::npos)
      return {};
    return xml.substr(i, end - i);
  }
}

/**
 * @brief Parse the ndi_color_info of one frame's metadata.
 *
 * Returns everything empty for a null or elementless string, which is the
 * common case: most senders declare nothing.
 */
inline MetadataColor parseColorInfo(const char* metadata) noexcept
{
  MetadataColor out;
  if(!metadata || !*metadata)
    return out;

  std::string_view xml{metadata};
  const auto at = xml.find("ndi_color_info");
  if(at == std::string_view::npos)
    return out;
  xml = xml.substr(at);

  // Stop at the end of this element so a second one in an ndi_metadata_group
  // cannot contribute attributes to this one.
  if(const auto close = xml.find('>'); close != std::string_view::npos)
    xml = xml.substr(0, close);

  const auto matrix = xmlAttribute(xml, "matrix");
  if(matrix == "bt_601")
    out.matrix = YuvStandard::BT601;
  else if(matrix == "bt_709")
    out.matrix = YuvStandard::BT709;
  // bt_2100 names BT.2100, whose non-constant-luminance Y'CbCr coefficients are
  // BT.2020's. H.273 reserves matrix 14 for ICtCp, which is a different
  // encoding entirely -- but a P216 buffer carries Y'CbCr, so NCL is the
  // reading that makes sense of the bytes actually sent.
  else if(matrix == "bt_2020" || matrix == "bt_2100")
    out.matrix = YuvStandard::BT2020;

  const auto transfer = xmlAttribute(xml, "transfer");
  if(transfer == "bt_601")
    out.transfer = AVCOL_TRC_SMPTE170M;
  else if(transfer == "bt_709")
    out.transfer = AVCOL_TRC_BT709;
  else if(transfer == "bt_2020")
    out.transfer = AVCOL_TRC_BT2020_10;
  else if(transfer == "bt_2100_pq")
    out.transfer = AVCOL_TRC_SMPTE2084;
  else if(transfer == "bt_2100_hlg")
    out.transfer = AVCOL_TRC_ARIB_STD_B67;

  const auto primaries = xmlAttribute(xml, "primaries");
  if(primaries == "bt_601")
    out.primaries = AVCOL_PRI_SMPTE170M;
  else if(primaries == "bt_709")
    out.primaries = AVCOL_PRI_BT709;
  else if(primaries == "bt_2020" || primaries == "bt_2100")
    out.primaries = AVCOL_PRI_BT2020;

  return out;
}

}
