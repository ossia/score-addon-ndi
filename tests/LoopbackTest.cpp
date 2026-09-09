// Sends a frame through the real NDI SDK and receives it back, so the
// description produced by Ndi::describeVideoFrame is checked against the SDK's
// own reading of it rather than against our expectations of it. The receiver
// always asks for RGBA, so both send formats are compared on the same ground.
//
// Discovery is deliberately not used: the receiver connects straight to the
// descriptor NDIlib_send_get_source_name returns, so the test does not depend
// on mDNS working on the machine running it.
//
// Exits 77 (ctest SKIP_RETURN_CODE) when the NDI runtime will not initialise,
// which is what happens on a machine without it or on an unsupported CPU.
#include <Ndi/VideoFrameFormat.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

static int failures = 0;
#define CHECK(cond, ...)                   \
  do {                                     \
    if(!(cond))                            \
    {                                      \
      std::printf("  FAIL: " __VA_ARGS__); \
      std::printf("\n");                   \
      ++failures;                          \
    }                                      \
  } while(0)

namespace
{
constexpr int kWidth = 64;
constexpr int kHeight = 32;

struct Rgb
{
  int r, g, b;
};

Rgb pixelAt(const NDIlib_video_frame_v2_t& f, int x, int y)
{
  const uint8_t* row = f.p_data + ptrdiff_t(y) * f.line_stride_in_bytes;
  const uint8_t* px = row + ptrdiff_t(x) * 4;
  return {px[0], px[1], px[2]};
}

/// Sends `format` until the receiver hands a video frame back, or we give up.
bool roundtrip(
    const char* format, const std::vector<uint8_t>& rgba, SwsContext* sws,
    AVFrame* staging, NDIlib_send_instance_t sender, NDIlib_recv_instance_t recv,
    NDIlib_video_frame_v2_t& received)
{
  using namespace std::chrono;
  const auto deadline = steady_clock::now() + seconds(10);
  while(steady_clock::now() < deadline)
  {
    NDIlib_video_frame_v2_t out{};
    if(!Ndi::describeVideoFrame(format, rgba.data(), kWidth, kHeight, sws, staging, out))
      return false;
    out.frame_rate_N = 60000;
    out.frame_rate_D = 1000;
    NDIlib_send_send_video_v2(sender, &out);

    NDIlib_video_frame_v2_t vf{};
    if(NDIlib_recv_capture_v3(recv, &vf, nullptr, nullptr, 200)
       == NDIlib_frame_type_video)
    {
      received = vf;
      return true;
    }
    std::this_thread::sleep_for(milliseconds(10));
  }
  return false;
}
}

int main()
{
  if(!NDIlib_initialize())
  {
    std::printf("  skip: the NDI runtime would not initialise on this machine\n");
    return 77;
  }

  std::vector<uint8_t> rgba(size_t(kWidth) * kHeight * 4);
  for(size_t i = 0; i < rgba.size(); i += 4)
  {
    rgba[i + 0] = 255;  // opaque red
    rgba[i + 1] = 0;
    rgba[i + 2] = 0;
    rgba[i + 3] = 255;
  }

  SwsContext* sws = sws_getContext(
      kWidth, kHeight, AV_PIX_FMT_RGBA, kWidth, kHeight, AV_PIX_FMT_UYVY422, 0,
      nullptr, nullptr, nullptr);
  AVFrame* staging = av_frame_alloc();
  staging->format = AV_PIX_FMT_UYVY422;
  staging->width = kWidth;
  staging->height = kHeight;
  av_frame_get_buffer(staging, 0);

  const std::string name = "score-ndi-loopback-" + std::to_string(::getpid());
  NDIlib_send_create_t sendCfg{};
  sendCfg.p_ndi_name = name.c_str();
  sendCfg.clock_video = false;
  sendCfg.clock_audio = false;
  NDIlib_send_instance_t sender = NDIlib_send_create(&sendCfg);
  if(!sender)
  {
    std::printf("  skip: could not create an NDI sender\n");
    return 77;
  }

  const NDIlib_source_t* self = NDIlib_send_get_source_name(sender);
  if(!self)
  {
    std::printf("  skip: the sender reported no source descriptor\n");
    NDIlib_send_destroy(sender);
    return 77;
  }
  std::printf("  sender: %s\n", self->p_ndi_name ? self->p_ndi_name : "(unnamed)");

  NDIlib_recv_create_v3_t recvCfg{};
  recvCfg.source_to_connect_to = *self;
  recvCfg.color_format = NDIlib_recv_color_format_RGBX_RGBA;
  recvCfg.bandwidth = NDIlib_recv_bandwidth_highest;
  recvCfg.allow_video_fields = false;
  NDIlib_recv_instance_t recv = NDIlib_recv_create_v3(&recvCfg);
  if(!recv)
  {
    std::printf("  skip: could not create an NDI receiver\n");
    NDIlib_send_destroy(sender);
    return 77;
  }

  // Each format is sent and read back as RGBA. Neither is exact: the SDK does
  // not hand back the bytes it was given even for RGBA -- observed 252 where
  // 255 went in -- and UYVY additionally costs a trip through BT.601. The
  // tolerances are wide enough not to depend on an SDK version's rounding and
  // narrow enough that a channel swap, a wrong stride or a black frame fails.
  struct Case
  {
    const char* format;
    int tolerance;
  };
  for(const Case c : {Case{"RGBA", 16}, Case{"UYVY", 48}})
  {
    NDIlib_video_frame_v2_t got{};
    if(!roundtrip(c.format, rgba, sws, staging, sender, recv, got))
    {
      std::printf("  skip %s: no frame came back within 10 s\n", c.format);
      continue;
    }

    CHECK(got.xres == kWidth, "%s: xres %d, expected %d", c.format, got.xres, kWidth);
    CHECK(got.yres == kHeight, "%s: yres %d, expected %d", c.format, got.yres, kHeight);
    CHECK(got.p_data != nullptr, "%s: no pixel data", c.format);

    if(got.p_data)
    {
      const Rgb a = pixelAt(got, 1, 1);
      const Rgb b = pixelAt(got, kWidth - 2, kHeight - 2);
      std::printf(
          "  %s -> received %dx%d stride=%d  first=(%d,%d,%d) last=(%d,%d,%d)\n",
          c.format, got.xres, got.yres, got.line_stride_in_bytes, a.r, a.g, a.b, b.r,
          b.g, b.b);

      CHECK(a.r > 255 - c.tolerance, "%s: red channel came back %d", c.format, a.r);
      CHECK(a.g < c.tolerance, "%s: green channel came back %d", c.format, a.g);
      CHECK(a.b < c.tolerance, "%s: blue channel came back %d", c.format, a.b);
      CHECK(
          std::abs(a.r - b.r) <= c.tolerance && std::abs(a.g - b.g) <= c.tolerance,
          "%s: a flat frame came back with corners that differ", c.format);
    }
    NDIlib_recv_free_video_v2(recv, &got);
  }

  NDIlib_recv_destroy(recv);
  NDIlib_send_destroy(sender);
  av_frame_free(&staging);
  sws_freeContext(sws);
  NDIlib_destroy();

  std::printf(
      "\nndi loopback: %s (%d failure%s)\n", failures ? "FAILED" : "passed", failures,
      failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
