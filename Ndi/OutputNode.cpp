#include <Gfx/GfxApplicationPlugin.hpp>
#include <Gfx/GfxExecContext.hpp>
#include <Gfx/GfxParameter.hpp>
#include <Gfx/Graph/RenderList.hpp>
#include <Gfx/SharedOutputSettings.hpp>

#include <score/gfx/OpenGL.hpp>

#include <ossia/network/base/device.hpp>

#include <QOffscreenSurface>
#include <QTimer>
#include <QtGui/private/qrhigles2_p.h>

#include <Ndi/OutputNode.hpp>

#include <wobjectimpl.h>
W_OBJECT_IMPL(Ndi::OutputDevice)
namespace Ndi
{

struct OutputNode : score::gfx::OutputNode
{
  OutputNode(const Ndi::Loader& ndi, const Gfx::SharedOutputSettings& set);
  virtual ~OutputNode();

  Gfx::SharedOutputSettings m_settings;
  std::weak_ptr<score::gfx::RenderList> m_renderer{};
  QRhiTexture* m_texture{};
  QRhiTextureRenderTarget* m_renderTarget{};
  std::function<void()> m_update;
  std::shared_ptr<score::gfx::RenderState> m_renderState{};
  QRhiReadbackResult m_readback;
  const Ndi::Loader& m_ndi;
  Ndi::Sender m_sender;
  bool m_hasSender{};

  void startRendering() override;
  void render() override;
  void onRendererChange() override;
  bool canRender() const override;
  void stopRendering() override;

  void setRenderer(std::shared_ptr<score::gfx::RenderList>) override;
  score::gfx::RenderList* renderer() const override;

  void createOutput(
      score::gfx::GraphicsApi graphicsApi, std::function<void()> onReady,
      std::function<void()> onUpdate, std::function<void()> onResize) override;
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
      const Ndi::Loader& ndi, const Gfx::SharedOutputSettings& set,
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

OutputNode::OutputNode(const Ndi::Loader& ndi, const Gfx::SharedOutputSettings& set)
    : score::gfx::OutputNode{}
    , m_settings{set}
    , m_ndi{ndi}
    , m_sender{m_ndi}
{
  input.push_back(new score::gfx::Port{this, {}, score::gfx::Types::Image, {}});
}

OutputNode::~OutputNode() { }
bool OutputNode::canRender() const
{
  return bool(m_renderState);
}

void OutputNode::startRendering() { }

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
      NDIlib_video_frame_v2_t frame;
      frame.xres = m_readback.pixelSize.width();
      frame.yres = m_readback.pixelSize.height();
      frame.FourCC = NDIlib_FourCC_type_RGBA;
      frame.p_data = (uint8_t*)m_readback.data.data();
      m_sender.send_video(frame);
    }
  }
}

score::gfx::OutputNode::Configuration OutputNode::configuration() const noexcept
{
  return {.manualRenderingRate = 1000. / m_settings.rate};
}
void OutputNode::onRendererChange() { }

void OutputNode::stopRendering() { }

void OutputNode::setRenderer(std::shared_ptr<score::gfx::RenderList> r)
{
  m_renderer = r;
}

score::gfx::RenderList* OutputNode::renderer() const
{
  return m_renderer.lock().get();
}

void OutputNode::createOutput(
    score::gfx::GraphicsApi graphicsApi, std::function<void()> onReady,
    std::function<void()> onUpdate, std::function<void()> onResize)
{
  m_renderState = std::make_shared<score::gfx::RenderState>();
  m_update = onUpdate;

  m_renderState->surface = QRhiGles2InitParams::newFallbackSurface();
  QRhiGles2InitParams params;
  params.fallbackSurface = m_renderState->surface;
  score::GLCapabilities caps;
  caps.setupFormat(params.format);
#include <Gfx/Qt5CompatPop>
  m_renderState->rhi = QRhi::create(QRhi::OpenGLES2, &params, {});
#include <Gfx/Qt5CompatPush>
  m_renderState->renderSize = QSize(m_settings.width, m_settings.height);
  m_renderState->outputSize = m_renderState->renderSize;
  m_renderState->api = score::gfx::GraphicsApi::OpenGL;
  m_renderState->version = caps.qShaderVersion;

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

  onReady();
}

void OutputNode::destroyOutput() { }

std::shared_ptr<score::gfx::RenderState> OutputNode::renderState() const
{
  return m_renderState;
}

score::gfx::OutputNodeRenderer*
OutputNode::createRenderer(score::gfx::RenderList& r) const noexcept
{
  score::gfx::TextureRenderTarget rt{
      m_texture, nullptr, nullptr, m_renderState->renderPassDescriptor, m_renderTarget};
  return new Gfx::InvertYRenderer{rt, const_cast<QRhiReadbackResult&>(m_readback)};
}

OutputDevice::OutputDevice(
    const Device::DeviceSettings& settings, const score::DocumentContext& ctx)
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
    if(plug)
    {
      auto set = m_settings.deviceSpecificSettings.value<Gfx::SharedOutputSettings>();

      m_protocol = new Gfx::gfx_protocol_base{plug->exec};
      m_dev = std::make_unique<ndi_output_device>(
          ndi, set, std::unique_ptr<ossia::net::protocol_base>(m_protocol),
          m_settings.name.toStdString());
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

#include <Gfx/Qt5CompatPop>
