#include <Gfx/GfxApplicationPlugin.hpp>
#include <Ndi/ReadbackPool.hpp>
#include <Ndi/WireEncodeRenderer.hpp>
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
  score::gfx::TextureRenderTarget m_renderTarget;
  std::function<void()> m_update;
  std::shared_ptr<score::gfx::RenderState> m_renderState{};
  // Created in createRenderer; render() tells it, frame by frame, whether to
  // encode and which slot to read back into.
  Ndi::WireEncodeRenderer* m_wire_renderer{};

  // The picture size the render target was actually created with, which is what
  // the sender describes to the SDK. Written once in createOutput, before the
  // sender thread starts, and read by it from then on.
  QSize m_pictureSize;
  // Readback pool. A frame handed to send_video_async stays owned by the SDK
  // until the next send, so the renderer must never target a buffer that is
  // still spoken for; the states in Ndi::ReadbackPool are what enforce that,
  // and they are what let every format send the readback itself with no copy
  // at all.
  //
  // Four buffers: at most one Queued, one Sending and one InFlight, so a Free
  // one always exists. A copy that WireEncodeRenderer finishes one frame late
  // is collected, and queued, before the next Free buffer is chosen, so it
  // never needs a fifth.
  //
  // The slots' host memory is allocated by WireEncodeRenderer, once, when the
  // direct readback path is taken (Ndi::SendSlot).
  //
  // Declared before m_sender: the SDK lets go of the last frame in
  // send_destroy, in ~Sender, which must run before this memory is freed.
  static constexpr int kBuffers = 4;
  Ndi::SendSlot m_slots[kBuffers];
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

  // Converted once here rather than per frame: toStdString() goes through
  // toUtf8(), which mallocs and frees a QByteArray, and the sender thread is a
  // realtime path. describeVideoFrame takes a string_view, so nothing needs a
  // fresh std::string.
  m_formatStr = m_settings.format.toStdString();

  // A format this addon cannot send would otherwise be discovered per frame, in
  // describeVideoFrame, on the sender thread, one refusal at a time.
  if(!Ndi::findWireFormat(m_formatStr))
  {
    qWarning() << "Ndi::OutputNode: unsupported output format"
               << m_settings.format << "-- nothing will be sent";
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

    // The slot's geometry is NOT the picture's, and how it differs depends on
    // the format: a packed format's encoder writes two pixels per texel, a
    // planar format's framestore is taller than the picture. So the picture
    // size comes from the render target that was actually created, and the
    // stride from the renderer, which knows where the bytes landed.
    const auto& slot = m_slots[idx];
    const int width = m_pictureSize.width();
    const int height = m_pictureSize.height();

    NDIlib_video_frame_v2_t ndiFrame{};
    ndiFrame.frame_rate_N = this->m_settings.rate * 10000;
    ndiFrame.frame_rate_D = 10000;

    // The GPU produced these bytes in the wire format already, so there is
    // nothing to convert and nothing to copy.
    if(!Ndi::describeVideoFrame(
           m_formatStr, slot.data, width, height, slot.stride, ndiFrame))
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
  // Idempotent on purpose. Graph::createAllRenderLists stops every output
  // before starting it, but Graph::createSingleRenderList -- the inspector's
  // texture-port preview -- calls startRendering() with no stopRendering()
  // ahead of it, on an output that may already be running. Assigning to a
  // joinable std::thread calls std::terminate(), so that path would take the
  // whole application down rather than misbehave.
  if(m_senderThread.joinable())
    return;

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

    // Nothing to encode or read back when nothing renders into this output or
    // nobody receives it: the scene still renders, the GPU->CPU part does not.
    const bool active
        = renderer->renderers.size() > 1 && m_sender.get_no_connections(0) > 0;
    if(m_wire_renderer)
      Ndi::beginSendFrame(*m_wire_renderer, m_pool, active);

    QRhiCommandBuffer* cb{};
    if(rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
      return;

    renderer->render(*cb);

    rhi->endOffscreenFrame();

    if(m_wire_renderer)
      Ndi::endSendFrame(*m_wire_renderer, m_pool);
  }
}

score::gfx::OutputNode::Configuration OutputNode::configuration() const noexcept
{
  return {.manualRenderingRate = 1000. / m_settings.rate};
}
void OutputNode::onRendererChange() { }

void OutputNode::stopRendering()
{
  // The renderer built next reads back into these slots from the first frame,
  // including the one the SDK was last handed: the SDK has to let go of it
  // before the pool calls it Free.
  m_pool.stopAndReclaim(m_senderThread, [this] { m_sender.flush_async(); });
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
  m_pictureSize = m_renderState->renderSize;

  auto rhi = m_renderState->rhi;

  // A 16-bit wire format wants more than 8 bits to encode from, or its extra
  // bits are 8-bit values scaled up. The scene renders into RGBA16F for those,
  // the way DirectVideoOutputNode does for the same reason (prefersFloatRender).
  const bool wantsFloat = Ndi::ndiEncoding(m_formatStr).floatRender;
  const auto texFormat
      = wantsFloat ? QRhiTexture::RGBA16F : QRhiTexture::RGBA8;
  if(wantsFloat && !rhi->isTextureFormatSupported(texFormat))
  {
    qWarning() << "Ndi::OutputNode: no RGBA16F render target available;"
               << m_settings.format
               << "will be encoded from 8-bit precision";
  }
  m_texture = rhi->newTexture(
      rhi->isTextureFormatSupported(texFormat) ? texFormat : QRhiTexture::RGBA8,
      m_renderState->renderSize, 1,
      QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
  m_texture->create();
  m_renderTarget = score::gfx::createOutputRenderTarget(*m_renderState, *m_texture);

  if(conf.onReady)
    conf.onReady();
}

void OutputNode::destroyOutput()
{
  if(!m_renderState)
    return;

  // Before anything touches the QRhi: the GpuResourceRegistry owns QRhiBuffers
  // (GpuResourceRegistry::env, raw_camera, ...) whose wrappers must be deleted
  // while the QRhi is still alive. score::gfx::OutputNode::releaseRegistry()
  // says so in as many words -- "concrete subclasses MUST call this from
  // destroyOutput() BEFORE the QRhi is torn down" -- and skipping it leaves the
  // Vulkan allocator holding live blocks at vmaDestroyAllocator:
  //
  //   ASSERT "Some allocations were not freed before destruction of this
  //           memory block!" (vk_mem_alloc.h)
  //
  // This never showed while the node forced OpenGL, because there is no VMA
  // there to complain. ScreenNode::destroyOutput carries the same call for the
  // same reason. It is idempotent.
  releaseRegistry();

  score::gfx::releaseOutputRenderTarget(*m_renderState, m_renderTarget);

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
  score::gfx::TextureRenderTarget rt = m_renderTarget;
  auto* self = const_cast<OutputNode*>(this);

  // Convert on the GPU, through score::gfx's encoder for this wire format,
  // with the Y-flip folded into the same shader. A rebuild lands here with a
  // fresh renderer, which ReadbackPool::reset() in startRendering() lines the
  // pool back up with.
  auto* enc = new Ndi::WireEncodeRenderer{
      *this, rt, std::span<Ndi::SendSlot>{self->m_slots}, m_formatStr,
      Ndi::colorSpaceSettingFromName(m_settings.colorSpace)};
  self->m_wire_renderer = enc;
  return enc;
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
