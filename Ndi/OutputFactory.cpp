#include "OutputFactory.hpp"

#include <State/Widgets/AddressFragmentLineEdit.hpp>

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QSpinBox>

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
    m_format->addItems({"RGBA", "UYVY"});

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
    if(m_format->currentText() == "RGBA")
      m_format->setCurrentIndex(0);
    else if(m_format->currentText() == "UYVY")
      m_format->setCurrentIndex(1);
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
        .format = m_format->currentText()};

    set.deviceSpecificSettings = QVariant::fromValue(std::move(specif));
    return set;
  }

  QComboBox* m_format{};
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
}

template <>
void DataStreamWriter::write(Ndi::OutputSettings& n)
{
  m_stream >> n.path >> n.width >> n.height >> n.rate;
  m_stream >> n.format;
}
template <>
void JSONReader::read(const Ndi::OutputSettings& n)
{
  obj["Path"] = n.path;
  obj["Width"] = n.width;
  obj["Height"] = n.height;
  obj["Rate"] = n.rate;
  obj["Format"] = n.format;
}

template <>
void JSONWriter::write(Ndi::OutputSettings& n)
{
  n.path = obj["Path"].toString();
  n.width = obj["Width"].toDouble();
  n.height = obj["Height"].toDouble();
  n.rate = obj["Rate"].toDouble();
  if(auto format = obj.tryGet("Format"))
    n.format = format->toString();
  else
    n.format = "RGBA";
}
