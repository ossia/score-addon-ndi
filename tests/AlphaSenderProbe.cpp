// Sends UYVA and PA16 -- the two NDI formats that carry alpha.
//
// Nothing available emits them: ndisink converts everything to UYVY, both Mac
// apps send UYVY, and score's own output has no alpha format. So the decoders
// for them would be exercised by synthetic frames only, which is how the dead
// `#ifdef AV_PIX_FMT_P216LE` survived for as long as it did. This puts real
// frames on a real wire instead.
//
// The picture is 75% SMPTE bars with an alpha ramp across it, so a receiver
// can check the colour and the alpha independently -- and a receiver that
// drops the alpha still sees correct bars, which is what makes the ramp the
// part worth looking at.
//
// Layouts, from Processing.NDI.structs.h. `stride` below is
// line_stride_in_bytes, which describes the first plane:
//
//   UYVA  UYVY 4:2:2 (stride = 2w) over h rows,
//         then 8-bit alpha (w bytes a row) over h rows.
//   PA16  16-bit luma (stride = 2w), then interleaved 16-bit CbCr at the same
//         size, then 16-bit alpha at the same size.
//
// Usage: AlphaSenderProbe <libndi.so> --format=UYVA|PA16 [--size=WxH]

// Processing.NDI.structs.h uses NULL without including anything that defines
// it; it compiles elsewhere only because other headers get there first.
#include <cstddef>

#include <Processing.NDI.Lib.h>
#include <dlfcn.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
const NDIlib_v5* g_ndi = nullptr;

bool loadNdi(const char* path)
{
  void* h = dlopen(path, RTLD_LOCAL | RTLD_LAZY);
  if(!h)
  {
    std::printf("dlopen(%s): %s\n", path, dlerror());
    return false;
  }
  const NDIlib_v5* (*load)(void) = nullptr;
  *((void**)&load) = dlsym(h, "NDIlib_v5_load");
  if(!load)
    return false;
  g_ndi = load();
  return g_ndi && g_ndi->initialize();
}

struct Yuv
{
  double y, u, v;
};

// Rec.709, limited range -- the same standard the addon's policy resolves to,
// so a receiver decoding with it should get the bars back exactly.
Yuv rgbToYuv709(double r, double g, double b)
{
  const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
  const double u = (b - y) / 1.8556;
  const double v = (r - y) / 1.5748;
  return {16.0 + y * 219.0 / 255.0, 128.0 + u * 224.0 / 255.0,
          128.0 + v * 224.0 / 255.0};
}

constexpr int kLevel = 191;
const int kBar[7][3] = {{1, 1, 1}, {1, 1, 0}, {0, 1, 1}, {0, 1, 0},
                        {1, 0, 1}, {1, 0, 0}, {0, 0, 1}};

Yuv barAt(int x, int w)
{
  const int b = (x * 7 / w) > 6 ? 6 : (x * 7 / w);
  return rgbToYuv709(kBar[b][0] * kLevel, kBar[b][1] * kLevel, kBar[b][2] * kLevel);
}

// Alpha ramps top to bottom, so a receiver can tell a carried alpha from an
// invented one: a dropped plane reads as a constant, not a ramp.
//
// Vertically rather than horizontally on purpose. The bars run left to right,
// so a horizontal ramp gives every bar a different alpha and makes the colour
// unreadable once anything composites it; down the picture, a single row has
// one alpha and the bars in it can still be checked against SMPTE.
uint8_t alphaAt(int y, int h)
{
  return uint8_t(y * 255 / (h - 1));
}
}

int main(int argc, char** argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::string path, format = "UYVA";
  int W = 1920, H = 1080;
  for(int i = 1; i < argc; i++)
  {
    const std::string a = argv[i];
    if(a.rfind("--format=", 0) == 0)
      format = a.substr(9);
    else if(a.rfind("--size=", 0) == 0)
    {
      const auto v = a.substr(7);
      const auto x = v.find('x');
      W = std::stoi(v.substr(0, x));
      H = std::stoi(v.substr(x + 1));
    }
    else
      path = a;
  }
  if(path.empty() || !loadNdi(path.c_str()))
  {
    std::printf("usage: AlphaSenderProbe <libndi.so> --format=UYVA|PA16\n");
    return 77;
  }

  const int stride = 2 * W;  // both formats describe a 2-bytes-per-pixel first plane
  std::vector<uint8_t> buf;
  NDIlib_FourCC_video_type_e fourcc{};

  if(format == "UYVA")
  {
    fourcc = NDIlib_FourCC_video_type_UYVA;
    buf.resize(size_t(stride) * H + size_t(W) * H);
    for(int y = 0; y < H; y++)
    {
      uint8_t* row = buf.data() + size_t(y) * stride;
      for(int x = 0; x < W; x += 2)
      {
        const auto a = barAt(x, W), b = barAt(x + 1, W);
        row[x * 2 + 0] = uint8_t((a.u + b.u) * 0.5 + 0.5);
        row[x * 2 + 1] = uint8_t(a.y + 0.5);
        row[x * 2 + 2] = uint8_t((a.v + b.v) * 0.5 + 0.5);
        row[x * 2 + 3] = uint8_t(b.y + 0.5);
      }
      uint8_t* ar = buf.data() + size_t(stride) * H + size_t(y) * W;
      std::memset(ar, alphaAt(y, H), W);
    }
  }
  else if(format == "PA16")
  {
    fourcc = NDIlib_FourCC_video_type_PA16;
    buf.resize(size_t(stride) * H * 3);
    auto* luma = reinterpret_cast<uint16_t*>(buf.data());
    auto* uv = reinterpret_cast<uint16_t*>(buf.data() + size_t(stride) * H);
    auto* al = reinterpret_cast<uint16_t*>(buf.data() + size_t(stride) * H * 2);
    for(int y = 0; y < H; y++)
      for(int x = 0; x < W; x++)
      {
        const auto c = barAt(x, W);
        luma[size_t(y) * W + x] = uint16_t(c.y * 257.0 + 0.5);
        if((x & 1) == 0)
        {
          const auto d = barAt(x + 1 < W ? x + 1 : x, W);
          uv[size_t(y) * W + x + 0] = uint16_t((c.u + d.u) * 0.5 * 257.0 + 0.5);
          uv[size_t(y) * W + x + 1] = uint16_t((c.v + d.v) * 0.5 * 257.0 + 0.5);
        }
        al[size_t(y) * W + x] = uint16_t(alphaAt(y, H) * 257);
      }
  }
  else
  {
    std::printf("unknown format %s\n", format.c_str());
    return 2;
  }

  NDIlib_send_create_t sc{};
  const std::string name = "alpha-" + format;
  sc.p_ndi_name = name.c_str();
  sc.clock_video = true;
  auto* send = g_ndi->send_create(&sc);
  if(!send)
    return 1;

  NDIlib_video_frame_v2_t f{};
  f.xres = W;
  f.yres = H;
  f.FourCC = fourcc;
  f.frame_rate_N = 30000;
  f.frame_rate_D = 1000;
  f.frame_format_type = NDIlib_frame_format_type_progressive;
  f.line_stride_in_bytes = stride;
  f.p_data = buf.data();

  std::printf(
      "serving %s as \"%s\" at %dx%d, %zu bytes -- ctrl-c to stop\n",
      format.c_str(), name.c_str(), W, H, buf.size());
  for(;;)
    g_ndi->send_send_video_v2(send, &f);
}
