#include "score_addon_ndi.hpp"

#include <score/plugins/FactorySetup.hpp>
#include <score/widgets/MessageBox.hpp>

#include <QTimer>

#include <Ndi/InputFactory.hpp>
#include <Ndi/Loader.hpp>
#include <Ndi/OutputFactory.hpp>

score_addon_ndi::score_addon_ndi()
{
  m_hasNDI = Ndi::Loader::instance().available();
  /*
  if(!m_hasNDI)
  {
    QTimer::singleShot(1, [] {
    score::warning(nullptr,
                   "NDI not found",
                   "NDI not available, please install an NDI library and reboot, or remove the NDI add-on.\n"
                   "NDI is searched for in the NDI_RUNTIME_DIR_V4 environment variable by default.");
    });
  }
  */
}

score_addon_ndi::~score_addon_ndi() { }

std::vector<std::unique_ptr<score::InterfaceBase>> score_addon_ndi::factories(
    const score::ApplicationContext& ctx, const score::InterfaceKey& key) const
{
  if(!m_hasNDI)
    return {};

  return instantiate_factories<
      score::ApplicationContext,
      FW<Device::ProtocolFactory, Ndi::InputFactory, Ndi::OutputFactory>>(ctx, key);
}

#include <score/plugins/PluginInstances.hpp>
SCORE_EXPORT_PLUGIN(score_addon_ndi)
