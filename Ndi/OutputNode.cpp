#include <Gfx/GfxApplicationPlugin.hpp>
#include <Ndi/ReadbackPool.hpp>
#include <Ndi/VideoFrameFormat.hpp>

#include <Gfx/GfxExecContext.hpp>
#include <Gfx/GfxParameter.hpp>
#include <Gfx/Graph/RenderList.hpp>
#include <Gfx/Graph/RenderState.hpp>
#include <Gfx/SharedOutputSettings.hpp>
#include <Video/Rescale.hpp>

#include <score/gfx/OpenGL.hpp>

#include <ossia/network/base/device.hpp>

#include <QOffscreenSurface>

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <QTimer>
#include <QtGui/private/qrhigles2_p.h>

#include <Ndi/OutputNode.hpp>
#include <Ndi/OutputSettings.hpp>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
struct SwsContext;
}

#include <wobjectimpl.h>
W_OBJECT_IMPL(Ndi::OutputDevice)
namespace Ndi
{
struct OutputSettings;
struct OutputNode : score::gfx::OutputNode
{
  OutputNode(const Ndi::Loader& ndi, const Ndi::OutputSettings& set);
  virtual ~OutputNode();

  Ndi::OutputSettings m_settings;
  std::weak_ptr<score::gfx::RenderList> m_renderer{};
  QRhiTexture* m_texture{};
  QRhiTextureRenderTarget* m_renderTarget{};
  std::function<void()> m_update;
  std::shared_ptr<score::gfx::RenderState> m_renderState{};
  Gfx::InvertYRenderer* m_inv_y_renderer{};
  // Readback pool. A frame handed to send_video_async stays owned by the SDK
  // until the next send, so the renderer must never target a buffer that is
  // still spoken for; the states in Ndi::ReadbackPool are what enforce that,
  // and they are what let the RGBA path send the readback itself with no copy
  // at all.
  //
  // Four buffers: at most one Queued, one Sending and one InFlight, so a Free
  // one always exists.
  static constexpr int kBuffers = 4;
  QRhiReadbackResult m_readback[kBuffers];
  Ndi::ReadbackPool<kBuffers> m_pool;
  const Ndi::Loader& m_ndi;
  Ndi::Sender m_sender;
  SwsContext* m_swsCtx{};
  // One staging frame per readback buffer. send_video_async keeps reading the
  // buffer it was handed until the NEXT send_video_async, so a single frame
  // cannot be reused: the sws_scale for frame N+1 would overwrite what the SDK
  // is still reading from frame N. Rotating in step with m_readback gives the
  // same two frames of slack that make the RGBA path safe.
  AVFrame* avframe[kBuffers]{};
  bool m_hasSender{};

  // Async sender thread members
  std::thread m_senderThread;

  void senderThreadFunc();

  void startRendering() override;
  void render() override;
  void onRendererChange() override;
  bool canRender() const override;
  void stopRendering() override;

  void setRenderer(std::shared_ptr<score::gfx::RenderList>) override;
  score::gfx::RenderList* renderer() const override;

  void createOutput(score::gfx::OutputConfiguration) override;
  void destroyOutput() override;

  std::shared_ptr<score::gfx::RenderState> renderState() const override;
  score::gfx::OutputNodeRenderer*
  createRenderer(score::gfx::RenderList& r) const noexcept override;

  Configuration configuration() const noexcept override;
};

class ndi_output_device : public ossia::net::device_base
{
  Gfx::gfx_node_base root;

public:
  ndi_output_device(
      const Ndi::Loader& ndi, const Ndi::OutputSettings& set,
      std::unique_ptr<ossia::net::protocol_base> proto, std::string name)
      : ossia::net::device_base{std::move(proto)}
      , root{
            *this, *static_cast<Gfx::gfx_protocol_base*>(m_protocol.get()),
            new OutputNode{ndi, set}, name}
  {
  }

  const Gfx::gfx_node_base& get_root_node() const override { return root; }
  Gfx::gfx_node_base& get_root_node() override { return root; }
};

OutputNode::OutputNode(const Ndi::Loader& ndi, const Ndi::OutputSettings& set)
    : score::gfx::OutputNode{}
    , m_settings{set}
    , m_ndi{ndi}
    , m_sender{m_ndi, set.path.toStdString()}
{
  input.push_back(new score::gfx::Port{this, {}, score::gfx::Types::Image, {}});

  AVPixelFormat fmt{AV_PIX_FMT_RGBA};
  if(m_settings.format == "UYVY")
    fmt = AV_PIX_FMT_UYVY422;

  // Only a format that needs a colour conversion needs somewhere to convert
  // into. RGBA is already the wire layout and is sent straight from the readback
  // buffer, so it allocates nothing here.
  if(fmt != AV_PIX_FMT_RGBA)
  {
    m_swsCtx = sws_getContext(
        m_settings.width, m_settings.height, AV_PIX_FMT_RGBA, m_settings.width,
        m_settings.height, AV_PIX_FMT_UYVY422, 0, 0, 0, 0);

    // One per readback buffer: the SDK reads the frame it was handed until the
    // next send, so the conversion for the next frame cannot reuse it.
    for(auto& f : avframe)
    {
      f = av_frame_alloc();
      f->format = fmt;
      f->width = m_settings.width;
      f->height = m_settings.height;
      av_frame_get_buffer(f, 0);
    }
  }
}

OutputNode::~OutputNode()
{
  // Ensure sender thread is stopped
  if(m_pool.m_running)
  {
    m_pool.requestStop();
    if(m_senderThread.joinable())
      m_senderThread.join();
  }

  if(m_swsCtx)
    sws_freeContext(m_swsCtx);

  for(auto& f : avframe)
    av_frame_free(&f);
}

void OutputNode::senderThreadFunc()
{
  while(m_pool.m_running)
  {
    const int idx = m_pool.acquireForSending();
    if(idx < 0)
      continue;

    // Read directly from the readback buffer
    auto& readback = m_readback[idx];
    auto width = readback.pixelSize.width();
    auto height = readback.pixelSize.height();

    NDIlib_video_frame_v2_t ndiFrame{};
    ndiFrame.xres = width;
    ndiFrame.yres = height;
    ndiFrame.frame_rate_N = this->m_settings.rate * 10000;
    ndiFrame.frame_rate_D = 10000;
    ndiFrame.frame_format_type = NDIlib_frame_format_type_progressive;

    if(!Ndi::describeVideoFrame(
           m_settings.format.toStdString(), (const uint8_t*)readback.data.data(),
           width, height, m_swsCtx, avframe[idx], ndiFrame))
    {
      m_pool.releaseUnsent(idx);
      continue;
    }

    // This call is the synchronising event for the previous frame: the SDK
    // has finished with that buffer and starts reading this one.
    m_sender.send_video_async(ndiFrame);

    m_pool.markSent(idx);
  }
}

bool OutputNode::canRender() const
{
  return bool(m_renderState);
}

void OutputNode::startRendering()
{
  m_pool.m_running = true;
  m_senderThread = std::thread(&OutputNode::senderThreadFunc, this);
}

void OutputNode::render()
{
  if(m_update)
    m_update();

  auto renderer = m_renderer.lock();
  if(renderer && m_renderState)
  {
    auto rhi = m_renderState->rhi;
    QRhiCommandBuffer* cb{};
    if(rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
      return;

    renderer->render(*cb);

    rhi->endOffscreenFrame();

    if(renderer->renderers.size() > 1)
    {
      if(m_sender.get_no_connections(0) > 0)
      {
        m_pool.queueForSend();
      }
    }

    m_inv_y_renderer->updateReadback(m_readback[m_pool.advanceToFreeBuffer()]);
  }
}

score::gfx::OutputNode::Configuration OutputNode::configuration() const noexcept
{
  return {.manualRenderingRate = 1000. / m_settings.rate};
}
void OutputNode::onRendererChange() { }

void OutputNode::stopRendering()
{
  m_pool.requestStop();
  if(m_senderThread.joinable())
    m_senderThread.join();
}

void OutputNode::setRenderer(std::shared_ptr<score::gfx::RenderList> r)
{
  m_renderer = r;
}

score::gfx::RenderList* OutputNode::renderer() const
{
  return m_renderer.lock().get();
}

void OutputNode::createOutput(score::gfx::OutputConfiguration conf)
{
  m_renderState = score::gfx::createRenderState(
      conf.graphicsApi, QSize(m_settings.width, m_settings.height), nullptr);
  if(!m_renderState || !m_renderState->rhi)
  {
    qWarning() << "Ndi::OutputNode: failed to create QRhi";
    m_renderState.reset();
    return;
  }
  m_renderState->outputSize = m_renderState->renderSize;

  auto rhi = m_renderState->rhi;
  m_texture = rhi->newTexture(
      QRhiTexture::RGBA8, m_renderState->renderSize, 1,
      QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
  m_texture->create();
  m_renderTarget = rhi->newTextureRenderTarget({m_texture});
  m_renderState->renderPassDescriptor
      = m_renderTarget->newCompatibleRenderPassDescriptor();
  m_renderTarget->setRenderPassDescriptor(m_renderState->renderPassDescriptor);
  m_renderTarget->create();

  if(conf.onReady)
    conf.onReady();
}

void OutputNode::destroyOutput()
{
  if(!m_renderState)
    return;

  delete m_renderTarget;
  m_renderTarget = nullptr;

  delete m_renderState->renderPassDescriptor;
  m_renderState->renderPassDescriptor = nullptr;

  delete m_texture;
  m_texture = nullptr;

  m_renderState->destroy();
  m_renderState.reset();
}

std::shared_ptr<score::gfx::RenderState> OutputNode::renderState() const
{
  return m_renderState;
}

score::gfx::OutputNodeRenderer*
OutputNode::createRenderer(score::gfx::RenderList& r) const noexcept
{
  score::gfx::TextureRenderTarget rt{
      .texture = m_texture,
      .renderPass = m_renderState->renderPassDescriptor,
      .renderTarget = m_renderTarget};
  return const_cast<Gfx::InvertYRenderer*&>(m_inv_y_renderer) = new Gfx::InvertYRenderer{
             *this, rt, const_cast<QRhiReadbackResult&>(m_readback[0])};
}

OutputDevice::OutputDevice(
    const Device::DeviceSettings& settings, const score::DocumentContext& ctx)
    : Gfx::GfxOutputDevice{settings, ctx}
{
}

OutputDevice::~OutputDevice() { }

void OutputDevice::disconnect()
{
  GfxOutputDevice::disconnect();
  auto prev = std::move(m_dev);
  m_dev = {};
  deviceChanged(prev.get(), nullptr);
}

bool OutputDevice::reconnect()
{
  disconnect();

  try
  {
    auto& ndi = Loader::instance();
    if(!ndi.available())
      return false;
    auto plug = m_ctx.findPlugin<Gfx::DocumentPlugin>();
    if(plug)
    {
      auto set = m_settings.deviceSpecificSettings.value<Ndi::OutputSettings>();

      m_protocol = new Gfx::gfx_protocol_base{plug->exec};
      m_dev = std::make_unique<ndi_output_device>(
          ndi, set, std::unique_ptr<ossia::net::protocol_base>(m_protocol),
          m_settings.name.toStdString());
      deviceChanged(nullptr, m_dev.get());
    }
  }
  catch(std::exception& e)
  {
    qDebug() << "Could not connect: " << e.what();
  }
  catch(...)
  {
    // TODO save the reason of the non-connection.
  }

  return connected();
}
}
