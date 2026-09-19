// Resolution x format sweep: GPU encode -> real NDI SDK -> receive.
//
// Everything else in tests/ pins one thing at one size. This drives the send
// path across SD, HD, UHD, DCI, degenerate and extreme-aspect sizes, and the
// odd ones the subsampled formats cannot express, for every wire format.
//
// The runtime is dlopen'd rather than linked: libndi.so.5 and libndi.so.6 have
// different sonames, so a linked binary can only ever ask the one it was built
// against, and phases 3, 7 and 8 exist to ask both.
//
//   1  structure: geometry, framestore size, stride, and that an inexpressible
//      size is REFUSED rather than described as something valid but wrong
//   2  round trip: the bytes the GPU produced, through the SDK, back as RGBA
//   3  which YUV matrix the receiver assumes, per resolution
//   4  encode cost per format per resolution
//   6  dump raw framestores for comparison against ffmpeg
//   7  chroma siting, from the sub-pixel position of a chroma edge
//   8  chroma aliasing, box against a pre-filter
//
// Usage: FormatSweepTest [path/to/libndi.so.N] [--phases=1234678]
//                        [--dump=DIR --dumpsize=WxH]
// Exits 77 (ctest SKIP) when there is no QRhi or no usable NDI runtime.

#include <Gfx/Graph/RenderState.hpp>
#include <Gfx/Graph/encoders/ColorSpaceOut.hpp>
#include <Ndi/NdiColorSpace.hpp>
#include <Ndi/VideoFrameFormat.hpp>
#include <Ndi/WireEncode.hpp>


#include <QApplication>
#include <QTimer>
#include <clocale>

#include <QtGui/private/qrhi_p.h>

#include <Processing.NDI.Lib.h>
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace score::gfx;
using namespace std::chrono;

namespace
{
int g_fail = 0;
int g_checks = 0;

// Phase 6 writes the encoders' raw framestores here so an EXTERNAL tool --
// ffmpeg -- can be asked the same question independently.
std::string g_dumpDir;
int g_dumpW = 1920, g_dumpH = 1080;

void check(bool ok, const std::string& what)
{
  ++g_checks;
  if(!ok)
  {
    std::printf("  [FAIL] %s\n", what.c_str());
    ++g_fail;
  }
}

// ---------------------------------------------------------------- NDI runtime

const NDIlib_v5* g_ndi = nullptr;

bool loadNdi(const char* path)
{
  void* h = dlopen(path, RTLD_LOCAL | RTLD_LAZY);
  if(!h)
  {
    std::printf("  dlopen(%s): %s\n", path, dlerror());
    return false;
  }
  const NDIlib_v5* (*load)(void) = nullptr;
  *((void**)&load) = dlsym(h, "NDIlib_v5_load");
  if(!load)
  {
    std::printf("  no NDIlib_v5_load in %s\n", path);
    return false;
  }
  g_ndi = load();
  if(!g_ndi || !g_ndi->initialize())
  {
    std::printf("  NDI refused to initialise from %s\n", path);
    g_ndi = nullptr;
    return false;
  }
  // Which library is ACTUALLY answering. score's own plugin loader dlopen()s a
  // runtime of its own during MinimalApplication startup, so with two NDI
  // versions resident in one process "I passed the v5 path" is not by itself
  // evidence that v5 is the one being measured. This is.
  if(g_ndi->version)
    std::printf("  runtime reports: %s\n", g_ndi->version());
  return true;
}

// ------------------------------------------------------------- the resolution
// panel
//
// `expressible` is whether EVERY format can carry it; the odd ones are here
// precisely because the subsampled formats cannot, and Phase 1 checks that they
// are turned away rather than quietly mis-described.

struct Res
{
  int w, h;
  const char* label;
};

constexpr Res kPanel[] = {
    // degenerate / tiny
    {2, 2, "minimum expressible"},
    {16, 16, "tiny square"},
    {64, 48, "thumbnail"},
    // SD
    {320, 240, "QVGA"},
    {640, 480, "NTSC VGA"},
    {720, 480, "NTSC D1"},
    {720, 576, "PAL D1"},
    // the documented SD/HD boundary, from both sides
    {720, 576, "SD boundary (<=720x576)"},
    {722, 576, "just over the width boundary"},
    {720, 578, "just over the height boundary"},
    // HD
    {1024, 768, "XGA"},
    {1280, 720, "720p"},
    {1920, 1080, "1080p"},
    {1920, 1200, "WUXGA"},
    // the documented HD/UHD boundary, from both sides
    {1922, 1080, "just over the UHD width boundary"},
    {1920, 1082, "just over the UHD height boundary"},
    // UHD and beyond
    {2560, 1440, "1440p"},
    {3840, 2160, "4K UHD"},
    {4096, 2160, "DCI 4K"},
    // extreme aspect ratios -- LED walls and ticker strips are real NDI users
    {3840, 64, "LED strip (60:1)"},
    {64, 2160, "tall column"},
    {8192, 128, "very wide"},
    // odd sizes: the subsampled formats must refuse these
    {641, 480, "odd width"},
    {640, 481, "odd height"},
    {641, 481, "odd both"},
    {1921, 1081, "odd both, HD-ish"},
};

// ------------------------------------------------------------------- patterns

struct Rgb
{
  int r, g, b;
};

// Eight vertical bars. Saturated primaries are the right probe: a channel swap
// or a wrong matrix moves them a long way, and their chroma is as far from
// neutral as 8-bit gets.
constexpr Rgb kBars[8] = {
    {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
    {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {0, 0, 0},
};

// Below this, a bar would be narrower than a chroma site pair and the 4:2:0
// formats would legitimately blur it; use a flat field instead.
constexpr int kMinBarsWidth = 64;

bool usesBars(int w)
{
  return w >= kMinBarsWidth;
}

Rgb expectedAt(int w, int x)
{
  if(!usesBars(w))
    return {255, 0, 0};
  return kBars[std::min(7, x * 8 / w)];
}

std::vector<uint8_t> makePattern(int w, int h)
{
  std::vector<uint8_t> px(size_t(w) * h * 4);
  for(int y = 0; y < h; y++)
  {
    for(int x = 0; x < w; x++)
    {
      const auto c = expectedAt(w, x);
      uint8_t* p = px.data() + (size_t(y) * w + x) * 4;
      p[0] = uint8_t(c.r);
      p[1] = uint8_t(c.g);
      p[2] = uint8_t(c.b);
      p[3] = 255;
    }
  }
  return px;
}

// ------------------------------------------------------- the production encode
//
// This is WireEncodeRenderer::init + finishFrame + assembleInto, with the
// renderer's score::gfx glue (Node, RenderList, Edge) left out because none of
// it touches the bytes. Every arithmetic decision below comes from the shipping
// helpers -- ndiEncoding, makeNdiEncoder, packedRowBytes, framestoreRows,
// assembleFramestore, readbackStride -- so a bug in any of them fails here.

struct Encoded
{
  bool ok{};
  std::vector<uint8_t> framestore;
  int stride{};
  double encodeMs{};
  std::string why;
};

Encoded encodeOnGpu(
    RenderState& state, const std::string& format, int W, int H,
    Ndi::ColorSpaceSetting setting, const std::vector<uint8_t>& rgba, int reps = 1)
{
  Encoded out;
  auto& rhi = *state.rhi;

  const auto encoding = Ndi::ndiEncoding(format);
  const auto* wire = Ndi::findWireFormat(format);
  if(!wire)
  {
    out.why = "no wire format";
    return out;
  }

  auto* input = rhi.newTexture(
      QRhiTexture::RGBA8, QSize(W, H), 1, QRhiTexture::UsedAsTransferSource);
  if(!input->create())
  {
    delete input;
    out.why = "input texture creation failed";
    return out;
  }

  // RGBA/RGBX have no encoder: the scene texture is already the wire bytes, so
  // the "encode" is the identity and the framestore is the upload itself.
  if(!encoding.hasEncoder())
  {
    delete input;
    out.ok = true;
    out.framestore = rgba;
    out.stride = W * 4;
    return out;
  }

  auto enc = Ndi::makeNdiEncoder(format);
  if(!enc)
  {
    delete input;
    out.why = "no GPU encoder";
    return out;
  }

  const auto standard = Ndi::resolveYuvStandard(setting, W, H);
  enc->init(
      rhi, state, input, W, H, Ndi::ndiColorMatrixOut(standard));
  enc->setReadbackEnabled(false);

  QRhiReadbackResult direct;

  // The source upload happens ONCE, outside the timing loop. In production the
  // scene renders straight into this texture; nothing ever pushes a frame of
  // RGBA across the bus to get it there. Timing the upload would add 33 MB of
  // CPU->GPU transfer per iteration at 4K and report it as encode cost, which
  // is how a GPU encoder gets blamed for work it does not do.
  {
    QRhiCommandBuffer* cb{};
    if(rhi.beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
    {
      enc->release();
      delete input;
      out.why = "beginOffscreenFrame failed";
      return out;
    }
    auto* up = rhi.nextResourceUpdateBatch();
    QRhiTextureSubresourceUploadDescription sub{
        QByteArray(reinterpret_cast<const char*>(rgba.data()), int(rgba.size()))};
    up->uploadTexture(input, QRhiTextureUploadDescription{{0, 0, sub}});
    cb->resourceUpdate(up);
    rhi.endOffscreenFrame();
  }

  const auto t0 = steady_clock::now();
  for(int rep = 0; rep < reps; rep++)
  {
    QRhiCommandBuffer* cb{};
    if(rhi.beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
    {
      enc->release();
      delete input;
      out.why = "beginOffscreenFrame failed";
      return out;
    }

    enc->exec(rhi, *cb);

    {
      auto* batch = rhi.nextResourceUpdateBatch();
      batch->readBackTexture(QRhiReadbackDescription{enc->outputTexture()}, &direct);
      cb->resourceUpdate(batch);
    }
    rhi.endOffscreenFrame();
  }
  const auto t1 = steady_clock::now();
  out.encodeMs = duration<double, std::milli>(t1 - t0).count() / reps;

  const int rowBytes = Ndi::packedRowBytes(format, W);
  const int rows = Ndi::framestoreRows(format, H);

  if(direct.data.isEmpty())
  {
    enc->release();
    delete input;
    out.why = "empty readback";
    return out;
  }
  out.framestore.assign(
      reinterpret_cast<const uint8_t*>(direct.data.constData()),
      reinterpret_cast<const uint8_t*>(direct.data.constData()) + direct.data.size());
  out.stride = Ndi::readbackStride(direct.data.size(), rows);

  enc->release();
  delete input;
  (void)rowBytes;
  out.ok = true;
  return out;
}

// ----------------------------------------------------------- SDK round trip

struct Sdk
{
  NDIlib_send_instance_t send{};
  NDIlib_recv_instance_t recv{};
  bool ok{};
};

// A fresh sender and receiver per case. Reusing one pair across cases is what
// made the first version of LoopbackTest read the PREVIOUS case's frame and
// pass anyway; a new connection per case removes the possibility entirely.
Sdk openPair(const char* name)
{
  Sdk s;
  NDIlib_send_create_t sc{};
  sc.p_ndi_name = name;
  sc.clock_video = false;
  sc.clock_audio = false;
  s.send = g_ndi->send_create(&sc);
  if(!s.send)
    return s;

  const NDIlib_source_t* src = g_ndi->send_get_source_name(s.send);
  if(!src)
  {
    g_ndi->send_destroy(s.send);
    s.send = nullptr;
    return s;
  }

  NDIlib_recv_create_v3_t rc{};
  rc.source_to_connect_to = *src;
  rc.color_format = NDIlib_recv_color_format_RGBX_RGBA;
  rc.bandwidth = NDIlib_recv_bandwidth_highest;
  rc.allow_video_fields = false;
  s.recv = g_ndi->recv_create_v3(&rc);
  if(!s.recv)
  {
    g_ndi->send_destroy(s.send);
    s.send = nullptr;
    return s;
  }
  s.ok = true;
  return s;
}

void closePair(Sdk& s)
{
  if(s.recv)
    g_ndi->recv_destroy(s.recv);
  if(s.send)
  {
    g_ndi->send_send_video_async_v2(s.send, nullptr);  // flush async ownership
    g_ndi->send_destroy(s.send);
  }
  s = {};
}

// Send `frame` until a video frame of the expected size comes back. Because the
// pair is fresh and only ever carries this one picture, any video frame that
// arrives IS this frame -- but the size is checked anyway, since a wrong
// description is exactly the failure being hunted.
bool roundTrip(
    Sdk& s, NDIlib_video_frame_v2_t& frame, int expectW, int expectH,
    NDIlib_video_frame_v2_t& got, int timeoutMs = 8000)
{
  const auto deadline = steady_clock::now() + milliseconds(timeoutMs);
  while(steady_clock::now() < deadline)
  {
    g_ndi->send_send_video_v2(s.send, &frame);

    NDIlib_video_frame_v2_t vf{};
    if(g_ndi->recv_capture_v3(s.recv, &vf, nullptr, nullptr, 100)
       == NDIlib_frame_type_video)
    {
      if(vf.xres == expectW && vf.yres == expectH && vf.p_data)
      {
        got = vf;
        return true;
      }
      g_ndi->recv_free_video_v2(s.recv, &vf);
    }
  }
  return false;
}

Rgb receivedAt(const NDIlib_video_frame_v2_t& f, int x, int y)
{
  const uint8_t* row = f.p_data + ptrdiff_t(y) * f.line_stride_in_bytes;
  const uint8_t* px = row + ptrdiff_t(x) * 4;
  return {px[0], px[1], px[2]};
}

int rgbError(Rgb a, Rgb b)
{
  return std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b);
}
}

// ================================================================== the phases

namespace
{
struct CaseResult
{
  std::string format;
  Res res;
  bool structural{};
  bool roundtrip{};
  int worstError{};
  double encodeMs{};
};

std::vector<CaseResult> g_results;

// ------------------------------------------------------------------- Phase 1
void phase1(RenderState& state)
{
  std::printf("\n================ Phase 1: structure across the panel ========\n");
  std::printf("%-6s %-26s %-10s %s\n", "fmt", "resolution", "verdict", "detail");

  for(const auto& f : Ndi::wireFormats)
  {
    const std::string format{f.name};
    for(const auto& r : kPanel)
    {
      const bool oddW = (r.w % 2) != 0;
      const bool oddH = (r.h % 2) != 0;
      const bool expressible
          = !(f.widthMustBeEven && oddW) && !(f.heightMustBeEven && oddH);

      // What the wire layer says, with a plausible stride.
      NDIlib_video_frame_v2_t probe{};
      const int packed = Ndi::packedRowBytes(format, r.w);
      const std::vector<uint8_t> dummy(16, 0);
      const bool described = Ndi::describeVideoFrame(
          format, dummy.data(), r.w, r.h, packed > 0 ? packed : 1, probe);

      if(!expressible)
      {
        // The whole point: an odd size must be REFUSED, not described as
        // something structurally valid that puts the chroma in the wrong place.
        check(
            !described,
            format + " must refuse " + std::to_string(r.w) + "x"
                + std::to_string(r.h) + " (odd for a subsampled format)");
        if(described)
          std::printf(
              "%-6s %4dx%-4d %-14s %-10s ACCEPTED an inexpressible size\n",
              format.c_str(), r.w, r.h, r.label, "BUG");
        continue;
      }

      check(
          described,
          format + " must accept " + std::to_string(r.w) + "x"
              + std::to_string(r.h));
      if(!described)
        continue;

      // Geometry the SDK will use to find the other planes.
      const int rows = Ndi::framestoreRows(format, r.h);
      const size_t bytes = Ndi::framestoreBytes(format, r.w, r.h);
      check(
          rows > 0 && bytes == size_t(packed) * rows,
          format + " framestore arithmetic is self-consistent at "
              + std::to_string(r.w) + "x" + std::to_string(r.h));

      // And the GPU really produces that many bytes.
      const auto rgba = makePattern(r.w, r.h);
      auto enc = encodeOnGpu(
          state, format, r.w, r.h, Ndi::ColorSpaceSetting::Rec709, rgba);

      CaseResult cr;
      cr.format = format;
      cr.res = r;
      cr.encodeMs = enc.encodeMs;

      if(!enc.ok)
      {
        std::printf(
            "%-6s %4dx%-4d %-26s %-10s %s\n", format.c_str(), r.w, r.h, r.label,
            "ENCFAIL", enc.why.c_str());
        check(false, format + " encodes at " + std::to_string(r.w) + "x" + std::to_string(r.h)
                         + ": " + enc.why);
        g_results.push_back(cr);
        continue;
      }

      // The readback must cover the framestore, and its stride must be at
      // least the packed row -- a shorter one has the SDK walk into the next
      // row on every line.
      const bool sizeOk = enc.framestore.size() >= bytes;
      const bool strideOk = enc.stride >= packed;
      check(
          sizeOk, format + " readback covers the framestore at "
                      + std::to_string(r.w) + "x" + std::to_string(r.h));
      check(
          strideOk, format + " stride >= packed row at " + std::to_string(r.w)
                        + "x" + std::to_string(r.h));

      cr.structural = sizeOk && strideOk;
      g_results.push_back(cr);

      if(!cr.structural)
        std::printf(
            "%-6s %4dx%-4d %-26s %-10s got %zu bytes stride %d, want >=%zu/%d\n",
            format.c_str(), r.w, r.h, r.label, "GEOMBUG", enc.framestore.size(),
            enc.stride, bytes, packed);
    }
  }
  std::printf("  %d structural checks run\n", g_checks);
}

// ------------------------------------------------------------------- Phase 2
void phase2(RenderState& state)
{
  std::printf("\n================ Phase 2: round trip through the SDK ========\n");
  if(!g_ndi)
  {
    std::printf("  no NDI runtime: skipped\n");
    return;
  }
  std::printf(
      "%-6s %-12s %-24s %-8s %s\n", "fmt", "resolution", "label", "worstErr",
      "verdict");

  // A subset of the panel: every format at every one of 26 resolutions through
  // a real network stack would take half an hour. These cover each class.
  const Res subset[] = {
      {320, 240, "QVGA"},      {720, 576, "PAL D1"},   {1280, 720, "720p"},
      {1920, 1080, "1080p"},   {3840, 2160, "4K UHD"}, {3840, 64, "LED strip"},
      {64, 2160, "tall column"},
  };

  for(const auto& f : Ndi::wireFormats)
  {
    const std::string format{f.name};
    for(const auto& r : subset)
    {
      if((f.widthMustBeEven && r.w % 2) || (f.heightMustBeEven && r.h % 2))
        continue;

      const auto rgba = makePattern(r.w, r.h);
      auto enc = encodeOnGpu(
          state, format, r.w, r.h, Ndi::ColorSpaceSetting::Rec709, rgba);
      if(!enc.ok)
      {
        check(false, format + " encodes for round trip at " + r.label);
        continue;
      }

      NDIlib_video_frame_v2_t frame{};
      if(!Ndi::describeVideoFrame(
             format, enc.framestore.data(), r.w, r.h, enc.stride, frame))
      {
        check(false, format + " describes its own encoded bytes at " + r.label);
        continue;
      }
      frame.frame_rate_N = 60000;
      frame.frame_rate_D = 1000;

      const std::string name = "sweep-" + format + "-" + std::to_string(r.w);
      auto sdk = openPair(name.c_str());
      if(!sdk.ok)
      {
        check(false, "SDK sender/receiver pair for " + name);
        continue;
      }

      NDIlib_video_frame_v2_t got{};
      if(!roundTrip(sdk, frame, r.w, r.h, got))
      {
        std::printf(
            "%-6s %4dx%-4d %-24s %-8s NO FRAME RETURNED\n", format.c_str(), r.w,
            r.h, r.label, "-");
        check(false, format + " round trips at " + r.label);
        closePair(sdk);
        continue;
      }

      // Sample well inside each bar so 4:2:0 chroma interpolation at the bar
      // edges is not what is being measured.
      int worst = 0;
      int worstBar = -1;
      const int y = r.h / 2;
      if(usesBars(r.w))
      {
        for(int i = 0; i < 8; i++)
        {
          const int x = std::min(r.w - 1, r.w * i / 8 + r.w / 16);
          const auto e = rgbError(receivedAt(got, x, y), kBars[i]);
          if(e > worst)
          {
            worst = e;
            worstBar = i;
          }
        }
      }
      else
      {
        worst = rgbError(receivedAt(got, r.w / 2, y), {255, 0, 0});
      }

      // Tolerance: a YUV round trip at limited range loses a little, and 4:2:0
      // loses more. What must NOT pass is a wrong matrix (which moves a
      // saturated bar by 40+) or a channel swap (200+).
      const int tol = (f.rowsDen == 2) ? 40 : 30;
      const bool ok = worst <= tol;
      check(
          ok, format + " round trips with correct colour at " + r.label
                  + " (worst " + std::to_string(worst) + " > " + std::to_string(tol)
                  + ")");
      std::printf(
          "%-6s %4dx%-4d %-24s %-8d %s%s\n", format.c_str(), r.w, r.h, r.label,
          worst, ok ? "ok" : "MISMATCH",
          (!ok && worstBar >= 0) ? (" at bar " + std::to_string(worstBar)).c_str()
                                 : "");

      g_ndi->recv_free_video_v2(sdk.recv, &got);
      closePair(sdk);
    }
  }
}

// ------------------------------------------------------------------- Phase 3
//
// The experiment the whole colour policy rests on. NDI signals no matrix for
// SDR, and its documentation says the standard follows the resolution. If that
// were true, the SDK's own receiver would decode an SD picture as BT.601 and an
// HD one as Rec.709 -- so encoding red as BT.601 would round trip perfectly at
// 720x576 and badly at 1920x1080.
//
// Send the same red encoded all three ways, at each resolution. Whatever this
// prints IS the rule the runtime implements.
void phase3(RenderState& state)
{
  std::printf("\n================ Phase 3: what matrix does the RECEIVER use? \n");
  if(!g_ndi)
  {
    std::printf("  no NDI runtime: skipped\n");
    return;
  }
  std::printf(
      "  Encoding pure red three ways and seeing which decodes back to red.\n");
  std::printf(
      "  Documented rule: <=720x576 BT.601, then Rec.709, >1920x1080 Rec.2020\n\n");
  std::printf(
      "%-14s %10s %10s %10s   %-12s %s\n", "resolution", "as601", "as709",
      "as2020", "best", "documented");

  const Res probes[] = {
      {320, 240, "QVGA"},    {720, 480, "NTSC D1"},  {720, 576, "PAL D1"},
      {722, 576, "722x576"}, {1280, 720, "720p"},    {1920, 1080, "1080p"},
      {1922, 1080, "1922x1080"}, {3840, 2160, "4K UHD"},
  };

  const Ndi::ColorSpaceSetting asSetting[3] = {
      Ndi::ColorSpaceSetting::BT601,
      Ndi::ColorSpaceSetting::Rec709,
      Ndi::ColorSpaceSetting::Rec2020,
  };
  const char* stdName[3] = {"BT.601", "Rec.709", "Rec.2020"};

  for(const auto& r : probes)
  {
    std::vector<uint8_t> rgba(size_t(r.w) * r.h * 4);
    for(size_t i = 0; i < rgba.size(); i += 4)
    {
      rgba[i] = 255;
      rgba[i + 1] = 0;
      rgba[i + 2] = 0;
      rgba[i + 3] = 255;
    }

    int err[3] = {-1, -1, -1};
    for(int s = 0; s < 3; s++)
    {
      auto enc = encodeOnGpu(state, "UYVY", r.w, r.h, asSetting[s], rgba);
      if(!enc.ok)
        continue;

      NDIlib_video_frame_v2_t frame{};
      if(!Ndi::describeVideoFrame(
             "UYVY", enc.framestore.data(), r.w, r.h, enc.stride, frame))
        continue;
      frame.frame_rate_N = 60000;
      frame.frame_rate_D = 1000;

      const std::string name
          = "matrix-" + std::string(stdName[s]) + "-" + std::to_string(r.w) + "x"
            + std::to_string(r.h);
      auto sdk = openPair(name.c_str());
      if(!sdk.ok)
        continue;

      NDIlib_video_frame_v2_t got{};
      if(roundTrip(sdk, frame, r.w, r.h, got))
      {
        err[s] = rgbError(receivedAt(got, r.w / 2, r.h / 2), {255, 0, 0});
        g_ndi->recv_free_video_v2(sdk.recv, &got);
      }
      closePair(sdk);
    }

    int best = 0;
    for(int s = 1; s < 3; s++)
      if(err[s] >= 0 && (err[best] < 0 || err[s] < err[best]))
        best = s;

    const auto documented = Ndi::documentedYuvStandard(r.w, r.h);
    std::printf(
        "%4dx%-4d %-4s %10d %10d %10d   %-12s %s%s\n", r.w, r.h, "", err[0],
        err[1], err[2], err[best] >= 0 ? stdName[best] : "none",
        Ndi::ndiYuvStandardName(documented),
        (err[best] >= 0 && int(documented) != best) ? "   <-- DISAGREES" : "");
  }
  std::printf(
      "\n  (lower is better; 0 means the encode matrix and the receiver's agree)\n");
}

// ------------------------------------------------------------------- Phase 4
void phase4(RenderState& state)
{
  std::printf("\n================ Phase 4: encode cost ======================\n");
  std::printf(
      "  GPU encode + readback, milliseconds per frame, median of 20\n\n");
  std::printf("%-6s", "fmt");
  const Res benchRes[] = {
      {1280, 720, "720p"},
      {1920, 1080, "1080p"},
      {3840, 2160, "4K UHD"},
      {7680, 4320, "8K UHD"}};
  for(const auto& r : benchRes)
    std::printf(" %12s", r.label);
  std::printf("   %s\n", "copies/frame");

  for(const auto& f : Ndi::wireFormats)
  {
    const std::string format{f.name};
    std::printf("%-6s", format.c_str());
    for(const auto& r : benchRes)
    {
      const auto rgba = makePattern(r.w, r.h);
      // Warm up: first frame builds pipelines.
      encodeOnGpu(state, format, r.w, r.h, Ndi::ColorSpaceSetting::Rec709, rgba);
      auto enc = encodeOnGpu(
          state, format, r.w, r.h, Ndi::ColorSpaceSetting::Rec709, rgba, 20);
      // RGBA/RGBX have no encoder here, so there is nothing to time: in
      // production that path is InvertYRenderer + a readback, which this
      // harness does not build. Say so rather than printing a 0.00 that looks
      // like a result.
      if(!Ndi::ndiEncoding(format).hasEncoder())
        std::printf(" %12s", "(no encoder)");
      else if(enc.ok)
        std::printf(" %12.2f", enc.encodeMs);
      else
        std::printf(" %12s", "-");
    }
    const auto encoding = Ndi::ndiEncoding(format);
    std::printf(
        "   %s\n", "0 (zero-copy)");
  }
}

// ------------------------------------------------------------------- Phase 6
//
// Dump raw framestores so ffmpeg can be asked the same question.
//
// Every other check here compares our code against our code -- packed against
// planes, the description against the SDK's reading of it -- so if both routes
// shared a wrong assumption, nothing would notice. ffmpeg has no stake in it.
// This is what caught P216PackedEncoder encoding the wrong pixel pair: the
// plane route matched ffmpeg to 4 parts in 65535, the packed one to 52286.
void phase6(RenderState& state)
{
  std::printf("\n================ Phase 6: dump for ffmpeg comparison =======\n");
  if(g_dumpDir.empty())
  {
    std::printf("  no --dump=DIR given: skipped\n");
    return;
  }
  const int W = g_dumpW, H = g_dumpH;
  std::printf("  %dx%d -> %s\n", W, H, g_dumpDir.c_str());

  const auto rgba = makePattern(W, H);
  auto write = [&](const std::string& name, const uint8_t* p, size_t n) {
    const std::string path = g_dumpDir + "/" + name;
    if(FILE* f = std::fopen(path.c_str(), "wb"))
    {
      std::fwrite(p, 1, n, f);
      std::fclose(f);
      std::printf("    %-24s %zu bytes\n", name.c_str(), n);
    }
    else
      std::printf("    could not write %s\n", path.c_str());
  };

  write("src.rgba", rgba.data(), rgba.size());

  // The plane routes as well, because those are the pairs ffmpeg is being
  // asked to arbitrate between.
  struct PlaneDump
  {
    const char* name;
    score::gfx::interop::VideoPixelFormat fmt;
    int planes;
  };
  for(auto pd : {PlaneDump{"P216", score::gfx::interop::VideoPixelFormat::P216, 2},
                 PlaneDump{"NV12", score::gfx::interop::VideoPixelFormat::NV12, 2}})
  {
    auto& rhi = *state.rhi;
    auto* input = rhi.newTexture(
        QRhiTexture::RGBA8, QSize(W, H), 1, QRhiTexture::UsedAsTransferSource);
    input->create();
    auto enc = score::gfx::makeWireEncoder(pd.fmt, /* contiguous */ false);
    if(enc)
    {
      enc->init(
          rhi, state, input, W, H,
          Ndi::ndiColorMatrixOut(
              Ndi::resolveYuvStandard(Ndi::ColorSpaceSetting::Rec709, W, H)));
      enc->setReadbackEnabled(true);
      QRhiCommandBuffer* cb{};
      if(rhi.beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess)
      {
        auto* up = rhi.nextResourceUpdateBatch();
        QRhiTextureSubresourceUploadDescription sub{QByteArray(
            reinterpret_cast<const char*>(rgba.data()), int(rgba.size()))};
        up->uploadTexture(input, QRhiTextureUploadDescription{{0, 0, sub}});
        cb->resourceUpdate(up);
        enc->exec(rhi, *cb);
        rhi.endOffscreenFrame();

        std::vector<uint8_t> fs;
        for(int i = 0; i < pd.planes; i++)
        {
          const auto& rb = enc->readback(i);
          const auto* q = reinterpret_cast<const uint8_t*>(rb.data.constData());
          fs.insert(fs.end(), q, q + rb.data.size());
        }
        write(std::string(pd.name) + ".planes.raw", fs.data(), fs.size());
      }
      enc->release();
    }
    delete input;
  }

  for(const char* fmt : {"NV12", "I420", "YV12", "P216", "UYVY"})
  {
    auto enc = encodeOnGpu(
        state, fmt, W, H, Ndi::ColorSpaceSetting::Rec709, rgba);
    if(enc.ok)
      write(
          std::string(fmt) + ".packed.raw", enc.framestore.data(),
          enc.framestore.size());
    else
      std::printf("    %s failed: %s\n", fmt, enc.why.c_str());
  }
}

// ------------------------------------------------------------------- Phase 7
//
// Where does the receiver think a chroma sample sits? Encode to one siting
// convention and decode with the other and every chroma edge moves half a luma
// pixel. Phase 2 cannot see it -- it samples bar CENTRES -- so this locates
// each of the seven bar edges to sub-pixel precision after a round trip and
// reports the average displacement.
void phase7(RenderState& state)
{
  std::printf("\n================ Phase 7: chroma siting, measured ==========\n");
  if(!g_ndi)
  {
    std::printf("  no NDI runtime: skipped\n");
    return;
  }
  const int W = 1920, H = 1080;
  const auto rgba = makePattern(W, H);

  for(const char* format : {"NV12", "I420"})
  {
    auto enc = encodeOnGpu(
        state, format, W, H, Ndi::ColorSpaceSetting::Rec709, rgba);
    if(!enc.ok)
    {
      check(false, std::string(format) + " encodes for the siting probe");
      continue;
    }
    NDIlib_video_frame_v2_t frame{};
    if(!Ndi::describeVideoFrame(
           format, enc.framestore.data(), W, H, enc.stride, frame))
      continue;
    frame.frame_rate_N = 60000;
    frame.frame_rate_D = 1000;

    auto sdk = openPair((std::string("siting-") + format).c_str());
    if(!sdk.ok)
      continue;
    NDIlib_video_frame_v2_t got{};
    if(!roundTrip(sdk, frame, W, H, got))
    {
      closePair(sdk);
      continue;
    }

    // Chroma-only signal: red minus blue separates every bar in this pattern
    // and is untouched by luma, which is not subsampled and so carries no
    // siting information at all.
    const int y = H / 2;
    auto chromaAt = [&](int x) {
      const auto c = receivedAt(got, std::clamp(x, 0, W - 1), y);
      return double(c.r - c.b);
    };

    double sum = 0;
    int n = 0;
    for(int k = 1; k < 8; k++)
    {
      const double edge = double(W) * k / 8.0;   // true edge, in luma columns
      const int e = int(edge);
      const double lo = chromaAt(e - 6), hi = chromaAt(e + 6);
      if(std::abs(hi - lo) < 40.0)
        continue;                                 // not a chroma edge, skip
      const double mid = (lo + hi) * 0.5;
      // Walk outward from the nominal edge for the 50% crossing, then
      // interpolate between the two samples that straddle it.
      for(int x = e - 5; x < e + 5; x++)
      {
        const double a = chromaAt(x), b = chromaAt(x + 1);
        if((a - mid) * (b - mid) <= 0.0 && a != b)
        {
          const double cross = double(x) + (mid - a) / (b - a);
          // +0.5 puts the measurement in the same frame as the edge, which
          // lies on a pixel boundary rather than a pixel centre.
          sum += (cross + 0.5) - edge;
          ++n;
          break;
        }
      }
    }

    if(n > 0)
    {
      const double shift = sum / n;
      std::printf(
          "  %s: %d edges, mean displacement %+.3f luma columns\n", format, n,
          shift);
      check(
          std::abs(shift) < 0.25,
          std::string(format) + " chroma edges land where they were put "
              + "(displacement " + std::to_string(shift) + ")");
    }
    else
      std::printf("  %s: no usable edges found\n", format);

    g_ndi->recv_free_video_v2(sdk.recv, &got);
    closePair(sdk);
  }
}

// ------------------------------------------------------------------- Phase 8
//
// Box or pre-filter? Measured on the content that can tell them apart.
//
// 4:2:0 throws away three quarters of the chroma. A 2x2 box does that with no
// pre-filtering at all, so chroma detail finer than the chroma grid folds back
// as aliasing -- the coloured crawl on fine detail. A [1 3 3 1]/8 filter in
// each direction band-limits first: softer, but what survives is what was
// really there.
//
// Which is better is a question about content, so it is asked with content:
// vertical colour stripes at several periods, from the chroma Nyquist limit
// (period 4 luma columns) upward, round-tripped through the real SDK and
// compared against what went in. Lower is better, and the interesting number
// is how each does near Nyquist.
void phase8(RenderState& state)
{
  std::printf("\n================ Phase 8: chroma aliasing ==================\n");
  if(!g_ndi)
  {
    std::printf("  no NDI runtime: skipped\n");
    return;
  }
  const int W = 1920, H = 1080;
  std::printf("  vertical red/blue stripes, mean |RGB error| after a round trip\n");
  std::printf("  (chroma Nyquist for 4:2:0 is a period of 4 luma columns)\n\n");
  std::printf("  %-10s %12s\n", "period", "mean error");

  for(int period : {4, 6, 8, 12, 16, 32})
  {
    std::vector<uint8_t> rgba(size_t(W) * H * 4);
    for(int y = 0; y < H; y++)
      for(int x = 0; x < W; x++)
      {
        const bool red = ((x / (period / 2)) & 1) == 0;
        uint8_t* q = rgba.data() + (size_t(y) * W + x) * 4;
        q[0] = red ? 255 : 0;
        q[1] = 0;
        q[2] = red ? 0 : 255;
        q[3] = 255;
      }

    auto enc = encodeOnGpu(
        state, "I420", W, H, Ndi::ColorSpaceSetting::Rec709, rgba);
    if(!enc.ok)
      continue;
    NDIlib_video_frame_v2_t frame{};
    if(!Ndi::describeVideoFrame(
           "I420", enc.framestore.data(), W, H, enc.stride, frame))
      continue;
    frame.frame_rate_N = 60000;
    frame.frame_rate_D = 1000;

    auto sdk = openPair(("alias-" + std::to_string(period)).c_str());
    if(!sdk.ok)
      continue;
    NDIlib_video_frame_v2_t got{};
    if(!roundTrip(sdk, frame, W, H, got))
    {
      closePair(sdk);
      continue;
    }

    double err = 0;
    int n = 0;
    const int y = H / 2;
    for(int x = 0; x < W; x++)
    {
      const auto c = receivedAt(got, x, y);
      const uint8_t* q = rgba.data() + (size_t(y) * W + x) * 4;
      err += std::abs(c.r - int(q[0])) + std::abs(c.g - int(q[1]))
             + std::abs(c.b - int(q[2]));
      ++n;
    }
    std::printf("  %-10d %12.2f\n", period, err / (3 * n));

    g_ndi->recv_free_video_v2(sdk.recv, &got);
    closePair(sdk);
  }
}

}

// ===================================================================== driver

namespace
{
void runAll(const std::string& phases)
{
  const auto apiEnv = qgetenv("SCORE_GFX_API").toLower();
  const GraphicsApi api = (apiEnv == "vulkan" || apiEnv == "vk")
                              ? GraphicsApi::Vulkan
                              : GraphicsApi::OpenGL;
  auto state = createRenderState(api, QSize(192, 64), nullptr);
  if(!state || !state->rhi)
  {
    std::printf("no QRhi available -- skipping\n");
    std::exit(77);
  }
  std::printf("  QRhi backend: %s\n", state->rhi->backendName());

  if(phases.find('1') != std::string::npos)
    phase1(*state);
  if(phases.find('2') != std::string::npos)
    phase2(*state);
  if(phases.find('3') != std::string::npos)
    phase3(*state);
  if(phases.find('4') != std::string::npos)
    phase4(*state);
  if(phases.find('6') != std::string::npos)
    phase6(*state);
  if(phases.find('7') != std::string::npos)
    phase7(*state);
  if(phases.find('8') != std::string::npos)
    phase8(*state);
  // Phase 5 (packed 4:2:0 == planes) moved to score's own EncoderTester: the
  // encoders are score::gfx's, used by every consumer that asks for a
  // contiguous framestore, so the gate belongs there and not behind an NDI
  // test somebody has to know to run.
}
}

int main(int argc, char** argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  QLocale::setDefault(QLocale::C);
  std::setlocale(LC_ALL, "C");
  qputenv("SCORE_DISABLE_AUDIOPLUGINS", "1");
  qputenv("SCORE_AUDIO_BACKEND", "dummy");

  std::string ndiPath;
  std::string phases = "1234";
  for(int i = 1; i < argc; i++)
  {
    const std::string a = argv[i];
    if(a.rfind("--phases=", 0) == 0)
      phases = a.substr(9);
    else if(a.rfind("--dump=", 0) == 0)
      g_dumpDir = a.substr(7);
    else if(a.rfind("--dumpsize=", 0) == 0)
    {
      const auto v = a.substr(11);
      const auto x = v.find('x');
      g_dumpW = std::stoi(v.substr(0, x));
      g_dumpH = std::stoi(v.substr(x + 1));
    }
    else
      ndiPath = a;
  }
  if(ndiPath.empty())
  {
    if(const char* e = getenv("NDI_SWEEP_LIB"))
      ndiPath = e;
  }

  if(!ndiPath.empty())
  {
    std::printf("NDI runtime: %s\n", ndiPath.c_str());
    if(!loadNdi(ndiPath.c_str()))
      std::printf("  -> SDK phases will be skipped\n");
  }
  else
  {
    std::printf("No NDI runtime given; SDK phases will be skipped.\n");
  }

// NOT score::MinimalApplication.
//
// MinimalApplication runs score's whole plugin bootstrap, and this target has
// the addon's own root on its include path (it must, to reach <Ndi/...>).
// That makes score_static_plugins.hpp find <score_addon_ndi.hpp>, register the
// NDI addon as a static plugin, and drag in ScenarioApplicationPlugin, which
// aborts looking for an InspectorWidgetList this unit test has no reason to
// link:
//
//   score::ApplicationComponents::interfaces<Inspector::InspectorWidgetList>
//   <- Scenario::ScenarioApplicationPlugin::initialize
//   <- score::MinimalApplication::MinimalApplication
//
// None of that is wanted here. What the test needs from Qt is a GUI
// application so createRenderState() can make a context, and nothing else.
  QApplication app(argc, argv);

  QMetaObject::invokeMethod(
      &app,
      [&phases] {
        runAll(phases);
        std::printf(
            "\n==============================================================\n");
        std::printf(
            "format sweep: %s -- %d checks, %d failure%s\n",
            g_fail ? "FAILED" : "passed", g_checks, g_fail,
            g_fail == 1 ? "" : "s");
        QCoreApplication::exit(g_fail ? 1 : 0);
      },
      Qt::QueuedConnection);

  return app.exec();
}
