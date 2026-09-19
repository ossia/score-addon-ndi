#include "InputNode.hpp"

#include <Ndi/InputStream.hpp>

#include <State/MessageListSerialization.hpp>
#include <State/Widgets/AddressFragmentLineEdit.hpp>

#include <Gfx/GfxApplicationPlugin.hpp>
#include <Gfx/GfxInputDevice.hpp>
#include <Gfx/Graph/VideoNode.hpp>
#include <Gfx/SharedInputSettings.hpp>

#include <ossia/detail/logger.hpp>
#include <Ndi/ColorInfo.hpp>
#include <Ndi/FrameFormat.hpp>
#include <Ndi/ReceiveLayout.hpp>
#include <Ndi/InputSettings.hpp>
#include <Ndi/NdiColorSpace.hpp>
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
//
// OWNERSHIP, on both success and failure:
//   - the AVFrame `f` belongs to the CALLER. This function never frees it.
//   - the NDI frame `ndi` belongs to THIS FUNCTION. On success its lifetime is
//     the AVBufferRef's, whose destructor calls recv_free_video; on failure it
//     is released here. Either way the caller must not release it.
//
AVFrame *ndi_video_to_avframe(const Ndi::Loader& loader,
                              NDIlib_recv_instance_t recv,
                              NDIlib_video_frame_v2_t *ndi,
                              AVFrame* f,
                              Ndi::ColorSpaceSetting colorSetting,
                              Video::Interlacing& interlacingOut)
{
  if (!f)
    return nullptr;

  int w = ndi->xres, h = ndi->yres;
  int s = ndi->line_stride_in_bytes;

  f->width  = w;
  f->height = h;
  f->pts    = ndi->timestamp;

  // Nothing in an NDI frame states the matrix, and a sender's declaration can
  // be false -- NDI Test Patterns sends BT.601 bars labelled matrix="bt_709" --
  // so the setting decides how much of it to believe. Transfer and primaries
  // are taken from the metadata when present: there is no other source.
  const auto declared = Ndi::parseColorInfo(ndi->p_metadata);
  f->colorspace = Ndi::avColorSpace(
      Ndi::resolveYuvStandard(colorSetting, w, h, declared));
  f->color_range = AVCOL_RANGE_MPEG;
  if(declared.transfer)
    f->color_trc = *declared.transfer;
  if(declared.primaries)
    f->color_primaries = *declared.primaries;

  // How this source delivers its fields. See Ndi/FrameFormat.hpp.
  const auto ff = Ndi::decodeFrameFormat(ndi->frame_format_type);
  if(!ff.accept)
  {
    loader.recv_free_video(recv, ndi);
    return nullptr;  // the caller owns f; we released the NDI frame
  }
  interlacingOut = ff.interlacing;

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 0, 0)
  if(ff.interlaced)
    f->flags |= AV_FRAME_FLAG_INTERLACED;
  if(ff.topField)
    f->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
#else
  f->interlaced_frame = ff.interlaced ? 1 : 0;
  f->top_field_first = ff.topField ? 1 : 0;
#endif

  const auto layout = Ndi::receiveLayout(ndi->FourCC, s, h);
  if(!layout.supported)
  {
    loader.recv_free_video(recv, ndi);
    return nullptr;  // the caller owns f; we released the NDI frame
  }
  f->format = layout.format;

  struct NDIVideoCtx {
    const Ndi::Loader* loader{};
    NDIlib_recv_instance_t recv{};
    NDIlib_video_frame_v2_t frame{};
  };
  auto ctx = (NDIVideoCtx*)av_malloc(sizeof(NDIVideoCtx));
  if (!ctx)
  {
    loader.recv_free_video(recv, ndi);
    return nullptr;  // the caller owns f; we released the NDI frame
  }
  ctx->loader = &loader;
  ctx->recv  = recv;
  ctx->frame = *ndi;

  const size_t total = layout.total;

  AVBufferRef *buf = av_buffer_create(ndi->p_data, total,
                                      [](void *opaque, uint8_t *data)
  { // dtor
    auto* ctx = (NDIVideoCtx *)opaque;
    ctx->loader->recv_free_video(ctx->recv, &ctx->frame);
    av_free(ctx);
  }, ctx,  AV_BUFFER_FLAG_READONLY);
  // ctx is not owned by anything yet, and nothing will free the NDI frame for
  // us, so do both here.
  if (!buf) { av_free(ctx); loader.recv_free_video(recv, ndi); return nullptr; }

  f->buf[0] = buf;  // transfer ownership
  f->data[0] = ndi->p_data + layout.offset[0];
  f->linesize[0] = layout.stride[0];
  for(int i = 1; i < layout.planeCount; i++)
  {
    f->buf[i] = av_buffer_ref(buf);
    if(!f->buf[i])
      goto oom;
    f->data[i] = ndi->p_data + layout.offset[i];
    f->linesize[i] = layout.stride[i];
  }

  if (ndi->picture_aspect_ratio > 0.0f && w > 0 && h > 0)
  {
    f->sample_aspect_ratio = av_d2q(
        (double)ndi->picture_aspect_ratio * h / w, 1 << 20);
  }

  return f;
oom:
  // Every path here has already done f->buf[0] = buf, so `buf` is the frame's
  // reference rather than a second one: the caller's av_frame_free releases it
  // and, through the buffer's destructor, the NDI frame too.
  return nullptr;
}

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
  info.bandwidth = NDIlib_recv_bandwidth_highest;
  info.source_to_connect_to = source;

  // 8-bit: ask for UYVY/RGBA and let the SDK de-interlace. Measured at about
  // 0.14 ms per frame, which is the cheapest correct handling of an interlaced
  // source there is.
  //
  // Best: 16-bit where the source has it -- and then the SDK ignores
  // allow_video_fields and delivers individual fields at twice the frame rate,
  // which is why this is a setting and not the default.
  bool wantsBest = m_receiveFormat == ReceiveFormat::Best;
  if(wantsBest && !m_ndi.supportsHDR())
  {
    // A v5 runtime answers a 16-bit request with a placeholder frame and no
    // error, so refuse rather than show one.
    ossia::logger().error(
        "NDI: '{}' asks for 16-bit but the loaded runtime is {}. Falling back to "
        "8-bit; install an NDI 6 runtime for 16-bit and HDR.",
        inputDevice, m_ndi.version());
    wantsBest = false;
  }

  info.color_format = wantsBest ? NDIlib_recv_color_format_best
                                : NDIlib_recv_color_format_UYVY_RGBA;
  info.allow_video_fields = wantsBest;

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

AVFrame* InputStream::read_frame_impl() noexcept
{
  NDIlib_video_frame_v2_t ndi_frame;
  ndi_frame.p_metadata = nullptr;
  switch(m_receiver.capture(&ndi_frame, nullptr, nullptr, 1000))
  {
    case NDIlib_frame_type_video: {
      // One table decides what is decodable: Ndi::receiveLayout. There used
      // to be a second here, and they had drifted -- this one dropped YV12,
      // UYVA and PA16 outright and named a different pixel format for P216.
      if(Ndi::receiveLayout(ndi_frame.FourCC, ndi_frame.line_stride_in_bytes,
                            ndi_frame.yres)
             .supported)
      {
        AVFrame* frame = m_frames.newFrame().release();

        auto interlacing = Video::Interlacing::None;
        if(auto res = ndi_video_to_avframe(
               this->m_ndi, this->m_receiver.impl, &ndi_frame, frame, m_colorSetting,
               interlacing))
        {
          // Publish what this frame turned out to be on the ImageFormat the GPU
          // decoder is built from. VideoNodeRenderer::checkFormat compares these
          // against the ones it last built with, so a source that changes colour
          // mid-stream -- switching HDR on, or a sender that starts declaring
          // something -- rebuilds the decoder instead of decoding with the old
          // matrix.
          this->color_space = static_cast<AVColorSpace>(res->colorspace);
          this->color_range = static_cast<AVColorRange>(res->color_range);
          this->color_trc = static_cast<AVColorTransferCharacteristic>(res->color_trc);
          this->color_primaries
              = static_cast<AVColorPrimaries>(res->color_primaries);

          // The picture is twice a field's height. Publishing the PICTURE size
          // here is what lets the renderer size its texture for the whole thing
          // while each frame fills half of it; videoDecoderNeedsRebuild knows
          // about the factor of two, so this does not look like a size change
          // on every frame.
          this->interlacing = interlacing;
          this->deinterlace = m_deinterlace;
          // UYVA and PA16 have no AVPixelFormat that describes their alpha
          // plane, so they name themselves here and the decoder factory reads
          // this before it reads pixel_format.
          this->native_format
              = Ndi::receiveLayout(
                    ndi_frame.FourCC, ndi_frame.line_stride_in_bytes, ndi_frame.yres)
                    .native;
          this->pixel_format = AVPixelFormat(res->format);
          this->width = res->width;
          this->height = (interlacing == Video::Interlacing::Fields)
                             ? res->height * 2
                             : res->height;
          if(ndi_frame.frame_rate_D > 0)
            this->fps = double(ndi_frame.frame_rate_N) / ndi_frame.frame_rate_D;
          return res;
        }
        else
        {
          // Only the AVFrame. ndi_video_to_avframe owns the NDI frame and has
          // already released it -- freeing it again here is a double free in
          // the SDK, and freeing `frame` used to be one in libavutil, because
          // the converter freed it too through its by-value parameter.
          av_frame_free(&frame);
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

    auto set = this->settings().deviceSpecificSettings.value<Ndi::InputSettings>();

    // Documents saved while the settings dialog was dropping the source name
    // have an empty path. The path row is hidden for NDI -- a device only ever
    // gets one from the source enumerator, which sets it to the source name,
    // which is also the device name -- so the name is the source that was meant.
    auto source = set.path;
    if(source.isEmpty())
      source = this->settings().name;
    if(source.isEmpty())
    {
      ossia::logger().error(
          "NDI: device '{}' names no source",
          this->settings().name.toStdString());
      return false;
    }

    auto plug = m_ctx.findPlugin<Gfx::DocumentPlugin>();
    if(plug)
    {
      m_stream = std::make_shared<InputStream>(ndi);
      m_stream->m_colorSetting = Ndi::colorSpaceSettingFromName(set.colorSpace);
      m_stream->m_receiveFormat = Ndi::receiveFormatFromName(set.receiveFormat);
      m_stream->m_deinterlace = Ndi::deinterlaceFromName(set.deinterlace);
      m_stream->load(source.toStdString());

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
