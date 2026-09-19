#include "OutputFactory.hpp"

#include <State/Widgets/AddressFragmentLineEdit.hpp>

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QSpinBox>

#include <Ndi/NdiColorSpace.hpp>
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
    m_format = new QComboBox{this};
    // Every format Ndi::wireFormats lists, in the order a user is likely to
    // want them: the two that need no conversion, then 8-bit 4:2:2, then
    // 16-bit, then the planar ones.
    m_format->addItems(
        {"RGBA", "RGBX", "BGRA", "BGRX", "UYVY", "P216", "NV12", "I420", "YV12"});

    // The matrix a receiver will assume is not signalled anywhere and is not
    // derivable -- see Ndi/NdiColorSpace.hpp for the measurements. Rec.709 is
    // what every receiver tested actually applies; the rest are here because
    // real equipment emits them.
    m_colorSpace = new QComboBox{this};
    for(auto s : Ndi::outputColorSpaceSettings)
      m_colorSpace->addItem(Ndi::colorSpaceSettingName(s));
    m_layout->addRow(tr("Color space"), m_colorSpace);

    this->m_layout->addRow(tr("Format"), m_format);

    setSettings(OutputFactory{}.defaultSettings());
  }

  void setSettings(const Device::DeviceSettings& settings) override
  {
    m_deviceNameEdit->setText(settings.name);

    const auto& set = settings.deviceSpecificSettings.value<OutputSettings>();
    m_shmPath->setText(set.path);
    m_width->setValue(set.width);
    m_height->setValue(set.height);
    m_rate->setValue(set.rate);
    // set.format, not m_format->currentText(): reading the combo box back
    // before it has been set compares it against its own first item, so the
    // saved format was never restored and every device reopened as RGBA.
    if(const int i = m_format->findText(set.format); i >= 0)
      m_format->setCurrentIndex(i);
    // An empty or unknown name resolves to the default rather than to nothing:
    // a device saved before this field existed must still open.
    m_colorSpace->setCurrentText(
        Ndi::colorSpaceSettingName(Ndi::colorSpaceSettingFromName(set.colorSpace)));
  }
  Device::DeviceSettings getSettings() const override
  {
    auto set = Gfx::SharedOutputSettingsWidget::getSettings();
    set.protocol = OutputFactory::static_concreteKey();

    const auto& base_s = set.deviceSpecificSettings.value<Gfx::SharedOutputSettings>();
    OutputSettings specif{
        .path = this->m_deviceNameEdit->text(),
        .width = base_s.width,
        .height = base_s.height,
        .rate = base_s.rate,
        .format = m_format->currentText(),
        .colorSpace = m_colorSpace->currentText()};

    set.deviceSpecificSettings = QVariant::fromValue(std::move(specif));
    return set;
  }

  QComboBox* m_format{};
  QComboBox* m_colorSpace{};
};

Device::ProtocolSettingsWidget* OutputFactory::makeSettingsWidget()
{
  return new OutputSettingsWidget{};
}

QString OutputFactory::prettyName() const noexcept
{
  return QObject::tr("NDI Output");
}

QUrl OutputFactory::manual() const noexcept
{
  return QUrl("https://ossia.io/score-docs/devices/ndi-device.html");
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
    OutputSettings set;
    set.width = 1280;
    set.height = 720;
    set.path = "ndi";
    set.rate = 60.;
    set.format = "RGBA";
    set.colorSpace = Ndi::colorSpaceSettingName(Ndi::ColorSpaceSetting::Rec709);
    s.deviceSpecificSettings = QVariant::fromValue(set);
    return s;
  }();
  return settings;
}

QVariant OutputFactory::makeProtocolSpecificSettings(const VisitorVariant& visitor) const
{
  return makeProtocolSpecificSettings_T<OutputSettings>(visitor);
}

void OutputFactory::serializeProtocolSpecificSettings(
    const QVariant& data, const VisitorVariant& visitor) const
{
  serializeProtocolSpecificSettings_T<OutputSettings>(data, visitor);
}
}

template <>
void DataStreamReader::read(const Ndi::OutputSettings& n)
{
  m_stream << n.path << n.width << n.height << n.rate;
  m_stream << n.format;
  m_stream << n.colorSpace;
}

template <>
void DataStreamWriter::write(Ndi::OutputSettings& n)
{
  m_stream >> n.path >> n.width >> n.height >> n.rate;
  m_stream >> n.format;
  // A stream written before this field existed ends here; QDataStream sets the
  // status and leaves the string empty, which resolves to the default.
  m_stream >> n.colorSpace;
}
template <>
void JSONReader::read(const Ndi::OutputSettings& n)
{
  obj["Path"] = n.path;
  obj["Width"] = n.width;
  obj["Height"] = n.height;
  obj["Rate"] = n.rate;
  obj["Format"] = n.format;
  obj["ColorSpace"] = n.colorSpace;
}

template <>
void JSONWriter::write(Ndi::OutputSettings& n)
{
  n.path = obj["Path"].toString();
  n.width = obj["Width"].toDouble();
  n.height = obj["Height"].toDouble();
  n.rate = obj["Rate"].toDouble();
  // toString() is rapidjson's GetString(), which asserts on a non-string value:
  // tryGet guards an absent key, not a present one of the wrong type.
  if(auto format = obj.tryGet("Format"); format && format->isString())
    n.format = format->toString();
  else
    n.format = "RGBA";
  if(auto cs = obj.tryGet("ColorSpace"); cs && cs->isString())
    n.colorSpace = cs->toString();
  else
    n.colorSpace = Ndi::colorSpaceSettingName(Ndi::ColorSpaceSetting::Rec709);
}
