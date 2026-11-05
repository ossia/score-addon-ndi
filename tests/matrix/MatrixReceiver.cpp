// Receives one frame from an NDI source as RGBA and compares it with
// pattern.fs, pixel by pixel. Used by ndi-two-instance-matrix.sh on the
// receiving score instance's relay output, so what it measures is the whole
// chain: score sender -> wire format -> score receiver -> relay.
//
// Usage: MatrixReceiver <libndi.so> <source-substring> <seconds> [first-frame-timeout]
//
// A source is published as soon as its device exists, which can be well
// before the instance renders anything, so the capture window only opens on
// the first frame, waited for up to first-frame-timeout seconds (default 30).
// Prints one line:
//   RESULT fourcc=.. WxH fps=.. mean=.. worst=..@(x,y) marker=.. alphaL=.. alphaR=..
// or RESULT nosource / noframe / unsupported.
//
// mean and worst are max-channel |error| in 8-bit units over the opaque part
// of the picture (the alpha strip and a 2-pixel band around the white square
// are left out). alphaL/alphaR are the mean alpha at 5% and 95% of the width
// in the middle row of the strip: a carried ramp reads low then high, a
// dropped one reads 255 at both.
#include <cstddef>

#include <Processing.NDI.Lib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

namespace
{
void expected(int x, int y, int W, int H, double& r, double& g, double& b)
{
  const double nx = (x + 0.5) / W, ny = (y + 0.5) / H;
  r = nx;
  g = ny;
  b = 0.25;
  if(nx < 0.1 && ny < 0.1)
    r = g = b = 1.0;
}
}

int main(int argc, char** argv)
{
  if(argc < 4)
  {
    std::printf("usage: MatrixReceiver <libndi.so> <source-substring> <seconds>\n");
    return 2;
  }
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  void* h = dlopen(argv[1], RTLD_LOCAL | RTLD_LAZY);
  if(!h)
  {
    std::printf("RESULT nolib\n");
    return 2;
  }
  auto load = (const NDIlib_v5* (*)())dlsym(h, "NDIlib_v5_load");
  const NDIlib_v5* ndi = load ? load() : nullptr;
  if(!ndi || !ndi->initialize())
  {
    std::printf("RESULT nolib\n");
    return 2;
  }
  const std::string filter = argv[2];
  const double secs = std::atof(argv[3]);
  const double firstTimeout = argc > 4 ? std::atof(argv[4]) : 30.0;

  auto find = ndi->find_create_v2(nullptr);
  const NDIlib_source_t* src = nullptr;
  uint32_t n = 0;
  for(int i = 0; i < 40 && !src; i++)
  {
    ndi->find_wait_for_sources(find, 500);
    auto s = ndi->find_get_current_sources(find, &n);
    for(uint32_t k = 0; k < n; k++)
      if(std::string(s[k].p_ndi_name).find(filter) != std::string::npos)
      {
        src = &s[k];
        break;
      }
  }
  if(!src)
  {
    std::printf("RESULT nosource\n");
    return 1;
  }

  NDIlib_recv_create_v3_t rc{};
  rc.source_to_connect_to = *src;
  rc.color_format = NDIlib_recv_color_format_RGBX_RGBA;
  rc.bandwidth = NDIlib_recv_bandwidth_highest;
  rc.allow_video_fields = false;
  auto recv = ndi->recv_create_v3(&rc);

  NDIlib_video_frame_v2_t f{};
  int frames = 0;
  bool have = false;
  {
    const auto w0 = std::chrono::steady_clock::now();
    while(!have
          && std::chrono::steady_clock::now() - w0
                 < std::chrono::duration<double>(firstTimeout))
    {
      if(ndi->recv_capture_v2(recv, &f, nullptr, nullptr, 500)
         == NDIlib_frame_type_video)
        have = true;
    }
  }
  if(!have)
  {
    std::printf("RESULT noframe\n");
    return 1;
  }
  const auto t0 = std::chrono::steady_clock::now();
  while(std::chrono::steady_clock::now() - t0 < std::chrono::duration<double>(secs))
  {
    NDIlib_video_frame_v2_t v{};
    if(ndi->recv_capture_v2(recv, &v, nullptr, nullptr, 500) == NDIlib_frame_type_video)
    {
      if(have)
        ndi->recv_free_video_v2(recv, &f);
      f = v;
      have = true;
      frames++;
    }
  }
  if(!have)
  {
    std::printf("RESULT noframe\n");
    return 1;
  }

  char fcc[5]{};
  std::memcpy(fcc, &f.FourCC, 4);
  const bool rgba = f.FourCC == NDIlib_FourCC_video_type_RGBA
                    || f.FourCC == NDIlib_FourCC_video_type_RGBX;
  const bool bgra = f.FourCC == NDIlib_FourCC_video_type_BGRA
                    || f.FourCC == NDIlib_FourCC_video_type_BGRX;
  if(!rgba && !bgra)
  {
    std::printf("RESULT fourcc=%s unsupported\n", fcc);
    return 1;
  }

  const int W = f.xres, H = f.yres, S = f.line_stride_in_bytes;
  auto px = [&](int x, int y, double& r, double& g, double& b, double& a) {
    const uint8_t* p = f.p_data + size_t(y) * S + size_t(x) * 4;
    r = rgba ? p[0] : p[2];
    g = p[1];
    b = rgba ? p[2] : p[0];
    a = p[3];
  };

  const int stripTop = int(H * 0.9) - 2;
  const int mx = int(W * 0.1), my = int(H * 0.1);
  double sum = 0, worst = 0;
  int wx = 0, wy = 0, count = 0;
  for(int y = 0; y < stripTop; y++)
    for(int x = 0; x < W; x++)
    {
      if(std::abs(x - mx) <= 2 && y <= my + 2)
        continue;
      if(std::abs(y - my) <= 2 && x <= mx + 2)
        continue;
      double er, eg, eb, r, g, b, a;
      expected(x, y, W, H, er, eg, eb);
      px(x, y, r, g, b, a);
      const double e = std::max(
          {std::abs(r - er * 255), std::abs(g - eg * 255), std::abs(b - eb * 255)});
      sum += e;
      count++;
      if(e > worst)
      {
        worst = e;
        wx = x;
        wy = y;
      }
    }

  // The white square at the top left is the orientation marker: a flipped or
  // mirrored picture has it somewhere else.
  double mr, mg, mb, ma;
  px(mx / 2, my / 2, mr, mg, mb, ma);
  const bool marker = mr > 235 && mg > 235 && mb > 235;

  auto alphaAt = [&](double fx) {
    const int y = int(H * 0.95);
    const int x0 = int(W * fx);
    double s = 0;
    int c = 0;
    for(int x = std::max(0, x0 - 4); x <= std::min(W - 1, x0 + 4); x++)
    {
      double r, g, b, a;
      px(x, y, r, g, b, a);
      s += a;
      c++;
    }
    return s / c;
  };

  // The strip as composited: score renders every output over opaque black, so
  // the alpha ramp arrives as a brightness ramp, colour times alpha.
  double stripSum = 0;
  int stripCount = 0;
  for(int y = int(H * 0.9) + 2; y < H; y++)
    for(int x = 0; x < W; x++)
    {
      double er, eg, eb, r, g, b, a;
      expected(x, y, W, H, er, eg, eb);
      const double k = (x + 0.5) / W;
      px(x, y, r, g, b, a);
      stripSum += std::max(
          {std::abs(r - er * k * 255), std::abs(g - eg * k * 255),
           std::abs(b - eb * k * 255)});
      stripCount++;
    }

  std::printf(
      "RESULT fourcc=%s %dx%d fps=%.1f mean=%.2f worst=%.1f@(%d,%d) marker=%d "
      "alphaL=%.0f alphaR=%.0f strip=%.2f\n",
      fcc, W, H, frames / secs, sum / std::max(count, 1), worst, wx, wy, marker ? 1 : 0,
      alphaAt(0.05), alphaAt(0.95), stripSum / std::max(stripCount, 1));
  // MATRIX_SAVE=file.ppm keeps the frame that was measured, for a cell that
  // has to be looked at.
  if(const char* save = std::getenv("MATRIX_SAVE"))
  {
    if(FILE* out = std::fopen(save, "wb"))
    {
      std::fprintf(out, "P6\n%d %d\n255\n", W, H);
      for(int y = 0; y < H; y++)
        for(int x = 0; x < W; x++)
        {
          double r, g, b, a;
          px(x, y, r, g, b, a);
          const uint8_t c[3] = {uint8_t(r), uint8_t(g), uint8_t(b)};
          std::fwrite(c, 1, 3, out);
        }
      std::fclose(out);
    }
  }
  ndi->recv_free_video_v2(recv, &f);
  ndi->recv_destroy(recv);
  ndi->find_destroy(find);
  return 0;
}
