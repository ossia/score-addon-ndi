#include "OutputFactory.hpp"

#include <State/Widgets/AddressFragmentLineEdit.hpp>

#include <QFormLayout>
#include <QLabel>

#include <Ndi/OutputNode.hpp>
namespace Ndi
{
class OutputSettingsWidget final : public Gfx::SharedOutputSettingsWidget
{
public:
  OutputSettingsWidget(QWidget* parent = nullptr)
      : Gfx::SharedOutputSettingsWidget{parent}
  {
    m_deviceNameEdit->setText("NDI Out");
    m_shmPath->setVisible(false);
    ((QLabel*)m_layout->labelForField(m_shmPath))->setVisible(false);

    setSettings(OutputFactory{}.defaultSettings());
  }

  Device::DeviceSettings getSettings() const override
  {
    auto set = Gfx::SharedOutputSettingsWidget::getSettings();
    set.protocol = OutputFactory::static_concreteKey();
    return set;
  }
};

Device::ProtocolSettingsWidget* OutputFactory::makeSettingsWidget()
{
  return new OutputSettingsWidget{};
}

QString OutputFactory::prettyName() const noexcept
{
  return QObject::tr("NDI Output");
}

Device::DeviceInterface* OutputFactory::makeDevice(
    const Device::DeviceSettings& settings, const Explorer::DeviceDocumentPlugin& doc,
    const score::DocumentContext& ctx)
{
  return new OutputDevice(settings, ctx);
}

const Device::DeviceSettings& OutputFactory::defaultSettings() const noexcept
{
  static const Device::DeviceSettings settings = [&]() {
    Device::DeviceSettings s;
    s.protocol = concreteKey();
    s.name = "NDI Output";
    Gfx::SharedOutputSettings set;
    set.width = 1280;
    set.height = 720;
    set.path = "ndi";
    set.rate = 60.;
    s.deviceSpecificSettings = QVariant::fromValue(set);
    return s;
  }();
  return settings;
}

}
