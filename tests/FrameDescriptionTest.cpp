// Adversarial inputs to Ndi::describeVideoFrame. tests/PixelFormatTest.cpp
// covers what the function is meant to do; this covers what it does when it is
// handed something it does not expect.
//
// The bar is set by the function's own contract, quoted from
// VideoFrameFormat.hpp: it returns "false when the format is not one this addon
// writes, in which case the frame must not be sent: NDIlib_video_frame_v2_t
// defaults its FourCC to UYVY, so sending it would hand the SDK a null pointer
// labelled as a valid format." A description that comes back true has therefore
// promised the caller that p_data, line_stride_in_bytes, xres and yres describe
// a frame the SDK can read. Every check below asks whether that promise holds.
//
// Run under -fsanitize=address to have the last group actually trap; without it
// the overruns are silent.
#include <Ndi/VideoFrameFormat.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
struct Fixture
{
  int w, h;
  std::vector<uint8_t> rgba;
  SwsContext* sws{};
  AVFrame* uyvy{};

  Fixture(int w, int h)
      : w{w}
      , h{h}
      , rgba(size_t(w) * h * 4, 0x40)
  {
    sws = sws_getContext(
        w, h, AV_PIX_FMT_RGBA, w, h, AV_PIX_FMT_UYVY422, 0, nullptr, nullptr, nullptr);
    uyvy = av_frame_alloc();
    uyvy->format = AV_PIX_FMT_UYVY422;
    uyvy->width = w;
    uyvy->height = h;
    av_frame_get_buffer(uyvy, 0);
  }
  ~Fixture()
  {
    av_frame_free(&uyvy);
    if(sws)
      sws_freeContext(sws);
  }
};

/// How many bytes a consumer that believes the description will read.
long long describedSize(const NDIlib_video_frame_v2_t& f)
{
  return 1LL * f.yres * f.line_stride_in_bytes;
}

// ---------------------------------------------------------------------------
// 1. A null readback pointer.
//
// The RGBA path const_casts whatever pointer it is given straight into p_data
// and reports success. On the readback path that pointer comes from
// QRhiReadbackResult::data, which is empty until a readback has landed in it.
// ---------------------------------------------------------------------------
void testNullReadback()
{
  NDIlib_video_frame_v2_t f{};
  const bool ok = Ndi::describeVideoFrame("RGBA", nullptr, 64, 32, nullptr, nullptr, f);
  std::printf(
      "  RGBA with a null readback: returned %s, p_data=%p, FourCC=%.4s\n",
      ok ? "true" : "false", (void*)f.p_data, (const char*)&f.FourCC);
  CHECK(
      !(ok && f.p_data == nullptr),
      "describeVideoFrame described a frame with p_data == nullptr under a valid "
      "FourCC and returned true -- exactly what the return-false path exists to "
      "prevent");
}

// ---------------------------------------------------------------------------
// 2. A staging frame with no buffer behind it.
//
// OutputNode's constructor calls av_frame_get_buffer(f, 0) on each of the four
// staging frames and does not look at the result. When it fails -- OOM at 2160p
// is the realistic way -- staging->data[0] stays null, and this is what the
// sender then does with it, once per frame, forever.
// ---------------------------------------------------------------------------
void testUnallocatedStaging()
{
  Fixture fx{64, 32};
  AVFrame* bare = av_frame_alloc();
  bare->format = AV_PIX_FMT_UYVY422;
  bare->width = 64;
  bare->height = 32;
  // Deliberately no av_frame_get_buffer: this is the state the frame is left in
  // when that call fails and nobody checks.

  NDIlib_video_frame_v2_t f{};
  const bool ok
      = Ndi::describeVideoFrame("UYVY", fx.rgba.data(), 64, 32, fx.sws, bare, f);
  std::printf(
      "  UYVY into an unallocated staging frame: returned %s, p_data=%p, "
      "FourCC=%.4s, stride=%d\n",
      ok ? "true" : "false", (void*)f.p_data, (const char*)&f.FourCC,
      f.line_stride_in_bytes);
  CHECK(
      !(ok && f.p_data == nullptr),
      "an unallocated staging frame was described as a valid UYVY frame with "
      "p_data == nullptr; the UYVY branch checks that staging is non-null but not "
      "that it has any pixels");
  av_frame_free(&bare);
}

// ---------------------------------------------------------------------------
// 3. sws_scale's return value.
//
// The UYVY branch calls sws_scale and reports success without looking at what
// came back. When the height it is given does not fit the context's, swscale
// rejects the slice, writes nothing, and the caller is handed a description of a
// frame that was never converted -- with xres/yres from the readback and
// line_stride from a staging frame sized for something else.
// ---------------------------------------------------------------------------
void testSwsScaleFailureIgnored()
{
  Fixture fx{64, 64};  // context and staging are 64x64
  const int bigW = 128, bigH = 128;
  std::vector<uint8_t> big(size_t(bigW) * bigH * 4, 0x7f);

  NDIlib_video_frame_v2_t f{};
  const bool ok
      = Ndi::describeVideoFrame("UYVY", big.data(), bigW, bigH, fx.sws, fx.uyvy, f);

  const long long have = 1LL * fx.uyvy->linesize[0] * fx.uyvy->height;
  const long long described = describedSize(f);
  std::printf(
      "  UYVY 128x128 through a 64x64 context: returned %s, describes %lld bytes "
      "over a %lld byte staging frame\n",
      ok ? "true" : "false", described, have);
  CHECK(
      !ok || described <= have,
      "describeVideoFrame ignored sws_scale's failure and returned true; the "
      "description tells the SDK to read %lld bytes from a %lld byte buffer, an "
      "overrun of %lld bytes",
      described, have, described - have);

  if(ok && described > have)
  {
    // Do what the SDK does with the description it was given. Under
    // -fsanitize=address this is where it stops.
    std::printf("  reading the frame as described (ASan traps here if built with it)\n");
    std::fflush(stdout);
    volatile uint8_t sink = 0;
    for(int y = 0; y < f.yres; y++)
      sink ^= f.p_data[1LL * y * f.line_stride_in_bytes];
    (void)sink;
    std::printf("  ...no trap: this build has no ASan, the overrun was silent\n");
  }
}

// ---------------------------------------------------------------------------
// 4. Dimensions that cannot describe a frame.
//
// Nothing validates width or height. A readback that never landed leaves
// QRhiReadbackResult::pixelSize at 0x0, and the sender passes that straight in.
// ---------------------------------------------------------------------------
void testDegenerateDimensions()
{
  std::vector<uint8_t> rgba(64 * 64 * 4, 0x11);
  struct Case
  {
    int w, h;
    const char* why;
  };
  for(const Case c :
      {Case{0, 32, "a readback that never landed"}, Case{64, 0, "zero height"},
       Case{-64, 32, "negative width"}, Case{64, -32, "negative height"}})
  {
    NDIlib_video_frame_v2_t f{};
    const bool ok
        = Ndi::describeVideoFrame("RGBA", rgba.data(), c.w, c.h, nullptr, nullptr, f);
    std::printf(
        "  RGBA %dx%d (%s): returned %s, xres=%d yres=%d stride=%d\n", c.w, c.h, c.why,
        ok ? "true" : "false", f.xres, f.yres, f.line_stride_in_bytes);
    CHECK(
        !ok, "a %dx%d frame was described as sendable (stride %d)", c.w, c.h,
        f.line_stride_in_bytes);
  }
}

// ---------------------------------------------------------------------------
// 5. Odd widths on the UYVY path.
//
// UYVY packs two pixels per four-byte macropixel, so an odd xres cannot be
// expressed: the last pixel has no partner to share its chroma with. Nothing
// rejects it, and what the receiver makes of an odd xres with a UYVY FourCC is
// its own business.
// ---------------------------------------------------------------------------
void testOddWidthUyvy()
{
  for(const int w : {1, 3, 63, 65})
  {
    Fixture fx{w, 4};
    if(!fx.sws)
    {
      std::printf("  UYVY %dx4: no context, skipped\n", w);
      continue;
    }
    NDIlib_video_frame_v2_t f{};
    const bool ok
        = Ndi::describeVideoFrame("UYVY", fx.rgba.data(), w, 4, fx.sws, fx.uyvy, f);
    std::printf(
        "  UYVY %dx4: returned %s, xres=%d stride=%d (packed minimum %d)\n", w,
        ok ? "true" : "false", f.xres, f.line_stride_in_bytes, 2 * w);
    // Pin, not a demand: this is what it does today. If it ever starts rejecting
    // odd widths, or rounding xres, the change should be a deliberate one.
    CHECK(ok, "UYVY at width %d used to be accepted and now is not", w);
    CHECK(
        f.xres == w, "UYVY at width %d reported xres %d; it used to pass the width "
                     "through unchanged",
        w, f.xres);
    CHECK(
        f.line_stride_in_bytes >= 2 * w,
        "UYVY at width %d has stride %d, below the %d bytes a packed row needs", w,
        f.line_stride_in_bytes, 2 * w);
  }
}

// ---------------------------------------------------------------------------
// 6. A staging frame in the wrong pixel format.
//
// The doc comment requires a UYVY422 staging frame; nothing checks. This is what
// happens with the one the RGBA path is handed in the tests -- an RGBA frame.
// ---------------------------------------------------------------------------
void testWrongStagingFormat()
{
  Fixture fx{64, 32};
  AVFrame* rgbaStage = av_frame_alloc();
  rgbaStage->format = AV_PIX_FMT_RGBA;
  rgbaStage->width = 64;
  rgbaStage->height = 32;
  av_frame_get_buffer(rgbaStage, 0);

  NDIlib_video_frame_v2_t f{};
  const bool ok
      = Ndi::describeVideoFrame("UYVY", fx.rgba.data(), 64, 32, fx.sws, rgbaStage, f);
  const long long have = 1LL * rgbaStage->linesize[0] * rgbaStage->height;
  std::printf(
      "  UYVY into an RGBA staging frame: returned %s, stride=%d, describes %lld "
      "over %lld bytes\n",
      ok ? "true" : "false", f.line_stride_in_bytes, describedSize(f), have);
  CHECK(
      !ok || describedSize(f) <= have,
      "UYVY described into a staging frame of the wrong format overruns it by %lld "
      "bytes",
      describedSize(f) - have);
  av_frame_free(&rgbaStage);
}

// ---------------------------------------------------------------------------
// 7. The RGBA stride assumption.
//
// The RGBA branch hardcodes line_stride_in_bytes = 4 * width, so it is only
// right if the readback is tightly packed. Qt's RGBA8 readbacks are (byteSize is
// width*height*4 exactly, and glReadPixels' default 4-byte pack alignment never
// pads a 4-byte-per-pixel row), so this is a pin on the assumption rather than a
// complaint -- but it is an assumption with nothing checking it, and a padded
// readback would be described as packed.
// ---------------------------------------------------------------------------
void testRgbaStride()
{
  for(const int w : {1, 3, 17, 1920, 3840})
  {
    std::vector<uint8_t> rgba(size_t(w) * 4 * 2, 0x22);
    NDIlib_video_frame_v2_t f{};
    const bool ok
        = Ndi::describeVideoFrame("RGBA", rgba.data(), w, 2, nullptr, nullptr, f);
    CHECK(ok && f.line_stride_in_bytes == 4 * w, "RGBA width %d: stride %d, expected %d",
          w, f.line_stride_in_bytes, 4 * w);
    CHECK(f.p_data == rgba.data(), "RGBA width %d: p_data must be the readback", w);
  }
  std::printf("  ok RGBA stride is 4*width and p_data is the readback, all widths\n");
}
}

int main()
{
  std::printf("-- null and unbacked pointers --\n");
  testNullReadback();
  testUnallocatedStaging();

  std::printf("\n-- conversion failures --\n");
  testSwsScaleFailureIgnored();
  testWrongStagingFormat();

  std::printf("\n-- dimensions --\n");
  testDegenerateDimensions();
  testOddWidthUyvy();
  testRgbaStride();

  std::printf(
      "\nndi frame description: %s (%d failure%s)\n", failures ? "FAILED" : "passed",
      failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
