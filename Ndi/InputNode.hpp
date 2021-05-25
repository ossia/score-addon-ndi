#pragma once

#include <Device/Protocol/DeviceInterface.hpp>
#include <Device/Protocol/DeviceSettings.hpp>

#include <ossia/gfx/texture_parameter.hpp>
#include <ossia/network/base/device.hpp>
#include <ossia/network/base/protocol.hpp>

#include <QLineEdit>

#include <Gfx/GfxDevice.hpp>
#include <Gfx/GfxExecContext.hpp>
#include <Gfx/Graph/VideoNode.hpp>
#include <Ndi/Loader.hpp>
#include <Video/FrameQueue.hpp>
#include <Video/VideoInterface.hpp>
class QComboBox;
namespace Ndi
{

class InputStream final : public Video::VideoInterface
{
public:
  explicit InputStream(const Ndi::Loader& ndi) noexcept;
  ~InputStream() noexcept;
  bool load(const std::string& inputDevice) noexcept;

  bool start() noexcept;
  void stop() noexcept;

  AVFrame* dequeue_frame() noexcept override;
  void release_frame(AVFrame* frame) noexcept override;

private:
  AVFrame* get_new_frame() noexcept;
  void buffer_thread() noexcept;
  AVFrame* read_frame_impl() noexcept;
  std::thread m_thread;
  Video::FrameQueue m_frames;

  std::atomic_bool m_running{};

  const Ndi::Loader& m_ndi;
  Ndi::Receiver m_receiver;
};

struct input_settings
{
  std::string device;
};

class input_protocol : public ossia::net::protocol_base
{
public:
  std::shared_ptr<InputStream> stream;
  input_protocol(Gfx::GfxExecutionAction& ctx);
  Gfx::GfxExecutionAction* context{};
  bool pull(ossia::net::parameter_base&) override { return false; }
  bool push(const ossia::net::parameter_base&, const ossia::value& v) override { return false; }
  bool push_raw(const ossia::net::full_parameter_data&) override { return false; }
  bool observe(ossia::net::parameter_base&, bool) override { return false; }
  bool update(ossia::net::node_base& node_base) override { return false; }

  void start_execution() override { stream->start(); }
  void stop_execution() override { stream->stop(); }
};

class input_parameter : public ossia::gfx::texture_input_parameter
{
  Gfx::GfxExecutionAction* context{};

public:
  std::shared_ptr<InputStream> stream;
  int32_t node_id{};
  score::gfx::VideoNode* node{};

  input_parameter(
      const input_settings& settings,
      ossia::net::node_base& n,
      Gfx::GfxExecutionAction* ctx)
      : ossia::gfx::texture_input_parameter{n}, context{ctx}
  {
    auto& proto = static_cast<input_protocol&>(n.get_device().get_protocol());
    stream = proto.stream;
    stream->load(settings.device);

    node = new score::gfx::VideoNode(proto.stream, {});
    node_id = context->ui->register_node(std::unique_ptr<score::gfx::VideoNode>(node));
  }
  void pull_texture(ossia::gfx::port_index idx) override
  {
    context->setEdge(Gfx::port_index{this->node_id, 0}, idx);
  }

  virtual ~input_parameter() { context->ui->unregister_node(node_id); }
};

class input_device;
class input_node : public ossia::net::node_base
{
  ossia::net::device_base& m_device;
  node_base* m_parent{};
  std::unique_ptr<input_parameter> m_parameter;

public:
  input_node(const input_settings& settings, ossia::net::device_base& dev, std::string name)
      : m_device{dev}
      , m_parameter{std::make_unique<input_parameter>(
            settings,
            *this,
            dynamic_cast<input_protocol&>(dev.get_protocol()).context)}
  {
    m_name = std::move(name);
  }

  input_parameter* get_parameter() const override { return m_parameter.get(); }

private:
  ossia::net::device_base& get_device() const override { return m_device; }
  ossia::net::node_base* get_parent() const override { return m_parent; }
  ossia::net::node_base& set_name(std::string) override { return *this; }
  ossia::net::parameter_base* create_parameter(ossia::val_type) override
  {
    return m_parameter.get();
  }
  bool remove_parameter() override { return false; }

  std::unique_ptr<ossia::net::node_base> make_child(const std::string& name) override
  {
    return {};
  }
  void removing_child(ossia::net::node_base& node_base) override { }
};

class input_device : public ossia::net::device_base
{
  input_node root;

public:
  input_device(
      const input_settings& settings,
      std::unique_ptr<ossia::net::protocol_base> proto,
      std::string name)
      : ossia::net::device_base{std::move(proto)}, root{settings, *this, name}
  {
  }

  const input_node& get_root_node() const override { return root; }
  input_node& get_root_node() override { return root; }
};

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

  input_protocol* m_protocol{};
  mutable std::unique_ptr<input_device> m_dev;
};

}

SCORE_SERIALIZE_DATASTREAM_DECLARE(, Ndi::InputSettings);
Q_DECLARE_METATYPE(Ndi::InputSettings)
W_REGISTER_ARGTYPE(Ndi::InputSettings)
