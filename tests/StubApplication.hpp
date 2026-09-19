#pragma once
// The smallest application context a score widget will run in.
//
// Widgets reach for the skin as soon as they are built -- the device name
// field validates as it is typed, and validation asks the skin for its
// colours -- and score::AppContext() binds a null reference with no
// application, so a bare QApplication segfaults in the widget's constructor.

#include <core/application/ApplicationInterface.hpp>
#include <core/application/ApplicationSettings.hpp>
#include <core/presenter/DocumentManager.hpp>

#include <score/plugins/settingsdelegate/SettingsDelegateModel.hpp>

#include <memory>
#include <vector>

namespace Ndi::Testing
{
struct StubApplication final : score::ApplicationInterface
{
  score::ApplicationSettings settings;
  score::ApplicationComponentsData componentsData;
  score::ApplicationComponents comps{componentsData};
  score::DocumentList documents;
  std::vector<std::unique_ptr<score::SettingsDelegateModel>> settingsModels;
  score::ApplicationContext ctx{settings, comps, documents, settingsModels};

  StubApplication() { settings.gui = false; m_instance = this; }
  const score::ApplicationContext& context() const override { return ctx; }
  const score::ApplicationComponents& components() const override { return comps; }
};
}
