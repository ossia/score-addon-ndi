// What each NDI frame_format_type turns into.
//
// This exists because of one line. NDIlib_frame_format_type_interleaved used
// to fall into a `default:` in ndi_video_to_avframe that freed the AVFrame and
// returned nullptr, so on libavutil >= 58 -- every FFmpeg from 6.0 -- an
// interleaved source produced no picture at all. Nothing logged, nothing
// crashed, no frame arrived.
//
// It was reachable in production: the receiver sets allow_video_fields to true
// whenever the input asks for the 16-bit "Best" format, and a source that had
// been pre-weaving for us then starts sending interleaved. NDI Signal
// Generator in Interlaced mode does exactly that -- measured at 100 frames out
// of 100 -- so choosing 16-bit on an interlaced source turned the picture
// black.
//
// The decision is pure now, so the case that used to be unreachable from a
// test is four lines away from one.

#include <Ndi/FrameFormat.hpp>

#include <cstdio>
#include <initializer_list>

namespace
{
int g_fail = 0;
void check(bool ok, const char* what)
{
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if(!ok)
    ++g_fail;
}
}

int main()
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  using I = Video::Interlacing;

  std::printf("\n== every frame_format_type produces a picture ==\n");
  {
    // The bug, stated as a test: no NDI frame format may be dropped.
    for(auto t : {NDIlib_frame_format_type_progressive,
                  NDIlib_frame_format_type_interleaved,
                  NDIlib_frame_format_type_field_0,
                  NDIlib_frame_format_type_field_1})
    {
      check(Ndi::decodeFrameFormat(t).accept, "this format is accepted");
    }
    check(
        Ndi::decodeFrameFormat(NDIlib_frame_format_type_e(999)).accept,
        "and so is an unknown one -- its pixels are still good");
  }

  std::printf("\n== progressive ==\n");
  {
    constexpr auto d = Ndi::decodeFrameFormat(NDIlib_frame_format_type_progressive);
    check(d.interlacing == I::None, "nothing for the GPU to resolve");
    check(!d.interlaced, "not flagged interlaced");
  }

  std::printf("\n== interleaved: a frame, not a pair of fields ==\n");
  {
    constexpr auto d = Ndi::decodeFrameFormat(NDIlib_frame_format_type_interleaved);
    check(d.accept, "ACCEPTED -- this is the case that used to be dropped");
    check(
        d.interlacing == I::Woven,
        "Woven: both fields are already in the picture, run the identity");
    check(d.interlaced, "still flagged interlaced, so a deinterlacer may act");
    check(!d.topField, "field parity is meaningless for a woven frame");
  }

  std::printf("\n== the two half-height fields ==\n");
  {
    constexpr auto f0 = Ndi::decodeFrameFormat(NDIlib_frame_format_type_field_0);
    constexpr auto f1 = Ndi::decodeFrameFormat(NDIlib_frame_format_type_field_1);
    check(f0.interlacing == I::Fields && f1.interlacing == I::Fields,
          "both need the GPU to resolve a stacked texture");
    check(f0.interlaced && f1.interlaced, "both flagged interlaced");
    // The parity is what GPUVideoDecoder::planeRows reads to decide which half
    // of the texture a field fills. Swapping these two interlaces the picture
    // against itself.
    check(f0.topField, "field_0 (even lines) marks the top half");
    check(!f1.topField, "field_1 (odd lines) does not");
  }

  std::printf(
      "\nndi frame format: %s (%d failure%s)\n", g_fail ? "FAILED" : "passed",
      g_fail, g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
