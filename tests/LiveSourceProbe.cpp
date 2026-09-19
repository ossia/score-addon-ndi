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
#include <utility>
#include <string>
#include <vector>

using namespace std::chrono;

namespace
{
const NDIlib_v5* g_ndi = nullptr;
bool g_bars = false;
std::string g_save;

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


// ------------------------------------------------------- SMPTE bar reference
//
// Decode a live frame through Ndi::receiveLayout and check the colour bars in
// it against the standard, rather than against anything this repository
// produced. The bar boundaries are found in the picture instead of assumed, so
// the check does not depend on the generator using seven equal columns.

// (Y, Cb, Cr) on an 8-bit scale at pixel (x, y), whatever the wire format.
bool sampleYuv(
    const NDIlib_video_frame_v2_t& f, const Ndi::ReceiveLayout& L, int x, int y,
    double& Y, double& U, double& V)
{
  if(f.FourCC == NDIlib_FourCC_video_type_UYVY)
  {
    const uint8_t* row = f.p_data + ptrdiff_t(y) * L.stride[0];
    const int pair = (x / 2) * 4;
    U = row[pair + 0];
    V = row[pair + 2];
    Y = row[pair + 1 + (x % 2) * 2];
    return true;
  }
  if(f.FourCC == NDIlib_FourCC_video_type_P216)
  {
    const auto* yp = reinterpret_cast<const uint16_t*>(
        f.p_data + L.offset[0] + ptrdiff_t(y) * L.stride[0]);
    const auto* uv = reinterpret_cast<const uint16_t*>(
        f.p_data + L.offset[1] + ptrdiff_t(y) * L.stride[1]);
    Y = yp[x] / 257.0;
    U = uv[(x / 2) * 2 + 0] / 257.0;
    V = uv[(x / 2) * 2 + 1] / 257.0;
    return true;
  }
  return false;
}

struct BarRef
{
  const char* name;
  double level;  // amplitude of the saturated components
};


// Write the decoded picture so it can be looked at rather than inferred.
void savePpm(const NDIlib_video_frame_v2_t& vf, const std::string& path)
{
  const auto L = Ndi::receiveLayout(vf.FourCC, vf.line_stride_in_bytes, vf.yres);
  double d;
  if(!L.supported || !vf.p_data || !sampleYuv(vf, L, 0, 0, d, d, d))
  {
    std::printf("    save: no sampler for %s\n", fourccName(vf.FourCC));
    return;
  }
  FILE* f = std::fopen(path.c_str(), "wb");
  if(!f)
    return;
  std::fprintf(f, "P6\n%d %d\n255\n", vf.xres, vf.yres);
  std::vector<uint8_t> row(size_t(vf.xres) * 3);
  for(int y = 0; y < vf.yres; y++)
  {
    for(int x = 0; x < vf.xres; x++)
    {
      double Y, U, V;
      sampleYuv(vf, L, x, y, Y, U, V);
      const auto c = yuvToRgb(Y, U, V, kMatrices[1].Kr, kMatrices[1].Kb);
      auto q = [](double v) {
        return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v + 0.5);
      };
      row[size_t(x) * 3 + 0] = q(c.r);
      row[size_t(x) * 3 + 1] = q(c.g);
      row[size_t(x) * 3 + 2] = q(c.b);
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  std::printf("    saved %dx%d to %s\n", vf.xres, vf.yres, path.c_str());
}

void reportBars(const NDIlib_video_frame_v2_t& vf)
{
  const auto L = Ndi::receiveLayout(vf.FourCC, vf.line_stride_in_bytes, vf.yres);
  double dummy;
  if(!L.supported || !vf.p_data
     || !sampleYuv(vf, L, 0, 0, dummy, dummy, dummy))
  {
    std::printf("    bars: no sampler for %s\n", fourccName(vf.FourCC));
    return;
  }

  // A quarter of the way down sits inside the tall bars of every SMPTE and EBU
  // layout, above the castellations and the pluge.
  const int y = vf.yres / 4;

  // Find the bars rather than assume them: walk the row and cut where the
  // sample moves sharply. Luma counts as well as chroma -- a SMPTE pattern has
  // two neutral columns of different brightness side by side, and on chroma
  // alone they merge into one segment whose centre is in the wrong one.
  std::vector<std::pair<int, int>> seg;
  {
    double pu = 0, pv = 0, py = 0, Y;
    int start = 0;
    for(int x = 0; x < vf.xres; x++)
    {
      double u, v;
      sampleYuv(vf, L, x, y, Y, u, v);
      if(x > 0
         && (std::abs(u - pu) + std::abs(v - pv) + std::abs(Y - py)) > 12.0)
      {
        if(x - start > vf.xres / 40)
          seg.emplace_back(start, x - 1);
        start = x;
      }
      pu = u;
      pv = v;
      py = Y;
    }
    if(vf.xres - start > vf.xres / 40)
      seg.emplace_back(start, vf.xres - 1);
  }
  std::printf("    bars: %zu segments across row %d\n", seg.size(), y);
  if(seg.size() < 7)
    return;

  // The seven top bars of a standard layout, in order. 100% is EBU; 75% is
  // SMPTE ECR 1-1978.
  const int pat[7][3] = {{1, 1, 1}, {1, 1, 0}, {0, 1, 1}, {0, 1, 0},
                         {1, 0, 1}, {1, 0, 0}, {0, 0, 1}};
  const char* barName[7]
      = {"white", "yellow", "cyan", "green", "magenta", "red", "blue"};
  const BarRef refs[] = {{"100%", 255.0}, {"75%", 191.0}};

  // Where the seven bars START among the segments. A generator may put a grey
  // field or a castellation either side of them, so slide a seven-wide window
  // and keep the alignment that fits best rather than assuming segment 0.
  auto scoreAt = [&](int m, double level, int first) {
    double err = 0;
    for(int i = 0; i < 7; i++)
    {
      const int x = (seg[first + i].first + seg[first + i].second) / 2;
      double Y, U, V;
      sampleYuv(vf, L, x, y, Y, U, V);
      const auto c = yuvToRgb(Y, U, V, kMatrices[m].Kr, kMatrices[m].Kb);
      err += std::abs(c.r - pat[i][0] * level) + std::abs(c.g - pat[i][1] * level)
             + std::abs(c.b - pat[i][2] * level);
    }
    return err / (7 * 3);
  };

  int barStart = 0;
  {
    double best = 1e9;
    for(int f = 0; f + 7 <= int(seg.size()); f++)
      for(auto r : refs)
        for(int m = 0; m < 3; m++)
          if(const double e = scoreAt(m, r.level, f); e < best)
          {
            best = e;
            barStart = f;
          }
  }
  if(barStart > 0)
    std::printf("      (the bars start at segment %d)\n", barStart);

  for(int m = 0; m < 3; m++)
  {
    double best = 1e9;
    const char* bestRef = "";
    for(auto r : refs)
      if(const double e = scoreAt(m, r.level, barStart); e < best)
      {
        best = e;
        bestRef = r.name;
      }
    std::printf(
        "      decoded as %-8s -> best fit %s bars, mean |RGB error| %.1f\n",
        kMatrices[m].name, bestRef, best);
  }

  // And the bars themselves, under the matrix the addon would pick.
  std::printf("      per bar, decoded as Rec.709 (limited):\n");
  for(int i = 0; i < 7; i++)
  {
    const auto& sg = seg[barStart + i];
    const int x = (sg.first + sg.second) / 2;
    double Y, U, V;
    sampleYuv(vf, L, x, y, Y, U, V);
    const auto c = yuvToRgb(Y, U, V, kMatrices[1].Kr, kMatrices[1].Kb);
    std::printf(
        "        %-8s x=%4d..%-4d  R=%6.1f G=%6.1f B=%6.1f\n", barName[i],
        sg.first, sg.second, c.r, c.g, c.b);
  }
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
  bool best = false, fields = false, fastest = false;
  for(int i = 1; i < argc; i++)
  {
    const std::string a = argv[i];
    if(a.rfind("--seconds=", 0) == 0)
      secs = std::stoi(a.substr(10));
    else if(a.rfind("--filter=", 0) == 0)
      filter = a.substr(9);
    else if(a == "--best")
      best = true;
    else if(a == "--fastest")
      fastest = true;
    else if(a == "--fields")
      fields = true;
    else if(a == "--bars")
      g_bars = true;
    else if(a.rfind("--save=", 0) == 0)
      g_save = a.substr(7);
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
      "receive mode: %s, fields %s\n\n",
      fastest ? "fastest (no conversion)"
      : best  ? "best (16-bit where offered)"
              : "8-bit",
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
    rc.color_format = fastest ? NDIlib_recv_color_format_fastest
                      : best    ? NDIlib_recv_color_format_best
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
        if(g_bars)
          reportBars(vf);
        if(!g_save.empty())
          savePpm(vf, g_save);

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
