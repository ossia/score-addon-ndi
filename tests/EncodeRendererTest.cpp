// Offscreen GPU check for the UYVY output path.
//
// Ndi::UyvyEncodeRenderer::finishFrame does three things that nothing else
// tests: it runs score::gfx::UYVYEncoder with the encoder's OWN readback
// disabled, it reads the encoder's output texture back into a buffer the output
// node owns, and it leaves the sender to work out that the picture is twice as
// wide as the texture that came back. Get that last part wrong and NDI ships a
// half-width picture that is still structurally valid, so nothing downstream
// complains.
//
// This runs exactly that sequence against a real QRhi and checks the bytes.
// The encoder itself is covered by tests/integration/EncoderMatrixTest.cpp;
// what is new here is the wiring the NDI addon puts around it.

#include <Gfx/Graph/RenderState.hpp>
#include <Gfx/Graph/encoders/ColorSpaceOut.hpp>
#include <Gfx/Graph/encoders/UYVY.hpp>
#include <Ndi/VideoFrameFormat.hpp>

#include <core/application/MinimalApplication.hpp>

#include <QApplication>
#include <QTimer>

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

void runTests()
{
  const int W = 192, H = 64;  // even width: UYVY carries two pixels per macropixel

  const QByteArray apiEnv = qgetenv("SCORE_TEST_API").toLower();
  const GraphicsApi api = (apiEnv == "vulkan" || apiEnv == "vk") ? GraphicsApi::Vulkan
                                                                 : GraphicsApi::OpenGL;
  auto state = createRenderState(api, QSize(W, H), nullptr);
  if(!state || !state->rhi)
  {
    // No GL-capable display: skip rather than fail, the way the loopback test
    // does when the NDI runtime will not initialise.
    std::printf("  skip: no QRhi (needs a GL-capable display)\n");
    std::exit(77);
  }
  auto& rhi = *state->rhi;
  std::printf("  backend=%s  %dx%d\n", rhi.backendName(), W, H);

  // Opaque red in, because its BT.601 limited-range encoding is a known,
  // asymmetric triple -- Y=81, U=90, V=240 -- so a channel swap or a black
  // frame cannot pass by accident. The loopback test pins the same values
  // independently against the real SDK.
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
  // The same matrix UyvyEncodeRenderer asks for. Leaving it defaulted gives
  // BT.709 FULL range (U=98 Y=54 V=255 for red), which is what this test caught
  // the first time it ran.
  enc.init(
      rhi, *state, input, W, H,
      colorMatrixOut(
          AVCOL_SPC_SMPTE170M, AVCOL_TRC_SMPTE170M, AVCOL_RANGE_MPEG,
          AVCOL_PRI_SMPTE170M));
  // What UyvyEncodeRenderer::init does: the node reads the output texture back
  // into its own pool, so the encoder must not also read it into its single
  // internal buffer.
  enc.setReadbackEnabled(false);
  check(enc.outputTexture() != nullptr, "encoder exposes an output texture");
  check(
      enc.outputTexture() && enc.outputTexture()->pixelSize() == QSize(W / 2, H),
      "output texture is HALF width (two pixels per UYVY macropixel)");

  // What UyvyEncodeRenderer::finishFrame does: upload, encode, then read the
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

  // The bytes themselves: (U, Y0, V, Y1) for red, allowing for the GPU's
  // rounding of the colour matrix.
  const auto* b = reinterpret_cast<const uint8_t*>(rb.data.constData());
  const int U = b[0], Y0 = b[1], V = b[2], Y1 = b[3];
  std::printf("    first macropixel: U=%d Y0=%d V=%d Y1=%d\n", U, Y0, V, Y1);
  check(std::abs(U - 90) <= 6, "U ~= 90 for red");
  check(std::abs(Y0 - 81) <= 6, "Y0 ~= 81 for red");
  check(std::abs(V - 240) <= 6, "V ~= 240 for red");
  check(std::abs(Y1 - 81) <= 6, "Y1 ~= 81 for red");
  // Not a gray frame, and not channel-swapped: V must sit well above U.
  check(V > U + 100, "V is well above U (red, not gray or swapped)");

  // The last row too, so a Y-flip that dropped rows or an encoder writing only
  // the first row would not pass. The encoder folds the flip in, and a solid
  // colour is flip-invariant -- what this catches is missing rows, not the flip
  // direction.
  const auto* last = b + size_t(rb.pixelSize.height() - 1) * stride;
  check(
      std::abs(last[1] - 81) <= 6 && std::abs(last[2] - 240) <= 6,
      "the last row is encoded too");

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
    Ndi::describeVideoFrame("UYVY", b, rb.pixelSize.width(), rb.pixelSize.height(), stride, f);
    check(
        f.xres != W,
        "sanity: the texture width really is different from the picture width, "
        "so the check above is not vacuous");
  }

  delete input;
  enc.release();
}
}

int main(int argc, char** argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  QLocale::setDefault(QLocale::C);
  std::setlocale(LC_ALL, "C");
  qputenv("SCORE_DISABLE_AUDIOPLUGINS", "1");
  qputenv("SCORE_AUDIO_BACKEND", "dummy");

  score::MinimalApplication app(argc, argv);

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
