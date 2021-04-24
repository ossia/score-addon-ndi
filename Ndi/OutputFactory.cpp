#include "OutputFactory.hpp"

#include <State/Widgets/AddressFragmentLineEdit.hpp>

#include <QFormLayout>

#include <Ndi/OutputNode.hpp>
namespace Ndi
{
QString OutputFactory::prettyName() const noexcept
{
  return QObject::tr("NDI Output");
}

QString OutputFactory::category() const noexcept
{
  return StandardCategories::video;
}

Device::DeviceEnumerator* OutputFactory::getEnumerator(const score::DocumentContext& ctx) const
{
  return nullptr;
}

Device::DeviceInterface* OutputFactory::makeDevice(
    const Device::DeviceSettings& settings,
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
    return s;
  }();
  return settings;
}

Device::AddressDialog* OutputFactory::makeAddAddressDialog(
    const Device::DeviceInterface& dev,
    const score::DocumentContext& ctx,
    QWidget* parent)
{
  return nullptr;
}

Device::AddressDialog* OutputFactory::makeEditAddressDialog(
    const Device::AddressSettings& set,
    const Device::DeviceInterface& dev,
    const score::DocumentContext& ctx,
    QWidget* parent)
{
  return nullptr;
}

Device::ProtocolSettingsWidget* OutputFactory::makeSettingsWidget()
{
  return new OutputSettingsWidget;
}

QVariant OutputFactory::makeProtocolSpecificSettings(const VisitorVariant& visitor) const
{
  return {};
}

void OutputFactory::serializeProtocolSpecificSettings(
    const QVariant& data,
    const VisitorVariant& visitor) const
{
}

bool OutputFactory::checkCompatibility(
    const Device::DeviceSettings& a,
    const Device::DeviceSettings& b) const noexcept
{
  return a.name != b.name;
}

OutputSettingsWidget::OutputSettingsWidget(QWidget* parent) : ProtocolSettingsWidget(parent)
{
  m_deviceNameEdit = new State::AddressFragmentLineEdit{this};

  auto layout = new QFormLayout;
  layout->addRow(tr("Device Name"), m_deviceNameEdit);

  setLayout(layout);

  setDefaults();
}

void OutputSettingsWidget::setDefaults()
{
  m_deviceNameEdit->setText("NDI Output");
}

Device::DeviceSettings OutputSettingsWidget::getSettings() const
{
  Device::DeviceSettings s;
  s.name = m_deviceNameEdit->text();
  s.protocol = OutputFactory::static_concreteKey();
  return s;
}

void OutputSettingsWidget::setSettings(const Device::DeviceSettings& settings)
{
  m_deviceNameEdit->setText(settings.name);
}

}
