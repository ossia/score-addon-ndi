// Adversarial cover for Ndi::describeVideoFrame: every way of asking it to
// describe something it cannot describe truthfully.
//
// This matters more than it looks. NDIlib_video_frame_v2_t defaults its FourCC
// to UYVY, so a frame that was never filled in does not look invalid to the
// SDK -- it looks like a UYVY frame with a null data pointer. Anything this
// function accepts, the SDK will read. Returning true for a description that
// does not match the memory behind it is how you get the SDK reading past the
// end of a buffer, which is what an earlier version of this code did.
#include <Ndi/VideoFrameFormat.hpp>

#include <cstdio>
#include <cstring>
#include <tuple>
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

/// Every refusal must also leave the frame unusable rather than plausible.
static void checkRefused(const char* what, const NDIlib_video_frame_v2_t& f, bool ok)
{
  CHECK(!ok, "%s was accepted", what);
  CHECK(f.p_data == nullptr, "%s was refused but left a data pointer behind", what);
}

int main()
{
  const int w = 64, h = 32;
  std::vector<uint8_t> rgba(size_t(w) * h * 4, 0x7F);
  std::vector<uint8_t> uyvy(size_t(w) * h * 2, 0x7F);

  std::printf("-- null and degenerate inputs --\n");
  {
    NDIlib_video_frame_v2_t f{};
    checkRefused(
        "a null data pointer", f,
        Ndi::describeVideoFrame("RGBA", nullptr, w, h, 4 * w, f));
  }
  for(const auto [ww, hh, label] :
      {std::tuple{0, h, "width 0"}, std::tuple{w, 0, "height 0"},
       std::tuple{-64, h, "negative width"}, std::tuple{w, -32, "negative height"}})
  {
    NDIlib_video_frame_v2_t f{};
    checkRefused(label, f, Ndi::describeVideoFrame("RGBA", rgba.data(), ww, hh, 4 * w, f));
  }
  {
    NDIlib_video_frame_v2_t f{};
    checkRefused(
        "stride 0", f, Ndi::describeVideoFrame("RGBA", rgba.data(), w, h, 0, f));
    NDIlib_video_frame_v2_t g{};
    checkRefused(
        "negative stride", g,
        Ndi::describeVideoFrame("RGBA", rgba.data(), w, h, -256, g));
  }
  std::printf("  ok\n");

  // A stride below the packed row size means the description claims a row is
  // narrower than the pixels it says are in it: the SDK would read each row
  // short and walk into the next one. Refusing is the only truthful answer.
  std::printf("-- a stride too small for the picture --\n");
  {
    NDIlib_video_frame_v2_t f{};
    checkRefused(
        "RGBA with a stride below 4*width", f,
        Ndi::describeVideoFrame("RGBA", rgba.data(), w, h, 4 * w - 4, f));
    NDIlib_video_frame_v2_t g{};
    checkRefused(
        "UYVY with a stride below 2*width", g,
        Ndi::describeVideoFrame("UYVY", uyvy.data(), w, h, 2 * w - 2, g));
    // Exactly packed is fine, and is the boundary either side of the above.
    NDIlib_video_frame_v2_t ok1{}, ok2{};
    CHECK(
        Ndi::describeVideoFrame("RGBA", rgba.data(), w, h, 4 * w, ok1),
        "RGBA at exactly the packed stride was refused");
    CHECK(
        Ndi::describeVideoFrame("UYVY", uyvy.data(), w, h, 2 * w, ok2),
        "UYVY at exactly the packed stride was refused");
  }
  std::printf("  ok\n");

  // UYVY carries two pixels in every four-byte macropixel, so an odd width has
  // no representation. The GPU encoder refuses to be built for one; this is the
  // same rule on the description side.
  std::printf("-- odd widths in UYVY --\n");
  for(const int odd : {1, 3, 63, 65})
  {
    NDIlib_video_frame_v2_t f{};
    char label[64];
    std::snprintf(label, sizeof label, "UYVY at width %d", odd);
    checkRefused(
        label, f, Ndi::describeVideoFrame("UYVY", uyvy.data(), odd, h, 2 * w, f));
  }
  // RGBA has no such constraint: one texel per pixel.
  {
    NDIlib_video_frame_v2_t f{};
    CHECK(
        Ndi::describeVideoFrame("RGBA", rgba.data(), 63, h, 4 * 64, f),
        "RGBA at an odd width was refused; only UYVY has a macropixel constraint");
  }
  std::printf("  ok\n");

  std::printf("-- formats this addon does not write --\n");
  {
    for(const char* bad : {"", " ", "uyvy", "rgba", "UYVA", "P216", "NV12", "I420"})
    {
      NDIlib_video_frame_v2_t f{};
      checkRefused(bad, f, Ndi::describeVideoFrame(bad, rgba.data(), w, h, 4 * w, f));
    }
  }
  std::printf("  ok\n");

  std::printf(
      "\nndi frame description: %s (%d failure%s)\n", failures ? "FAILED" : "passed",
      failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
