// The send path from the scene texture to the bytes the SDK reads, through the
// real Ndi::WireEncodeRenderer, for every wire format.
//
// EncodeRendererTest and FormatSweepTest check the encoders. This checks what
// the output node does around them: the readback goes straight into one of
// four send slots -- imported pages on Vulkan, a buffer finished one frame late
// on OpenGL -- the slots rotate through Ndi::ReadbackPool exactly as in
// OutputNode::render, and the sender reads each frame through the slot's
// data/stride. Three things are checked per format:
//
//  1. the frames the "sender" takes are byte-identical to the same scene read
//     back through QRhi's own readback, which is what every encoder test
//     measures; any offset, stride or ordering mistake in the direct path, or a
//     slot handed over before its copy finished, breaks that equality;
//  2. the layout and the values: P216 is decoded on the CPU as NDI's headers
//     describe it -- 16-bit luma rows, then interleaved 16-bit Cb/Cr rows of
//     the same stride -- and compared with the picture pixel by pixel, the
//     other formats are probed for orientation and level;
//  3. a frame nobody receives reads nothing back and queues nothing.
//
// Needs a QRhi; exits 77 without one. SCORE_TEST_API=vulkan picks Vulkan.

#include <Gfx/Graph/Graph.hpp>
#include <Gfx/Graph/OutputNode.hpp>
#include <Gfx/Graph/RenderList.hpp>
#include <Gfx/Graph/RenderState.hpp>
#include <Gfx/Graph/VideoNode.hpp>
#include <Ndi/ReadbackPool.hpp>
#include <Ndi/VideoFrameFormat.hpp>
#include <Ndi/WireEncode.hpp>
#include <Ndi/WireEncodeRenderer.hpp>
#include <Video/VideoInterface.hpp>

#include <QApplication>
#include <QtGui/private/qrhi_p.h>

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace score::gfx;

namespace
{
int g_fail = 0;
void check(bool ok, const std::string& what)
{
  if(!ok)
  {
    std::printf("    [FAIL] %s\n", what.c_str());
    ++g_fail;
  }
}

// ------------------------------------------------------------------ the scene
//
// R grows to the right, G grows downwards, B is constant, and the top-left
// tenth is white: every pixel is distinct along both axes, so a flipped,
// mirrored, shifted or transposed frame cannot pass for the right one.
constexpr int kBlue = 64;

struct Rgb
{
  double r, g, b;
};

Rgb pictureAt(int x, int y, int W, int H)
{
  if(x < W / 10 && y < H / 10)
    return {255, 255, 255};
  return {
      std::round(x * 255.0 / (W - 1)), std::round(y * 255.0 / (H - 1)), double(kBlue)};
}

struct OneFrameSource final : Video::VideoInterface
{
  AVFrame* m_frame{};
  explicit OneFrameSource(AVFrame* f)
      : m_frame{f}
  {
  }
  AVFrame* dequeue_frame() noexcept override { return m_frame; }
  void release_frame(AVFrame*) noexcept override { }
};

// ----------------------------------------------------- the output under test
//
// What Ndi::OutputNode does on the render thread, minus the SDK: the same
// render target format choice, the same renderer, the same slots and pool,
// and the same beginSendFrame/endSendFrame calls around the frame. The sender
// thread's part -- acquire, read through data/stride, markSent -- is done
// inline after each frame, which is one of the interleavings the pool's model
// check covers.
struct SlotOutput final : score::gfx::OutputNode
{
  static constexpr int kSlots = 4;

  QSize m_size;
  std::string m_format;
  std::shared_ptr<RenderState> m_state;
  QRhiTexture* m_tex{};
  QRhiTextureRenderTarget* m_rt{};
  std::weak_ptr<RenderList> m_renderer;
  Ndi::SendSlot m_slots[kSlots];
  Ndi::ReadbackPool<kSlots> m_pool;
  Ndi::WireEncodeRenderer* m_wire{};

  SlotOutput(QSize sz, std::string format)
      : m_size{sz}
      , m_format{std::move(format)}
  {
    input.push_back(new score::gfx::Port{this, {}, score::gfx::Types::Image, {}});
    for(auto& s : m_slots)
      s.allocate(Ndi::framestoreBytes(m_format, sz.width(), sz.height()));
    m_pool.m_running = true;
  }

  void createOutput(score::gfx::OutputConfiguration conf) override
  {
    m_state = createRenderState(conf.graphicsApi, m_size, nullptr);
    if(!m_state || !m_state->rhi)
      return;
    m_state->outputSize = m_state->renderSize;
    auto rhi = m_state->rhi;
    const auto fmt = Ndi::ndiEncoding(m_format).floatRender
                             && rhi->isTextureFormatSupported(QRhiTexture::RGBA16F)
                         ? QRhiTexture::RGBA16F
                         : QRhiTexture::RGBA8;
    m_tex = rhi->newTexture(
        fmt, m_state->renderSize, 1,
        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
    m_tex->create();
    m_rt = rhi->newTextureRenderTarget({m_tex});
    m_state->renderPassDescriptor = m_rt->newCompatibleRenderPassDescriptor();
    m_rt->setRenderPassDescriptor(m_state->renderPassDescriptor);
    m_rt->create();
    if(conf.onReady)
      conf.onReady();
  }

  void destroyOutput() override
  {
    if(!m_state)
      return;
    releaseRegistry();
    delete m_rt;
    m_rt = nullptr;
    delete m_state->renderPassDescriptor;
    m_state->renderPassDescriptor = nullptr;
    delete m_tex;
    m_tex = nullptr;
    m_state->destroy();
    m_state.reset();
  }

  void renderFrame(bool active)
  {
    auto r = m_renderer.lock();
    if(!r || !m_state || !m_wire)
      return;
    auto rhi = m_state->rhi;
    Ndi::beginSendFrame(*m_wire, m_pool, active);
    QRhiCommandBuffer* cb{};
    if(rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
      return;
    r->render(*cb);
    rhi->endOffscreenFrame();
    Ndi::endSendFrame(*m_wire, m_pool);
  }

  void render() override { renderFrame(true); }

  score::gfx::OutputNodeRenderer*
  createRenderer(RenderList& r) const noexcept override
  {
    score::gfx::TextureRenderTarget rt{
        .texture = m_tex,
        .renderPass = m_state->renderPassDescriptor,
        .renderTarget = m_rt};
    auto* self = const_cast<SlotOutput*>(this);
    self->m_wire = new Ndi::WireEncodeRenderer{
        *this, rt, std::span<Ndi::SendSlot>{self->m_slots}, m_format,
        Ndi::ColorSpaceSetting::Rec709};
    return self->m_wire;
  }

  std::shared_ptr<RenderState> renderState() const override { return m_state; }
  bool canRender() const override { return bool(m_state); }
  void startRendering() override { }
  void stopRendering() override { }
  void onRendererChange() override { }
  void setRenderer(std::shared_ptr<RenderList> r) override { m_renderer = r; }
  RenderList* renderer() const override { return m_renderer.lock().get(); }
  Configuration configuration() const noexcept override
  {
    return {.manualRenderingRate = 1000. / 60.};
  }
};

struct Sent
{
  std::vector<uint8_t> bytes; // the framestore, rows packed
  int stride{};
};

struct Run
{
  bool ok{};
  bool direct{};
  bool late{};
  std::vector<Sent> frames;
  int queuedWhileInactive{-1};
};

// Renders `frames` frames of the scene through a SlotOutput and returns every
// frame the pool hands to the sender, read the way the sender reads it.
Run runOnce(GraphicsApi api, const std::string& format, int W, int H, int frames)
{
  Run run;

  AVFrame* frame = av_frame_alloc();
  frame->format = AV_PIX_FMT_RGBA;
  frame->width = W;
  frame->height = H;
  std::vector<uint8_t> store(size_t(W) * H * 4);
  for(int y = 0; y < H; y++)
    for(int x = 0; x < W; x++)
    {
      const auto c = pictureAt(x, y, W, H);
      auto* p = &store[(size_t(y) * W + x) * 4];
      p[0] = uint8_t(c.r);
      p[1] = uint8_t(c.g);
      p[2] = uint8_t(c.b);
      p[3] = 255;
    }
  frame->data[0] = store.data();
  frame->linesize[0] = W * 4;

  auto source = std::make_shared<OneFrameSource>(frame);
  source->width = W;
  source->height = H;
  source->pixel_format = AV_PIX_FMT_RGBA;

  auto* video = new score::gfx::VideoNode(source, std::nullopt);
  auto* out = new SlotOutput(QSize(W, H), format);

  {
    score::gfx::Graph graph;
    graph.addNode(video);
    graph.addNode(out);
    graph.addEdge(
        video->output[0], out->input[0], Process::CableType::ImmediateGlutton);
    graph.createAllRenderLists(api);

    if(out->m_wire)
    {
      run.ok = true;
      const int rows = Ndi::framestoreRows(format, H);
      const int rowBytes = Ndi::packedRowBytes(format, W);

      auto take = [&] {
        while(out->m_pool.m_frameReady.load())
        {
          const int idx = out->m_pool.acquireForSending();
          if(idx < 0)
            break;
          const auto& slot = out->m_slots[idx];
          Sent s;
          s.stride = slot.stride;
          NDIlib_video_frame_v2_t f{};
          if(Ndi::describeVideoFrame(format, slot.data, W, H, slot.stride, f))
          {
            s.bytes.resize(size_t(rowBytes) * rows);
            for(int r = 0; r < rows; r++)
              std::memcpy(
                  s.bytes.data() + size_t(r) * rowBytes,
                  slot.data + size_t(r) * slot.stride, rowBytes);
          }
          run.frames.push_back(std::move(s));
          out->m_pool.markSent(idx);
        }
      };

      // The first frames build pipelines and upload the picture; they are
      // not received, so they must not reach the sender either.
      for(int i = 0; i < 3; i++)
      {
        video->update();
        out->renderFrame(false);
      }
      take();
      const int warmupSent = int(run.frames.size());

      for(int i = 0; i < frames; i++)
      {
        video->update();
        out->renderFrame(true);
        if(i == 0)
        {
          run.direct = out->m_wire->readsIntoSlots();
          run.late = out->m_wire->finishesOneFrameLate();
        }
        take();
      }

      // Nobody receiving: the frame still renders, but nothing is read back,
      // so nothing new reaches the sender. The copy a late backend left
      // pending from the last active frame is queued at the start of this one,
      // so take that first.
      out->renderFrame(false);
      take();
      const int before = int(run.frames.size());
      out->renderFrame(false);
      out->renderFrame(false);
      take();
      run.queuedWhileInactive = int(run.frames.size()) - before + warmupSent;
      run.frames.erase(run.frames.begin(), run.frames.begin() + warmupSent);
    }
    graph.clearEdges();
  }

  av_frame_free(&frame);
  return run;
}

// BT.709 limited range, the default send matrix.
Rgb decode709(double Y, double Cb, double Cr, double scale)
{
  const double y = (Y / scale - 16.0) / 219.0;
  const double u = (Cb / scale - 128.0) / 224.0;
  const double v = (Cr / scale - 128.0) / 224.0;
  return {
      255.0 * (y + 1.5748 * v), 255.0 * (y - 0.1873 * u - 0.4681 * v),
      255.0 * (y + 1.8556 * u)};
}

double rgbError(const Rgb& a, const Rgb& b)
{
  return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
}

// P216 as Processing.NDI.structs.h describes it: a 16bpp luminance buffer,
// immediately followed by an interleaved buffer of 16bpp Cb, Cr pairs.
void checkP216(const Sent& s, int W, int H)
{
  check(s.stride == 2 * W, "P216 stride is two bytes per pixel");
  const auto* luma = s.bytes.data();
  const auto* chroma = luma + size_t(s.stride) * H;
  auto word = [](const uint8_t* p) { return double(p[0] | (p[1] << 8)); };

  double worst = 0, sum = 0;
  int wx = 0, wy = 0, n = 0;
  for(int y = 0; y < H; y++)
    for(int x = 0; x < W; x++)
    {
      // The white square's edges are where 4:2:2 averaging mixes two colours.
      if((std::abs(x - W / 10) <= 2 && y < H / 10 + 2)
         || (std::abs(y - H / 10) <= 1 && x < W / 10 + 2))
        continue;
      const double Y = word(luma + size_t(y) * s.stride + x * 2);
      const auto* cc = chroma + size_t(y) * s.stride + (x / 2) * 4;
      const auto got = decode709(Y, word(cc), word(cc + 2), 256.0);
      const double e = rgbError(got, pictureAt(x, y, W, H));
      sum += e;
      ++n;
      if(e > worst)
      {
        worst = e;
        wx = x;
        wy = y;
      }
    }
  std::printf(
      "    P216 decoded on the CPU: mean |error| %.2f, worst %.1f at (%d,%d)\n",
      sum / n, worst, wx, wy);
  // A 16-bit sample read as code/65535 rather than code/65280 is 0.4% off,
  // about 1.6/255 on this picture: these bounds are below that.
  check(sum / n < 1.0, "P216 decodes to the picture (mean error < 1/255)");
  check(worst < 2.0, "P216 decodes to the picture everywhere (worst < 2/255)");

  // 16 bits that mean something: the luma of the white square, which an
  // 8-bit value scaled up would put at 235 * 257 rather than at 235 * 256.
  const double white = word(luma);
  std::printf("    P216 white luma word: %.0f (expect 60160)\n", white);
  check(std::abs(white - 60160) <= 64, "P216 white is 235 << 8");
}

void checkPacked(const std::string& format, const Sent& s, int W, int H)
{
  // Orientation and level from luma alone, in whichever layout the format
  // has: the top-left white square, then the bottom-right corner, where the
  // picture is (255, 255, 64).
  auto yAt = [&](int x, int y) -> double {
    const auto* row = s.bytes.data() + size_t(y) * s.stride;
    if(format == "RGBA" || format == "RGBX")
    {
      const auto* p = row + x * 4;
      return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
    }
    if(format == "BGRA" || format == "BGRX")
    {
      const auto* p = row + x * 4;
      return 0.2126 * p[2] + 0.7152 * p[1] + 0.0722 * p[0];
    }
    if(format == "UYVY")
      return (row[(x / 2) * 4 + 1 + 2 * (x & 1)] - 16.0) * 255.0 / 219.0;
    // NV12, I420, YV12: the luma plane comes first, one byte per pixel.
    return (row[x] - 16.0) * 255.0 / 219.0;
  };
  const double tl = yAt(0, 0);
  const double br = yAt(W - 1, H - 1);
  const double bl = yAt(0, H - 1);
  const double expBr = 0.2126 * 255 + 0.7152 * 255 + 0.0722 * kBlue;
  const double expBl = 0.2126 * 0 + 0.7152 * 255 + 0.0722 * kBlue;
  std::printf(
      "    luma top-left %.1f (255), bottom-right %.1f (%.1f), bottom-left %.1f (%.1f)\n",
      tl, br, expBr, bl, expBl);
  check(std::abs(tl - 255) < 4, format + ": the white square is at the top left");
  check(std::abs(br - expBr) < 4, format + ": the bottom-right corner is right");
  check(std::abs(bl - expBl) < 4, format + ": the bottom-left corner is right");
}

void runFormat(GraphicsApi api, const std::string& format, int W, int H)
{
  std::printf("\n  == %s %dx%d ==\n", format.c_str(), W, H);
  constexpr int kFrames = 7;

  qunsetenv("SCORE_GFX_DIRECT_READBACK");
  const auto direct = runOnce(api, format, W, H, kFrames);
  qputenv("SCORE_GFX_DIRECT_READBACK", "never");
  const auto viaQRhi = runOnce(api, format, W, H, kFrames);
  qunsetenv("SCORE_GFX_DIRECT_READBACK");

  check(direct.ok && viaQRhi.ok, format + ": the renderer was created");
  if(!direct.ok || !viaQRhi.ok)
    return;

  std::printf(
      "    path: %s%s; %zu frames sent (QRhi readback: %zu)\n",
      direct.direct ? "straight into the send slots" : "QRhi readback",
      direct.late ? ", finished one frame late" : "", direct.frames.size(),
      viaQRhi.frames.size());
  check(!viaQRhi.direct, format + ": SCORE_GFX_DIRECT_READBACK=never is honoured");

  // A frame finished late is still sent; the only frame that never is, is
  // none. All seven active frames have to come out.
  check(int(direct.frames.size()) == kFrames, format + ": every active frame is sent");
  check(int(viaQRhi.frames.size()) == kFrames, format + ": every QRhi frame is sent");
  check(direct.queuedWhileInactive == 0, format + ": nothing is sent while nobody receives");

  const size_t expected = Ndi::framestoreBytes(format, W, H);
  for(const auto& f : direct.frames)
  {
    check(f.bytes.size() == expected, format + ": the slot describes a whole framestore");
    check(
        f.stride == Ndi::packedRowBytes(format, W) || !direct.direct,
        format + ": a direct slot has the packed stride");
  }
  if(direct.frames.empty() || viaQRhi.frames.empty())
    return;

  const auto& ref = viaQRhi.frames.back().bytes;
  bool identical = true;
  for(const auto& f : direct.frames)
    identical = identical && f.bytes == ref;
  check(identical, format + ": every sent frame equals QRhi's readback of it, byte for byte");

  if(format == "P216")
    checkP216(direct.frames.back(), W, H);
  else
    checkPacked(format, direct.frames.back(), W, H);
}

void runTests()
{
  const QByteArray apiEnv = qgetenv("SCORE_TEST_API").toLower();
  const GraphicsApi api = (apiEnv == "vulkan" || apiEnv == "vk") ? GraphicsApi::Vulkan
                                                                 : GraphicsApi::OpenGL;
  {
    auto probe = createRenderState(api, QSize(64, 64), nullptr);
    if(!probe || !probe->rhi)
    {
      std::printf("  skip: no QRhi\n");
      std::exit(77);
    }
    std::printf("  backend=%s\n", probe->rhi->backendName());
    probe->destroy();
  }

  for(const auto& f : Ndi::wireFormats)
    runFormat(api, std::string(f.name), 320, 180);

  // A width the 4:2:0 encoders cannot pack four pixels to a texel at, and one
  // with no padding to hide behind.
  runFormat(api, "I420", 322, 180);
  runFormat(api, "P216", 322, 182);
}
}

int main(int argc, char** argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  QLocale::setDefault(QLocale::C);
  std::setlocale(LC_ALL, "C");
  qputenv("SCORE_DISABLE_AUDIOPLUGINS", "1");
  qputenv("SCORE_AUDIO_BACKEND", "dummy");

  // A plain QApplication, not score::MinimalApplication, for the reason given
  // in EncodeRendererTest.cpp.
  QApplication app(argc, argv);
  std::setlocale(LC_ALL, "C");

  QMetaObject::invokeMethod(
      &app,
      [] {
        runTests();
        std::printf(
            "\nndi send slots: %s (%d failure%s)\n", g_fail ? "FAILED" : "passed",
            g_fail, g_fail == 1 ? "" : "s");
        QCoreApplication::exit(g_fail ? 1 : 0);
      },
      Qt::QueuedConnection);

  return app.exec();
}
