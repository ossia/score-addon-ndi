#include "score_addon_ndi.hpp"

#include <score/plugins/FactorySetup.hpp>
#include <Ndi/InputFactory.hpp>
#include <Ndi/OutputFactory.hpp>

score_addon_ndi::score_addon_ndi()
{
}

score_addon_ndi::~score_addon_ndi()
{
}

std::vector<std::unique_ptr<score::InterfaceBase>>
score_addon_ndi::factories(
    const score::ApplicationContext& ctx, const score::InterfaceKey& key) const
{
  return instantiate_factories<
      score::ApplicationContext,
      FW<Device::ProtocolFactory
      , Ndi::InputFactory
      , Ndi::OutputFactory
      >
  >(ctx, key);
}


#include <score/plugins/PluginInstances.hpp>
SCORE_EXPORT_PLUGIN(score_addon_ndi)
