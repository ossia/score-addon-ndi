// Drives Ndi::describeVideoFrame directly: no GPU, no NDI runtime, no score.
// What it pins is the description handed to the SDK -- FourCC, data pointer and
// line stride -- for bytes that are ALREADY in the wire format.
//
// There is no colour conversion to test any more: RGBA comes back from the
// scene as RGBA, and UYVY is produced by score::gfx::UYVYEncoder on the GPU
// (covered by Gfx/tests/EncoderTester.cpp). This function only describes those
// bytes, and the property that matters is that it describes them without
// copying: p_data must be the caller's pointer, every time.
#include <Ndi/VideoFrameFormat.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, ...)                     \
  do {                                       \
    if(!(cond))                              \
    {                                        \
      std::printf("  FAIL: " __VA_ARGS__);   \
      std::printf("\n");                     \
      ++failures;                            \
    }                                        \
  } while(0)

int main()
{
  const int w = 64, h = 32;

  // RGBA: one texel per pixel, four bytes each.
  {
    std::vector<uint8_t> bytes(size_t(w) * h * 4, 0xAB);
    NDIlib_video_frame_v2_t f{};
    const bool ok = Ndi::describeVideoFrame("RGBA", bytes.data(), w, h, 4 * w, f);
    CHECK(ok, "RGBA was refused");
    CHECK(f.FourCC == NDIlib_FourCC_video_type_RGBA, "RGBA: wrong FourCC");
    CHECK(f.line_stride_in_bytes == 4 * w, "RGBA: stride %d, expected %d",
          f.line_stride_in_bytes, 4 * w);
    CHECK(f.xres == w && f.yres == h, "RGBA: wrong dimensions");
    // Zero copy: the SDK reads the readback itself. What makes that safe is the
    // ReadbackPool's ownership states, not a copy here -- a copy on this path
    // would be pure latency.
    CHECK(f.p_data == bytes.data(), "RGBA: must be sent from the readback, not copied");
    std::printf("  ok RGBA   stride=%d  zero-copy\n", f.line_stride_in_bytes);
  }

  // UYVY: two pixels per four-byte macropixel, so a packed row is 2*w bytes.
  {
    std::vector<uint8_t> bytes(size_t(w) * h * 2, 0xCD);
    NDIlib_video_frame_v2_t f{};
    const bool ok = Ndi::describeVideoFrame("UYVY", bytes.data(), w, h, 2 * w, f);
    CHECK(ok, "UYVY was refused");
    CHECK(f.FourCC == NDIlib_FourCC_video_type_UYVY, "UYVY: wrong FourCC");
    CHECK(f.line_stride_in_bytes == 2 * w, "UYVY: stride %d, expected %d",
          f.line_stride_in_bytes, 2 * w);
    CHECK(f.xres == w && f.yres == h, "UYVY: wrong dimensions");
    CHECK(f.p_data == bytes.data(), "UYVY: must be sent from the readback, not copied");
    std::printf("  ok UYVY   stride=%d  zero-copy\n", f.line_stride_in_bytes);
  }

  // A padded readback: the backend may hand back rows wider than the packed
  // size, and the stride has to be reported as it is rather than recomputed.
  {
    const int padded = 4 * w + 64;
    std::vector<uint8_t> bytes(size_t(padded) * h, 0x11);
    NDIlib_video_frame_v2_t f{};
    const bool ok = Ndi::describeVideoFrame("RGBA", bytes.data(), w, h, padded, f);
    CHECK(ok, "a padded RGBA readback was refused");
    CHECK(f.line_stride_in_bytes == padded, "padded RGBA: stride %d, expected %d",
          f.line_stride_in_bytes, padded);
    std::printf("  ok RGBA   padded stride=%d honoured\n", f.line_stride_in_bytes);
  }

  // readbackStride derives the stride from what actually came back.
  {
    CHECK(Ndi::readbackStride(4 * w * h, h) == 4 * w, "readbackStride: packed RGBA");
    CHECK(Ndi::readbackStride(2 * w * h, h) == 2 * w, "readbackStride: packed UYVY");
    CHECK(Ndi::readbackStride(0, h) == 0, "readbackStride: empty readback must be 0");
    CHECK(Ndi::readbackStride(4 * w * h, 0) == 0, "readbackStride: zero height must be 0");
    std::printf("  ok readbackStride\n");
  }

  // Only the two formats the addon writes are accepted. NDIlib_video_frame_v2_t
  // defaults its FourCC to UYVY, so anything accepted by mistake would be sent
  // as a valid-looking UYVY frame.
  {
    std::vector<uint8_t> bytes(size_t(w) * h * 4, 0);
    for(const char* bad :
        {"", "uyvy", "rgba", "BGRA", "UYVA", "P216", "PA16", "YV12", "I420", "NV12",
         "RGBX", "BGRX"})
    {
      NDIlib_video_frame_v2_t f{};
      const bool ok = Ndi::describeVideoFrame(bad, bytes.data(), w, h, 4 * w, f);
      CHECK(!ok, "format '%s' was accepted; only RGBA and UYVY are written", bad);
      CHECK(
          f.p_data == nullptr,
          "format '%s' was refused but left a data pointer behind", bad);
    }
    std::printf("  ok refusal of the formats this addon does not write\n");
  }

  std::printf(
      "\nndi pixel format tests: %s (%d failure%s)\n", failures ? "FAILED" : "passed",
      failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
