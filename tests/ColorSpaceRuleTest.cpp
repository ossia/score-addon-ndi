// Which YUV standard an NDI device uses, and how the setting decides it.
//
// NDI carries no colour signalling for SDR frames, and the three things that
// could answer the question disagree: the sender's metadata (which can be
// false), the SDK's resolution table (which no implementation applies), and
// what score assumes for unlabelled video. Ndi/NdiColorSpace.hpp has the
// measurements; this pins the policy built on them.
//
// This needs no GPU and no NDI runtime: the rules are pure functions of two
// integers and a setting, and the shader they produce is a string. The GPU side
// -- that these matrices really do come out of the encoder as these bytes -- is
// EncodeRendererTest.

#include <Ndi/ColorInfo.hpp>
#include <Ndi/NdiColorSpace.hpp>

#include <cstdio>

namespace
{
int g_fail = 0;

void check(bool ok, const char* what)
{
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if(!ok)
    ++g_fail;
}

void checkResolved(
    Ndi::ColorSpaceSetting setting, int w, int h, Ndi::YuvStandard expected,
    const char* what, const Ndi::MetadataColor& md = {})
{
  const auto got = Ndi::resolveYuvStandard(setting, w, h, md);
  const bool ok = got == expected;
  std::printf(
      "  [%s] %-24s %4dx%-4d -> %-8s%s (%s)\n", ok ? "PASS" : "FAIL",
      Ndi::colorSpaceSettingName(setting), w, h, Ndi::ndiYuvStandardName(got),
      ok ? "" : " -- WRONG", what);
  if(!ok)
    ++g_fail;
}

bool has(const QString& shader, const char* needle)
{
  return shader.contains(QLatin1String(needle));
}

// Every matrix in ColorSpaceOut.hpp is identified by its leading coefficient,
// which is unique across the six SDR matrices. The limited-range ones also
// carry 16/255 = 0.062745 as the Y offset; the full-range ones do not.
constexpr const char* kBt601Limited = "0.256788";
constexpr const char* kBt709Limited = "0.182586";
constexpr const char* kBt2020Limited = "0.225613";
constexpr const char* kBt601Full = "0.299,";
constexpr const char* kBt709Full = "0.2126,";
constexpr const char* kBt2020Full = "0.2627,";
constexpr const char* kLimitedOffset = "0.062745";

void checkShader(const QString& shader, const char* expectedMatrix, const char* what)
{
  std::printf("  -- %s --\n", what);
  check(has(shader, "convert_from_rgb"), "defines convert_from_rgb()");
  check(has(shader, expectedMatrix), "uses the expected matrix");
  check(has(shader, kLimitedOffset), "limited range: 16/255 luma offset");

  // Full range is what the encoders default to, and what NDI never wants: the
  // SDK's table gives full range to the alpha channel alone, and the HDR
  // section says full-range signals are not supported by NDI Tools.
  const bool anyFull
      = has(shader, kBt601Full) || has(shader, kBt709Full) || has(shader, kBt2020Full);
  check(!anyFull, "no full-range matrix");

  int matrices = 0;
  for(const char* m : {kBt601Limited, kBt709Limited, kBt2020Limited})
    if(has(shader, m))
      ++matrices;
  check(matrices == 1, "exactly one matrix in the shader");
}

void testDefault()
{
  std::printf("\n== the default is Rec.709 at every size ==\n");
  // The regression this file exists for. Every receiver measured decodes
  // Rec.709 whatever the size, so an NDI device that varies its matrix by
  // resolution is wrong everywhere except the sizes where the two agree.
  using S = Ndi::ColorSpaceSetting;
  for(auto [w, h] : {std::pair{64, 32}, std::pair{720, 576}, std::pair{1280, 720},
                     std::pair{1920, 1080}, std::pair{3840, 2160}})
    checkResolved(S::Rec709, w, h, Ndi::YuvStandard::BT709, "the default");

  check(
      Ndi::colorSpaceSettingFromName(QString{}) == S::Rec709,
      "an empty setting name resolves to the default");
  check(
      Ndi::colorSpaceSettingFromName("nonsense") == S::Rec709,
      "an unknown setting name resolves to the default");
  for(auto s : Ndi::inputColorSpaceSettings)
    check(
        Ndi::colorSpaceSettingFromName(Ndi::colorSpaceSettingName(s)) == s,
        Ndi::colorSpaceSettingName(s));
}

void testExplicit()
{
  std::printf("\n== the explicit settings mean themselves ==\n");
  using S = Ndi::ColorSpaceSetting;
  // Size and metadata are both ignored: that is the point of choosing one.
  const Ndi::MetadataColor lying{.matrix = Ndi::YuvStandard::BT2020};
  checkResolved(S::BT601, 3840, 2160, Ndi::YuvStandard::BT601, "size ignored", lying);
  checkResolved(S::Rec709, 64, 32, Ndi::YuvStandard::BT709, "size ignored", lying);
  checkResolved(S::Rec2020, 64, 32, Ndi::YuvStandard::BT2020, "size ignored", lying);
}

void testAutoNdiRules()
{
  std::printf("\n== Auto (NDI rules priority): the documented table ==\n");
  using S = Ndi::ColorSpaceSetting;
  using Y = Ndi::YuvStandard;

  // SD. The SDK states the HD condition (xres>720 || yres>576) and the UHD one
  // (xres>1920 || yres>1080); SD is what neither claims.
  checkResolved(S::AutoNdiRules, 720, 576, Y::BT601, "PAL SD, the largest SD size");
  checkResolved(S::AutoNdiRules, 720, 486, Y::BT601, "NTSC SD");

  // Either dimension crossing is enough -- the SDK's condition is an ||.
  checkResolved(S::AutoNdiRules, 722, 576, Y::BT709, "first width past SD");
  checkResolved(S::AutoNdiRules, 720, 578, Y::BT709, "first height past SD");
  checkResolved(S::AutoNdiRules, 1024, 768, Y::BT709, "XGA: HD by NDI's rule");
  checkResolved(S::AutoNdiRules, 1920, 1080, Y::BT709, "1080p, the largest HD size");

  checkResolved(S::AutoNdiRules, 1922, 1080, Y::BT2020, "first width past HD");
  checkResolved(S::AutoNdiRules, 1920, 1082, Y::BT2020, "first height past HD");
  checkResolved(S::AutoNdiRules, 3840, 2160, Y::BT2020, "2160p");
  checkResolved(S::AutoNdiRules, 2560, 1440, Y::BT2020, "1440p: UHD by NDI's rule");
  // The || again, and why it is worth a test: score can use any size, and a
  // portrait 1080x1920 picture is "UHD" here because yres > 1080.
  checkResolved(S::AutoNdiRules, 1080, 1920, Y::BT2020, "portrait 1080x1920");

  // Degenerate sizes cannot reach the encoder -- WireEncodeRenderer::init
  // refuses them before asking for a matrix -- but the rule must still answer.
  checkResolved(S::AutoNdiRules, 0, 0, Y::BT601, "degenerate size falls back to SD");
}

void testAutoHeuristic()
{
  std::printf("\n== Auto (heuristic): what score assumes elsewhere ==\n");
  using S = Ndi::ColorSpaceSetting;
  using Y = Ndi::YuvStandard;
  // Gfx/Graph/decoders/ColorSpace.hpp's fallback: width >= 1280.
  checkResolved(S::AutoHeuristic, 1279, 720, Y::BT601, "just below the threshold");
  checkResolved(S::AutoHeuristic, 1280, 720, Y::BT709, "at the threshold");
  checkResolved(S::AutoHeuristic, 3840, 2160, Y::BT709, "no BT.2020 in this one");
  // Where it disagrees with the SDK table, which is the reason both exist.
  check(
      Ndi::resolveYuvStandard(S::AutoHeuristic, 1024, 768, {})
          != Ndi::resolveYuvStandard(S::AutoNdiRules, 1024, 768, {}),
      "the heuristic and the SDK rule disagree at 1024x768");
}

void testAutoMetadata()
{
  std::printf("\n== Auto (metadata priority): the sender's declaration ==\n");
  using S = Ndi::ColorSpaceSetting;
  using Y = Ndi::YuvStandard;

  const auto hlg = Ndi::parseColorInfo(
      "<ndi_color_info primaries=\"bt_2020\" transfer=\"bt_2100_hlg\" "
      "matrix=\"bt_2020\" />");
  check(hlg.matrix && *hlg.matrix == Y::BT2020, "parses matrix=bt_2020");
  check(
      hlg.transfer && *hlg.transfer == AVCOL_TRC_ARIB_STD_B67,
      "parses transfer=bt_2100_hlg");
  check(
      hlg.primaries && *hlg.primaries == AVCOL_PRI_BT2020, "parses primaries=bt_2020");
  checkResolved(S::AutoMetadata, 1920, 1080, Y::BT2020, "declared wins over size", hlg);

  // The real HDR source on this network declares bt_2100 across the board.
  const auto bt2100 = Ndi::parseColorInfo(
      "<ndi_color_info primaries=\"bt_2100\" transfer=\"bt_2100_hlg\" "
      "matrix=\"bt_2100\" />");
  check(
      bt2100.matrix && *bt2100.matrix == Y::BT2020,
      "matrix=bt_2100 is BT.2020's non-constant-luminance matrix");

  // A frame that declares nothing falls back to the default, not to the SDK
  // table: a silent sender is far more likely to be one of the many that simply
  // encode Rec.709.
  checkResolved(S::AutoMetadata, 720, 576, Y::BT709, "no metadata -> the default");
  checkResolved(
      S::AutoMetadata, 720, 576, Y::BT709, "empty metadata -> the default",
      Ndi::parseColorInfo(""));
  checkResolved(
      S::AutoMetadata, 720, 576, Y::BT709, "unrelated metadata -> the default",
      Ndi::parseColorInfo("<ndi_tally_echo on_program=\"false\"/>"));

  // And the one that matters: the other settings must ignore a declaration,
  // because a shipping sender gets it wrong. NDI Test Patterns set to 601 sends
  // BT.601 bars carrying exactly this string.
  const auto lying = Ndi::parseColorInfo(
      "<ndi_color_info primaries=\"bt_709\" transfer=\"bt_709\" matrix=\"bt_709\" />");
  checkResolved(
      S::BT601, 1920, 1080, Y::BT601, "an explicit setting ignores a false claim",
      lying);
}

void testParser()
{
  std::printf("\n== the metadata parser ==\n");
  const auto none = Ndi::parseColorInfo(nullptr);
  check(!none.matrix && !none.transfer && !none.primaries, "null metadata is empty");

  // Single quotes, extra whitespace, and attributes in another order.
  const auto odd
      = Ndi::parseColorInfo("<ndi_color_info  matrix = 'bt_601'  primaries='bt_601'/>");
  check(
      odd.matrix && *odd.matrix == Ndi::YuvStandard::BT601,
      "single quotes and spacing around =");
  check(!odd.transfer, "an absent attribute stays absent");

  // A group with another element first: the colour element's own attributes
  // must be the ones read.
  const auto grouped = Ndi::parseColorInfo(
      "<ndi_metadata_group><ndi_tally_echo on_program=\"false\"/>"
      "<ndi_color_info matrix=\"bt_709\"/></ndi_metadata_group>");
  check(
      grouped.matrix && *grouped.matrix == Ndi::YuvStandard::BT709,
      "finds the element inside a metadata group");

  // Truncated XML must not read past the end or invent a value.
  const auto truncated = Ndi::parseColorInfo("<ndi_color_info matrix=\"bt_7");
  check(!truncated.matrix, "an unterminated attribute is refused");
  const auto noValue = Ndi::parseColorInfo("<ndi_color_info matrix>");
  check(!noValue.matrix, "an attribute with no value is refused");

  // A longer name must not match the short one.
  const auto longer
      = Ndi::parseColorInfo("<ndi_color_info matrix_hint=\"bt_601\" matrix=\"bt_709\"/>");
  check(
      longer.matrix && *longer.matrix == Ndi::YuvStandard::BT709,
      "matrix_hint does not satisfy matrix");
}

void testShaders()
{
  std::printf("\n== the shader each standard selects ==\n");
  const auto sd = Ndi::ndiColorMatrixOut(Ndi::YuvStandard::BT601);
  const auto hd = Ndi::ndiColorMatrixOut(Ndi::YuvStandard::BT709);
  const auto uhd = Ndi::ndiColorMatrixOut(Ndi::YuvStandard::BT2020);

  checkShader(sd, kBt601Limited, "BT.601 limited");
  checkShader(hd, kBt709Limited, "Rec.709 limited");
  checkShader(uhd, kBt2020Limited, "Rec.2020 limited");

  std::printf("  -- Rec.2020 is matrix-only --\n");
  // NDI's table is a statement about matrix coefficients. An SDR NDI frame
  // carries no primaries signal, so a receiver inverts the matrix and stops
  // there; gamut-mapping BT.709 -> BT.2020 on our side would desaturate the
  // picture against every one of them. bt2020SdrOutShader does exactly that
  // unless the INPUT transfer is AVCOL_TRC_UNSPECIFIED, which is why
  // ndiColorMatrixOut passes it.
  check(!has(uhd, "gamutConvertOut"), "no gamut conversion");
  check(!has(uhd, "Oetf"), "no OETF: the picture's transfer is left alone");
  check(!has(uhd, "Eotf"), "no EOTF: the picture's transfer is left alone");

  std::printf("  -- the three differ --\n");
  check(sd != hd, "BT.601 and Rec.709 select different shaders");
  check(hd != uhd, "Rec.709 and Rec.2020 select different shaders");
  check(sd != uhd, "BT.601 and Rec.2020 select different shaders");

  // The send-side convenience overload must agree with the setting.
  check(
      Ndi::ndiColorMatrixOut(Ndi::ColorSpaceSetting::Rec709, 64, 32)
          == Ndi::ndiColorMatrixOut(Ndi::YuvStandard::BT709),
      "the default yields Rec.709 even at an SD size");
  check(
      Ndi::ndiColorMatrixOut(Ndi::ColorSpaceSetting::AutoNdiRules, 64, 32)
          == Ndi::ndiColorMatrixOut(Ndi::YuvStandard::BT601),
      "Auto (NDI rules) yields BT.601 at an SD size");
}

void testAvMapping()
{
  std::printf("\n== the decode side's AVColorSpace ==\n");
  check(
      Ndi::avColorSpace(Ndi::YuvStandard::BT601) == AVCOL_SPC_SMPTE170M, "BT.601");
  check(Ndi::avColorSpace(Ndi::YuvStandard::BT709) == AVCOL_SPC_BT709, "Rec.709");
  check(
      Ndi::avColorSpace(Ndi::YuvStandard::BT2020) == AVCOL_SPC_BT2020_NCL,
      "Rec.2020 is non-constant-luminance, not ICtCp");
}
}

int main()
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  testDefault();
  testExplicit();
  testAutoNdiRules();
  testAutoHeuristic();
  testAutoMetadata();
  testParser();
  testShaders();
  testAvMapping();
  std::printf(
      "\nndi colour space rule: %s (%d failure%s)\n", g_fail ? "FAILED" : "passed",
      g_fail, g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
