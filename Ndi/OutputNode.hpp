#pragma once
#include <Gfx/GfxDevice.hpp>
#include <Gfx/Graph/NodeRenderer.hpp>
#include <Gfx/Graph/OutputNode.hpp>
#include <Ndi/Loader.hpp>

namespace Ndi
{
struct OutputNode;
class OutputRenderer : public score::gfx::GenericNodeRenderer
{
public:
  QRhiReadbackResult m_readback;
  explicit OutputRenderer(const score::gfx::RenderState& st, const OutputNode& parent);

  void runPass(score::gfx::RenderList& renderer, QRhiCommandBuffer& cb, QRhiResourceUpdateBatch& updateBatch)
      override;
};

struct OutputNode : score::gfx::OutputNode
{
  OutputNode(const Ndi::Loader& ndi);
  virtual ~OutputNode();

  score::gfx::RenderList* m_renderer{};
  QRhiTexture* m_texture{};
  QRhiTextureRenderTarget* m_renderTarget{};
  std::function<void()> m_update;
  std::shared_ptr<score::gfx::RenderState> m_renderState{};
  const Ndi::Loader& m_ndi;
  Ndi::Sender m_sender;
  bool m_hasSender{};

  void startRendering() override;
  void onRendererChange() override;
  bool canRender() const override;
  void stopRendering() override;

  void setRenderer(score::gfx::RenderList* r) override;
  score::gfx::RenderList* renderer() const override;

  void createOutput(
      score::gfx::GraphicsApi graphicsApi,
      std::function<void()> onReady,
      std::function<void()> onUpdate,
      std::function<void()> onResize) override;
  void destroyOutput() override;

  score::gfx::RenderState* renderState() const override;
  score::gfx::NodeRenderer* createRenderer(score::gfx::RenderList& r) const noexcept override;

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
