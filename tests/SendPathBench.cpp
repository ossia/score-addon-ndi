// Measures the per-frame cost of the NDI send path, so the trade-offs in it can
// be argued from numbers.
//
// Three questions:
//
//  1. What does the sws_scale on the sender thread cost? That is the work the
//    GPU UYVY encoder in Gfx/Graph/encoders/UYVY.hpp would remove -- it does
//    RGBA -> UYVY in a fragment shader, Y-flip included, into a width/2 x height
//    RGBA8 target, which also halves the bytes coming back over the bus.
//  2. What would a copy cost? A full-frame memcpy on the RGBA path was tried and
//    reverted in favour of the ownership states; this is what was saved.
//  3. Does anything in the send path allocate per frame? Two candidates: the
//    QString::toStdString() the sender does on m_settings.format for every
//    frame, and QRhiReadbackResult::data.data(), which is QByteArray's
//    non-const data() and detaches -- a full-frame copy -- if the array is
//    shared.
//
// Prints numbers; asserts only the things that are actually invariants.
#include <Ndi/VideoFrameFormat.hpp>

#include <QByteArray>
#include <QString>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>
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

// --- allocation counting ---------------------------------------------------
// Two levels, because they see different things. operator new catches what this
// translation unit allocates; Qt's containers go through malloc inside
// libQt6Core, which operator new never sees, so mallinfo2 is needed for those.
// Each measurement below says which one it used, and each has a control.
namespace
{
std::size_t g_allocs = 0;
std::size_t g_allocBytes = 0;
bool g_counting = false;
}

void* operator new(std::size_t n)
{
  if(g_counting)
  {
    ++g_allocs;
    g_allocBytes += n;
  }
  void* p = std::malloc(n ? n : 1);
  if(!p)
    throw std::bad_alloc{};
  return p;
}
void operator delete(void* p) noexcept
{
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept
{
  std::free(p);
}
void* operator new[](std::size_t n)
{
  return operator new(n);
}
void operator delete[](void* p) noexcept
{
  std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept
{
  std::free(p);
}

namespace
{
struct Counted
{
  std::size_t allocs, bytes;
};

/// Bytes glibc has handed out, which covers Qt's own allocations too.
std::size_t mallocInUse()
{
  return mallinfo2().uordblks;
}

template <typename F>
Counted counting(F&& f)
{
  const std::size_t a0 = g_allocs, b0 = g_allocBytes;
  g_counting = true;
  f();
  g_counting = false;
  return {g_allocs - a0, g_allocBytes - b0};
}

using clk = std::chrono::steady_clock;

template <typename F>
double msPerCall(F&& f, int iters)
{
  f();  // warm
  const auto t0 = clk::now();
  for(int i = 0; i < iters; i++)
    f();
  const auto t1 = clk::now();
  return std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
}

struct Size
{
  const char* name;
  int w, h;
};
constexpr Size kSizes[] = {
    {"720p", 1280, 720}, {"1080p", 1920, 1080}, {"2160p", 3840, 2160}};

// -------------------------------------------------------------------------
void benchConversion()
{
  std::printf("  %-7s %10s %12s %12s %12s %10s\n", "size", "readback", "sws_scale",
              "memcpy", "budget@60fps", "sws % of");
  std::printf("  %-7s %10s %12s %12s %12s %10s\n", "", "bytes", "ms/frame", "ms/frame",
              "ms", "budget");
  for(const Size s : kSizes)
  {
    std::vector<uint8_t> rgba(size_t(s.w) * s.h * 4, 0x60);
    std::vector<uint8_t> dst(size_t(s.w) * s.h * 4, 0);

    SwsContext* sws = sws_getContext(
        s.w, s.h, AV_PIX_FMT_RGBA, s.w, s.h, AV_PIX_FMT_UYVY422, 0, nullptr, nullptr,
        nullptr);
    AVFrame* staging = av_frame_alloc();
    staging->format = AV_PIX_FMT_UYVY422;
    staging->width = s.w;
    staging->height = s.h;
    av_frame_get_buffer(staging, 0);
    CHECK(sws && staging->data[0], "%s: could not set up the conversion", s.name);
    if(!sws || !staging->data[0])
      continue;

    const int iters = s.w >= 3840 ? 60 : 200;
    NDIlib_video_frame_v2_t f{};
    const double sws_ms = msPerCall(
        [&] {
          Ndi::describeVideoFrame("UYVY", rgba.data(), s.w, s.h, sws, staging, f);
        },
        iters);
    const double memcpy_ms = msPerCall(
        [&] { std::memcpy(dst.data(), rgba.data(), rgba.size()); }, iters);
    const double budget = 1000.0 / 60.0;

    std::printf(
        "  %-7s %10zu %12.3f %12.3f %12.3f %9.1f%%\n", s.name, rgba.size(), sws_ms,
        memcpy_ms, budget, 100.0 * sws_ms / budget);

    av_frame_free(&staging);
    sws_freeContext(sws);
  }

  std::printf("\n  bytes off the GPU per frame, RGBA readback vs the GPU UYVY encoder:\n");
  for(const Size s : kSizes)
  {
    const size_t rgbaBytes = size_t(s.w) * s.h * 4;
    // encoders/UYVY.hpp renders into an RGBA8 target of width/2 x height, each
    // texel carrying one (U,Y0,V,Y1) macropixel.
    const size_t uyvyBytes = size_t(s.w / 2) * s.h * 4;
    std::printf(
        "    %-7s %9zu -> %9zu bytes  (%.0f%% less, and no sws_scale at all)\n", s.name,
        rgbaBytes, uyvyBytes, 100.0 * (1.0 - double(uyvyBytes) / double(rgbaBytes)));
  }
}

// -------------------------------------------------------------------------
// The RGBA path's cost, and what it allocates.
void benchRgbaPath()
{
  std::vector<uint8_t> rgba(size_t(1920) * 1080 * 4, 0x60);
  NDIlib_video_frame_v2_t f{};
  const double ms = msPerCall(
      [&] { Ndi::describeVideoFrame("RGBA", rgba.data(), 1920, 1080, nullptr, nullptr, f); },
      20000);
  std::printf(
      "  describeVideoFrame RGBA at 1080p: %.6f ms/frame (it only fills in five "
      "fields)\n",
      ms);

  const Counted c = counting([&] {
    for(int i = 0; i < 1000; i++)
      Ndi::describeVideoFrame("RGBA", rgba.data(), 1920, 1080, nullptr, nullptr, f);
  });
  std::printf("  describeVideoFrame RGBA: %zu allocations over 1000 frames\n", c.allocs);
  CHECK(c.allocs == 0, "the zero-copy RGBA path allocated %zu times in 1000 frames",
        c.allocs);
}

// -------------------------------------------------------------------------
// What the sender does with m_settings.format, once per frame.
void benchFormatString()
{
  const QString format = QStringLiteral("UYVY");

  // Hold the results so the allocations are still outstanding when measured;
  // a malloc/free pair inside the loop would net to nothing.
  std::vector<std::string> held;
  held.reserve(1000);
  const std::size_t before = mallocInUse();
  for(int i = 0; i < 1000; i++)
    held.push_back(format.toStdString());
  const std::size_t after = mallocInUse();

  // Control: the same 1000 std::strings built without going through QString. A
  // four-character std::string fits in the small-string buffer and allocates
  // nothing, so the difference is what QString::toStdString adds.
  std::vector<std::string> plain;
  plain.reserve(1000);
  const std::size_t before2 = mallocInUse();
  for(int i = 0; i < 1000; i++)
    plain.emplace_back("UYVY");
  const std::size_t after2 = mallocInUse();

  const double ms = msPerCall([&] {
    const std::string s = format.toStdString();
    asm volatile("" ::"r"(s.data()));
  }, 100000);
  const double msPlain = msPerCall([&] {
    const std::string s = "UYVY";
    asm volatile("" ::"r"(s.data()));
  }, 100000);

  std::printf(
      "  format.toStdString() x1000: %+lld bytes still allocated, %.6f ms each\n",
      (long long)(after - before), ms);
  std::printf(
      "  control, std::string(\"UYVY\") x1000: %+lld bytes, %.6f ms each\n",
      (long long)(after2 - before2), msPlain);
  // Nothing is retained either way: toUtf8()'s QByteArray is freed before the
  // std::string is returned, and a four-character std::string fits in the small
  // buffer. The cost is the transient malloc/free pair, which only the timing
  // shows.
  std::printf(
      "    -> nothing retained, but %.0f ns/frame more than the plain string: "
      "toStdString() goes through toUtf8(), which mallocs and frees a QByteArray "
      "once per frame\n"
      "       on the sender thread to compare a four-character format that never "
      "changes. describeVideoFrame takes a string_view, so nothing needs the "
      "std::string. At %.4f%% of a 60fps frame it is not where the time goes.\n",
      (ms - msPlain) * 1e6, 100.0 * (ms - msPlain) / (1000.0 / 60.0));
}

// -------------------------------------------------------------------------
// QRhiReadbackResult::data.data() -- can it detach and copy a whole frame?
void benchReadbackDetach()
{
  const qsizetype n = qsizetype(1920) * 1080 * 4;

  // Filled the way QRhi fills it: resize, then write. resize() detaches if
  // shared, so what comes out is unshared and data() is free.
  QByteArray a;
  a.resize(n);
  std::memset(a.data(), 0x33, size_t(n));
  const void* before = a.constData();
  const std::size_t m0 = mallocInUse();
  for(int i = 0; i < 1000; i++)
    asm volatile("" ::"r"(a.data()));
  const long long unsharedBytes = (long long)(mallocInUse() - m0);
  std::printf(
      "  QByteArray::data() on an unshared %lld-byte readback, 1000 calls: %+lld "
      "bytes allocated, pointer %s\n",
      (long long)n, unsharedBytes, before == a.constData() ? "unchanged" : "MOVED");
  CHECK(
      unsharedBytes == 0 && before == a.constData(),
      "readback.data.data() copied an unshared buffer: %+lld bytes", unsharedBytes);

  // The control. If nothing here allocates either, the check above is measuring
  // nothing and its silence means nothing.
  QByteArray shared = a;  // now refcount 2
  const std::size_t m1 = mallocInUse();
  asm volatile("" ::"r"(shared.data()));
  const long long detachBytes = (long long)(mallocInUse() - m1);
  std::printf(
      "  the same call on a shared copy, once: %+lld bytes  <- what one detach "
      "costs\n",
      detachBytes);
  CHECK(
      detachBytes > 0,
      "a shared QByteArray::data() did not allocate either, so the check above "
      "cannot tell a detach from no detach");
  std::printf(
      "    -> the readback is unshared because QRhi resizes it before writing, so "
      "data() is free today. Nothing enforces that: anyone who copies the\n"
      "       QByteArray out (to keep the bytes alive, say) turns the zero-copy "
      "path into a full-frame copy on the sender thread. constData() would not.\n");
}
}

int main()
{
  std::printf("-- conversion and copy cost --\n");
  benchConversion();
  std::printf("\n-- the RGBA path --\n");
  benchRgbaPath();
  std::printf("\n-- per-frame allocations in the send path --\n");
  benchFormatString();
  benchReadbackDetach();
  std::printf(
      "\nndi send path bench: %s (%d failure%s)\n", failures ? "FAILED" : "passed",
      failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
