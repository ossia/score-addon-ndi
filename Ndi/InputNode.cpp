#include "InputNode.hpp"

#include <ossia/detail/flicks.hpp>

#include <QDebug>
#include <QElapsedTimer>

#include <fmt/format.h>

#include <functional>


#include "InputNode.hpp"

#include <State/MessageListSerialization.hpp>
#include <State/Widgets/AddressFragmentLineEdit.hpp>

#include <score/serialization/MimeVisitor.hpp>

#include <ossia-qt/name_utils.hpp>

#include <QComboBox>
#include <QFormLayout>
#include <QMenu>
#include <QMimeData>
#include <Gfx/GfxApplicationPlugin.hpp>
#include <wobjectimpl.h>

W_OBJECT_IMPL(Ndi::InputDevice)

SCORE_SERALIZE_DATASTREAM_DEFINE(Ndi::InputSettings);

namespace Ndi
{

InputStream::InputStream(const Ndi::Loader& ndi) noexcept
  : m_ndi{ndi}
  , m_receiver{ndi}
{
  realTime = true;
}

InputStream::~InputStream() noexcept
{
  stop();
}

bool InputStream::load(const std::string& inputDevice) noexcept
{
  NDIlib_source_t source;
  source.p_ndi_name = inputDevice.c_str();
  source.p_url_address = nullptr;
  m_receiver.connect(&source);
  pixel_format = AV_PIX_FMT_UYVY422;
  width = 1920;
  height = 1080;

  return true;
}

bool InputStream::start() noexcept
{
  if (m_running)
    return false;

  m_running.store(true, std::memory_order_release);
  // TODO use a thread pool
  m_thread = std::thread{[this] { this->buffer_thread(); }};
  return true;
}

void InputStream::stop() noexcept
{
  // Stop the running status
  m_running.store(false, std::memory_order_release);

  if (m_thread.joinable())
    m_thread.join();

  // Remove frames that were in flight
  m_frames.drain();
}

AVFrame* InputStream::dequeue_frame() noexcept
{
  return m_frames.dequeue();
}

void InputStream::release_frame(AVFrame* frame) noexcept
{
  m_frames.release(frame);
}

void InputStream::buffer_thread() noexcept
{
  while (m_running.load(std::memory_order_acquire))
  {
    if (auto f = read_frame_impl())
    {
      m_frames.enqueue(f);
    }
  }
}

static std::optional<AVPixelFormat> getPixelFormat(NDIlib_FourCC_video_type_e fourcc)
{
  switch (fourcc)
  {
    case NDIlib_FourCC_video_type_UYVY:
      return AV_PIX_FMT_UYVY422;
    case NDIlib_FourCC_video_type_UYVA:
      // ? return AV_PIX_FMT_YUVA422P;
      return std::nullopt;
    case NDIlib_FourCC_video_type_P216:
      return AV_PIX_FMT_YUV422P16LE;
    case NDIlib_FourCC_video_type_PA16:
      // ? return AV_PIX_FMT_YUVA422P;
      return std::nullopt;
    case NDIlib_FourCC_video_type_YV12:
      // ? fmt = AV_PIX_FMT_YUV420P;
      return std::nullopt;
    case NDIlib_FourCC_video_type_I420:
      return AV_PIX_FMT_YUV420P;
    case NDIlib_FourCC_video_type_NV12:
      return AV_PIX_FMT_NV12;
    case NDIlib_FourCC_video_type_BGRA:
      return AV_PIX_FMT_BGRA;
      break;
    case NDIlib_FourCC_video_type_BGRX:
      return AV_PIX_FMT_BGR0;
      break;
    case NDIlib_FourCC_video_type_RGBA:
      return AV_PIX_FMT_RGBA;
      break;
    case NDIlib_FourCC_video_type_RGBX:
      return AV_PIX_FMT_RGB0;
      break;
    default:
      return std::nullopt;
  }
}

AVFrame* InputStream::read_frame_impl() noexcept
{
  NDIlib_video_frame_v2_t ndi_frame;
  switch (m_receiver.capture(&ndi_frame, nullptr, nullptr, 1000))
  {
    case NDIlib_frame_type_video:
    {
      if(auto format = getPixelFormat(ndi_frame.FourCC))
      {
        AVFrame* frame = m_frames.newFrame();
        frame->format = *format;

        frame->data[0] = ndi_frame.p_data;
        frame->linesize[0] = ndi_frame.line_stride_in_bytes;
        frame->width = ndi_frame.xres;
        frame->height = ndi_frame.yres;

        return frame;
      }
    }

    case NDIlib_frame_type_audio:
      break;
    case NDIlib_frame_type_metadata:
      break;
    case NDIlib_frame_type_error:
      break;
    case NDIlib_frame_type_none:
      break;
    case NDIlib_frame_type_status_change:
      break;
    default:
      break;
  }
  return nullptr;
}

input_protocol::input_protocol(Gfx::GfxExecutionAction& ctx)
    : protocol_base{flags{}}, context{&ctx}
{
  stream = std::make_shared<InputStream>(Loader::instance());
}

InputDevice::~InputDevice() { }

bool InputDevice::reconnect()
{
  disconnect();

  try
  {
    auto& ndi = Loader::instance();
    if(!ndi.available())
      return false;

    auto set = this->settings().deviceSpecificSettings.value<InputSettings>();
    input_settings ossia_stgs{
        set.device.toStdString()
    };

    auto plug = m_ctx.findPlugin<Gfx::DocumentPlugin>();
    if (plug)
    {
      m_protocol = new input_protocol{plug->exec};
      m_dev = std::make_unique<input_device>(
          ossia_stgs,
          std::unique_ptr<ossia::net::protocol_base>(m_protocol),
          this->settings().name.toStdString());
    }
  }
  catch (std::exception& e)
  {
    qDebug() << "Could not connect: " << e.what();
  }
  catch (...)
  {
    // TODO save the reason of the non-connection.
  }

  return connected();
}

}

template <>
void DataStreamReader::read(const Ndi::InputSettings& n)
{
  m_stream << n.device;
  insertDelimiter();
}

template <>
void DataStreamWriter::write(Ndi::InputSettings& n)
{
  m_stream >> n.device;
  checkDelimiter();
}

template <>
void JSONReader::read(const Ndi::InputSettings& n)
{
  obj["Device"] = n.device;
}

template <>
void JSONWriter::write(Ndi::InputSettings& n)
{
  n.device = obj["Device"].toString();
}
