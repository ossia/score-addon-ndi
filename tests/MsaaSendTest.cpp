// The NDI output at the Samples setting: the scene is drawn into a
// multisampled target, resolved, and what is sent is the resolved picture,
// anti-aliased like a window's.
//
// What Ndi::OutputNode does on the render thread, minus the SDK and the graph:
// score::gfx::createOutputRenderTarget around the picture texture,
// Ndi::WireEncodeRenderer reading the resolved texture into the send slots,
// Ndi::ReadbackPool around each frame. RenderState::samples is set the way
// createRenderState sets it from the settings in the application. That the
// graph's passes follow the target's sample count is tests/gfx's
// GfxOffscreenOutputMsaa; it needs score's application context, which this
// test does without (see main).
//
// The scene is a white square turned so that none of its edges follows the
// pixel grid, drawn at the target's sample count, the way the graph's last pass
// into an output is. A single-sample target only holds black and white; at
// four samples the pixels the edges cross come out grey. Before this, the
// picture texture was the single-sample target itself, and on OpenGL the
// output's QRhi did not come up at all at Samples > 1: the receiver got the
// Null backend's flat picture.
//
// Needs a QRhi; exits 77 without one. SCORE_TEST_API=vulkan picks Vulkan.

#include <Gfx/Graph/OutputNode.hpp>
#include <Gfx/Graph/RenderList.hpp>
#include <Gfx/Graph/RenderState.hpp>
#include <Gfx/Graph/Utils.hpp>
#include <Ndi/ReadbackPool.hpp>
#include <Ndi/VideoFrameFormat.hpp>
#include <Ndi/WireEncode.hpp>
#include <Ndi/WireEncodeRenderer.hpp>

#include <QApplication>
#include <QtGui/private/qrhi_p.h>

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace score::gfx;

namespace
{
int g_fail = 0;
void check(bool ok, const std::string& what)
{
  std::printf("    [%s] %s\n", ok ? " ok " : "FAIL", what.c_str());
  if(!ok)
    ++g_fail;
}

constexpr int kW = 160;
constexpr int kH = 90;

// Half the frame, turned by 0.35 rad: two triangles, no vertex input.
constexpr const char* kVertex = R"_(#version 450
out gl_PerVertex { vec4 gl_Position; };
void main()
{
  const vec2 corners[6] = vec2[6](
      vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(0.5, 0.5),
      vec2(-0.5, -0.5), vec2(0.5, 0.5), vec2(-0.5, 0.5));
  const float c = cos(0.35), s = sin(0.35);
  vec2 p = corners[gl_VertexIndex];
  gl_Position = vec4(c * p.x - s * p.y, s * p.x + c * p.y, 0.0, 1.0);
}
)_";

constexpr const char* kFragment = R"_(#version 450
layout(location = 0) out vec4 fragColor;
void main()
{
  fragColor = vec4(1.0);
}
)_";

struct MsaaSlotOutput final : score::gfx::OutputNode
{
  static constexpr int kSlots = 4;

  std::string m_format;
  int m_samples{1};
  std::shared_ptr<RenderState> m_state;
  QRhiTexture* m_tex{};
  TextureRenderTarget m_rt;
  std::weak_ptr<RenderList> m_renderer;
  Ndi::SendSlot m_slots[kSlots];
  Ndi::ReadbackPool<kSlots> m_pool;

  MsaaSlotOutput(std::string format, int samples)
      : m_format{std::move(format)}
      , m_samples{samples}
  {
    input.push_back(new score::gfx::Port{this, {}, score::gfx::Types::Image, {}});
    for(auto& s : m_slots)
      s.allocate(Ndi::framestoreBytes(m_format, kW, kH));
    m_pool.m_running = true;
  }

  ~MsaaSlotOutput() override { destroyOutput(); }

  void createOutput(score::gfx::OutputConfiguration conf) override
  {
    m_state = createRenderState(conf.graphicsApi, QSize(kW, kH), nullptr);
    if(!m_state || !m_state->rhi)
      return;
    // What createRenderState does with the Samples setting: the closest count
    // the device supports.
    int samples = 1;
    for(int c : m_state->rhi->supportedSampleCounts())
      if(c <= m_samples)
        samples = c;
    m_state->samples = samples;
    m_state->outputSize = m_state->renderSize;
    m_tex = m_state->rhi->newTexture(
        QRhiTexture::RGBA8, m_state->renderSize, 1,
        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
    m_tex->create();
    m_rt = createOutputRenderTarget(*m_state, *m_tex);
    if(conf.onReady)
      conf.onReady();
  }

  void destroyOutput() override
  {
    if(!m_state)
      return;
    releaseRegistry();
    releaseOutputRenderTarget(*m_state, m_rt);
    delete m_tex;
    m_tex = nullptr;
    m_state->destroy();
    m_state.reset();
  }

  void render() override { }

  score::gfx::OutputNodeRenderer* createRenderer(RenderList&) const noexcept override
  {
    return nullptr;
  }

  TextureRenderTarget currentRenderTarget() const noexcept override { return m_rt; }
  std::shared_ptr<RenderState> renderState() const override { return m_state; }
  bool canRender() const override { return bool(m_state) && bool(m_rt); }
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
  bool ok{};
  int stateSamples{}, targetSamples{};
  std::vector<int> luma; // per pixel, the format's own scale
  int coloured{};
};

//! The scene, drawn the way the graph's last pass into an output is: a
//! pipeline at the target's sample count, into the target.
struct Scene
{
  QRhiGraphicsPipeline* pipeline{};
  QRhiShaderResourceBindings* srb{};

  bool init(const RenderState& state, const TextureRenderTarget& rt)
  {
    auto& rhi = *state.rhi;
    auto [vs, fs] = score::gfx::makeShaders(state, kVertex, kFragment);
    srb = rhi.newShaderResourceBindings();
    srb->create();
    pipeline = rhi.newGraphicsPipeline();
    pipeline->setShaderStages(
        {{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
    pipeline->setShaderResourceBindings(srb);
    pipeline->setSampleCount(rt.sampleCount());
    pipeline->setRenderPassDescriptor(rt.renderPass);
    return pipeline->create();
  }
  void release()
  {
    delete pipeline;
    delete srb;
  }
  void draw(QRhiCommandBuffer& cb, const TextureRenderTarget& rt)
  {
    cb.beginPass(rt.renderTarget, Qt::black, {1.0f, 0});
    cb.setGraphicsPipeline(pipeline);
    cb.setShaderResources(srb);
    cb.setViewport(QRhiViewport(0, 0, kW, kH));
    cb.draw(6);
    cb.endPass();
  }
};

Sent runOnce(GraphicsApi api, const std::string& format, int samples)
{
  Sent out;
  MsaaSlotOutput node{format, samples};
  node.createOutput({.graphicsApi = api});
  if(!node.m_state || !node.m_rt)
    return out;
  out.stateSamples = node.m_state->samples;
  out.targetSamples = node.m_rt.sampleCount();

  auto& rhi = *node.m_state->rhi;
  Scene scene;
  {
    RenderList list{node, node.m_state};
    Ndi::WireEncodeRenderer wire{
        node, node.m_rt, std::span<Ndi::SendSlot>{node.m_slots}, format,
        Ndi::ColorSpaceSetting::Rec709};
    if(scene.init(*node.m_state, node.m_rt))
    {
      auto* res = rhi.nextResourceUpdateBatch();
      wire.init(list, *res);

      std::vector<uint8_t> last;
      int stride = 0;
      for(int i = 0; i < 6; i++)
      {
        Ndi::beginSendFrame(wire, node.m_pool, true);
        QRhiCommandBuffer* cb{};
        if(rhi.beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
          break;
        if(res)
        {
          cb->resourceUpdate(res);
          res = nullptr;
        }
        scene.draw(*cb, node.m_rt);
        QRhiResourceUpdateBatch* none{};
        wire.finishFrame(list, *cb, none);
        rhi.endOffscreenFrame();
        Ndi::endSendFrame(wire, node.m_pool);

        while(node.m_pool.m_frameReady.load())
        {
          const int idx = node.m_pool.acquireForSending();
          if(idx < 0)
            break;
          const auto& slot = node.m_slots[idx];
          const int rows = Ndi::framestoreRows(format, kH);
          last.assign(slot.data, slot.data + size_t(slot.stride) * rows);
          stride = slot.stride;
          node.m_pool.markSent(idx);
        }
      }
      if(res)
        res->release();

      if(!last.empty())
      {
        out.ok = true;
        for(int y = 0; y < kH; y++)
        {
          const auto* row = last.data() + size_t(y) * stride;
          for(int x = 0; x < kW; x++)
          {
            if(format == "UYVY")
            {
              out.luma.push_back(row[(x / 2) * 4 + 1 + 2 * (x & 1)]);
            }
            else
            {
              const auto* p = row + x * 4;
              out.luma.push_back(p[0]);
              if(std::abs(p[0] - p[1]) > 8 || std::abs(p[0] - p[2]) > 8)
                out.coloured++;
            }
          }
        }
      }
    }
    rhi.finish();
    wire.release(list);
    scene.release();
  }
  node.destroyOutput();
  return out;
}

void runCase(GraphicsApi api, const std::string& format, int samples)
{
  std::printf("\n  == %s, Samples=%d ==\n", format.c_str(), samples);
  const auto s = runOnce(api, format, samples);
  check(s.ok, format + ": a frame reached the sender");
  if(!s.ok)
    return;

  const auto [mn, mx] = std::minmax_element(s.luma.begin(), s.luma.end());
  const double span = double(*mx - *mn);
  int dark = 0, bright = 0, between = 0;
  for(int v : s.luma)
  {
    const double t = span > 0 ? (v - *mn) / span : 0.;
    if(t < 0.1)
      dark++;
    else if(t > 0.9)
      bright++;
    else
      between++;
  }
  const int total = kW * kH;
  std::printf(
      "    render list samples %d, target samples %d, range %d..%d, dark %d, "
      "bright %d, between %d, coloured %d\n",
      s.stateSamples, s.targetSamples, *mn, *mx, dark, bright, between, s.coloured);

  check(s.coloured == 0, format + ": nothing but greys");
  check(span > 150, format + ": black and white are both there");
  check(bright > total / 6 && bright < total / 3, format + ": the square is a quarter of the frame");
  check(dark > total / 2, format + ": the rest is black");
  if(samples == 1 || s.stateSamples < 2)
  {
    check(s.targetSamples == 1, format + ": the target is single-sample");
    check(between == 0, format + ": no partly covered pixel");
    if(samples > 1)
      std::printf("    note: this device has no multisampling\n");
    return;
  }
  check(s.targetSamples == s.stateSamples, format + ": the target has the render list's sample count");
  check(between >= (kW + kH) / 3, format + ": the edges are anti-aliased");
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

  for(const char* format : {"RGBA", "UYVY"})
    for(int samples : {1, 4})
      runCase(api, format, samples);
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
            "\nndi msaa send: %s (%d failure%s)\n", g_fail ? "FAILED" : "passed",
            g_fail, g_fail == 1 ? "" : "s");
        QCoreApplication::exit(g_fail ? 1 : 0);
      },
      Qt::QueuedConnection);

  return app.exec();
}
