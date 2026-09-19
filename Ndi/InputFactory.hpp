#pragma once
#include <Device/Protocol/DeviceInterface.hpp>
#include <Device/Protocol/DeviceSettings.hpp>
#include <Device/Protocol/ProtocolFactoryInterface.hpp>
#include <Device/Protocol/ProtocolSettingsWidget.hpp>

#include <Gfx/GfxDevice.hpp>
#include <Gfx/SharedInputSettings.hpp>

#include <Ndi/InputSettings.hpp>

#include <QComboBox>
#include <QLineEdit>

namespace Ndi
{

class InputFactory final : public Gfx::SharedInputProtocolFactory
{
  SCORE_CONCRETE("ae78b7c6-6400-483e-b45b-fd6ff87ec700")
public:
  QString prettyName() const noexcept override;
  QUrl manual() const noexcept override;

  Device::DeviceEnumerators
  getEnumerators(const score::DocumentContext& ctx) const override;

  Device::DeviceInterface* makeDevice(
      const Device::DeviceSettings& settings,
      const Explorer::DeviceDocumentPlugin& plugin,
      const score::DocumentContext& ctx) override;
  const Device::DeviceSettings& defaultSettings() const noexcept override;

  Device::ProtocolSettingsWidget* makeSettingsWidget() override;

  // Ndi::InputSettings adds a field to the shared struct, so the base class's
  // serializers would drop it.
  QVariant makeProtocolSpecificSettings(const VisitorVariant& visitor) const override;
  void serializeProtocolSpecificSettings(
      const QVariant& data, const VisitorVariant& visitor) const override;
};

class InputSettingsWidget final : public Gfx::SharedInputSettingsWidget
{
public:
  InputSettingsWidget(QWidget* parent = nullptr);

  Device::DeviceSettings getSettings() const override;
  void setSettings(const Device::DeviceSettings& settings) override;

private:
  QComboBox* m_colorSpace{};
  QComboBox* m_receiveFormat{};
  QComboBox* m_deinterlace{};
};

}
