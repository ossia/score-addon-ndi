#include "InputNode.hpp"

#include <State/MessageListSerialization.hpp>
#include <State/Widgets/AddressFragmentLineEdit.hpp>

#include <Gfx/GfxApplicationPlugin.hpp>
#include <Gfx/GfxInputDevice.hpp>
#include <Gfx/Graph/VideoNode.hpp>
#include <Gfx/SharedInputSettings.hpp>
#include <Video/ExternalInput.hpp>
#include <Video/FrameQueue.hpp>
#include <Video/VideoInterface.hpp>

#include <score/serialization/MimeVisitor.hpp>

#include <ossia/detail/flicks.hpp>

#include <ossia-qt/name_utils.hpp>

#include <QComboBox>
#include <QDebug>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QMenu>
#include <QMimeData>

#include <Ndi/Loader.hpp>
#include <fmt/format.h>

#include <wobjectimpl.h>

#include <functional>

extern "C" {
#include <libavformat/avformat.h>
}

W_OBJECT_IMPL(Ndi::InputDevice)

namespace Ndi
{

// Convert a NDI frame into an AVFrame.
// The frame planes are reference-counted through AVBufferRef
// so that av_frame_free frees the frame data.
AVFrame *ndi_video_to_avframe(const Ndi::Loader& loader,
                              NDIlib_recv_instance_t recv,
                              NDIlib_video_frame_v2_t *ndi,
                              AVFrame* f)
{
  if (!f)
    return nullptr;

  int w = ndi->xres, h = ndi->yres;
  int s = ndi->line_stride_in_bytes;

  f->width  = w;
  f->height = h;
  f->pts    = ndi->timestamp;

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 0, 0)
  switch (ndi->frame_format_type) {
    case NDIlib_frame_format_type_progressive:
      break;
    case NDIlib_frame_format_type_field_0:
      f->flags |= AV_FRAME_FLAG_INTERLACED;
      f->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
      break;
    case NDIlib_frame_format_type_field_1:
      f->flags |= AV_FRAME_FLAG_INTERLACED;
      break;
    case NDIlib_frame_format_type_interleaved:
    default:
      av_frame_free(&f);
      return nullptr;
  }
#else
  switch (ndi->frame_format_type) {
    case NDIlib_frame_format_type_interleaved:
      f->interlaced_frame = 1; f->top_field_first = 1; break;
    case NDIlib_frame_format_type_field_0:
      f->interlaced_frame = 1; f->top_field_first = 1; break;
    case NDIlib_frame_format_type_field_1:
      f->interlaced_frame = 1; f->top_field_first = 0; break;
    default: break;
  }
#endif

  switch (ndi->FourCC) {
    case NDIlib_FourCC_video_type_UYVY: f->format = AV_PIX_FMT_UYVY422; break;
    case NDIlib_FourCC_video_type_UYVA: f->format = AV_PIX_FMT_UYVY422; break; // alpha dropped
    case NDIlib_FourCC_video_type_BGRA: f->format = AV_PIX_FMT_BGRA;    break;
    case NDIlib_FourCC_video_type_BGRX: f->format = AV_PIX_FMT_BGR0;    break;
    case NDIlib_FourCC_video_type_RGBA: f->format = AV_PIX_FMT_RGBA;    break;
    case NDIlib_FourCC_video_type_RGBX: f->format = AV_PIX_FMT_RGB0;    break;
    case NDIlib_FourCC_video_type_NV12: f->format = AV_PIX_FMT_NV12;    break;
    case NDIlib_FourCC_video_type_I420: f->format = AV_PIX_FMT_YUV420P; break;
    case NDIlib_FourCC_video_type_YV12: f->format = AV_PIX_FMT_YUV420P; break; // planes reordered
#ifdef AV_PIX_FMT_P216LE
    case NDIlib_FourCC_video_type_P216: f->format = AV_PIX_FMT_P216LE;  break;
    case NDIlib_FourCC_video_type_PA16: f->format = AV_PIX_FMT_P216LE;  break; // alpha dropped
#endif
    default:
      av_frame_free(&f);
      return nullptr;
  }

  struct NDIVideoCtx {
    const Ndi::Loader* loader{};
    NDIlib_recv_instance_t recv{};
    NDIlib_video_frame_v2_t frame{};
  };
  auto ctx = (NDIVideoCtx*)av_malloc(sizeof(NDIVideoCtx));
  if (!ctx)
  {
    av_frame_free(&f);
    return nullptr;
  }
  ctx->loader = &loader;
  ctx->recv  = recv;
  ctx->frame = *ndi;

  size_t total;
  switch (ndi->FourCC) {
    case NDIlib_FourCC_video_type_I420:
    case NDIlib_FourCC_video_type_YV12:
      total = (size_t)s * h + 2 * ((s / 2) * (h / 2));
      break;
    case NDIlib_FourCC_video_type_NV12:
      total = (size_t)s * h + (size_t)s * (h / 2);
      break;
    case NDIlib_FourCC_video_type_P216:
      total = (size_t)s * h * 2;
      break;
    case NDIlib_FourCC_video_type_PA16:
      total = (size_t)s * h * 3;
      break;
    case NDIlib_FourCC_video_type_UYVA:
      total = (size_t)s * h * 2;
      break;
    default:
      total = (size_t)s * h;
      break;
  }

  AVBufferRef *buf = av_buffer_create(ndi->p_data, total,
                                      [](void *opaque, uint8_t *data)
  { // dtor
    auto* ctx = (NDIVideoCtx *)opaque;
    ctx->loader->recv_free_video(ctx->recv, &ctx->frame);
    av_free(ctx);
  }, ctx,  AV_BUFFER_FLAG_READONLY);
  if (!buf) { av_free(ctx); av_frame_free(&f); return nullptr; }

#define ADDREF(slot) \
  do { \
        f->buf[(slot)] = av_buffer_ref(buf); \
        if (!f->buf[(slot)]) goto oom; \
  } while (0)

  switch (ndi->FourCC) {

    case NDIlib_FourCC_video_type_UYVA:
      // FIXME no proper zero-copy alpha format in ffmpeg for this yet :(
    case NDIlib_FourCC_video_type_UYVY:

    case NDIlib_FourCC_video_type_BGRA:
    case NDIlib_FourCC_video_type_BGRX:
    case NDIlib_FourCC_video_type_RGBA:
    case NDIlib_FourCC_video_type_RGBX:
      f->buf[0]      = buf;  // transfer ownership
      f->data[0]     = ndi->p_data;
      f->linesize[0] = s;
      break;

    case NDIlib_FourCC_video_type_NV12:
      f->buf[0] = buf;
      ADDREF(1);
      f->data[0]     = ndi->p_data;
      f->data[1]     = ndi->p_data + (size_t)s * h;
      f->linesize[0] = s;
      f->linesize[1] = s;
      break;

    case NDIlib_FourCC_video_type_I420: {
      int cs = s / 2, ch = h / 2;
      f->buf[0] = buf;
      ADDREF(1); ADDREF(2);
      f->data[0]     = ndi->p_data;
      f->data[1]     = ndi->p_data + (size_t)s * h;
      f->data[2]     = ndi->p_data + (size_t)s * h + (size_t)cs * ch;
      f->linesize[0] = s;
      f->linesize[1] = cs;
      f->linesize[2] = cs;
      break;
    }

    case NDIlib_FourCC_video_type_YV12: {
      int cs = s / 2, ch = h / 2;
      uint8_t *cr = ndi->p_data + (size_t)s  * h;
      uint8_t *cb = cr           + (size_t)cs * ch;
      f->buf[0] = buf;
      ADDREF(1); ADDREF(2);
      f->data[0]     = ndi->p_data;
      f->data[1]     = cb;
      f->data[2]     = cr;
      f->linesize[0] = s;
      f->linesize[1] = cs;
      f->linesize[2] = cs;
      break;
    }

    case NDIlib_FourCC_video_type_P216:
      f->buf[0] = buf;
      ADDREF(1);
      f->data[0]     = ndi->p_data;
      f->data[1]     = ndi->p_data + (size_t)s * h;
      f->linesize[0] = s;
      f->linesize[1] = s;
      break;

    case NDIlib_FourCC_video_type_PA16:
      // FIXME ffmpeg does not have any compatible format yet.
      // Maybe we should sws_scale :'(
      f->buf[0] = buf;
      ADDREF(1);
      f->data[0]     = ndi->p_data;
      f->data[1]     = ndi->p_data + (size_t)s * h;
      f->linesize[0] = s;
      f->linesize[1] = s;
      break;

    default:
      goto oom;
  }

#undef ADDREF

  if (ndi->picture_aspect_ratio > 0.0f && w > 0 && h > 0)
  {
    f->sample_aspect_ratio = av_d2q(
        (double)ndi->picture_aspect_ratio * h / w, 1 << 20);
  }

  return f;
oom:
  av_buffer_unref(&buf);
  av_frame_free(&f);
  return nullptr;
}

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
};
W_OBJECT_IMPL(InputStream)

InputStream::InputStream(const Ndi::Loader& ndi) noexcept
    : m_ndi{ndi}
    , m_receiver{ndi}
{
  realTime = true;
  startTimer(1000);
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
  NDIlib_recv_create_v3_t info;
  info.allow_video_fields = false;
  info.bandwidth = NDIlib_recv_bandwidth_highest;
  info.color_format = NDIlib_recv_color_format_UYVY_RGBA;
  info.source_to_connect_to = source;

  m_receiver.create(info);

  pixel_format = AV_PIX_FMT_UYVY422;
  width = 0;
  height = 0;

  return true;
}

bool InputStream::start() noexcept
{
  if(m_running)
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

  if(m_thread.joinable())
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

void InputStream::timerEvent(QTimerEvent* t)
{
  if(m_receiver.has_ptz())
  {
    ptz_changed(true);
    killTimer(t->timerId());
  }
}

void InputStream::buffer_thread() noexcept
{
  while(m_running.load(std::memory_order_acquire))
  {
    if(auto f = read_frame_impl())
    {
      m_frames.enqueue(f);
      if(m_frames.size() > 2)
      {
        release_frame(m_frames.dequeue_one());
      }
    }
  }
}

static std::optional<AVPixelFormat> getPixelFormat(NDIlib_FourCC_video_type_e fourcc)
{
  switch(fourcc)
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
  ndi_frame.p_metadata = nullptr;
  switch(m_receiver.capture(&ndi_frame, nullptr, nullptr, 1000))
  {
    case NDIlib_frame_type_video: {
      if(auto format = getPixelFormat(ndi_frame.FourCC))
      {
        AVFrame* frame = m_frames.newFrame().release();

        if(auto res = ndi_video_to_avframe(this->m_ndi, this->m_receiver.impl, &ndi_frame, frame))
        {
          return res;
        }
        else
        {
          av_frame_free(&frame);
          this->m_ndi.recv_free_video(this->m_receiver.impl, &ndi_frame);
          return nullptr;
        }
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

InputDevice::~InputDevice() { }

void InputDevice::createPtz()
{
  if(m_dev && m_stream)
  {
    Ndi::Receiver& cam = m_stream->receiver();
    auto& root = m_dev->get_root_node();
    nodeCreated(ossia::net::create_node(root, "/ptz"));
    nodeCreated(ossia::net::create_node(root, "/ptz/preset"));
    nodeCreated(ossia::net::create_node(root, "/ptz/focus"));
    nodeCreated(ossia::net::create_node(root, "/ptz/wb"));
    nodeCreated(ossia::net::create_node(root, "/ptz/exposure"));

    {
      auto p = ossia::create_parameter(root, "/ptz/zoom", "float");
      p->add_callback(
          [&cam](const ossia::value& v) { cam.zoom(ossia::convert<float>(v)); });
      nodeCreated(p->get_node());
    }
    {
      auto p = ossia::create_parameter(root, "/ptz/pan", "float");
      p->add_callback(
          [&cam](const ossia::value& v) { cam.pan(ossia::convert<float>(v)); });
      nodeCreated(p->get_node());
    }
    {
      auto p = ossia::create_parameter(root, "/ptz/tilt", "float");
      p->add_callback(
          [&cam](const ossia::value& v) { cam.tilt(ossia::convert<float>(v)); });
      nodeCreated(p->get_node());
    }
    {
      auto p = ossia::create_parameter(root, "/ptz/pan/speed", "float");
      p->add_callback(
          [&cam](const ossia::value& v) { cam.pan_speed(ossia::convert<float>(v)); });
      nodeCreated(p->get_node());
    }
    {
      auto p = ossia::create_parameter(root, "/ptz/tilt/speed", "float");
      p->add_callback(
          [&cam](const ossia::value& v) { cam.tilt_speed(ossia::convert<float>(v)); });
      nodeCreated(p->get_node());
    }
    {
      auto p = ossia::create_parameter(root, "/ptz/preset/store", "int");
      p->add_callback(
          [&cam](const ossia::value& v) { cam.store_preset(ossia::convert<int>(v)); });
      nodeCreated(p->get_node());
    }
    {
      auto p = ossia::create_parameter(root, "/ptz/preset/recall", "int");
      p->add_callback(
          [&cam](const ossia::value& v) { cam.recall_preset(ossia::convert<int>(v)); });
      nodeCreated(p->get_node());
    }
    {
      auto p = ossia::create_parameter(root, "/ptz/focus/auto", "impulse");
      p->add_callback([&cam](const ossia::value& v) { cam.auto_focus(); });
      nodeCreated(p->get_node());
    }
    {
      auto p = ossia::create_parameter(root, "/ptz/focus/manual", "float");
      p->add_callback(
          [&cam](const ossia::value& v) { cam.focus(ossia::convert<float>(v)); });
      nodeCreated(p->get_node());
    }
    {
      auto p = ossia::create_parameter(root, "/ptz/focus/speed", "float");
      p->add_callback(
          [&cam](const ossia::value& v) { cam.focus_speed(ossia::convert<float>(v)); });
      nodeCreated(p->get_node());
    }
    {
      auto p = ossia::create_parameter(root, "/ptz/wb/auto", "impulse");
      p->add_callback([&cam](const ossia::value& v) { cam.white_balance_auto(); });
      nodeCreated(p->get_node());
    }
    {
      auto p = ossia::create_parameter(root, "/ptz/wb/indoor", "impulse");
      p->add_callback([&cam](const ossia::value& v) { cam.white_balance_indoor(); });
      nodeCreated(p->get_node());
    }
    {
      auto p = ossia::create_parameter(root, "/ptz/wb/outdoor", "impulse");
      p->add_callback([&cam](const ossia::value& v) { cam.white_balance_outdoor(); });
      nodeCreated(p->get_node());
    }
    {
      auto p = ossia::create_parameter(root, "/ptz/wb/oneshot", "impulse");
      p->add_callback([&cam](const ossia::value& v) { cam.white_balance_oneshot(); });
      nodeCreated(p->get_node());
    }
    {
      auto p = ossia::create_parameter(root, "/ptz/wb/manual", "rgb");
      p->add_callback([&cam](const ossia::value& v) {
        ossia::vec3f col = ossia::convert<ossia::vec3f>(v);
        cam.white_balance_manual(col[0], col[2]);
      });
      nodeCreated(p->get_node());
    }
    {
      auto p = ossia::create_parameter(root, "/ptz/exposure/auto", "impulse");
      p->add_callback([&cam](const ossia::value& v) { cam.exposure_auto(); });
      nodeCreated(p->get_node());
    }
    {
      auto p = ossia::create_parameter(root, "/ptz/exposure/manual", "float");
      p->add_callback([&cam](const ossia::value& v) {
        cam.exposure_manual(ossia::convert<float>(v));
      });
      nodeCreated(p->get_node());
    }
  }
}

void InputDevice::disconnect()
{
  m_stream.reset();
  GfxInputDevice::disconnect();

  auto prev = std::move(m_dev);
  m_dev = {};
  deviceChanged(prev.get(), nullptr);
}

bool InputDevice::reconnect()
{
  disconnect();

  try
  {
    auto& ndi = Loader::instance();
    if(!ndi.available())
      return false;

    auto set = this->settings().deviceSpecificSettings.value<Gfx::SharedInputSettings>();

    auto plug = m_ctx.findPlugin<Gfx::DocumentPlugin>();
    if(plug)
    {
      m_stream = std::make_shared<InputStream>(ndi);
      m_stream->load(set.path.toStdString());

      m_protocol = new Gfx::video_texture_input_protocol{m_stream, plug->exec};
      m_dev = std::make_unique<Gfx::video_texture_input_device>(
          std::unique_ptr<ossia::net::protocol_base>(m_protocol),
          this->settings().name.toStdString());
      if(m_stream->receiver().has_ptz())
        createPtz();
      else
        connect(
            m_stream.get(), &InputStream::ptz_changed, this, &InputDevice::createPtz);

      deviceChanged(nullptr, m_dev.get());
    }
  }
  catch(std::exception& e)
  {
    qDebug() << "Could not connect: " << e.what();
  }
  catch(...)
  {
    // TODO save the reason of the non-connection.
  }

  return connected();
}
}
