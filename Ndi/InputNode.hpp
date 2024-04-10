#pragma once

#include <Device/Protocol/DeviceInterface.hpp>
#include <Device/Protocol/DeviceSettings.hpp>

#include <Gfx/GfxExecContext.hpp>
#include <Gfx/GfxInputDevice.hpp>

#include <ossia/gfx/texture_parameter.hpp>
#include <ossia/network/base/device.hpp>
#include <ossia/network/base/protocol.hpp>

#include <QLineEdit>
class QComboBox;
namespace Ndi
{
class InputStream;
class InputDevice final : public Gfx::GfxInputDevice
{
  W_OBJECT(InputDevice)
public:
  using GfxInputDevice::GfxInputDevice;
  ~InputDevice();

private:
  void createPtz();
  bool reconnect() override;
  void disconnect() override;

  ossia::net::device_base* getDevice() const override { return m_dev.get(); }

  Gfx::video_texture_input_protocol* m_protocol{};
  mutable std::unique_ptr<Gfx::video_texture_input_device> m_dev;
  std::shared_ptr<InputStream> m_stream;
};

}
