#pragma once

#include <Device/Protocol/DeviceInterface.hpp>
#include <Device/Protocol/DeviceSettings.hpp>

#include <ossia/gfx/texture_parameter.hpp>
#include <ossia/network/base/device.hpp>
#include <ossia/network/base/protocol.hpp>

#include <QLineEdit>

#include <Gfx/GfxInputDevice.hpp>
#include <Gfx/GfxExecContext.hpp>
class QComboBox;
namespace Ndi
{

struct InputSettings
{
  QString device;
};

class InputDevice final : public Gfx::GfxInputDevice
{
  W_OBJECT(InputDevice)
public:
  using GfxInputDevice::GfxInputDevice;
  ~InputDevice();

private:
  bool reconnect() override;
  ossia::net::device_base* getDevice() const override { return m_dev.get(); }

  Gfx::video_texture_input_protocol* m_protocol{};
  mutable std::unique_ptr<Gfx::video_texture_input_device> m_dev;
};

}

SCORE_SERIALIZE_DATASTREAM_DECLARE(, Ndi::InputSettings);
Q_DECLARE_METATYPE(Ndi::InputSettings)
W_REGISTER_ARGTYPE(Ndi::InputSettings)
