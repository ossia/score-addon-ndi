// What real NDI senders on the network actually put on the wire.
//
// Every other test in this directory sends something we encoded ourselves and
// reads it back, so both ends share our assumptions. This one asks sources we
// do not control -- NDI Test Patterns, NDI Signal Generator, a camera -- and
// reports what they really do: their FourCC, their frame_format_type, the
// ndi_color_info metadata they declare, and, by measuring the pixels, the YUV
// matrix they ACTUALLY used.
//
// That last measurement is the point. SDR NDI signals no matrix, senders that
// do declare one can be wrong (NDI Test Patterns declares matrix="bt_709" on
// frames it encoded as BT.601), and the addon's colour policy is a bet about
// what is really out there. This re-runs that bet against live sources, per
// runtime, and prints the evidence.
//
// The runtime is dlopen'd so the same binary can ask v5 and v6 in turn.
//
// Usage: LiveSourceProbe <path/to/libndi.so.N> [--seconds=N] [--filter=substr]
//        [--best]   ask for 16-bit/best instead of 8-bit
//        [--fields] allow the source to deliver fields rather than frames

#include <Ndi/ColorInfo.hpp>
#include <Ndi/FrameFormat.hpp>
#include <Ndi/ReceiveLayout.hpp>
#include <Ndi/NdiColorSpace.hpp>

#include <Processing.NDI.Lib.h>
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace std::chrono;

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
  {
    std::printf("no NDIlib_v5_load in %s\n", path);
    return false;
  }
  g_ndi = load();
  if(!g_ndi || !g_ndi->initialize())
  {
    std::printf("NDI would not initialise from %s\n", path);
    return false;
  }
  std::printf("runtime: %s  (%s)\n", g_ndi->version ? g_ndi->version() : "?", path);
  return true;
}

const char* fourccName(NDIlib_FourCC_video_type_e f)
{
  switch(f)
  {
    case NDIlib_FourCC_video_type_UYVY: return "UYVY";
    case NDIlib_FourCC_video_type_UYVA: return "UYVA";
    case NDIlib_FourCC_video_type_P216: return "P216";
    case NDIlib_FourCC_video_type_PA16: return "PA16";
    case NDIlib_FourCC_video_type_YV12: return "YV12";
    case NDIlib_FourCC_video_type_I420: return "I420";
    case NDIlib_FourCC_video_type_NV12: return "NV12";
    case NDIlib_FourCC_video_type_BGRA: return "BGRA";
    case NDIlib_FourCC_video_type_BGRX: return "BGRX";
    case NDIlib_FourCC_video_type_RGBA: return "RGBA";
    case NDIlib_FourCC_video_type_RGBX: return "RGBX";
    default: return "(other)";
  }
}

const char* formatTypeName(NDIlib_frame_format_type_e t)
{
  switch(t)
  {
    case NDIlib_frame_format_type_progressive: return "progressive";
    case NDIlib_frame_format_type_interleaved: return "interleaved (woven)";
    case NDIlib_frame_format_type_field_0: return "field_0 (even lines)";
    case NDIlib_frame_format_type_field_1: return "field_1 (odd lines)";
    default: return "(unknown)";
  }
}

// ------------------------------------------------------- measuring the matrix
//
// Decode a YUV sample with each of the three standards and see which lands on
// a legal colour. Saturated bars are the discriminator: decoded with the wrong
// matrix they leave the 0..255 range, and how far out they go is a number.

struct Rgb
{
  double r, g, b;
};

// Limited-range YUV -> RGB, per standard. Kr/Kb define the matrix.
Rgb yuvToRgb(double Y, double U, double V, double Kr, double Kb)
{
  const double y = (Y - 16.0) / 219.0;
  const double u = (U - 128.0) / 224.0;
  const double v = (V - 128.0) / 224.0;
  const double Kg = 1.0 - Kr - Kb;
  const double r = y + 2.0 * (1.0 - Kr) * v;
  const double b = y + 2.0 * (1.0 - Kb) * u;
  const double g = (y - Kr * r - Kb * b) / Kg;
  return {r * 255.0, g * 255.0, b * 255.0};
}

struct Matrix
{
  const char* name;
  double Kr, Kb;
};
constexpr Matrix kMatrices[3] = {
    {"BT.601", 0.299, 0.114},
    {"Rec.709", 0.2126, 0.0722},
    {"Rec.2020", 0.2627, 0.0593},
};

// How far outside the legal 0..255 range a decode lands. A correct matrix on
// broadcast-legal bars gives ~0; a wrong one pushes the saturated primaries
// well out, and it is that asymmetry -- not a guess at the exact bar levels --
// that identifies the encoding.
double outOfGamut(Rgb c)
{
  auto excess = [](double v) {
    if(v < 0.0)
      return -v;
    if(v > 255.0)
      return v - 255.0;
    return 0.0;
  };
  return excess(c.r) + excess(c.g) + excess(c.b);
}

// One (Y, U, V) sample from a UYVY frame at pixel x, y.
void sampleUyvy(
    const NDIlib_video_frame_v2_t& f, int x, int y, double& Y, double& U, double& V)
{
  const uint8_t* row = f.p_data + ptrdiff_t(y) * f.line_stride_in_bytes;
  const int pair = (x / 2) * 4;
  U = row[pair + 0];
  V = row[pair + 2];
  Y = row[pair + 1 + (x % 2) * 2];
}

void reportMatrix(const NDIlib_video_frame_v2_t& vf)
{
  if(vf.FourCC != NDIlib_FourCC_video_type_UYVY)
  {
    std::printf(
        "    matrix measurement: needs UYVY, got %s -- skipped\n",
        fourccName(vf.FourCC));
    return;
  }

  // Sample a horizontal line across the picture. On a bar pattern this crosses
  // every bar; on anything else it is still a fair sample of the frame.
  const int y = vf.yres / 4;  // upper quarter: the bar row on SMPTE patterns
  double total[3] = {0, 0, 0};
  int n = 0;
  for(int i = 0; i < 64; i++)
  {
    const int x = std::min(vf.xres - 1, vf.xres * i / 64 + vf.xres / 128);
    double Y, U, V;
    sampleUyvy(vf, x, y, Y, U, V);
    for(int m = 0; m < 3; m++)
      total[m] += outOfGamut(yuvToRgb(Y, U, V, kMatrices[m].Kr, kMatrices[m].Kb));
    ++n;
  }

  int best = 0, worst = 0;
  for(int m = 1; m < 3; m++)
  {
    if(total[m] < total[best])
      best = m;
    if(total[m] > total[worst])
      worst = m;
  }

  std::printf("    out-of-gamut if decoded as:");
  for(int m = 0; m < 3; m++)
    std::printf("  %s=%.1f", kMatrices[m].name, total[m] / n);

  // The measurement only discriminates when the picture actually contains
  // saturated colour. A flat or desaturated pattern decodes in gamut under
  // every matrix, and the three scores come out equal -- reporting the
  // numerically smallest as "the answer" would then be inventing a result from
  // floating-point noise. Say inconclusive instead, and say why.
  const double spread = (total[worst] - total[best]) / n;
  const bool decisive = spread > 1.0;
  if(decisive)
    std::printf(
        "\n    -> pixels are most consistent with %s (spread %.1f)\n",
        kMatrices[best].name, spread);
  else
    std::printf(
        "\n    -> INCONCLUSIVE (spread %.2f): this picture has no colour "
        "saturated enough to separate the matrices\n",
        spread);

  // And what the addon's policy would pick for this frame, by each setting.
  const auto declared = Ndi::parseColorInfo(vf.p_metadata);
  const Ndi::ColorSpaceSetting autos[3] = {
      Ndi::ColorSpaceSetting::AutoHeuristic,
      Ndi::ColorSpaceSetting::AutoMetadata,
      Ndi::ColorSpaceSetting::AutoNdiRules,
  };
  std::printf("    addon would choose:");
  for(auto s : autos)
  {
    const auto got = Ndi::resolveYuvStandard(s, vf.xres, vf.yres, declared);
    // Only flag a disagreement when the measurement was decisive enough to
    // disagree with.
    const bool differs
        = decisive
          && std::string(Ndi::ndiYuvStandardName(got)) != kMatrices[best].name;
    std::printf(
        "  %s=%s%s", Ndi::colorSpaceSettingName(s), Ndi::ndiYuvStandardName(got),
        differs ? "(!)" : "");
  }
  std::printf("\n");
}
}

int main(int argc, char** argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  std::string path;
  std::string filter;
  // Connect straight to a source name, skipping discovery. Reconfiguring a
  // sender makes it re-register, and mDNS then takes its time -- a sweep that
  // waits for discovery after every change spends most of its life waiting and
  // still misses sources that are demonstrably sending.
  std::string connectTo;
  int secs = 4;
  bool best = false, fields = false;
  for(int i = 1; i < argc; i++)
  {
    const std::string a = argv[i];
    if(a.rfind("--seconds=", 0) == 0)
      secs = std::stoi(a.substr(10));
    else if(a.rfind("--filter=", 0) == 0)
      filter = a.substr(9);
    else if(a == "--best")
      best = true;
    else if(a == "--fields")
      fields = true;
    else if(a.rfind("--connect=", 0) == 0)
      connectTo = a.substr(10);
    else
      path = a;
  }
  if(path.empty())
  {
    std::printf("usage: LiveSourceProbe <libndi.so> [--seconds=N] [--filter=s]"
                " [--best] [--fields]\n");
    return 2;
  }
  if(!loadNdi(path.c_str()))
    return 77;

  std::printf(
      "receive mode: %s, fields %s\n\n", best ? "best (16-bit where offered)" : "8-bit",
      fields ? "allowed" : "not allowed");

  std::vector<NDIlib_source_t> keep;
  std::vector<std::string> names;
  NDIlib_find_instance_t finder = nullptr;

  {
    finder = g_ndi->find_create_v2(nullptr);
    if(!finder)
    {
      std::printf("could not create a finder\n");
      return 1;
    }
  }

  if(!connectTo.empty())
  {
    // Wait for ONE named source rather than for discovery to settle. A sender
    // that is reconfigured re-registers, and the network then takes its time:
    // waiting for the whole list to stop growing after every change spends
    // most of a sweep waiting and still misses senders that are demonstrably
    // on air. A name cannot be connected to without being resolved first --
    // p_url_address is null and recv_connect has nowhere to go -- so the
    // finder stays, it is just asked a narrower question.
    std::printf("waiting for \"%s\"...\n", connectTo.c_str());
    const auto deadline = steady_clock::now() + std::chrono::seconds(30);
    while(steady_clock::now() < deadline && keep.empty())
    {
      g_ndi->find_wait_for_sources(finder, 1000);
      uint32_t n = 0;
      const NDIlib_source_t* srcs = g_ndi->find_get_current_sources(finder, &n);
      for(uint32_t i = 0; i < n; i++)
      {
        const std::string nm = srcs[i].p_ndi_name ? srcs[i].p_ndi_name : "";
        if(nm == connectTo)
        {
          names.push_back(nm);
          keep.push_back(srcs[i]);
          break;
        }
      }
    }
    if(keep.empty())
      std::printf("  never appeared\n");
  }
  else
  {
    // mDNS on a busy network answers in its own time: a single 5 s wait found
    // one source on one run and two on the next, from the same two senders.
    // Keep asking until the count stops growing.
    std::printf("discovering...\n");
    uint32_t n = 0;
    const NDIlib_source_t* srcs = nullptr;
    for(int round = 0; round < 6; round++)
    {
      g_ndi->find_wait_for_sources(finder, 2000);
      uint32_t cur = 0;
      srcs = g_ndi->find_get_current_sources(finder, &cur);
      if(cur == n && round >= 2)
        break;
      n = cur;
    }
    std::printf("found %u source%s\n\n", n, n == 1 ? "" : "s");

    for(uint32_t i = 0; i < n; i++)
    {
      const std::string nm = srcs[i].p_ndi_name ? srcs[i].p_ndi_name : "";
      if(!filter.empty() && nm.find(filter) == std::string::npos)
        continue;
      names.push_back(nm);
      keep.push_back(srcs[i]);
    }
  }

  for(size_t i = 0; i < keep.size(); i++)
  {
    std::printf("== %s ==\n", names[i].c_str());

    NDIlib_recv_create_v3_t rc{};
    // The name string must outlive the connect call; keep[] owns it via the
    // finder, which is still alive.
    rc.source_to_connect_to = keep[i];
    rc.color_format = best ? NDIlib_recv_color_format_best
                           : NDIlib_recv_color_format_UYVY_RGBA;
    rc.bandwidth = NDIlib_recv_bandwidth_highest;
    rc.allow_video_fields = fields;
    auto* recv = g_ndi->recv_create_v3(&rc);
    if(!recv)
    {
      std::printf("    could not connect\n\n");
      continue;
    }

    const auto deadline = steady_clock::now() + std::chrono::seconds(secs);
    int frames = 0;
    int byType[4] = {0, 0, 0, 0};
    bool reported = false;
    while(steady_clock::now() < deadline)
    {
      NDIlib_video_frame_v2_t vf{};
      if(g_ndi->recv_capture_v3(recv, &vf, nullptr, nullptr, 500)
         != NDIlib_frame_type_video)
        continue;
      ++frames;
      if(vf.frame_format_type >= 0 && vf.frame_format_type < 4)
        ++byType[vf.frame_format_type];

      if(!reported)
      {
        reported = true;
        std::printf(
            "    %dx%d  %s  %s  %.3f fps  stride %d\n", vf.xres, vf.yres,
            fourccName(vf.FourCC), formatTypeName(vf.frame_format_type),
            vf.frame_rate_D ? double(vf.frame_rate_N) / vf.frame_rate_D : 0.0,
            vf.line_stride_in_bytes);
        std::printf(
            "    metadata: %s\n", vf.p_metadata ? vf.p_metadata : "(none)");
        const auto ci = Ndi::parseColorInfo(vf.p_metadata);
        std::printf(
            "    parsed: matrix=%s transfer=%s primaries=%s\n",
            ci.matrix ? Ndi::ndiYuvStandardName(*ci.matrix) : "-",
            ci.transfer ? "set" : "-", ci.primaries ? "set" : "-");
        reportMatrix(vf);

        // What the addon would make of this exact frame. Ndi::receiveLayout
        // and Ndi::decodeFrameFormat are unit-tested against a table; this is
        // the same pair asked about bytes a real sender produced.
        {
          const auto L = Ndi::receiveLayout(
              vf.FourCC, vf.line_stride_in_bytes, vf.yres);
          const auto ff = Ndi::decodeFrameFormat(vf.frame_format_type);
          std::printf(
              "    layout: %s, %d plane(s), total %zu B, strides",
              L.supported ? "supported" : "UNSUPPORTED", L.planeCount, L.total);
          for(int i = 0; i < L.planeCount; i++)
            std::printf(" %d", L.stride[i]);
          std::printf(
              " | interlacing %s%s\n",
              ff.interlacing == Video::Interlacing::Fields   ? "Fields"
              : ff.interlacing == Video::Interlacing::Woven  ? "Woven"
                                                             : "None",
              // Parity only means something for a half-height field.
              ff.interlacing != Video::Interlacing::Fields ? ""
              : ff.topField                                ? ", parity field_0"
                                                           : ", parity field_1");

          // The primary stride the SDK reports must be at least the tight row
          // the layout assumes, or every plane after the first is misplaced.
          if(L.supported && vf.line_stride_in_bytes < L.stride[0])
            std::printf("    !! stride %d is below the layout's %d\n",
                        vf.line_stride_in_bytes, L.stride[0]);

          // Read one sample from each plane at the offsets the layout gives.
          // A wrong chroma offset reads luma as chroma, which shows up as a
          // wildly off-neutral value on a test pattern.
          if(L.supported && L.planeCount >= 2 && vf.p_data)
          {
            const int mid = vf.yres / 2;
            if(vf.FourCC == NDIlib_FourCC_video_type_P216)
            {
              const auto* y = reinterpret_cast<const uint16_t*>(
                  vf.p_data + L.offset[0] + size_t(mid) * L.stride[0]);
              const auto* uv = reinterpret_cast<const uint16_t*>(
                  vf.p_data + L.offset[1] + size_t(mid) * L.stride[1]);
              std::printf(
                  "    mid row via layout: Y=%u Cb=%u Cr=%u (8-bit %u %u %u)\n",
                  y[0], uv[0], uv[1], y[0] / 257, uv[0] / 257, uv[1] / 257);
            }
          }
        }

      }
      g_ndi->recv_free_video_v2(recv, &vf);
    }

    std::printf(
        "    %d frames in %ds -- progressive %d, interleaved %d, field_0 %d,"
        " field_1 %d\n\n",
        frames, secs, byType[1], byType[0], byType[2], byType[3]);
    g_ndi->recv_destroy(recv);
  }

  if(finder)
    g_ndi->find_destroy(finder);
  return 0;
}
