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
};
}
