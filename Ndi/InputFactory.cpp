#include "InputFactory.hpp"

#include <State/Widgets/AddressFragmentLineEdit.hpp>

#include <QFormLayout>
#include <QLabel>

#include <Ndi/InputNode.hpp>
#include <Ndi/Loader.hpp>

#include <set>
namespace Ndi
{
class InputEnumerator : public Device::DeviceEnumerator
{
  std::set<QString> m_known;
  Ndi::Finder find{Loader::instance()};

public:
  InputEnumerator() { startTimer(1000); }

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
        Gfx::SharedInputSettings set;
        set.path = name;

        Device::DeviceSettings dev;
        dev.name = name;
        dev.protocol = InputFactory::static_concreteKey();
        dev.deviceSpecificSettings = QVariant::fromValue(set);
        deviceAdded(dev.name, dev);

        m_known.insert(name);
      }
    }

    for(auto it = m_known.begin(); it != m_known.end();)
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

  void enumerate(std::function<void(const QString&, const Device::DeviceSettings&)> f)
      const override
  {
  }
};

QString InputFactory::prettyName() const noexcept
{
  return QObject::tr("NDI Input");
}

QUrl InputFactory::manual() const noexcept
{
  return QUrl("https://ossia.io/score-docs/devices/ndi-device.html");
}

Device::DeviceEnumerators
InputFactory::getEnumerators(const score::DocumentContext& ctx) const
{
  auto& ndi = Loader::instance();
  if(!ndi.available())
    return {};
  else
    return {{"Sources", new InputEnumerator}};
}

Device::DeviceInterface* InputFactory::makeDevice(
    const Device::DeviceSettings& settings, const Explorer::DeviceDocumentPlugin& plugin,
    const score::DocumentContext& ctx)
{
  return new InputDevice(settings, ctx);
}

const Device::DeviceSettings& InputFactory::defaultSettings() const noexcept
{
  static const Device::DeviceSettings settings = [&]() {
    Device::DeviceSettings s;
    s.protocol = concreteKey();
    s.name = "NDI Input";
    Gfx::SharedInputSettings specif;
    s.deviceSpecificSettings = QVariant::fromValue(specif);
    return s;
  }();
  return settings;
}

Device::ProtocolSettingsWidget* InputFactory::makeSettingsWidget()
{
  return new InputSettingsWidget;
}

InputSettingsWidget::InputSettingsWidget(QWidget* parent)
    : SharedInputSettingsWidget(parent)
{
  m_deviceNameEdit->setText("NDI In");
  m_shmPath->setVisible(false);
  ((QLabel*)m_layout->labelForField(m_shmPath))->setVisible(false);
  setSettings(InputFactory{}.defaultSettings());
}

Device::DeviceSettings InputSettingsWidget::getSettings() const
{
  auto set = SharedInputSettingsWidget::getSettings();
  set.protocol = InputFactory::static_concreteKey();
  return set;
}

}
