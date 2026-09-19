// A live NDI frame, through the real GPU decoder, checked against SMPTE.
//
// Everything else stops at the AVFrame. LiveSourceProbe decodes with its own
// CPU maths, which says the layout and the matrix are right but says nothing
// about the shader that actually draws the picture; FormatSweepTest checks the
// send direction. This closes the receive direction: a frame from a sender we
// do not control, wrapped exactly as Ndi::receiveLayout says, pushed through
// score::gfx's VideoNode and its GPU decoder, rendered, read back, and its
// colour bars compared with the values SMPTE ECR 1-1978 specifies.
//
// Needs a live source and a QRhi, so it is a tool rather than a ctest.
//
// Usage: LiveDecodeProbe <libndi.so> [--best] [--filter=substr] [--save=out.ppm]

#include <Gfx/Graph/Graph.hpp>
#include <Gfx/InvertYRenderer.hpp>
#include <Gfx/Graph/OutputNode.hpp>
#include <Gfx/Graph/RenderList.hpp>
#include <Gfx/Graph/RenderState.hpp>
#include <Gfx/Graph/VideoNode.hpp>
#include <Ndi/FrameFormat.hpp>
#include <Ndi/NdiColorSpace.hpp>
#include <Ndi/ReceiveLayout.hpp>
#include <Video/VideoInterface.hpp>

#include <QApplication>
#include <QtGui/private/qrhi_p.h>

#include <Processing.NDI.Lib.h>
#include <dlfcn.h>

#include <clocale>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace score::gfx;

namespace
{
int g_fail = 0;
void check(bool ok, const std::string& what)
{
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if(!ok)
    ++g_fail;
}

const NDIlib_v5* g_ndi = nullptr;

bool loadNdi(const char* path)
{
  void* h = dlopen(path, RTLD_LOCAL | RTLD_LAZY);
  if(!h)
    return false;
  const NDIlib_v5* (*load)(void) = nullptr;
  *((void**)&load) = dlsym(h, "NDIlib_v5_load");
  if(!load)
    return false;
  g_ndi = load();
  return g_ndi && g_ndi->initialize();
}

// ------------------------------------------------- one captured frame, as a
// video source
//
// The decoder wants a Video::VideoInterface. This is one that has exactly one
// frame and hands it over forever, so a single render is enough.
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

// ------------------------------------------------------- an offscreen output
//
// The smallest OutputNode that renders into a texture we can read back. Every
// override below is boilerplate except createOutput and render.
struct TextureOutput final : score::gfx::OutputNode
{
  QSize m_size;
  std::shared_ptr<RenderState> m_state;
  QRhiTexture* m_tex{};
  QRhiTextureRenderTarget* m_rt{};
  std::weak_ptr<RenderList> m_renderer;
  QRhiReadbackResult m_readback;

  explicit TextureOutput(QSize sz)
      : m_size{sz}
  {
    input.push_back(new score::gfx::Port{this, {}, score::gfx::Types::Image, {}});
  }

  void createOutput(score::gfx::OutputConfiguration conf) override
  {
    m_state = createRenderState(conf.graphicsApi, m_size, nullptr);
    if(!m_state || !m_state->rhi)
      return;
    m_state->outputSize = m_state->renderSize;
    auto rhi = m_state->rhi;
    m_tex = rhi->newTexture(
        QRhiTexture::RGBA8, m_state->renderSize, 1,
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

  void render() override
  {
    auto r = m_renderer.lock();
    if(!r || !m_state)
      return;
    auto rhi = m_state->rhi;
    QRhiCommandBuffer* cb{};
    if(rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
      return;
    r->render(*cb);
    auto* batch = rhi->nextResourceUpdateBatch();
    batch->readBackTexture(QRhiReadbackDescription{m_tex}, &m_readback);
    cb->resourceUpdate(batch);
    rhi->endOffscreenFrame();
  }

  score::gfx::OutputNodeRenderer*
  createRenderer(RenderList& r) const noexcept override
  {
    score::gfx::TextureRenderTarget rt{
        .texture = m_tex,
        .renderPass = m_state->renderPassDescriptor,
        .renderTarget = m_rt};
    return new Gfx::InvertYRenderer{
        *this, rt, const_cast<QRhiReadbackResult&>(m_readback)};
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

// ------------------------------------------------------------- the reference

struct Rgb
{
  double r, g, b;
};

constexpr int kLevel = 191;  // 75% of full scale
const int kBar[7][3] = {{1, 1, 1}, {1, 1, 0}, {0, 1, 1}, {0, 1, 0},
                        {1, 0, 1}, {1, 0, 0}, {0, 0, 1}};
const char* kBarName[7]
    = {"white", "yellow", "cyan", "green", "magenta", "red", "blue"};

// The output renderer here is InvertYRenderer, which is what this codebase
// uses to get a readback the right way up for a wire format -- so the readback
// is upside down relative to the picture. Undo it on the way in rather than
// sampling mirrored rows everywhere.
Rgb readbackAt(const QRhiReadbackResult& rb, int x, int y)
{
  const int h = rb.pixelSize.height();
  const int stride = rb.data.size() / h;
  const auto* p = reinterpret_cast<const uint8_t*>(rb.data.constData())
                  + ptrdiff_t(h - 1 - y) * stride + ptrdiff_t(x) * 4;
  return {double(p[0]), double(p[1]), double(p[2])};
}
}

int main(int argc, char** argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::string path, filter, save;
  bool best = false, fastest = false;
  for(int i = 1; i < argc; i++)
  {
    const std::string a = argv[i];
    if(a.rfind("--filter=", 0) == 0)
      filter = a.substr(9);
    else if(a.rfind("--save=", 0) == 0)
      save = a.substr(7);
    else if(a == "--best")
      best = true;
    else if(a == "--fastest")
      fastest = true;
    else
      path = a;
  }
  if(path.empty() || !loadNdi(path.c_str()))
  {
    std::printf("usage: LiveDecodeProbe <libndi.so> [--best] [--filter=s]\n");
    return 77;
  }

  QApplication app(argc, argv);
  // QApplication calls setlocale(LC_ALL, "") on X11.
  std::setlocale(LC_ALL, "C");

  // --- find a source and take one frame -------------------------------------
  auto* finder = g_ndi->find_create_v2(nullptr);
  NDIlib_source_t src{};
  bool have = false;
  for(int round = 0; round < 8 && !have; round++)
  {
    g_ndi->find_wait_for_sources(finder, 1000);
    uint32_t n = 0;
    const NDIlib_source_t* all = g_ndi->find_get_current_sources(finder, &n);
    for(uint32_t i = 0; i < n; i++)
    {
      const std::string nm = all[i].p_ndi_name ? all[i].p_ndi_name : "";
      if(filter.empty() || nm.find(filter) != std::string::npos)
      {
        src = all[i];
        have = true;
        break;
      }
    }
  }
  if(!have)
  {
    std::printf("no source matched\n");
    return 77;
  }
  std::printf("source: %s\n", src.p_ndi_name);

  NDIlib_recv_create_v3_t rc{};
  rc.source_to_connect_to = src;
  // UYVY_RGBA makes the SDK convert every YUV source to UYVY before we see
  // it, so it never exercises the planar receive paths. "fastest" hands over
  // whatever the sender actually put on the wire.
  rc.color_format = fastest ? NDIlib_recv_color_format_fastest
                    : best  ? NDIlib_recv_color_format_best
                            : NDIlib_recv_color_format_UYVY_RGBA;
  rc.bandwidth = NDIlib_recv_bandwidth_highest;
  rc.allow_video_fields = best || fastest;
  auto* recv = g_ndi->recv_create_v3(&rc);

  NDIlib_video_frame_v2_t vf{};
  bool got = false;
  for(int i = 0; i < 100 && !got; i++)
    got = g_ndi->recv_capture_v3(recv, &vf, nullptr, nullptr, 200)
          == NDIlib_frame_type_video;
  if(!got)
  {
    std::printf("no frame\n");
    return 77;
  }

  const auto L
      = Ndi::receiveLayout(vf.FourCC, vf.line_stride_in_bytes, vf.yres);
  const auto ff = Ndi::decodeFrameFormat(vf.frame_format_type);
  const char* fcc = "(other)";
  switch(vf.FourCC)
  {
    case NDIlib_FourCC_video_type_UYVY: fcc = "UYVY"; break;
    case NDIlib_FourCC_video_type_UYVA: fcc = "UYVA"; break;
    case NDIlib_FourCC_video_type_P216: fcc = "P216"; break;
    case NDIlib_FourCC_video_type_PA16: fcc = "PA16"; break;
    case NDIlib_FourCC_video_type_YV12: fcc = "YV12"; break;
    case NDIlib_FourCC_video_type_I420: fcc = "I420"; break;
    case NDIlib_FourCC_video_type_NV12: fcc = "NV12"; break;
    case NDIlib_FourCC_video_type_BGRA: fcc = "BGRA"; break;
    case NDIlib_FourCC_video_type_BGRX: fcc = "BGRX"; break;
    case NDIlib_FourCC_video_type_RGBA: fcc = "RGBA"; break;
    case NDIlib_FourCC_video_type_RGBX: fcc = "RGBX"; break;
    default: break;
  }
  std::printf(
      "frame:  %dx%d  %s  %d plane(s)  interlacing %s\n", vf.xres, vf.yres, fcc,
      L.planeCount,
      ff.interlacing == Video::Interlacing::Fields  ? "Fields"
      : ff.interlacing == Video::Interlacing::Woven ? "Woven"
                                                    : "None");
  check(L.supported, "the frame has a layout");
  if(!L.supported)
    return 1;

  // --- wrap it as an AVFrame, the way the input node does -------------------
  //
  // Copied rather than referenced: the SDK frame is freed below, and this
  // keeps the decoder's lifetime independent of the receiver's.
  AVFrame* frame = av_frame_alloc();
  frame->format = L.format;
  frame->width = vf.xres;
  frame->height = vf.yres;
  std::vector<uint8_t> store(
      reinterpret_cast<const uint8_t*>(vf.p_data),
      reinterpret_cast<const uint8_t*>(vf.p_data) + L.total);
  for(int i = 0; i < L.planeCount; i++)
  {
    frame->data[i] = store.data() + L.offset[i];
    frame->linesize[i] = L.stride[i];
  }
  g_ndi->recv_free_video_v2(recv, &vf);
  g_ndi->recv_destroy(recv);
  g_ndi->find_destroy(finder);

  // --- drive the real decoder ----------------------------------------------
  //
  // The picture is the full height even when the frame is one field, which is
  // what VideoNodeRenderer expects to size its texture from.
  const int pictureH
      = ff.interlacing == Video::Interlacing::Fields ? frame->height * 2
                                                     : frame->height;
  auto source = std::make_shared<OneFrameSource>(frame);
  source->width = frame->width;
  source->height = pictureH;
  source->pixel_format = AVPixelFormat(L.format);
  source->color_space = Ndi::avColorSpace(Ndi::resolveYuvStandard(
      Ndi::ColorSpaceSetting::Rec709, frame->width, pictureH));
  source->color_range = AVCOL_RANGE_MPEG;
  source->interlacing = ff.interlacing;
  source->deinterlace = Video::Deinterlace::Bob;

  auto* video = new score::gfx::VideoNode(source, std::nullopt);
  auto* out = new TextureOutput(QSize(frame->width, pictureH));

  score::gfx::Graph graph;
  graph.addNode(video);
  graph.addNode(out);
  graph.addEdge(video->output[0], out->input[0], Process::CableType::ImmediateGlutton);

  const auto apiEnv = qgetenv("SCORE_GFX_API").toLower();
  graph.createAllRenderLists(
      (apiEnv == "vulkan" || apiEnv == "vk") ? GraphicsApi::Vulkan
                                             : GraphicsApi::OpenGL);
  // VideoNode::update() is what advances its reader; VideoNodeRenderer only
  // uploads when the reader's frame index has moved past the one it drew. With
  // nothing ticking the graph, no update means no frame and a black picture.
  for(int i = 0; i < 4; i++)  // first frames build pipelines and upload
  {
    video->update();
    out->render();
  }

  const auto& rb = out->m_readback;
  check(!rb.data.isEmpty(), "the GPU produced a picture");
  if(rb.data.isEmpty())
    return 1;
  std::printf(
      "render: %dx%d, %d bytes\n", rb.pixelSize.width(), rb.pixelSize.height(),
      int(rb.data.size()));

  if(!save.empty())
  {
    if(FILE* f = std::fopen(save.c_str(), "wb"))
    {
      std::fprintf(f, "P6\n%d %d\n255\n", rb.pixelSize.width(), rb.pixelSize.height());
      for(int y = 0; y < rb.pixelSize.height(); y++)
        for(int x = 0; x < rb.pixelSize.width(); x++)
        {
          const auto c = readbackAt(rb, x, y);
          const uint8_t px[3]
              = {uint8_t(c.r), uint8_t(c.g), uint8_t(c.b)};
          std::fwrite(px, 1, 3, f);
        }
      std::fclose(f);
      std::printf("saved %s\n", save.c_str());
    }
  }

  // --- the bars, as rendered -----------------------------------------------
  //
  // Segment the rendered row the same way LiveSourceProbe segments the wire,
  // on luma and chroma together, so the two neutral columns of a SMPTE pattern
  // do not merge.
  const int y = rb.pixelSize.height() / 4;
  std::vector<std::pair<int, int>> seg;
  {
    Rgb prev{};
    int start = 0;
    for(int x = 0; x < rb.pixelSize.width(); x++)
    {
      const auto c = readbackAt(rb, x, y);
      if(x > 0
         && (std::abs(c.r - prev.r) + std::abs(c.g - prev.g)
             + std::abs(c.b - prev.b))
                > 30.0)
      {
        if(x - start > rb.pixelSize.width() / 40)
          seg.emplace_back(start, x - 1);
        start = x;
      }
      prev = c;
    }
    if(rb.pixelSize.width() - start > rb.pixelSize.width() / 40)
      seg.emplace_back(start, rb.pixelSize.width() - 1);
  }
  std::printf("bars:   %zu segments across row %d\n", seg.size(), y);
  check(seg.size() >= 7, "the rendered picture has at least seven segments");
  if(seg.size() < 7)
    return 1;

  auto scoreAt = [&](int first) {
    double err = 0;
    for(int i = 0; i < 7; i++)
    {
      const int x = (seg[first + i].first + seg[first + i].second) / 2;
      const auto c = readbackAt(rb, x, y);
      err += std::abs(c.r - kBar[i][0] * kLevel)
             + std::abs(c.g - kBar[i][1] * kLevel)
             + std::abs(c.b - kBar[i][2] * kLevel);
    }
    return err / (7 * 3);
  };

  int first = 0;
  double best_err = 1e9;
  for(int f = 0; f + 7 <= int(seg.size()); f++)
    if(const double e = scoreAt(f); e < best_err)
    {
      best_err = e;
      first = f;
    }

  std::printf("        (the bars start at segment %d)\n", first);
  for(int i = 0; i < 7; i++)
  {
    const auto& sg = seg[first + i];
    const auto c = readbackAt(rb, (sg.first + sg.second) / 2, y);
    std::printf(
        "        %-8s x=%4d..%-4d  R=%5.0f G=%5.0f B=%5.0f   (want %3d %3d %3d)\n",
        kBarName[i], sg.first, sg.second, c.r, c.g, c.b, kBar[i][0] * kLevel,
        kBar[i][1] * kLevel, kBar[i][2] * kLevel);
  }
  std::printf("        mean |RGB error| vs 75%% SMPTE: %.2f\n", best_err);

  // The GPU path rounds differently from a CPU decode and the readback is
  // 8-bit, so a couple of units is expected; a wrong matrix costs 11 and a
  // wrong plane offset very much more.
  check(best_err < 5.0, "the rendered bars match the SMPTE reference");

  graph.clearEdges();
  std::printf(
      "\nlive decode: %s (%d failure%s)\n", g_fail ? "FAILED" : "passed", g_fail,
      g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
