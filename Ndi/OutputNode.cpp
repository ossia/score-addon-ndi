#include <ossia/network/base/device.hpp>

#include <QTimer>
#include <QtGui/private/qrhigles2_p.h>

#include <Gfx/GfxApplicationPlugin.hpp>
#include <Gfx/GfxExecContext.hpp>
#include <Gfx/GfxParameter.hpp>
#include <Gfx/Graph/RenderList.hpp>
#include <Ndi/OutputNode.hpp>
#include <wobjectimpl.h>
W_OBJECT_IMPL(Ndi::OutputDevice)
namespace Ndi
{

class ndi_output_device : public ossia::net::device_base
{
  Gfx::gfx_node_base root;

public:
  ndi_output_device(
      const Ndi::Loader& ndi,
      std::unique_ptr<ossia::net::protocol_base> proto,
      std::string name)
      : ossia::net::device_base{std::move(proto)}, root{*this, new OutputNode{ndi}, name}
  {
  }

  const Gfx::gfx_node_base& get_root_node() const override { return root; }
  Gfx::gfx_node_base& get_root_node() override { return root; }
};

OutputNode::OutputNode(const Ndi::Loader& ndi)
  : score::gfx::OutputNode{}
  , m_ndi{ndi}
  , m_sender{m_ndi}
{
  input.push_back(new score::gfx::Port{this, {}, score::gfx::Types::Image, {}});
  m_timer = new QTimer;
  QObject::connect(m_timer, &QTimer::timeout, [this] {
    if (m_update)
      m_update();


    auto renderer = m_renderer.lock();
    if (renderer && m_renderState)
    {
      auto rhi = m_renderState->rhi;
      QRhiCommandBuffer* cb{};
      if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
        return;

      renderer->render(*cb);

      rhi->endOffscreenFrame();

      if(renderer->renderers.size() > 1)
      {
        NDIlib_video_frame_v2_t frame;
        frame.xres = m_readback.pixelSize.width();
        frame.yres = m_readback.pixelSize.height();
        frame.FourCC = NDIlib_FourCC_type_RGBA;
        frame.p_data = (uint8_t*)m_readback.data.data();
        m_sender.send_video(frame);
      }
    }
  });
}

OutputNode::~OutputNode() { }
bool OutputNode::canRender() const
{
  return bool(m_renderState);
}

void OutputNode::startRendering()
{
  m_timer->start(16);
}

void OutputNode::render()
{
}

score::gfx::OutputNode::Configuration OutputNode::configuration() const noexcept
{
  return { .manualRenderingRate = 60. };
}
void OutputNode::onRendererChange()
{
}

void OutputNode::stopRendering()
{
  m_timer->stop();
}

void OutputNode::setRenderer(std::shared_ptr<score::gfx::RenderList> r)
{
  m_renderer = r;
}

score::gfx::RenderList* OutputNode::renderer() const
{
  return m_renderer.lock().get();
}

void OutputNode::createOutput(
    score::gfx::GraphicsApi graphicsApi,
    std::function<void()> onReady,
    std::function<void()> onUpdate,
    std::function<void()> onResize)
{
  m_renderState = std::make_shared<score::gfx::RenderState>();
  m_update = onUpdate;

  m_renderState->surface = QRhiGles2InitParams::newFallbackSurface();
  QRhiGles2InitParams params;
  params.fallbackSurface = m_renderState->surface;
#include <Gfx/Qt5CompatPop>
  m_renderState->rhi = QRhi::create(QRhi::OpenGLES2, &params, {});
#include <Gfx/Qt5CompatPush>
  m_renderState->size = QSize(1280, 720);
  m_renderState->api = score::gfx::GraphicsApi::OpenGL;

  auto rhi = m_renderState->rhi;
  m_texture = rhi->newTexture(QRhiTexture::RGBA8, m_renderState->size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
  m_texture->create();
  m_renderTarget = rhi->newTextureRenderTarget({m_texture});
  m_renderState->renderPassDescriptor = m_renderTarget->newCompatibleRenderPassDescriptor();
  m_renderTarget->setRenderPassDescriptor(m_renderState->renderPassDescriptor);
  m_renderTarget->create();

  onReady();
}

void OutputNode::destroyOutput()
{
}

score::gfx::RenderState* OutputNode::renderState() const
{
  return m_renderState.get();
}

score::gfx::OutputNodeRenderer* OutputNode::createRenderer(score::gfx::RenderList& r) const noexcept
{
  score::gfx::TextureRenderTarget rt{m_texture, nullptr, m_renderState->renderPassDescriptor, m_renderTarget};
  return new Gfx::InvertYRenderer{rt, const_cast<QRhiReadbackResult&>(m_readback)};
}

OutputDevice::OutputDevice(
    const Device::DeviceSettings& settings,
    const score::DocumentContext& ctx)
    : Gfx::GfxOutputDevice{settings, ctx}
{
}

OutputDevice::~OutputDevice() { }

bool OutputDevice::reconnect()
{
  disconnect();

  try
  {
    auto& ndi = Loader::instance();
    if(!ndi.available())
      return false;
    auto plug = m_ctx.findPlugin<Gfx::DocumentPlugin>();
    if (plug)
    {
      m_protocol = new Gfx::gfx_protocol_base{plug->exec};
      m_dev = std::make_unique<ndi_output_device>(
          ndi,
          std::unique_ptr<ossia::net::protocol_base>(m_protocol),
          m_settings.name.toStdString());
    }
  }
  catch (std::exception& e)
  {
    qDebug() << "Could not connect: " << e.what();
  }
  catch (...)
  {
    // TODO save the reason of the non-connection.
  }

  return connected();
}
}

#include <Gfx/Qt5CompatPop>
