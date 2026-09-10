#include <Gfx/GfxApplicationPlugin.hpp>
#include <Ndi/ReadbackPool.hpp>
#include <Ndi/UyvyEncodeRenderer.hpp>
#include <Ndi/VideoFrameFormat.hpp>

#include <Gfx/GfxExecContext.hpp>
#include <Gfx/GfxParameter.hpp>
#include <Gfx/Graph/RenderList.hpp>
#include <Gfx/Graph/RenderState.hpp>
#include <Gfx/SharedOutputSettings.hpp>

#include <score/gfx/OpenGL.hpp>

#include <ossia/network/base/device.hpp>

#include <QOffscreenSurface>

#include <atomic>
#include <functional>
#include <string>
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
  std::string m_formatStr;  // m_settings.format, converted once, not per frame
  std::weak_ptr<score::gfx::RenderList> m_renderer{};
  QRhiTexture* m_texture{};
  QRhiTextureRenderTarget* m_renderTarget{};
  std::function<void()> m_update;
  std::shared_ptr<score::gfx::RenderState> m_renderState{};
  // One of these is created in createRenderer, according to the format; the
  // callback below points whichever it is at the next pool buffer.
  Gfx::InvertYRenderer* m_inv_y_renderer{};
  Ndi::UyvyEncodeRenderer* m_uyvy_renderer{};
  std::function<void(QRhiReadbackResult&)> m_updateReadback;
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
  // Converted once here rather than per frame: toStdString() goes through
  // toUtf8(), which mallocs and frees a QByteArray, and the sender thread is a
  // realtime path. describeVideoFrame takes a string_view, so nothing needs a
  // fresh std::string.
  m_formatStr = m_settings.format.toStdString();

  if(m_settings.format == "UYVY")
    fmt = AV_PIX_FMT_UYVY422;

  // Only a format that needs a colour conversion needs somewhere to convert
  // into. RGBA is already the wire layout and is sent straight from the readback
  // buffer, so it allocates nothing here.
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

  // The last frame handed to send_video_async is still owned by the SDK: the
  // call that releases it is send_destroy, and that lives in ~Sender, a member,
  // which does not run until after this body. Freeing the staging frames here
  // would hand the SDK freed memory to read -- measured as 8 of 8 frames
  // corrupted on the wire, and invisible to ASan because libndi is not
  // instrumented. Synchronise explicitly first.
  m_sender.flush_async();

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
    const int height = readback.pixelSize.height();

    // The readback's own width is NOT always the picture width. UYVYEncoder
    // writes an RGBA8 target at half width, each texel carrying two pixels as
    // (U, Y0, V, Y1), so the picture is twice as wide as the texture that came
    // back. RGBA is one texel per pixel and needs no such correction.
    const bool uyvy = (m_formatStr == "UYVY");
    const int width = uyvy ? readback.pixelSize.width() * 2 : readback.pixelSize.width();

    NDIlib_video_frame_v2_t ndiFrame{};
    ndiFrame.frame_rate_N = this->m_settings.rate * 10000;
    ndiFrame.frame_rate_D = 10000;

    // The GPU produced these bytes in the wire format already -- RGBA straight
    // from the scene, UYVY through UYVYEncoder -- so there is nothing to convert
    // and nothing to copy. The stride comes from the readback because the
    // backend may pad rows.
    const int stride = Ndi::readbackStride(readback.data.size(), height);
    if(!Ndi::describeVideoFrame(
           m_formatStr, (const uint8_t*)readback.data.data(), width, height,
           stride, ndiFrame))
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
  // A rebuild reuses this node with a brand-new renderer that reads back into
  // buffer 0; clear anything the previous run left behind first.
  m_pool.reset();
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

    {
      auto& rb = m_readback[m_pool.advanceToFreeBuffer()];
      if(m_uyvy_renderer)
        m_uyvy_renderer->updateReadback(rb);
      else if(m_inv_y_renderer)
        m_inv_y_renderer->updateReadback(rb);
    }
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
  auto& readback0 = const_cast<QRhiReadbackResult&>(m_readback[0]);
  auto* self = const_cast<OutputNode*>(this);

  // A rebuild lands here with a fresh renderer bound to buffer 0, which is what
  // ReadbackPool::reset() in startRendering() lines the pool back up with.
  if(m_formatStr == "UYVY")
  {
    // Convert on the GPU. The encoder folds the Y-flip into the same shader, so
    // there is no InvertYRenderer in this path at all -- and no sws_scale on the
    // sender thread, which at 2160p cost 21.5 ms against a 16.67 ms budget.
    auto* enc = new Ndi::UyvyEncodeRenderer{*this, rt, readback0};
    self->m_uyvy_renderer = enc;
    self->m_inv_y_renderer = nullptr;
      return enc;
  }

  auto* inv = new Gfx::InvertYRenderer{*this, rt, readback0};
  self->m_inv_y_renderer = inv;
  self->m_uyvy_renderer = nullptr;
  return inv;
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
