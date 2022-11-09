#pragma once
#include <Device/Protocol/DeviceInterface.hpp>
#include <Device/Protocol/DeviceSettings.hpp>
#include <Device/Protocol/ProtocolFactoryInterface.hpp>
#include <Device/Protocol/ProtocolSettingsWidget.hpp>

#include <Gfx/GfxDevice.hpp>
#include <Gfx/SharedOutputSettings.hpp>

#include <QLineEdit>

namespace Ndi
{
class gfx_protocol_base;
class OutputFactory final : public Gfx::SharedOutputProtocolFactory
{
  SCORE_CONCRETE("07651c13-83de-48b8-a450-abe2891051e8")
public:
  QString prettyName() const noexcept override;

  Device::DeviceInterface* makeDevice(
      const Device::DeviceSettings& settings, const Explorer::DeviceDocumentPlugin& doc,
      const score::DocumentContext& ctx) override;
  const Device::DeviceSettings& defaultSettings() const noexcept override;

  Device::ProtocolSettingsWidget* makeSettingsWidget() override;
};
}
