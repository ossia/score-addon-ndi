// Every FourCC the input accepts, and where its planes are.
//
// The send path had nine formats under test and the receive path had one:
// UYVY, because that is what the senders on hand happened to emit. The other
// ten were exercised only by whatever arrived, which is how UYVA came to
// report a buffer half again as large as the SDK's frame -- it is 4:2:2:4, so
// its alpha plane is half the size of its UYVY part, not equal to it.
//
// Pure, so the whole table is reachable without an SDK, a sender or a GPU.

#include <Ndi/ReceiveLayout.hpp>

#include <cstdio>
#include <string>

namespace
{
int g_fail = 0;
void check(bool ok, const std::string& what)
{
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if(!ok)
    ++g_fail;
}

constexpr int W = 1920, H = 1080;

// The frame's line_stride_in_bytes, which describes the primary plane.
constexpr int strideOf(int bytesPerPixel)
{
  return W * bytesPerPixel;
}
}

int main()
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  std::printf("\n== packed formats: one plane, one picture's worth ==\n");
  {
    struct Case
    {
      NDIlib_FourCC_video_type_e fourcc;
      const char* name;
      AVPixelFormat expect;
      int bpp;
    };
    const Case packed[] = {
        {NDIlib_FourCC_video_type_UYVY, "UYVY", AV_PIX_FMT_UYVY422, 2},
        {NDIlib_FourCC_video_type_BGRA, "BGRA", AV_PIX_FMT_BGRA, 4},
        {NDIlib_FourCC_video_type_BGRX, "BGRX", AV_PIX_FMT_BGR0, 4},
        {NDIlib_FourCC_video_type_RGBA, "RGBA", AV_PIX_FMT_RGBA, 4},
        {NDIlib_FourCC_video_type_RGBX, "RGBX", AV_PIX_FMT_RGB0, 4},
    };
    for(auto c : packed)
    {
      const int s = strideOf(c.bpp);
      const auto l = Ndi::receiveLayout(c.fourcc, s, H);
      check(l.supported && l.format == c.expect, std::string(c.name) + " maps to its pixel format");
      check(l.planeCount == 1 && l.offset[0] == 0 && l.stride[0] == s,
            std::string(c.name) + " is one plane at p_data");
      check(l.total == size_t(s) * H, std::string(c.name) + " occupies stride * height");
      check(l.native == Video::VideoPixelFormat::Unknown,
            std::string(c.name) + " needs no native override");
    }
  }

  // The one that was wrong. UYVY plus a FULL-resolution 8-bit alpha plane:
  // the alpha is W bytes a row, not the UYVY stride of 2*W.
  std::printf("\n== UYVA: 4:2:2:4, so the alpha plane is half the UYVY part ==\n");
  {
    const int s = strideOf(2);
    const auto l = Ndi::receiveLayout(NDIlib_FourCC_video_type_UYVA, s, H);
    check(l.supported && l.format == AV_PIX_FMT_UYVY422, "UYVA decodes as UYVY");
    check(l.planeCount == 2, "the alpha plane is carried, not dropped");
    check(l.native == Video::VideoPixelFormat::UYVA422A,
          "and named, since no AVPixelFormat describes this layout");
    check(l.offset[1] == size_t(s) * H && l.stride[1] == s / 2,
          "the alpha starts after the UYVY and is one byte per pixel");
    check(l.total == size_t(s) * H + size_t(s / 2) * H,
          "total counts both planes");
    check(l.total == size_t(s) * H * 3 / 2, "which is 1.5x the UYVY part, not 2x");
  }

  std::printf("\n== NV12: luma then interleaved chroma at half height ==\n");
  {
    const int s = strideOf(1);
    const auto l = Ndi::receiveLayout(NDIlib_FourCC_video_type_NV12, s, H);
    check(l.format == AV_PIX_FMT_NV12 && l.planeCount == 2, "two planes");
    check(l.offset[1] == size_t(s) * H, "chroma starts after the luma");
    check(l.stride[1] == s, "interleaved chroma keeps the full stride");
    check(l.total == size_t(s) * H * 3 / 2, "1.5 pictures' worth");
  }

  // I420 and YV12 differ ONLY here, and a swap is a picture with its reds and
  // blues exchanged -- obvious on screen, invisible in review.
  std::printf("\n== I420 and YV12: the same bytes, the chroma planes exchanged ==\n");
  {
    const int s = strideOf(1);
    const size_t luma = size_t(s) * H;
    const size_t plane = size_t(s / 2) * (H / 2);

    const auto i420 = Ndi::receiveLayout(NDIlib_FourCC_video_type_I420, s, H);
    const auto yv12 = Ndi::receiveLayout(NDIlib_FourCC_video_type_YV12, s, H);

    check(i420.format == AV_PIX_FMT_YUV420P && yv12.format == AV_PIX_FMT_YUV420P,
          "both decode as YUV420P");
    check(i420.planeCount == 3 && yv12.planeCount == 3, "three planes each");

    check(i420.offset[1] == luma, "I420: Cb is the first chroma plane");
    check(i420.offset[2] == luma + plane, "I420: Cr is the second");
    check(yv12.offset[1] == luma + plane, "YV12: Cb is the SECOND chroma plane");
    check(yv12.offset[2] == luma, "YV12: Cr is the first");

    check(i420.stride[1] == s / 2 && i420.stride[2] == s / 2,
          "chroma rows are half a luma row");
    check(i420.total == yv12.total && i420.total == luma + 2 * plane,
          "and they occupy the same memory");
  }

  std::printf("\n== the 16-bit pair ==\n");
  {
    const int s = strideOf(2);  // 16-bit luma
    const auto p216 = Ndi::receiveLayout(NDIlib_FourCC_video_type_P216, s, H);
    check(p216.format == AV_PIX_FMT_P216LE && p216.planeCount == 2, "P216: two planes");
    check(p216.offset[1] == size_t(s) * H && p216.stride[1] == s,
          "its chroma plane is the same size as its luma (4:2:2)");
    check(p216.total == 2 * size_t(s) * H, "two planes' worth");

    const auto pa16 = Ndi::receiveLayout(NDIlib_FourCC_video_type_PA16, s, H);
    check(pa16.planeCount == 3, "PA16 carries its alpha plane");
    check(pa16.native == Video::VideoPixelFormat::PA16,
          "and is named, since P216LE describes only its first two planes");
    check(pa16.offset[2] == 2 * size_t(s) * H && pa16.stride[2] == s,
          "the 16-bit alpha is the third plane, full size");
    check(pa16.total == 3 * size_t(s) * H, "three planes' worth");
  }

  std::printf("\n== what must be refused ==\n");
  {
    check(!Ndi::receiveLayout(NDIlib_FourCC_video_type_e(0x1234), 3840, H).supported,
          "an unknown FourCC has no layout");
    check(!Ndi::receiveLayout(NDIlib_FourCC_video_type_UYVY, 0, H).supported,
          "a zero stride has no layout");
    check(!Ndi::receiveLayout(NDIlib_FourCC_video_type_UYVY, 3840, 0).supported,
          "nor a zero height");
  }

  // An odd height halves to the wrong number of chroma rows if it is not
  // handled, and NDI can deliver one for a field.
  std::printf("\n== odd height ==\n");
  {
    const int s = strideOf(1);
    const auto l = Ndi::receiveLayout(NDIlib_FourCC_video_type_I420, s, 541);
    check(l.offset[1] == size_t(s) * 541, "the chroma still starts after the luma");
    check(l.offset[2] == l.offset[1] + size_t(s / 2) * 270,
          "and the second plane after 270 chroma rows, not 270.5");
  }

  std::printf(
      "\nndi receive layout: %s (%d failure%s)\n", g_fail ? "FAILED" : "passed", g_fail,
      g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
