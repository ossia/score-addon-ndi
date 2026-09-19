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
#include <string_view>
#include <algorithm>
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

/// Sends `format` until the receiver hands a video frame OF THIS FORMAT back.
///
/// The settle loop is not politeness, it is correctness. The send/receive
/// pipeline is several frames deep, so the first frame that arrives after a
/// format switch is still the PREVIOUS format's -- which means a loop that sends
/// one format and takes the next frame reports each case's label against the
/// case before it. Every colour assertion here was measuring the wrong frame
/// until this loop existed, and it did not show up as a failure because the
/// frame before was also red.
///
/// Nothing in an NDI frame identifies which send produced it, so the settle is
/// by time: keep sending this case and throw away everything that arrives for
/// long enough to flush what was queued before it.
bool roundtrip(
    const char* format, const std::vector<uint8_t>& wire, int strideBytes,
    NDIlib_send_instance_t sender, NDIlib_recv_instance_t recv,
    NDIlib_video_frame_v2_t& received)
{
  using namespace std::chrono;

  NDIlib_video_frame_v2_t out{};
  if(!Ndi::describeVideoFrame(format, wire.data(), kWidth, kHeight, strideBytes, out))
    return false;
  out.frame_rate_N = 60000;
  out.frame_rate_D = 1000;

  const auto settle = steady_clock::now() + milliseconds(1200);
  while(steady_clock::now() < settle)
  {
    NDIlib_send_send_video_v2(sender, &out);
    NDIlib_video_frame_v2_t stale{};
    while(NDIlib_recv_capture_v3(recv, &stale, nullptr, nullptr, 0)
          == NDIlib_frame_type_video)
      NDIlib_recv_free_video_v2(recv, &stale);
    std::this_thread::sleep_for(milliseconds(16));
  }

  const auto deadline = steady_clock::now() + seconds(10);
  while(steady_clock::now() < deadline)
  {
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

  // The GPU produces the wire bytes in production (UYVYEncoder for UYVY), so
  // this packs them the same way rather than converting on the CPU: there is no
  // swscale in this addon any more. BT.601 limited range puts opaque red at
  // Y=81, U=90, V=240, which is what the pixel-format test pins independently.
  //
  // Rec.709 limited range, which is what this addon now encodes at every size
  // and what the runtime decodes at every size. Measured against libndi 5.6.1
  // and 6.2.0.3: the receiver converts YUV to RGB with Rec.709 whatever the
  // resolution, SD included, so these bytes come back as pure red. The SDK
  // documentation's SD/HD/UHD table would have called for BT.601 at 64x32, and
  // packing that here returns (255, 24, 0) instead -- which is precisely the
  // fault this policy avoids. Ndi/NdiColorSpace.hpp carries the measurements.
  //
  // The tolerances stay wide because what this test is for is the round trip:
  // the FourCC, the stride and, for the planar formats, whether the SDK finds
  // the chroma planes where describeVideoFrame said they would be.
  std::vector<uint8_t> uyvy(size_t(kWidth) * kHeight * 2);
  for(size_t i = 0; i < uyvy.size(); i += 4)
  {
    uyvy[i + 0] = 102;  // U
    uyvy[i + 1] = 63;   // Y0
    uyvy[i + 2] = 240;  // V
    uyvy[i + 3] = 63;   // Y1
  }

  // The same red in every other format this addon sends, packed by hand for the
  // same reason: what is under test is whether the SDK reads a framestore
  // described by Ndi::describeVideoFrame the way we said it would -- the plane
  // offsets above all, which no amount of local checking can confirm. If our
  // idea of where the chroma plane starts were wrong, the picture would come
  // back with the wrong colour or the wrong geometry, and only a real receiver
  // can say so.
  const auto packWire = [&](std::string_view fmt) -> std::vector<uint8_t> {
    const size_t bytes = size_t(Ndi::packedRowBytes(fmt, kWidth))
                         * Ndi::framestoreRows(fmt, kHeight);
    std::vector<uint8_t> v(bytes, 0);
    const size_t luma = size_t(kWidth) * kHeight;

    if(fmt == "RGBA" || fmt == "RGBX")
      return rgba;
    if(fmt == "UYVY")
      return uyvy;
    if(fmt == "BGRA" || fmt == "BGRX")
    {
      for(size_t i = 0; i < v.size(); i += 4)
      {
        v[i + 0] = 0;    // B
        v[i + 1] = 0;    // G
        v[i + 2] = 255;  // R
        v[i + 3] = 255;
      }
      return v;
    }
    if(fmt == "P216")
    {
      // 16-bit: the 8-bit values scaled by 257, which is what the encoder
      // produces for an 8-bit source. Y plane, then interleaved Cb,Cr at full
      // height -- 4:2:2, so the chroma plane is as tall as the luma one.
      auto* w16 = reinterpret_cast<uint16_t*>(v.data());
      for(size_t i = 0; i < luma; i++)
        w16[i] = uint16_t(63 * 257);
      for(size_t i = 0; i < luma; i += 2)
      {
        w16[luma + i + 0] = uint16_t(102 * 257);
        w16[luma + i + 1] = uint16_t(240 * 257);
      }
      return v;
    }
    if(fmt == "NV12")
    {
      std::fill_n(v.begin(), luma, uint8_t(63));
      for(size_t i = luma; i + 1 < v.size(); i += 2)
      {
        v[i + 0] = 102;
        v[i + 1] = 240;
      }
      return v;
    }
    if(fmt == "I420" || fmt == "YV12")
    {
      // I420 is Y, U, V; YV12 is Y, V, U. Red's U and V are 150 apart, so
      // getting this backwards is not a subtle difference on the wire.
      const size_t chroma = luma / 4;
      std::fill_n(v.begin(), luma, uint8_t(63));
      const uint8_t first = (fmt == "I420") ? 102 : 240;
      const uint8_t second = (fmt == "I420") ? 240 : 102;
      std::fill_n(v.begin() + luma, chroma, first);
      std::fill_n(v.begin() + luma + chroma, chroma, second);
      return v;
    }
    return v;
  };

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
  std::printf("  runtime: %s\n", NDIlib_version());

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
  for(const Case c : {Case{"RGBA", 16}, Case{"RGBX", 16}, Case{"BGRA", 16},
                      Case{"BGRX", 16}, Case{"UYVY", 20}, Case{"P216", 20},
                      Case{"NV12", 20}, Case{"I420", 20}, Case{"YV12", 20}})
  {
    NDIlib_video_frame_v2_t got{};
    const std::vector<uint8_t> wire = packWire(c.format);
    const int stride = Ndi::packedRowBytes(c.format, kWidth);
    if(wire.empty() || stride <= 0)
    {
      CHECK(false, "%s: nothing to send", c.format);
      continue;
    }
    if(!roundtrip(c.format, wire, stride, sender, recv, got))
    {
      // Not a pass: a format this addon offers that the installed runtime will
      // not carry is exactly what this test exists to find. The runtime version
      // is printed at the top so it can be told from a network problem.
      CHECK(false, "%s: no frame came back within 10 s", c.format);
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
  NDIlib_destroy();

  std::printf(
      "\nndi loopback: %s (%d failure%s)\n", failures ? "FAILED" : "passed", failures,
      failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
