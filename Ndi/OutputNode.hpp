#pragma once
#include <Gfx/GfxDevice.hpp>
#include <Gfx/Graph/NodeRenderer.hpp>
#include <Gfx/Graph/OutputNode.hpp>
#include <Gfx/InvertYRenderer.hpp>

#include <Ndi/Loader.hpp>

namespace Ndi
{
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
