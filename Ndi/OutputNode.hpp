#pragma once
#include <Gfx/GfxDevice.hpp>
#include <Gfx/Graph/nodes.hpp>
#include <Ndi/Loader.hpp>

namespace Ndi
{

class OutputRenderer : public RenderedNode
{
public:
  QRhiReadbackResult m_readback;
  using RenderedNode::RenderedNode;
  TextureRenderTarget createRenderTarget(const RenderState& state) override;

  void runPass(Renderer& renderer, QRhiCommandBuffer& cb, QRhiResourceUpdateBatch& updateBatch)
      override;
};

struct OutputNode : ::OutputNode
{
  OutputNode(const Ndi::Loader& ndi);
  virtual ~OutputNode();

  Renderer* m_renderer{};
  QRhiTexture* m_texture{};
  QRhiTextureRenderTarget* m_renderTarget{};
  std::function<void()> m_update;
  std::shared_ptr<RenderState> m_renderState{};
  const Ndi::Loader& m_ndi;
  Ndi::Sender m_sender;
  bool m_hasSender{};

  void startRendering() override;
  void onRendererChange() override;
  bool canRender() const override;
  void stopRendering() override;

  void setRenderer(Renderer* r) override;
  Renderer* renderer() const override;

  void createOutput(
      GraphicsApi graphicsApi,
      std::function<void()> onReady,
      std::function<void()> onUpdate,
      std::function<void()> onResize) override;
  void destroyOutput() override;

  RenderState* renderState() const override;
  score::gfx::NodeRenderer* createRenderer() const noexcept override;

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
