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

    if (m_renderer && m_renderState)
    {
      auto rhi = m_renderState->rhi;
      QRhiCommandBuffer* cb{};
      if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
        return;

      m_renderer->render(*cb);

      rhi->endOffscreenFrame();

      if(m_renderer->renderers.size() > 1)
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

void OutputNode::onRendererChange()
{
}

void OutputNode::stopRendering()
{
  m_timer->stop();
}

void OutputNode::setRenderer(score::gfx::RenderList* r)
{
  m_renderer = r;
}

score::gfx::RenderList* OutputNode::renderer() const
{
  return m_renderer;
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

const score::gfx::Mesh& OutputNode::mesh() const noexcept
{
  return score::gfx::TexturedTriangle::instance();
}

score::gfx::RenderState* OutputNode::renderState() const
{
  return m_renderState.get();
}

score::gfx::OutputNodeRenderer* OutputNode::createRenderer(score::gfx::RenderList& r) const noexcept
{
  score::gfx::TextureRenderTarget rt{m_texture, m_renderState->renderPassDescriptor, m_renderTarget};
  return new OutputRenderer{rt, const_cast<QRhiReadbackResult&>(m_readback)};
}

OutputRenderer::OutputRenderer(score::gfx::TextureRenderTarget rt,  QRhiReadbackResult& readback)
    : score::gfx::OutputNodeRenderer{}
    , m_inputTarget{std::move(rt)}
    , m_readback{readback}
{
}

void OutputRenderer::init(score::gfx::RenderList& renderer)
{
  m_renderTarget = score::gfx::createRenderTarget(renderer.state, QRhiTexture::Format::RGBA8, m_inputTarget.texture->pixelSize());

  auto& mesh = score::gfx::TexturedTriangle::instance();
  m_mesh = renderer.initMeshBuffer(mesh);

  // We need to have a pass to invert the Y coordinate to go
  // from GL direction (Y up) to normal video (Y down)
  // FIXME spout likely needs the same
  static const constexpr auto gl_filter = R"_(#version 450
    layout(location = 0) in vec2 v_texcoord;
    layout(location = 0) out vec4 fragColor;

    layout(binding = 3) uniform sampler2D tex;

    void main()
    {
fragColor = vec4(1);
      //fragColor = texture(tex, vec2(v_texcoord.x, 1. - v_texcoord.y));
    }
    )_";
  std::tie(m_vertexS, m_fragmentS) = score::gfx::makeShaders(mesh.defaultVertexShader(), gl_filter);

  // Put the input texture, where all the input nodes are rendering, in a sampler.
  {
    auto sampler = renderer.state.rhi->newSampler(
        QRhiSampler::Linear,
        QRhiSampler::Linear,
        QRhiSampler::None,
        QRhiSampler::ClampToEdge,
        QRhiSampler::ClampToEdge);

    sampler->setName("FullScreenImageNode::sampler");
    sampler->create();

    m_samplers.push_back({sampler, this->m_inputTarget.texture});
  }

  m_p = score::gfx::buildPipeline(
      renderer,
      mesh,
      m_vertexS,
      m_fragmentS,
      m_renderTarget,
      nullptr,
      nullptr,
      m_samplers);
}

void OutputRenderer::update(score::gfx::RenderList& renderer, QRhiResourceUpdateBatch& res)
{
}

void OutputRenderer::release(score::gfx::RenderList&)
{
  m_p.release();
  for(auto& s : m_samplers)
  {
    delete s.sampler;
  }
  m_samplers.clear();
  m_renderTarget.release();
}

void OutputRenderer::finishFrame(
    score::gfx::RenderList& renderer,
    QRhiCommandBuffer& cb)
{
  cb.beginPass(m_renderTarget.renderTarget, Qt::black, {1.0f, 0}, nullptr);
  {
    const auto sz = renderer.state.size;
    cb.setGraphicsPipeline(m_p.pipeline);
    cb.setShaderResources(m_p.srb);
    cb.setViewport(QRhiViewport(0, 0, sz.width(), sz.height()));

    assert(this->m_mesh.mesh);
    assert(this->m_mesh.mesh->usage().testFlag(QRhiBuffer::VertexBuffer));

    auto& mesh = score::gfx::TexturedTriangle::instance();
    mesh.setupBindings(*this->m_mesh.mesh, this->m_mesh.index, cb);

    cb.draw(mesh.vertexCount);
  }

  auto next = renderer.state.rhi->nextResourceUpdateBatch();

  QRhiReadbackDescription rb(m_inputTarget.texture);
  next->readBackTexture(rb, &m_readback);
  cb.endPass(next);
  qDebug("rendered year");

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
