// Offscreen GPU check for the encoded output paths.
//
// Ndi::WireEncodeRenderer::finishFrame does three things that nothing else
// tests: it runs a score::gfx wire encoder with the encoder's OWN readback
// disabled, it reads the encoder's output texture back into a buffer the output
// node owns, and it leaves the sender to work out that the picture is twice as
// wide as the texture that came back. Get that last part wrong and NDI ships a
// half-width picture that is still structurally valid, so nothing downstream
// complains.
//
// This runs exactly that sequence against a real QRhi and checks the bytes.
// The encoder itself is covered by tests/integration/EncoderMatrixTest.cpp;
// what is new here is the wiring the NDI addon puts around it.
//
// The multi-plane formats add a fourth thing -- assembling the planes into one
// framestore after the frame -- which runP216Case covers along with the 16-bit
// encoding itself.
//
// It also runs it at four sizes, because the fourth thing that path does is
// pick a colour matrix: NDI fixes the YUV standard by resolution and signals it
// nowhere (SDK Documentation v6.2, "Video Frames", p.54), so the same red must
// come out as three different triples depending on how big the picture is.
// ColorSpaceRuleTest pins the rule itself; this pins that the encoder really
// applies it.

#include <Gfx/Graph/RenderState.hpp>
#include <Gfx/Graph/encoders/ColorSpaceOut.hpp>
#include <Gfx/Graph/encoders/UYVY.hpp>
#include <Ndi/NdiColorSpace.hpp>
#include <Ndi/VideoFrameFormat.hpp>
#include <Ndi/WireEncode.hpp>


#include <QApplication>
#include <QTimer>

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace score::gfx;

namespace
{
int g_fail = 0;
void check(bool ok, const char* what)
{
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if(!ok)
    ++g_fail;
}

// Opaque red encoded limited-range, per standard: an asymmetric triple in every
// case, so a channel swap or a black frame cannot pass by accident, and far
// enough apart between standards that the wrong matrix cannot pass either.
// LoopbackTest pins the BT.601 one independently against the real SDK.
struct Expected
{
  int u, y, v;
};
constexpr Expected kRed[3] = {
    {90, 81, 240},   // BT.601 limited
    {102, 63, 240},  // Rec.709 limited
    {97, 74, 240},   // Rec.2020 limited
};
constexpr Expected expectedRed(Ndi::YuvStandard s)
{
  return kRed[int(s)];
}

// The measured pair must be nearer the expected standard than either other one.
// Rec.709 and Rec.2020 differ by only 5 in U for red, so a tolerance wide
// enough for GPU rounding is not by itself enough to tell them apart -- what
// separates them is that the distance to the right answer is the smallest.
int nearestStandard(int u, int y)
{
  int best = 0, bestD = 1 << 30;
  for(int i = 0; i < 3; i++)
  {
    const int d = std::abs(u - kRed[i].u) + std::abs(y - kRed[i].y);
    if(d < bestD)
    {
      bestD = d;
      best = i;
    }
  }
  return best;
}

// One run of WireEncodeRenderer::init + finishFrame's exact sequence, at one
// picture size, checking the bytes that come out.
void runCase(
    RenderState& state, int W, int H, Ndi::ColorSpaceSetting setting,
    bool structuralChecks)
{
  auto& rhi = *state.rhi;
  const auto standard = Ndi::resolveYuvStandard(setting, W, H);
  const auto exp = expectedRed(standard);
  std::printf(
      "\n  == %dx%d  %s -> %s (expect U=%d Y=%d V=%d for red) ==\n", W, H,
      Ndi::colorSpaceSettingName(setting), Ndi::ndiYuvStandardName(standard), exp.u,
      exp.y, exp.v);

  auto* input
      = rhi.newTexture(QRhiTexture::RGBA8, QSize(W, H), 1, QRhiTexture::UsedAsTransferSource);
  input->create();
  std::vector<uint8_t> px(size_t(W) * H * 4);
  for(size_t i = 0; i < px.size(); i += 4)
  {
    px[i + 0] = 255;
    px[i + 1] = 0;
    px[i + 2] = 0;
    px[i + 3] = 255;
  }

  UYVYEncoder enc;
  // The matrix comes from the same call WireEncodeRenderer::init makes, not
  // from a copy of it here: a test holding its own copy of the rule would still
  // pass with the production path hardcoded to one standard. Leaving it
  // defaulted gives BT.709 FULL range (U=98 Y=54 V=255 for red), which is what
  // this test caught the first time it ran.
  enc.init(rhi, state, input, W, H, Ndi::ndiColorMatrixOut(setting, W, H));
  // What WireEncodeRenderer::init does: the node reads the output texture back
  // into its own pool, so the encoder must not also read it into its single
  // internal buffer.
  enc.setReadbackEnabled(false);
  check(enc.outputTexture() != nullptr, "encoder exposes an output texture");
  check(
      enc.outputTexture() && enc.outputTexture()->pixelSize() == QSize(W / 2, H),
      "output texture is HALF width (two pixels per UYVY macropixel)");

  // What WireEncodeRenderer::finishFrame does: upload, encode, then read the
  // encoder's output texture into a buffer of our choosing.
  QRhiReadbackResult rb;
  {
    QRhiCommandBuffer* cb{};
    rhi.beginOffscreenFrame(&cb);
    auto* up = rhi.nextResourceUpdateBatch();
    QRhiTextureSubresourceUploadDescription sub{
        QByteArray(reinterpret_cast<const char*>(px.data()), int(px.size()))};
    up->uploadTexture(input, QRhiTextureUploadDescription{{0, 0, sub}});
    cb->resourceUpdate(up);

    enc.exec(rhi, *cb);

    auto* batch = rhi.nextResourceUpdateBatch();
    batch->readBackTexture(QRhiReadbackDescription{enc.outputTexture()}, &rb);
    cb->resourceUpdate(batch);
    rhi.endOffscreenFrame();
  }

  check(
      enc.readback(0).data.isEmpty(),
      "the encoder's own readback stayed empty (no second transfer per frame)");
  check(!rb.data.isEmpty(), "our readback received the encoded frame");
  if(rb.data.isEmpty())
    return;

  check(rb.pixelSize == QSize(W / 2, H), "readback is half width, full height");

  const int stride = Ndi::readbackStride(rb.data.size(), rb.pixelSize.height());
  check(stride >= 2 * W, "stride covers a packed UYVY row");

  // The bytes themselves: (U, Y0, V, Y1) for red in the standard this size
  // calls for, allowing for the GPU's rounding of the colour matrix.
  const auto* b = reinterpret_cast<const uint8_t*>(rb.data.constData());
  const int U = b[0], Y0 = b[1], V = b[2], Y1 = b[3];
  std::printf("    first macropixel: U=%d Y0=%d V=%d Y1=%d\n", U, Y0, V, Y1);
  check(std::abs(U - exp.u) <= 4, "U matches the standard for this size");
  check(std::abs(Y0 - exp.y) <= 4, "Y0 matches the standard for this size");
  check(std::abs(V - exp.v) <= 4, "V matches the standard for this size");
  check(std::abs(Y1 - exp.y) <= 4, "Y1 matches the standard for this size");
  // And it is nearer this standard than the other two -- Rec.709 and Rec.2020
  // sit 5 apart in U, so the tolerance above cannot separate them on its own.
  check(
      nearestStandard(U, Y0) == int(standard),
      "the encoding is nearest THIS standard, not another one");
  // Not a gray frame, and not channel-swapped: V must sit well above U.
  check(V > U + 100, "V is well above U (red, not gray or swapped)");

  // The last row too, so a Y-flip that dropped rows or an encoder writing only
  // the first row would not pass. The encoder folds the flip in, and a solid
  // colour is flip-invariant -- what this catches is missing rows, not the flip
  // direction.
  const auto* last = b + size_t(rb.pixelSize.height() - 1) * stride;
  check(
      std::abs(last[1] - exp.y) <= 4 && std::abs(last[2] - exp.v) <= 4,
      "the last row is encoded too");

  if(structuralChecks)
  {
    // And the part the sender gets wrong if it trusts the readback's width: the
    // picture is twice as wide as the texture.
    {
      NDIlib_video_frame_v2_t f{};
      const int pictureWidth = rb.pixelSize.width() * 2;
      const bool ok = Ndi::describeVideoFrame(
          "UYVY", b, pictureWidth, rb.pixelSize.height(), stride, f);
      check(ok, "describeVideoFrame accepts the encoded readback");
      check(f.xres == W, "xres is the PICTURE width, not the texture width");
      check(f.yres == H, "yres is the picture height");
      check(f.FourCC == NDIlib_FourCC_video_type_UYVY, "FourCC is UYVY");
      check(f.p_data == b, "zero copy: p_data is the readback itself");
      check(f.line_stride_in_bytes == stride, "stride is the readback's own");
    }

    // The counter-check: passing the texture width through, which is the mistake
    // this test exists to catch, must not silently produce a valid-looking frame
    // of the wrong size.
    {
      NDIlib_video_frame_v2_t f{};
      Ndi::describeVideoFrame(
          "UYVY", b, rb.pixelSize.width(), rb.pixelSize.height(), stride, f);
      check(
          f.xres != W,
          "sanity: the texture width really is different from the picture width, "
          "so the check above is not vacuous");
    }
  }

  delete input;
  enc.release();
}

// The 16-bit path.
//
// P216 has two encoders in score::gfx: two plane textures, and one texture that
// IS the framestore. NDI takes the second, so the send path has no assembly
// step for it -- but the first one is the trusted implementation, so what this
// checks is that the two agree BYTE FOR BYTE. If they ever disagree, one of
// them is wrong and this says which bytes.
void runP216Case(RenderState& state, int W, int H)
{
  auto& rhi = *state.rhi;
  std::printf("\n  == %dx%d P216 (16-bit 4:2:2) ==\n", W, H);

  auto* input
      = rhi.newTexture(QRhiTexture::RGBA8, QSize(W, H), 1, QRhiTexture::UsedAsTransferSource);
  input->create();
  std::vector<uint8_t> px(size_t(W) * H * 4);
  for(size_t i = 0; i < px.size(); i += 4)
  {
    px[i + 0] = 255;
    px[i + 1] = 0;
    px[i + 2] = 0;
    px[i + 3] = 255;
  }

  const auto encoding = Ndi::ndiEncoding("P216");
  check(encoding.hasEncoder(), "P216 has a GPU encoder");
  check(encoding.floatRender, "P216 asks for a float render target");

  const auto matrix = Ndi::ndiColorMatrixOut(Ndi::ColorSpaceSetting::Rec709, W, H);

  // The production encoder: one texture, one readback, already the framestore.
  auto packed = Ndi::makeNdiEncoder("P216");
  // And the plane-based one, as the reference.
  auto planar = score::gfx::makeWireEncoder(
      score::gfx::interop::VideoPixelFormat::P216, /* contiguous */ false);
  if(!packed || !planar)
  {
    check(false, "both P216 encoders exist");
    delete input;
    return;
  }
  packed->init(rhi, state, input, W, H, matrix);
  planar->init(rhi, state, input, W, H, matrix);
  check(packed->planeCount() == 1, "the packed encoder is single-plane");
  check(planar->planeCount() == 2, "the plane encoder has two planes");
  check(packed->outputTexture() != nullptr, "the packed encoder exposes a texture");

  {
    QRhiCommandBuffer* cb{};
    rhi.beginOffscreenFrame(&cb);
    auto* up = rhi.nextResourceUpdateBatch();
    QRhiTextureSubresourceUploadDescription sub{
        QByteArray(reinterpret_cast<const char*>(px.data()), int(px.size()))};
    up->uploadTexture(input, QRhiTextureUploadDescription{{0, 0, sub}});
    cb->resourceUpdate(up);
    packed->exec(rhi, *cb);
    planar->exec(rhi, *cb);
    rhi.endOffscreenFrame();
  }

  const auto& packedRb = packed->readback(0);
  const auto& y = planar->readback(0);
  const auto& uv = planar->readback(1);
  check(!packedRb.data.isEmpty(), "the packed framestore came back");
  check(!y.data.isEmpty() && !uv.data.isEmpty(), "both planes came back");
  if(packedRb.data.isEmpty() || y.data.isEmpty() || uv.data.isEmpty())
  {
    packed->release();
    planar->release();
    delete input;
    return;
  }

  // The property that separates P216 from P010: 4:2:2 keeps every line's
  // chroma, so the UV plane is half width and FULL height.
  check(y.pixelSize == QSize(W, H), "Y plane is the full picture");
  check(uv.pixelSize == QSize(W / 2, H), "UV plane is half width, FULL height");
  check(
      packedRb.pixelSize == QSize(W / 2, H * 2),
      "the packed framestore is half width, twice the height");

  const int rowBytes = Ndi::packedRowBytes("P216", W);
  const size_t total = Ndi::framestoreBytes("P216", W, H);
  check(rowBytes == 2 * W, "P216 luma row is 2 bytes per pixel");
  check(total == size_t(4) * W * H, "framestore is two planes of 2*W*H");
  check(size_t(packedRb.data.size()) == total, "the readback IS the framestore");

  // Packed-equals-planes is score's gate now, over four formats and seven
  // sizes, in score-plugin-gfx/tests/EncoderTester.cpp. What stays here is
  // what is NDI's business: the framestore geometry and its description.

  const auto* got = reinterpret_cast<const uint8_t*>(packedRb.data.constData());

  // And the values themselves, in the standard the default calls for.
  const auto exp8 = expectedRed(
      Ndi::resolveYuvStandard(Ndi::ColorSpaceSetting::Rec709, W, H));
  const auto* y16 = reinterpret_cast<const uint16_t*>(got);
  const auto* uv16 = reinterpret_cast<const uint16_t*>(got + size_t(rowBytes) * H);
  std::printf(
      "    Y=%u  U=%u  V=%u   (8-bit equivalents %u %u %u)\n", y16[0], uv16[0], uv16[1],
      y16[0] / 257, uv16[0] / 257, uv16[1] / 257);
  check(std::abs(int(y16[0]) - exp8.y * 257) <= 300, "Y matches the standard");
  check(std::abs(int(uv16[0]) - exp8.u * 257) <= 300, "U matches the standard");
  check(std::abs(int(uv16[1]) - exp8.v * 257) <= 300, "V matches the standard");

  const auto* yLast = y16 + size_t(H - 1) * W;
  const auto* uvLast = uv16 + size_t(H - 1) * W;
  check(std::abs(int(yLast[0]) - exp8.y * 257) <= 300, "Y's last row is encoded");
  check(std::abs(int(uvLast[1]) - exp8.v * 257) <= 300, "UV's last row is encoded");

  // And what the sender would put on the wire.
  {
    NDIlib_video_frame_v2_t f{};
    const int rows = Ndi::framestoreRows("P216", H);
    const int stride = Ndi::readbackStride(int(packedRb.data.size()), rows);
    check(rows == 2 * H, "P216 framestore is twice the picture's rows");
    check(stride == rowBytes, "the stride derived from the framestore is the row size");
    check(
        Ndi::describeVideoFrame("P216", got, W, H, stride, f),
        "describeVideoFrame accepts the framestore");
    check(f.FourCC == NDIlib_FourCC_video_type_P216, "FourCC is P216");
    check(f.xres == W && f.yres == H, "describes the picture, not the framestore");
    check(f.p_data == got, "zero copy: p_data is the readback itself");
    // Where the SDK will look for chroma: p_uv = p_data + stride * yres.
    check(
        f.p_data + size_t(f.line_stride_in_bytes) * f.yres
            == reinterpret_cast<const uint8_t*>(uv16),
        "the SDK's p_uv lands exactly on the chroma plane");
  }

  packed->release();
  planar->release();
  delete input;
}

// The 8-bit planar formats. What is really under test is Ndi::ndiEncoding's
// plane table: it claims each encoder's plane geometry and byte width, and
// nothing else checks that claim against the encoder that actually runs. A
// wrong entry there does not crash -- it assembles a framestore of the right
// total size with the planes at the wrong offsets, which NDI sends happily.
//
// Returns the assembled framestore so the caller can compare two formats that
// differ only in plane order.
std::vector<uint8_t> runPlanarCase(RenderState& state, const char* fmt, int W, int H)
{
  auto& rhi = *state.rhi;
  const auto exp = expectedRed(
      Ndi::resolveYuvStandard(Ndi::ColorSpaceSetting::Rec709, W, H));
  std::printf("\n  == %dx%d %s ==\n", W, H, fmt);

  auto* input
      = rhi.newTexture(QRhiTexture::RGBA8, QSize(W, H), 1, QRhiTexture::UsedAsTransferSource);
  input->create();
  std::vector<uint8_t> px(size_t(W) * H * 4);
  for(size_t i = 0; i < px.size(); i += 4)
  {
    px[i + 0] = 255;
    px[i + 1] = 0;
    px[i + 2] = 0;
    px[i + 3] = 255;
  }

  const auto encoding = Ndi::ndiEncoding(fmt);
  auto enc = Ndi::makeNdiEncoder(fmt);
  if(!enc)
  {
    check(false, "no encoder");
    delete input;
    return {};
  }
  enc->init(
      rhi, state, input, W, H,
      Ndi::ndiColorMatrixOut(Ndi::ColorSpaceSetting::Rec709, W, H));

  // The 4:2:0 formats used to come back as two or three plane textures that
  // this test concatenated. They now come back as one texture that IS the
  // framestore, like P216 -- so there is no assembly step, no concatenation,
  // and one readback instead of three.
  //
  // That the packed bytes EQUAL what the plane encoders produce is checked in
  // score's own tests/EncoderTester.cpp, where the encoders live and where
  // every consumer of makeWireEncoder benefits from it. What is NDI's business,
  // and stays here, is the framestore geometry and the plane ORDER -- which is
  // the only thing separating I420 from YV12.
  check(enc->planeCount() == 1, "4:2:0 sends as a single packed framestore");

  // Exactly what WireEncodeRenderer does: the encoder's own readback is off,
  // and the node reads the output texture into a buffer it owns.
  enc->setReadbackEnabled(false);
  QRhiReadbackResult rb;
  {
    QRhiCommandBuffer* cb{};
    rhi.beginOffscreenFrame(&cb);
    auto* up = rhi.nextResourceUpdateBatch();
    QRhiTextureSubresourceUploadDescription sub{
        QByteArray(reinterpret_cast<const char*>(px.data()), int(px.size()))};
    up->uploadTexture(input, QRhiTextureUploadDescription{{0, 0, sub}});
    cb->resourceUpdate(up);
    enc->exec(rhi, *cb);
    auto* batch = rhi.nextResourceUpdateBatch();
    batch->readBackTexture(QRhiReadbackDescription{enc->outputTexture()}, &rb);
    cb->resourceUpdate(batch);
    rhi.endOffscreenFrame();
  }

  const size_t total = Ndi::framestoreBytes(fmt, W, H);
  check(size_t(rb.data.size()) == total, "the readback IS the framestore");
  if(size_t(rb.data.size()) != total)
  {
    std::printf(
        "    readback %d bytes, framestore should be %zu\n", int(rb.data.size()),
        total);
    enc->release();
    delete input;
    return {};
  }

  const auto* b = reinterpret_cast<const uint8_t*>(rb.data.constData());
  std::vector<uint8_t> out(b, b + total);

  // Luma first, at the standard this size calls for.
  std::printf("    first luma byte: %d (expect %d)\n", out[0], exp.y);
  check(std::abs(int(out[0]) - exp.y) <= 4, "luma matches the standard");

  // And the frame describes truthfully, with the stride the SDK walks the
  // other planes from.
  {
    const int stride = Ndi::readbackStride(int(total), Ndi::framestoreRows(fmt, H));
    check(stride == Ndi::packedRowBytes(fmt, W), "stride is the packed row size");
    NDIlib_video_frame_v2_t f{};
    check(
        Ndi::describeVideoFrame(fmt, b, W, H, stride, f),
        "describeVideoFrame accepts the framestore");
    check(f.xres == W && f.yres == H, "describes the picture, not the framestore");
    check(f.p_data == b, "zero copy: p_data is the readback itself");
  }

  enc->release();
  delete input;
  return out;
}

void runTests()
{
  const QByteArray apiEnv = qgetenv("SCORE_TEST_API").toLower();
  const GraphicsApi api = (apiEnv == "vulkan" || apiEnv == "vk") ? GraphicsApi::Vulkan
                                                                 : GraphicsApi::OpenGL;
  auto state = createRenderState(api, QSize(192, 64), nullptr);
  if(!state || !state->rhi)
  {
    // No GL-capable display: skip rather than fail, the way the loopback test
    // does when the NDI runtime will not initialise.
    std::printf("  skip: no QRhi (needs a GL-capable display)\n");
    std::exit(77);
  }
  std::printf("  backend=%s\n", state->rhi->backendName());

  // The render state's own size is irrelevant here -- it carries the backend,
  // and the encoder is told the picture size explicitly. Every width is even:
  // UYVY carries two pixels per macropixel and init() refuses odd ones.
  //
  // The default first, at sizes that would have varied under the old
  // resolution rule: every receiver measured decodes Rec.709 whatever the size,
  // so the default must produce the same triple at all four.
  using S = Ndi::ColorSpaceSetting;
  runCase(*state, 192, 64, S::Rec709, true);
  runCase(*state, 720, 576, S::Rec709, false);
  runCase(*state, 1280, 720, S::Rec709, true);
  runCase(*state, 1922, 1082, S::Rec709, false);

  // Then the settings that do vary, at the boundaries of the rules they follow:
  // 720x576 is the largest SD picture and 1922x1082 the smallest UHD one, so an
  // off-by-one in either comparison shows up here as a wrong matrix.
  runCase(*state, 720, 576, S::AutoNdiRules, false);    // -> BT.601
  runCase(*state, 1280, 720, S::AutoNdiRules, false);   // -> Rec.709
  runCase(*state, 1922, 1082, S::AutoNdiRules, false);  // -> Rec.2020
  runCase(*state, 1024, 768, S::AutoHeuristic, false);  // -> BT.601, where the
                                                        // SDK rule says Rec.709
  runCase(*state, 192, 64, S::BT601, false);
  runCase(*state, 192, 64, S::Rec2020, false);

  // And the 16-bit format, at the addon's default size.
  runP216Case(*state, 1280, 720);

  // The 8-bit planar formats. NV12 interleaves its chroma, I420 and YV12 keep
  // U and V in separate planes -- and differ ONLY in which comes first, which
  // is the last check here.
  const int W = 1280, H = 720;
  runPlanarCase(*state, "NV12", W, H);
  const auto i420 = runPlanarCase(*state, "I420", W, H);
  const auto yv12 = runPlanarCase(*state, "YV12", W, H);

  std::printf("\n  == I420 vs YV12: the chroma planes are exchanged ==\n");
  if(!i420.empty() && !yv12.empty() && i420.size() == yv12.size())
  {
    const auto exp = expectedRed(
        Ndi::resolveYuvStandard(Ndi::ColorSpaceSetting::Rec709, W, H));
    const size_t chroma = size_t(W) * H;            // where the first chroma plane starts
    const size_t second = chroma + size_t(W / 2) * (H / 2);
    std::printf(
        "    I420: first chroma plane %u, second %u\n", i420[chroma], i420[second]);
    std::printf(
        "    YV12: first chroma plane %u, second %u\n", yv12[chroma], yv12[second]);
    // Red is the right probe for this: its U and V are about as far apart as
    // 8-bit values get (102 and 240 at Rec.709), so a swap cannot hide.
    check(std::abs(int(i420[chroma]) - exp.u) <= 4, "I420 puts U first");
    check(std::abs(int(i420[second]) - exp.v) <= 4, "I420 puts V second");
    check(std::abs(int(yv12[chroma]) - exp.v) <= 4, "YV12 puts V first");
    check(std::abs(int(yv12[second]) - exp.u) <= 4, "YV12 puts U second");
    check(
        std::equal(i420.begin(), i420.begin() + chroma, yv12.begin()),
        "and their luma planes are identical");
  }
  else
  {
    check(false, "I420 and YV12 both produced a framestore");
  }
}
}

int main(int argc, char** argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  QLocale::setDefault(QLocale::C);
  std::setlocale(LC_ALL, "C");
  qputenv("SCORE_DISABLE_AUDIOPLUGINS", "1");
  qputenv("SCORE_AUDIO_BACKEND", "dummy");

// NOT score::MinimalApplication.
//
// MinimalApplication runs score's whole plugin bootstrap, and this target has
// the addon's own root on its include path (it must, to reach <Ndi/...>).
// That makes score_static_plugins.hpp find <score_addon_ndi.hpp>, register the
// NDI addon as a static plugin, and drag in ScenarioApplicationPlugin, which
// aborts looking for an InspectorWidgetList this unit test has no reason to
// link:
//
//   score::ApplicationComponents::interfaces<Inspector::InspectorWidgetList>
//   <- Scenario::ScenarioApplicationPlugin::initialize
//   <- score::MinimalApplication::MinimalApplication
//
// None of that is wanted here. What the test needs from Qt is a GUI
// application so createRenderState() can make a context, and nothing else.
  QApplication app(argc, argv);

  QMetaObject::invokeMethod(
      &app,
      [] {
        runTests();
        std::printf(
            "\nndi encode renderer: %s (%d failure%s)\n", g_fail ? "FAILED" : "passed",
            g_fail, g_fail == 1 ? "" : "s");
        QCoreApplication::exit(g_fail ? 1 : 0);
      },
      Qt::QueuedConnection);

  return app.exec();
}
