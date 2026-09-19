#include "InputFactory.hpp"

#include <Ndi/NdiColorSpace.hpp>

#include <State/Widgets/AddressFragmentLineEdit.hpp>

#include <Gfx/Widgets/CameraPreviewWidget.hpp>

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QTimerEvent>

#include <Ndi/HxDecoder.hpp>
#include <Ndi/InputNode.hpp>
#include <Ndi/InputStream.hpp>
#include <Ndi/Loader.hpp>

extern "C" {
#include <libavutil/pixdesc.h>
}

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

  // Editable: a source that is not broadcasting yet cannot be discovered, and
  // the SDK connects to it by name as soon as it appears.
  m_source = new QComboBox{this};
  m_source->setEditable(true);
  m_source->setInsertPolicy(QComboBox::NoInsert);
  m_source->setMinimumWidth(320);
  m_source->setToolTip(
      tr("Pick a source on the network, or type the name of one that is not "
         "online yet."));
  m_refresh = new QPushButton{tr("Refresh"), this};
  {
    auto* row = new QWidget{this};
    auto* lay = new QHBoxLayout{row};
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(m_source, 1);
    lay->addWidget(m_refresh);
    m_layout->addRow(tr("Source"), row);
  }

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

  m_preview = new Gfx::CameraPreviewWidget{this};
  m_preview->setFixedSize(320, 180);
  m_layout->addRow(tr("Preview"), m_preview);

  m_properties = new QLabel{this};
  m_properties->setTextFormat(Qt::PlainText);
  m_layout->addRow(QString{}, m_properties);

  if(!Ndi::hxDecoderAvailable(Loader::instance()))
  {
    auto* warn = new QLabel{
        tr("NDI|HX sources (phones, PTZ cameras) cannot be decoded: this NDI "
           "runtime needs FFmpeg %1, which is not installed. Such sources will "
           "show a \"Video decoder not found\" placeholder. See "
           "<a href=\"https://ndi.video/formats\">ndi.video/formats</a>.")
            .arg(hxDecoderLibs(ndiVersionMajor(Loader::instance().version())).avcodec),
        this};
    warn->setWordWrap(true);
    warn->setOpenExternalLinks(true);
    m_layout->addRow(warn);
  }

  m_debounce = new QTimer{this};
  m_debounce->setSingleShot(true);
  m_debounce->setInterval(400);
  connect(m_debounce, &QTimer::timeout, this, [this] { restartPreview(); });

  auto queue = [this] { m_debounce->start(); };
  connect(m_source, &QComboBox::currentTextChanged, this, queue);
  for(auto* c : {m_colorSpace, m_receiveFormat, m_deinterlace})
    connect(c, &QComboBox::currentIndexChanged, this, queue);
  connect(m_refresh, &QPushButton::clicked, this, [this] { refreshSources(); });

  if(Loader::instance().available())
  {
    m_finder = std::make_unique<Ndi::Finder>(Loader::instance());
    m_discoveryTimer = startTimer(1000);
  }
  m_propertiesTimer = startTimer(500);

  setSettings(InputFactory{}.defaultSettings());
  refreshSources();
}

InputSettingsWidget::~InputSettingsWidget() = default;

void InputSettingsWidget::timerEvent(QTimerEvent* ev)
{
  if(ev->timerId() == m_discoveryTimer)
    refreshSources();
  else if(ev->timerId() == m_propertiesTimer)
    updateProperties();
  else
    SharedInputSettingsWidget::timerEvent(ev);
}

void InputSettingsWidget::refreshSources()
{
  if(!m_finder)
    return;

  uint32_t n = 0;
  m_finder->wait_for_sources(0);
  auto* sources = m_finder->get_current_sources(&n);

  std::set<QString> found;
  for(uint32_t i = 0; i < n; i++)
    found.insert(QString::fromUtf8(sources[i].p_ndi_name));

  std::set<QString> listed;
  for(int i = 0; i < m_source->count(); i++)
    listed.insert(m_source->itemText(i));
  if(listed == found)
    return;

  // Keep whatever is in the edit field: it can name a source that is not on
  // the network yet, which is the point of being able to type one.
  const auto typed = m_source->currentText();
  {
    QSignalBlocker block{m_source};
    m_source->clear();
    for(const auto& s : found)
      m_source->addItem(s);
    m_source->setCurrentText(typed);
  }
}

void InputSettingsWidget::restartPreview()
{
  m_preview->clear();

  const auto source = m_source->currentText();
  if(source.isEmpty() || !Loader::instance().available())
    return;

  auto stream = std::make_shared<Ndi::InputStream>(Loader::instance());
  stream->m_colorSetting = Ndi::colorSpaceSettingFromName(m_colorSpace->currentText());
  stream->m_receiveFormat = Ndi::receiveFormatFromName(m_receiveFormat->currentText());
  stream->m_deinterlace = Ndi::deinterlaceFromName(m_deinterlace->currentText());
  stream->load(source.toStdString());

  m_preview->setInput(std::move(stream));
}

void InputSettingsWidget::updateProperties()
{
  const auto* fmt = m_preview->metadata();
  if(!fmt || fmt->width <= 0 || fmt->height <= 0)
  {
    m_properties->setText(
        m_source->currentText().isEmpty() ? tr("No source selected")
                                          : tr("Waiting for the source..."));
    return;
  }

  const char* pix = av_get_pix_fmt_name(fmt->pixel_format);
  QString text = tr("%1x%2").arg(fmt->width).arg(fmt->height);
  if(pix)
    text += QStringLiteral(" - %1").arg(pix);
  if(fmt->fps > 0)
    text += tr(" - %1 fps").arg(fmt->fps, 0, 'f', 2);
  m_properties->setText(text);
}

Device::DeviceSettings InputSettingsWidget::getSettings() const
{
  auto set = SharedInputSettingsWidget::getSettings();
  set.protocol = InputFactory::static_concreteKey();

  Ndi::InputSettings specif;
  specif.path = m_source->currentText();
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
  m_source->setCurrentText(set.path);

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
