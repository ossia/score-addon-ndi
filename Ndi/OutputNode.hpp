#pragma once
#include <Gfx/GfxDevice.hpp>
#include <Gfx/Graph/NodeRenderer.hpp>
#include <Gfx/Graph/OutputNode.hpp>
#include <Ndi/Loader.hpp>

namespace Ndi
{
struct OutputNode;
class OutputRenderer final : public score::gfx::OutputNodeRenderer
{
public:
  explicit OutputRenderer(score::gfx::TextureRenderTarget rt,  QRhiReadbackResult& readback);

  score::gfx::TextureRenderTarget m_inputTarget;
  score::gfx::TextureRenderTarget m_renderTarget;

  QShader m_vertexS, m_fragmentS;

  std::vector<score::gfx::Sampler> m_samplers;

  score::gfx::Pipeline m_p;

  score::gfx::MeshBuffers m_mesh{};

  score::gfx::TextureRenderTarget renderTargetForInput(const score::gfx::Port& p) override { return m_inputTarget; }

  void finishFrame(score::gfx::RenderList& renderer, QRhiCommandBuffer& cb) override;


  void init(score::gfx::RenderList& renderer) override;
  void update(score::gfx::RenderList& renderer, QRhiResourceUpdateBatch& res) override;
  void release(score::gfx::RenderList&) override;

private:
  QRhiReadbackResult& m_readback;
};

struct OutputNode : score::gfx::OutputNode
{
  OutputNode(const Ndi::Loader& ndi);
  virtual ~OutputNode();

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
  void onRendererChange() override;
  bool canRender() const override;
  void stopRendering() override;

  void setRenderer(std::shared_ptr<score::gfx::RenderList>) override;
  score::gfx::RenderList* renderer() const override;

  void createOutput(
      score::gfx::GraphicsApi graphicsApi,
      std::function<void()> onReady,
      std::function<void()> onUpdate,
      std::function<void()> onResize) override;
  void destroyOutput() override;

  score::gfx::RenderState* renderState() const override;
  score::gfx::OutputNodeRenderer* createRenderer(score::gfx::RenderList& r) const noexcept override;

  QTimer* m_timer{};
};

class OutputDevice final : public Gfx::GfxOutputDevice
{
  W_OBJECT(OutputDevice)
public:
  OutputDevice(const Device::DeviceSettings& settings, const score::DocumentContext& ctx);
  ~OutputDevice();

private:
  bool reconnect() override;
  ossia::net::device_base* getDevice() const override { return m_dev.get(); }

  Gfx::gfx_protocol_base* m_protocol{};
  mutable std::unique_ptr<ossia::net::device_base> m_dev;
};

}
