#pragma once
#include <Ndi/InputSettings.hpp>
#include <Ndi/Loader.hpp>
#include <Ndi/NdiColorSpace.hpp>

#include <Video/ExternalInput.hpp>
#include <Video/FrameQueue.hpp>

#include <QObject>

#include <verdigris>

#include <atomic>
#include <string>
#include <thread>

namespace Ndi
{
class InputStream final
    : public QObject
    , public Video::ExternalInput
{
  W_OBJECT(InputStream)
public:
  explicit InputStream(const Ndi::Loader& ndi) noexcept;
  ~InputStream() noexcept;
  bool load(const std::string& inputDevice) noexcept;

  bool start() noexcept override;
  void stop() noexcept override;

  AVFrame* dequeue_frame() noexcept override;
  void release_frame(AVFrame* frame) noexcept override;

  void ptz_changed(bool state) W_SIGNAL(ptz_changed, state)

  Ndi::Receiver& receiver() noexcept { return m_receiver; }

private:
  void timerEvent(QTimerEvent* t) override;
  void buffer_thread() noexcept;
  AVFrame* read_frame_impl() noexcept;
  std::thread m_thread;
  Video::FrameQueue m_frames;

  std::atomic_bool m_running{};

  const Ndi::Loader& m_ndi;
  Ndi::Receiver m_receiver;

public:
  /// How much of a frame's own colour declaration to believe, and what to ask
  /// the SDK for. Set before load(); read on the receive thread from then on.
  Ndi::ColorSpaceSetting m_colorSetting{Ndi::ColorSpaceSetting::Rec709};
  Ndi::ReceiveFormat m_receiveFormat{Ndi::ReceiveFormat::EightBit};
  Video::Deinterlace m_deinterlace{Video::Deinterlace::Weave};
};
}
