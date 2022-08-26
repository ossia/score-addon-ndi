#pragma once
#include <Gfx/GfxDevice.hpp>
#include <Gfx/Graph/NodeRenderer.hpp>
#include <Gfx/Graph/OutputNode.hpp>
#include <Gfx/InvertYRenderer.hpp>

#include <Ndi/Loader.hpp>

namespace Ndi
{
struct OutputNode;

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
  QTimer* m_timer{};
};

class OutputDevice final : public Gfx::GfxOutputDevice
{
  W_OBJECT(OutputDevice)
public:
  OutputDevice(
      const Device::DeviceSettings& settings, const score::DocumentContext& ctx);
  ~OutputDevice();

private:
  bool reconnect() override;
  ossia::net::device_base* getDevice() const override { return m_dev.get(); }

  Gfx::gfx_protocol_base* m_protocol{};
  mutable std::unique_ptr<ossia::net::device_base> m_dev;
};

}
