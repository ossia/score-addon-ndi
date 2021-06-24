#pragma once
#include <Device/Protocol/DeviceInterface.hpp>
#include <Device/Protocol/DeviceSettings.hpp>
#include <Device/Protocol/ProtocolFactoryInterface.hpp>
#include <Device/Protocol/ProtocolSettingsWidget.hpp>

#include <QLineEdit>

#include <Gfx/GfxDevice.hpp>

namespace Ndi
{

class InputFactory final : public Device::ProtocolFactory
{
  SCORE_CONCRETE("ae78b7c6-6400-483e-b45b-fd6ff87ec700")
  QString prettyName() const noexcept override;
  QString category() const noexcept override;
  Device::DeviceEnumerator* getEnumerator(const score::DocumentContext& ctx) const override;

  Device::DeviceInterface*
  makeDevice(const Device::DeviceSettings& settings, const Explorer::DeviceDocumentPlugin& plugin, const score::DocumentContext& ctx) override;
  const Device::DeviceSettings& defaultSettings() const noexcept override;
  Device::AddressDialog* makeAddAddressDialog(
      const Device::DeviceInterface& dev,
      const score::DocumentContext& ctx,
      QWidget* parent) override;
  Device::AddressDialog* makeEditAddressDialog(
      const Device::AddressSettings&,
      const Device::DeviceInterface& dev,
      const score::DocumentContext& ctx,
      QWidget*) override;

  Device::ProtocolSettingsWidget* makeSettingsWidget() override;

  QVariant makeProtocolSpecificSettings(const VisitorVariant& visitor) const override;

  void serializeProtocolSpecificSettings(const QVariant& data, const VisitorVariant& visitor)
      const override;

  bool checkCompatibility(const Device::DeviceSettings& a, const Device::DeviceSettings& b)
      const noexcept override;
};

class InputSettingsWidget final : public Device::ProtocolSettingsWidget
{
public:
  InputSettingsWidget(QWidget* parent = nullptr);

  Device::DeviceSettings getSettings() const override;
  void setSettings(const Device::DeviceSettings& settings) override;

private:
  void setDefaults();
  QLineEdit* m_deviceNameEdit{};
  Device::DeviceSettings m_settings;
};

}
