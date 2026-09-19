#include "InputFactory.hpp"

#include <Ndi/NdiColorSpace.hpp>

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
        Ndi::InputSettings set;
        set.path = name;
        set.colorSpace = Ndi::colorSpaceSettingName(Ndi::ColorSpaceSetting::Rec709);
        set.receiveFormat = Ndi::receiveFormatName(Ndi::ReceiveFormat::EightBit);
        set.deinterlace = Ndi::deinterlaceName(Video::Deinterlace::Weave);

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
    Ndi::InputSettings specif;
    specif.colorSpace = Ndi::colorSpaceSettingName(Ndi::ColorSpaceSetting::Rec709);
    specif.receiveFormat = Ndi::receiveFormatName(Ndi::ReceiveFormat::EightBit);
    specif.deinterlace = Ndi::deinterlaceName(Video::Deinterlace::Weave);
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

  // A received frame does not say which matrix it was encoded with, and the
  // three things that could answer disagree -- see Ndi/NdiColorSpace.hpp. The
  // automatic entries differ in which one they believe.
  m_colorSpace = new QComboBox{this};
  for(auto s : Ndi::inputColorSpaceSettings)
    m_colorSpace->addItem(Ndi::colorSpaceSettingName(s));
  m_layout->addRow(tr("Color space"), m_colorSpace);

  // 16-bit is not free: the SDK stops de-interlacing when asked for it and
  // starts delivering individual fields at twice the frame rate. Say so here
  // rather than leaving it to be discovered.
  m_receiveFormat = new QComboBox{this};
  for(auto f : Ndi::receiveFormats)
    m_receiveFormat->addItem(Ndi::receiveFormatName(f));
  m_receiveFormat->setToolTip(
      tr("8-bit: the SDK de-interlaces and hands over progressive frames.\n"
         "Best available: 16-bit where the source has it, but interlaced "
         "sources then arrive as individual fields, which score de-interlaces "
         "itself."));
  m_layout->addRow(tr("Receive format"), m_receiveFormat);

  // Only reachable through the 16-bit format, which is the only one that
  // delivers fields at all.
  m_deinterlace = new QComboBox{this};
  for(auto d : Ndi::deinterlaceModes)
    m_deinterlace->addItem(Ndi::deinterlaceName(d));
  m_deinterlace->setToolTip(
      tr("Applies only when a source delivers individual fields, which happens "
         "with the 16-bit receive format."));
  m_layout->addRow(tr("Deinterlace"), m_deinterlace);

  setSettings(InputFactory{}.defaultSettings());
}

Device::DeviceSettings InputSettingsWidget::getSettings() const
{
  auto set = SharedInputSettingsWidget::getSettings();
  set.protocol = InputFactory::static_concreteKey();

  Ndi::InputSettings specif;
  specif.path = m_shmPath->text();
  specif.colorSpace = m_colorSpace->currentText();
  specif.receiveFormat = m_receiveFormat->currentText();
  specif.deinterlace = m_deinterlace->currentText();
  set.deviceSpecificSettings = QVariant::fromValue(specif);
  return set;
}

void InputSettingsWidget::setSettings(const Device::DeviceSettings& settings)
{
  SharedInputSettingsWidget::setSettings(settings);
  const auto set = settings.deviceSpecificSettings.value<Ndi::InputSettings>();

  // QVariant does not know that InputSettings derives from SharedInputSettings:
  // the base widget asked the variant for a SharedInputSettings, got a
  // default-constructed one, and emptied the source name with it.
  m_shmPath->setText(set.path);

  // An empty or unknown name resolves to the default rather than to nothing: a
  // device saved before this field existed must still open.
  m_colorSpace->setCurrentText(
      Ndi::colorSpaceSettingName(Ndi::colorSpaceSettingFromName(set.colorSpace)));
  m_receiveFormat->setCurrentText(
      Ndi::receiveFormatName(Ndi::receiveFormatFromName(set.receiveFormat)));
  m_deinterlace->setCurrentText(
      Ndi::deinterlaceName(Ndi::deinterlaceFromName(set.deinterlace)));
}

QVariant InputFactory::makeProtocolSpecificSettings(const VisitorVariant& visitor) const
{
  return makeProtocolSpecificSettings_T<Ndi::InputSettings>(visitor);
}

void InputFactory::serializeProtocolSpecificSettings(
    const QVariant& data, const VisitorVariant& visitor) const
{
  serializeProtocolSpecificSettings_T<Ndi::InputSettings>(data, visitor);
}

}

template <>
void DataStreamReader::read(const Ndi::InputSettings& n)
{
  m_stream << n.path;
  m_stream << n.colorSpace;
  m_stream << n.receiveFormat;
  m_stream << n.deinterlace;
}

template <>
void DataStreamWriter::write(Ndi::InputSettings& n)
{
  m_stream >> n.path;
  // A stream written before these fields existed ends here; QDataStream sets
  // the status and leaves the strings empty, which resolve to the defaults.
  m_stream >> n.colorSpace;
  m_stream >> n.receiveFormat;
  m_stream >> n.deinterlace;
}

template <>
void JSONReader::read(const Ndi::InputSettings& n)
{
  obj["Path"] = n.path;
  obj["ColorSpace"] = n.colorSpace;
  obj["ReceiveFormat"] = n.receiveFormat;
  obj["Deinterlace"] = n.deinterlace;
}

template <>
void JSONWriter::write(Ndi::InputSettings& n)
{
  n.path = obj["Path"].toString();
  // toString() is rapidjson's GetString(), which asserts on a non-string value:
  // tryGet guards an absent key, not a present one of the wrong type.
  if(auto cs = obj.tryGet("ColorSpace"); cs && cs->isString())
    n.colorSpace = cs->toString();
  else
    n.colorSpace = Ndi::colorSpaceSettingName(Ndi::ColorSpaceSetting::Rec709);
  if(auto rf = obj.tryGet("ReceiveFormat"); rf && rf->isString())
    n.receiveFormat = rf->toString();
  else
    n.receiveFormat = Ndi::receiveFormatName(Ndi::ReceiveFormat::EightBit);
  if(auto di = obj.tryGet("Deinterlace"); di && di->isString())
    n.deinterlace = di->toString();
  else
    n.deinterlace = Ndi::deinterlaceName(Video::Deinterlace::Weave);
}
