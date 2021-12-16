#include "InputFactory.hpp"

#include <State/Widgets/AddressFragmentLineEdit.hpp>

#include <QFormLayout>

#include <Ndi/InputNode.hpp>
#include <set>
namespace Ndi
{
class InputEnumerator : public Device::DeviceEnumerator
{
  std::set<QString> m_known;
  Ndi::Finder find{Loader::instance()};
public:
  InputEnumerator()
  {
    startTimer(1000);
  }

  void timerEvent(QTimerEvent* ev) override
  {
    uint32_t num_sources = 0;
    find.wait_for_sources(10);
    auto sources = find.get_current_sources(&num_sources);

    std::set<QString> new_nodes;
    for(uint32_t i = 0; i < num_sources; i++)
    {
      QString name = sources[i].p_ndi_name;
      new_nodes.insert(name);
      if(m_known.find(name) == m_known.end())
      {
        InputSettings set;
        set.device = name;

        Device::DeviceSettings dev;
        dev.name = name;
        dev.protocol = InputFactory::static_concreteKey();
        dev.deviceSpecificSettings = QVariant::fromValue(set);
        deviceAdded(dev);

        m_known.insert(name);
      }
    }

    for(auto it = m_known.begin(); it != m_known.end(); )
    {
      if(!new_nodes.contains(*it))
      {
        deviceRemoved(*it);
        it = m_known.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

  void enumerate(std::function<void(const Device::DeviceSettings&)> f) const override
  {
  }
};

QString InputFactory::prettyName() const noexcept
{
  return QObject::tr("NDI Input");
}

QString InputFactory::category() const noexcept
{
  return StandardCategories::video;
}

Device::DeviceEnumerator* InputFactory::getEnumerator(const score::DocumentContext& ctx) const
{
  auto& ndi = Loader::instance();
  if(!ndi.available())
    return nullptr;
  else
    return new InputEnumerator;
}

Device::DeviceInterface*
InputFactory::makeDevice(const Device::DeviceSettings& settings, const Explorer::DeviceDocumentPlugin& plugin, const score::DocumentContext& ctx)
{
  return new InputDevice(settings, ctx);
}

const Device::DeviceSettings& InputFactory::defaultSettings() const noexcept
{
  static const Device::DeviceSettings settings = [&]() {
    Device::DeviceSettings s;
    s.protocol = concreteKey();
    s.name = "NDI Input";
    InputSettings specif;
    s.deviceSpecificSettings = QVariant::fromValue(specif);
    return s;
  }();
  return settings;
}

Device::AddressDialog* InputFactory::makeAddAddressDialog(
    const Device::DeviceInterface& dev,
    const score::DocumentContext& ctx,
    QWidget* parent)
{
  return nullptr;
}

Device::AddressDialog* InputFactory::makeEditAddressDialog(
    const Device::AddressSettings& set,
    const Device::DeviceInterface& dev,
    const score::DocumentContext& ctx,
    QWidget* parent)
{
  return nullptr;
}

Device::ProtocolSettingsWidget* InputFactory::makeSettingsWidget()
{
  return new InputSettingsWidget;
}

QVariant InputFactory::makeProtocolSpecificSettings(const VisitorVariant& visitor) const
{
  return makeProtocolSpecificSettings_T<InputSettings>(visitor);
}

void InputFactory::serializeProtocolSpecificSettings(
    const QVariant& data,
    const VisitorVariant& visitor) const
{
  serializeProtocolSpecificSettings_T<InputSettings>(data, visitor);
}

bool InputFactory::checkCompatibility(
    const Device::DeviceSettings& a,
    const Device::DeviceSettings& b) const noexcept
{
  return a.name != b.name;
}

InputSettingsWidget::InputSettingsWidget(QWidget* parent) : ProtocolSettingsWidget(parent)
{
  m_deviceNameEdit = new State::AddressFragmentLineEdit{this};
  checkForChanges(m_deviceNameEdit);

  auto layout = new QFormLayout;
  layout->addRow(tr("Device Name"), m_deviceNameEdit);
  setLayout(layout);

  setDefaults();
}

void InputSettingsWidget::setDefaults()
{
  m_deviceNameEdit->setText("ndi");
}

Device::DeviceSettings InputSettingsWidget::getSettings() const
{
  Device::DeviceSettings s = m_settings;
  s.name = m_deviceNameEdit->text();
  s.protocol = InputFactory::static_concreteKey();
  return s;
}

void InputSettingsWidget::setSettings(const Device::DeviceSettings& settings)
{
  m_settings = settings;
  m_deviceNameEdit->setText(settings.name);
}
}
