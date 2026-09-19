// What the settings dialog gives back must be what it was given.
//
// Ndi::InputSettings derives from Gfx::SharedInputSettings so that the source
// name lives in one place. QVariant does not model that: asking a variant
// holding the derived type for the base one yields a default-constructed base,
// not a slice. The shared widget does exactly that to populate its path field,
// so every NDI device edited through the dialog was written back with an empty
// source name -- and a device with no source receives nothing, forever, without
// an error anywhere.
//
// Every field the dialog owns is checked, not just the path: the same hazard
// applies to anything else the base class might one day read that way.

#include <core/application/ApplicationInterface.hpp>
#include <core/application/ApplicationSettings.hpp>
#include <core/presenter/DocumentManager.hpp>

#include <score/plugins/settingsdelegate/SettingsDelegateModel.hpp>

#include <Ndi/InputFactory.hpp>
#include <Ndi/NdiColorSpace.hpp>

#include <QApplication>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace
{
// The device name field validates as it is typed, and validation asks the skin
// for its colours, and the skin asks for the application context -- which a
// bare QApplication does not have, so score::AppContext() binds a null
// reference and the widget's own constructor segfaults. This is the smallest
// thing that answers: no plugins, no documents, and gui=false so the skin does
// not go looking for fonts either.
struct StubApplication final : score::ApplicationInterface
{
  score::ApplicationSettings settings;
  score::ApplicationComponentsData componentsData;
  score::ApplicationComponents comps{componentsData};
  score::DocumentList documents;
  std::vector<std::unique_ptr<score::SettingsDelegateModel>> settingsModels;
  score::ApplicationContext ctx{settings, comps, documents, settingsModels};

  StubApplication() { m_instance = this; }
  const score::ApplicationContext& context() const override { return ctx; }
  const score::ApplicationComponents& components() const override { return comps; }
};

int g_fail = 0;
void check(bool ok, const std::string& what)
{
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if(!ok)
    ++g_fail;
}
}

int main(int argc, char** argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app{argc, argv};
  StubApplication stub;
  stub.settings.gui = false;

  // Not the defaults, in every field: a widget that ignored its argument and
  // kept what its constructor set would otherwise pass.
  Ndi::InputSettings in;
  in.path = "MAC-MINI.LOCAL (Test Patterns)";
  in.colorSpace = Ndi::colorSpaceSettingName(Ndi::ColorSpaceSetting::BT601);
  in.receiveFormat = Ndi::receiveFormatName(Ndi::ReceiveFormat::Best);
  in.deinterlace = Ndi::deinterlaceName(Video::Deinterlace::Bob);

  Device::DeviceSettings dev;
  dev.name = "NDI In";
  dev.protocol = Ndi::InputFactory::static_concreteKey();
  dev.deviceSpecificSettings = QVariant::fromValue(in);

  Ndi::InputSettingsWidget w;
  w.setSettings(dev);
  const auto out = w.getSettings();
  const auto got = out.deviceSpecificSettings.value<Ndi::InputSettings>();

  std::printf("\n== a device opened in the dialog and closed again ==\n");
  check(out.name == dev.name, "the device keeps its name");
  check(out.protocol == dev.protocol, "and its protocol");
  check(
      got.path == in.path,
      "the source name survives -- \"" + got.path.toStdString() + "\"");
  check(got.colorSpace == in.colorSpace, "the colour space survives");
  check(got.receiveFormat == in.receiveFormat, "the receive format survives");
  check(got.deinterlace == in.deinterlace, "the deinterlace mode survives");

  // The hazard itself, stated once so that the reason for the widget's
  // explicit assignment is on record rather than inferred.
  std::printf("\n== why the widget cannot let the base class do it ==\n");
  check(
      QVariant::fromValue(in).value<Gfx::SharedInputSettings>().path.isEmpty(),
      "a variant holding InputSettings has no SharedInputSettings in it");

  std::printf(
      "\nndi settings round trip: %s (%d failure%s)\n", g_fail ? "FAILED" : "passed",
      g_fail, g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
