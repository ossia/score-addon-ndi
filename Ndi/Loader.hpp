#pragma once
#include <Processing.NDI.Lib.h>

namespace Ndi
{
struct Loader
{
  static const Loader& instance()
  {
    static Loader loader;
    return loader;
  }

  explicit Loader();
  ~Loader();

  bool available() const noexcept { return bool(m_lib); }

  // Send API
  auto send_create() const noexcept { return m_lib->send_create(nullptr); }
  auto send_destroy(NDIlib_send_instance_t sender) const noexcept
  {
    m_lib->send_destroy(sender);
  }
  auto send_send_video(
      NDIlib_send_instance_t sender, NDIlib_video_frame_v2_t& frame) const noexcept
  {
    m_lib->send_send_video_v2(sender, &frame);
  }
  auto send_send_video_async(
      NDIlib_send_instance_t sender, NDIlib_video_frame_v2_t& frame) const noexcept
  {
    m_lib->send_send_video_async_v2(sender, &frame);
  }

  // Find API
  auto find_create() const noexcept { return m_lib->find_create_v2(nullptr); }
  auto find_destroy(NDIlib_find_instance_t finder) const noexcept
  {
    return m_lib->find_destroy(finder);
  }
  auto
  find_wait_for_sources(NDIlib_find_instance_t finder, uint32_t timeout) const noexcept
  {
    return m_lib->find_wait_for_sources(finder, timeout);
  }
  auto find_get_current_sources(
      NDIlib_find_instance_t finder, uint32_t* n_sources) const noexcept
  {
    return m_lib->find_get_current_sources(finder, n_sources);
  }
  auto find_get_sources(
      NDIlib_find_instance_t finder, uint32_t* n_sources,
      uint32_t timeout) const noexcept
  {
    return m_lib->find_get_sources(finder, n_sources, timeout);
  }

  // Recv API
  auto recv_create(NDIlib_recv_create_v3_t* inst = nullptr) const noexcept
  {
    return m_lib->recv_create_v3(inst);
  }
  auto recv_destroy(NDIlib_recv_instance_t recv) const noexcept
  {
    return m_lib->recv_destroy(recv);
  }
  auto
  recv_connect(NDIlib_recv_instance_t recv, const NDIlib_source_t* source) const noexcept
  {
    return m_lib->recv_connect(recv, source);
  }
  auto recv_capture(
      NDIlib_recv_instance_t recv, NDIlib_video_frame_v2_t* vf,
      NDIlib_audio_frame_v3_t* af, NDIlib_metadata_frame_t* meta,
      uint32_t timeout) const noexcept
  {
    return m_lib->recv_capture_v3(recv, vf, af, meta, timeout);
  }
  auto recv_free_audio(
      NDIlib_recv_instance_t recv, NDIlib_audio_frame_v3_t* frame) const noexcept
  {
    return m_lib->recv_free_audio_v3(recv, frame);
  }
  auto recv_free_video(
      NDIlib_recv_instance_t recv, NDIlib_video_frame_v2_t* frame) const noexcept
  {
    return m_lib->recv_free_video_v2(recv, frame);
  }
  auto recv_ptz_is_supported(NDIlib_recv_instance_t recv) const noexcept
  {
    return m_lib->recv_ptz_is_supported(recv);
  }
  auto recv_ptz_zoom(NDIlib_recv_instance_t recv, float zoom) const noexcept
  {
    return m_lib->recv_ptz_zoom(recv, zoom);
  }
  auto
  recv_ptz_pan_tilt(NDIlib_recv_instance_t recv, float pan, float tilt) const noexcept
  {
    return m_lib->recv_ptz_pan_tilt(recv, pan, tilt);
  }
  auto recv_ptz_pan_tilt_speed(
      NDIlib_recv_instance_t recv, float pan, float tilt) const noexcept
  {
    return m_lib->recv_ptz_pan_tilt_speed(recv, pan, tilt);
  }
  auto recv_ptz_store_preset(NDIlib_recv_instance_t recv, int p) const noexcept
  {
    return m_lib->recv_ptz_store_preset(recv, p);
  }
  auto recv_ptz_recall_preset(NDIlib_recv_instance_t recv, int p, float s) const noexcept
  {
    return m_lib->recv_ptz_recall_preset(recv, p, s);
  }
  auto recv_ptz_auto_focus(NDIlib_recv_instance_t recv) const noexcept
  {
    return m_lib->recv_ptz_auto_focus(recv);
  }
  auto recv_ptz_focus(NDIlib_recv_instance_t recv, float pan) const noexcept
  {
    return m_lib->recv_ptz_focus(recv, pan);
  }
  auto recv_ptz_focus_speed(NDIlib_recv_instance_t recv, float pan) const noexcept
  {
    return m_lib->recv_ptz_focus_speed(recv, pan);
  }
  auto recv_ptz_white_balance_auto(NDIlib_recv_instance_t recv) const noexcept
  {
    return m_lib->recv_ptz_white_balance_auto(recv);
  }
  auto recv_ptz_white_balance_indoor(NDIlib_recv_instance_t recv) const noexcept
  {
    return m_lib->recv_ptz_white_balance_indoor(recv);
  }
  auto recv_ptz_white_balance_outdoor(NDIlib_recv_instance_t recv) const noexcept
  {
    return m_lib->recv_ptz_white_balance_outdoor(recv);
  }
  auto recv_ptz_white_balance_oneshot(NDIlib_recv_instance_t recv) const noexcept
  {
    return m_lib->recv_ptz_white_balance_oneshot(recv);
  }
  auto recv_ptz_white_balance_manual(
      NDIlib_recv_instance_t recv, float r, float b) const noexcept
  {
    return m_lib->recv_ptz_white_balance_manual(recv, r, b);
  }
  auto recv_ptz_exposure_auto(NDIlib_recv_instance_t recv) const noexcept
  {
    return m_lib->recv_ptz_exposure_auto(recv);
  }
  auto recv_ptz_exposure_manual(NDIlib_recv_instance_t recv, float pan) const noexcept
  {
    return m_lib->recv_ptz_exposure_manual(recv, pan);
  }
  auto send_get_no_connections(
      NDIlib_send_instance_t s, uint32_t timeout_in_ms) const noexcept
  {
    return m_lib->send_get_no_connections(s, timeout_in_ms);
  }

private:
  void* m_ndi_dll{};
  const NDIlib_v5* m_lib{};
};

struct Sender
{
  const Loader& ndi;
  NDIlib_send_instance_t impl{};

  Sender(const Loader& ndi)
      : ndi{ndi}
      , impl{ndi.send_create()}
  {
  }

  ~Sender() { ndi.send_destroy(impl); }

  void send_video(NDIlib_video_frame_v2_t& frame) { ndi.send_send_video(impl, frame); }
  void send_video_async(NDIlib_video_frame_v2_t& frame)
  {
    ndi.send_send_video_async(impl, frame);
  }
  int get_no_connections(uint32_t timeout)
  {
    return ndi.send_get_no_connections(impl, timeout);
  }
};

struct Finder
{
  const Loader& ndi;
  NDIlib_find_instance_t impl{};

  Finder(const Loader& ndi)
      : ndi{ndi}
      , impl{ndi.find_create()}
  {
  }

  ~Finder() { ndi.find_destroy(impl); }

  auto wait_for_sources(uint32_t timeout)
  {
    return ndi.find_wait_for_sources(impl, timeout);
  }
  auto get_current_sources(uint32_t* n_sources)
  {
    return ndi.find_get_current_sources(impl, n_sources);
  }
  auto get_sources(uint32_t* n_sources, uint32_t timeout)
  {
    return ndi.find_get_sources(impl, n_sources, timeout);
  }
};

struct Receiver
{
  const Loader& ndi;
  NDIlib_recv_instance_t impl{};

  Receiver(const Loader& ndi)
      : ndi{ndi}
  {
  }

  ~Receiver()
  {
    if(impl)
      ndi.recv_destroy(impl);
  }

  auto create(NDIlib_recv_create_v3_t setup) { impl = ndi.recv_create(&setup); }

  auto connect(const NDIlib_source_t* source) { return ndi.recv_connect(impl, source); }

  auto capture(
      NDIlib_video_frame_v2_t* vf, NDIlib_audio_frame_v3_t* af,
      NDIlib_metadata_frame_t* meta, uint32_t timeout)
  {
    return ndi.recv_capture(impl, vf, af, meta, timeout);
  }

  auto free_audio(NDIlib_audio_frame_v3_t* frame)
  {
    return ndi.recv_free_audio(impl, frame);
  }

  auto free_video(NDIlib_video_frame_v2_t* frame)
  {
    return ndi.recv_free_video(impl, frame);
  }

  bool has_ptz() const noexcept { return ndi.recv_ptz_is_supported(impl); }

  void zoom(float zoom) const noexcept { ndi.recv_ptz_zoom(impl, zoom); }
  void pan(float pan) noexcept { ndi.recv_ptz_pan_tilt(impl, m_pan = pan, m_tilt); }
  void tilt(float tilt) noexcept { ndi.recv_ptz_pan_tilt(impl, m_pan, m_tilt = tilt); }
  void pan_speed(float pan) noexcept
  {
    ndi.recv_ptz_pan_tilt_speed(impl, m_pan_speed = pan, m_tilt_speed);
  }
  void tilt_speed(float tilt) noexcept
  {
    ndi.recv_ptz_pan_tilt_speed(impl, m_pan_speed, m_tilt_speed = tilt);
  }
  void store_preset(int p) const noexcept { ndi.recv_ptz_store_preset(impl, p); }
  void recall_preset(int p) const noexcept { ndi.recv_ptz_recall_preset(impl, p, 0.f); }
  void auto_focus() const noexcept { ndi.recv_ptz_auto_focus(impl); }
  void focus(float pan) const noexcept { ndi.recv_ptz_focus(impl, pan); }
  void focus_speed(float pan) const noexcept { ndi.recv_ptz_focus_speed(impl, pan); }
  void white_balance_auto() const noexcept { ndi.recv_ptz_white_balance_auto(impl); }
  void white_balance_indoor() const noexcept { ndi.recv_ptz_white_balance_indoor(impl); }
  void white_balance_outdoor() const noexcept
  {
    ndi.recv_ptz_white_balance_outdoor(impl);
  }
  void white_balance_oneshot() const noexcept
  {
    ndi.recv_ptz_white_balance_oneshot(impl);
  }
  void white_balance_manual(float r, float b) const noexcept
  {
    ndi.recv_ptz_white_balance_manual(impl, r, b);
  }
  void exposure_auto() const noexcept { ndi.recv_ptz_exposure_auto(impl); }
  void exposure_manual(float pan) const noexcept
  {
    ndi.recv_ptz_exposure_manual(impl, pan);
  }

private:
  float m_pan = 0.;
  float m_tilt = 0.;
  float m_pan_speed = 0.;
  float m_tilt_speed = 0.;
};
}
