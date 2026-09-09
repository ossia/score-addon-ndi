// Drives Ndi::describeVideoFrame directly: no GPU, no NDI runtime, no score.
// What it pins is the description handed to the SDK -- FourCC, data pointer and
// line stride -- and the RGBA -> UYVY422 conversion behind it.
#include <Ndi/VideoFrameFormat.hpp>

#include <cstdio>
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

  // Opaque red: BT.601 limited range puts it at Y=81, U=90, V=240.
  std::vector<uint8_t> rgba(size_t(w) * h * 4);
  for(size_t i = 0; i < rgba.size(); i += 4)
  {
    rgba[i + 0] = 255;
    rgba[i + 1] = 0;
    rgba[i + 2] = 0;
    rgba[i + 3] = 255;
  }

  SwsContext* sws = sws_getContext(
      w, h, AV_PIX_FMT_RGBA, w, h, AV_PIX_FMT_UYVY422, 0, nullptr, nullptr, nullptr);
  CHECK(sws, "sws_getContext returned null");

  AVFrame* staging = av_frame_alloc();
  staging->format = AV_PIX_FMT_UYVY422;
  staging->width = w;
  staging->height = h;
  CHECK(av_frame_get_buffer(staging, 0) == 0, "av_frame_get_buffer failed");

  {
    NDIlib_video_frame_v2_t f{};
    const bool ok
        = Ndi::describeVideoFrame("RGBA", rgba.data(), w, h, sws, staging, f);
    CHECK(ok, "RGBA was refused");
    CHECK(f.FourCC == NDIlib_FourCC_video_type_RGBA, "RGBA: wrong FourCC");
    CHECK(f.line_stride_in_bytes == 4 * w, "RGBA: stride %d, expected %d",
          f.line_stride_in_bytes, 4 * w);
    CHECK(f.p_data == rgba.data(), "RGBA: must point at the readback, not a copy");
    CHECK(f.xres == w && f.yres == h, "RGBA: wrong dimensions");
    std::printf("  ok RGBA   stride=%d\n", f.line_stride_in_bytes);
  }

  {
    NDIlib_video_frame_v2_t f{};
    const bool ok
        = Ndi::describeVideoFrame("UYVY", rgba.data(), w, h, sws, staging, f);
    CHECK(ok, "UYVY was refused");
    CHECK(f.FourCC == NDIlib_FourCC_video_type_UYVY, "UYVY: wrong FourCC");
    CHECK(f.p_data == staging->data[0], "UYVY: must point at the staging frame");
    CHECK(f.line_stride_in_bytes >= 2 * w, "UYVY: stride %d below the packed minimum %d",
          f.line_stride_in_bytes, 2 * w);

    const uint8_t U = f.p_data[0], Y0 = f.p_data[1], V = f.p_data[2], Y1 = f.p_data[3];
    CHECK(Y0 > 70 && Y0 < 95, "UYVY: Y=%u is not red's luma", Y0);
    CHECK(V > 225, "UYVY: V=%u too low for red", V);
    CHECK(U > 80 && U < 100, "UYVY: U=%u out of range for red", U);
    CHECK(Y0 == Y1, "UYVY: flat input gave different luma samples, %u and %u", Y0, Y1);
    std::printf("  ok UYVY   stride=%d  U=%u Y=%u V=%u\n", f.line_stride_in_bytes, U, Y0, V);
  }

  // Anything else must be refused rather than described. The frame type defaults
  // its FourCC to UYVY, so a described-but-unfilled frame would send a null
  // pointer under a valid-looking format.
  for(const char* bad : {"BGRA", "NV12", "P216", "YV12", "I420", "UYVA", "", "uyvy"})
  {
    NDIlib_video_frame_v2_t f{};
    const bool ok = Ndi::describeVideoFrame(bad, rgba.data(), w, h, sws, staging, f);
    CHECK(!ok, "format '%s' was accepted; only RGBA and UYVY are written", bad);
  }
  std::printf("  ok refusal of the formats this addon does not write\n");

  av_frame_free(&staging);
  sws_freeContext(sws);
  std::printf("\nndi pixel format tests: %s (%d failure%s)\n",
              failures ? "FAILED" : "passed", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
