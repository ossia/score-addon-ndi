// Checks the premise the whole readback pool is built on, against the real SDK.
//
// tests/LoopbackTest.cpp sends with NDIlib_send_send_video_v2 -- the synchronous
// call, which copies before it returns. Production sends with
// NDIlib_send_send_video_async_v2, whose contract is the opposite: the buffer
// must stay valid and unmodified until the next send or until send_destroy. The
// four-buffer pool and the zero-copy send path both
// exist only because of that contract, and nothing tested it.
//
// Two things here:
//
//  1. that the SDK really does read the buffer after the async send returns, so
//     the ownership states are load-bearing rather than defensive;
//  2. the shutdown order. ~OutputNode releases its buffers in its own body,
//     which runs before its members are destroyed --
//     and Ndi::Sender, whose destructor makes the send_destroy that ends the
//     SDK's claim on the last frame, is a member. Built with
//     -fsanitize=address, the last group reads freed memory if the SDK touches
//     the buffer during send_destroy.
//
// Exits 77 (ctest SKIP_RETURN_CODE) when the NDI runtime will not initialise.
#include <Ndi/VideoFrameFormat.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

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
constexpr int kWidth = 64;
constexpr int kHeight = 32;
constexpr size_t kBytes = size_t(kWidth) * kHeight * 4;

void fill(uint8_t* p, uint8_t r, uint8_t g, uint8_t b)
{
  for(size_t i = 0; i < kBytes; i += 4)
  {
    p[i + 0] = r;
    p[i + 1] = g;
    p[i + 2] = b;
    p[i + 3] = 255;
  }
}

struct Rgb
{
  int r, g, b;
};
Rgb pixelAt(const NDIlib_video_frame_v2_t& f, int x, int y)
{
  const uint8_t* px = f.p_data + ptrdiff_t(y) * f.line_stride_in_bytes + ptrdiff_t(x) * 4;
  return {px[0], px[1], px[2]};
}

// -------------------------------------------------------------------------
// 1. Does the SDK read the buffer after the async send has returned?
//
// Send red asynchronously, then overwrite that buffer with green without any
// intervening synchronising call -- which is precisely what the pool exists to
// stop the renderer doing. If what comes out is green, the SDK was still reading
// the buffer and the ownership states are doing real work. If it is red, this
// SDK build copied eagerly and the premise cannot be confirmed here; say so
// rather than pretend either way.
// -------------------------------------------------------------------------
void testAsyncOwnershipIsObservable(
    NDIlib_send_instance_t sender, NDIlib_recv_instance_t recv)
{
  using namespace std::chrono;
  std::vector<uint8_t> buf(kBytes);
  std::vector<uint8_t> other(kBytes);

  int red = 0, green = 0, neither = 0;
  const auto deadline = steady_clock::now() + seconds(10);
  for(int attempt = 0; attempt < 30 && steady_clock::now() < deadline; attempt++)
  {
    fill(buf.data(), 255, 0, 0);  // red
    NDIlib_video_frame_v2_t f{};
    CHECK(
        Ndi::describeVideoFrame("RGBA", buf.data(), kWidth, kHeight, 4 * kWidth, f),
        "RGBA was refused");
    f.frame_rate_N = 60000;
    f.frame_rate_D = 1000;
    NDIlib_send_send_video_async_v2(sender, &f);

    // No synchronising call in between: the SDK still owns buf here.
    fill(buf.data(), 0, 255, 0);  // green

    // Hand over a different buffer, which releases buf.
    fill(other.data(), 0, 0, 255);
    NDIlib_video_frame_v2_t g{};
    Ndi::describeVideoFrame("RGBA", other.data(), kWidth, kHeight, 4 * kWidth, g);
    g.frame_rate_N = 60000;
    g.frame_rate_D = 1000;
    NDIlib_send_send_video_async_v2(sender, &g);

    NDIlib_video_frame_v2_t got{};
    while(NDIlib_recv_capture_v3(recv, &got, nullptr, nullptr, 100)
          == NDIlib_frame_type_video)
    {
      if(got.p_data && got.xres == kWidth)
      {
        const Rgb p = pixelAt(got, 1, 1);
        if(p.r > 200 && p.g < 60)
          ++red;
        else if(p.g > 200 && p.r < 60)
          ++green;
        else if(!(p.b > 200 && p.r < 60))
          ++neither;
      }
      NDIlib_recv_free_video_v2(recv, &got);
    }
    if(red + green > 4)
      break;
  }

  std::printf(
      "  a buffer overwritten after the async send came back: red(as sent)=%d "
      "green(overwritten)=%d other=%d\n",
      red, green, neither);
  if(green > 0)
    std::printf(
        "  -> the SDK does read the buffer after the send returns; the pool's "
        "ownership states are load-bearing\n");
  else if(red > 0)
    std::printf(
        "  -> this SDK build appears to copy eagerly, so this test cannot confirm "
        "the async contract; the pool must still honour it, the SDK only "
        "guarantees the documented behaviour\n");
  else
    std::printf("  -> nothing identifiable came back; inconclusive\n");
  CHECK(red + green + neither > 0, "no frames came back at all, nothing was measured");
}

// -------------------------------------------------------------------------
// 2. Freeing a buffer the SDK still owns.
//
// ~OutputNode's body releases the buffers it owns. Its body runs
// before its members are destroyed, and Ndi::Sender -- whose destructor makes
// the send_destroy that ends the SDK's claim on the last frame it was handed --
// is a member. So the frame the SDK is still reading is freed first and the
// synchronising call comes second.
//
// AddressSanitizer cannot see this: libndi is not instrumented, so its own loads
// are invisible to the shadow map, and a build with -fsanitize=address reports
// nothing here (verified). What is visible is the output: freeing the buffer
// while the SDK owns it puts whatever the allocator did with the memory on the
// wire. Each round below sends a known flat colour from a fresh allocation and
// compares what the receiver gets, with and without the free.
// -------------------------------------------------------------------------
constexpr int kBigW = 640;
constexpr int kBigH = 360;
constexpr size_t kBigBytes = size_t(kBigW) * kBigH * 4;

struct RoundResult
{
  int matched{}, corrupted{}, received{};
};

/// How the sent buffer is released.
enum FreeMode
{
  kKeepAlive,           ///< control: freed only after send_destroy
  kFreeUnsynchronised,  ///< the pre-fix ~OutputNode order
  kFlushThenFree        ///< the fix: Sender::flush_async() before the free
};

RoundResult sendAndFree(FreeMode mode, int rounds)
{
  RoundResult res{};
  const std::string name = std::string("score-ndi-async-uaf-")
                           + (mode == kKeepAlive ? "kept-" : mode == kFreeUnsynchronised ? "freed-" : "flushed-")
                           + std::to_string(::getpid());
  NDIlib_send_create_t cfg{};
  cfg.p_ndi_name = name.c_str();
  cfg.clock_video = false;
  cfg.clock_audio = false;
  NDIlib_send_instance_t sender = NDIlib_send_create(&cfg);
  if(!sender)
    return res;
  const NDIlib_source_t* self = NDIlib_send_get_source_name(sender);
  if(!self)
  {
    NDIlib_send_destroy(sender);
    return res;
  }
  NDIlib_recv_create_v3_t recvCfg{};
  recvCfg.source_to_connect_to = *self;
  recvCfg.color_format = NDIlib_recv_color_format_RGBX_RGBA;
  recvCfg.bandwidth = NDIlib_recv_bandwidth_highest;
  recvCfg.allow_video_fields = false;
  NDIlib_recv_instance_t recv = NDIlib_recv_create_v3(&recvCfg);
  if(!recv)
  {
    NDIlib_send_destroy(sender);
    return res;
  }
  for(int i = 0; i < 50 && NDIlib_send_get_no_connections(sender, 100) == 0; i++)
    ;

  std::vector<uint8_t*> kept;
  std::vector<uint8_t*> scratchKeep;
  for(int round = 0; round < rounds; round++)
  {
    auto* buf = (uint8_t*)std::malloc(kBigBytes);
    for(size_t i = 0; i < kBigBytes; i += 4)
    {
      buf[i + 0] = 200;
      buf[i + 1] = 100;
      buf[i + 2] = 50;
      buf[i + 3] = 255;
    }
    NDIlib_video_frame_v2_t f{};
    Ndi::describeVideoFrame("RGBA", buf, kBigW, kBigH, 4 * kBigW, f);
    f.frame_rate_N = 60000;
    f.frame_rate_D = 1000;
    NDIlib_send_send_video_async_v2(sender, &f);

    if(mode == kFlushThenFree)
    {
      // What Ndi::Sender::flush_async() does: send_video_async(NULL) is one of
      // the SDK's documented synchronising events, so it has let go of this
      // buffer by the time the call returns and freeing is safe.
      NDIlib_send_send_video_async_v2(sender, nullptr);
      std::free(buf);
    }
    else if(mode == kFreeUnsynchronised)
    {
      std::free(buf);  // the order ~OutputNode used before the fix
    }
    else
    {
      kept.push_back(buf);  // released only after send_destroy
    }

    // Whichever branch was taken, the process carries on allocating. In the
    // freed case that is what hands the SDK's still-live pointer to somebody
    // else; in the kept case it changes nothing, which is the point of running
    // both. Same size and same call, so the two differ only in the free.
    auto* scratch = (uint8_t*)std::malloc(kBigBytes);
    std::memset(scratch, 0x00, kBigBytes);
    scratchKeep.push_back(scratch);

    for(int attempt = 0; attempt < 12; attempt++)
    {
      NDIlib_video_frame_v2_t got{};
      if(NDIlib_recv_capture_v3(recv, &got, nullptr, nullptr, 100)
         != NDIlib_frame_type_video)
        continue;
      if(got.p_data && got.xres == kBigW)
      {
        ++res.received;
        int wrong = 0;
        for(int y = 0; y < got.yres; y += 17)
          for(int x = 0; x < got.xres; x += 13)
          {
            const uint8_t* p
                = got.p_data + size_t(y) * got.line_stride_in_bytes + size_t(x) * 4;
            if(std::abs(p[0] - 200) > 12 || std::abs(p[1] - 100) > 12
               || std::abs(p[2] - 50) > 12)
              ++wrong;
          }
        if(wrong == 0)
          ++res.matched;
        else
          ++res.corrupted;
      }
      NDIlib_recv_free_video_v2(recv, &got);
      break;
    }
  }

  NDIlib_recv_destroy(recv);
  NDIlib_send_destroy(sender);  // the synchronising call, far too late
  for(auto* p : kept)
    std::free(p);
  for(auto* p : scratchKeep)
    std::free(p);
  return res;
}

void testFreeWhileOwned()
{
  const int rounds = 8;
  // Control first: the same sends, the same allocations, nothing freed early.
  // Without it a corrupted frame below could just as well be the SDK's RGBA
  // roundtrip being lossy.
  const RoundResult kept = sendAndFree(kKeepAlive, rounds);
  std::printf(
      "  buffers kept alive:        %d received, %d matched what was sent, %d "
      "corrupted\n",
      kept.received, kept.matched, kept.corrupted);
  CHECK(
      kept.received > 0,
      "no frames came back with the buffers kept alive; nothing below is measurable");
  CHECK(
      kept.corrupted == 0,
      "%d of %d frames came back corrupted even with the buffer kept alive, so a "
      "corrupted frame is not evidence of anything",
      kept.corrupted, kept.received);

  // The hazard itself, kept as a measurement rather than an assertion: this is
  // the sequence ~OutputNode used before the fix, and it is what makes the
  // flush load-bearing. It is EXPECTED to corrupt; if it ever stops, the check
  // below has gone vacuous and this says so.
  const RoundResult freed = sendAndFree(kFreeUnsynchronised, rounds);
  std::printf(
      "  freed with no synchronising call: %d received, %d matched, %d corrupted "
      "(expected to corrupt -- this is why the flush exists)\n",
      freed.received, freed.matched, freed.corrupted);
  CHECK(
      freed.received > 0, "no frames came back with the buffers freed; inconclusive");
  CHECK(
      freed.corrupted > 0,
      "freeing a buffer the SDK still owns did NOT corrupt anything this run, so "
      "the flushed case below proves nothing: either the SDK stopped reading "
      "asynchronously or the harness is no longer measuring what it thinks");

  // The fix: Sender::flush_async() before the free.
  const RoundResult flushed = sendAndFree(kFlushThenFree, rounds);
  std::printf(
      "  flush_async() then freed:         %d received, %d matched, %d corrupted\n",
      flushed.received, flushed.matched, flushed.corrupted);
  CHECK(
      flushed.received > 0,
      "no frames came back in the flushed case; inconclusive");
  CHECK(
      flushed.corrupted == 0,
      "%d of %d frames were corrupted even though send_video_async(NULL) ran "
      "before the free, so ~OutputNode's flush_async() does not actually end the "
      "SDK's claim on the buffer",
      flushed.corrupted, flushed.received);
}

/// The sequence ~OutputNode ran BEFORE the fix: async send, free the staging
/// frame, then send_destroy from ~Sender. Kept as a smoke check that it does not
/// crash outright -- the content check above is what shows the actual damage.
/// ~OutputNode now calls Sender::flush_async() before freeing, so this is a
/// record of the hazard, not a description of current behaviour.
void testShutdownOrder()
{
  std::printf("  the order ~OutputNode uses: async send -> free the frame -> "
              "send_destroy\n");
  std::fflush(stdout);

  const std::string name = "score-ndi-async-shutdown-" + std::to_string(::getpid());
  NDIlib_send_create_t cfg{};
  cfg.p_ndi_name = name.c_str();
  cfg.clock_video = false;
  cfg.clock_audio = false;
  NDIlib_send_instance_t sender = NDIlib_send_create(&cfg);
  CHECK(sender, "could not create a sender for the shutdown check");
  if(!sender)
    return;

  // The readback buffer is what gets sent; there is no staging frame any more,
  // because the GPU produces the wire bytes directly.
  auto* readback = (uint8_t*)std::malloc(kBytes);
  fill(readback, 255, 0, 0);

  NDIlib_video_frame_v2_t f{};
  CHECK(
      Ndi::describeVideoFrame("RGBA", readback, kWidth, kHeight, 4 * kWidth, f),
      "RGBA was refused");
  f.frame_rate_N = 60000;
  f.frame_rate_D = 1000;
  NDIlib_send_send_video_async_v2(sender, &f);

  // ~OutputNode's body, before the fix: no synchronising call has happened.
  std::free(readback);

  // Only now do the members go: Ndi::Sender::~Sender -> send_destroy.
  NDIlib_send_destroy(sender);
  std::printf("  survived (no crash); see the content check above for the damage\n");
}

/// The order that honours the contract, for comparison: send_destroy first.
void testShutdownOrderCorrected()
{
  const std::string name = "score-ndi-async-shutdown-ok-" + std::to_string(::getpid());
  NDIlib_send_create_t cfg{};
  cfg.p_ndi_name = name.c_str();
  cfg.clock_video = false;
  cfg.clock_audio = false;
  NDIlib_send_instance_t sender = NDIlib_send_create(&cfg);
  if(!sender)
    return;

  auto* readback = (uint8_t*)std::malloc(kBytes);
  fill(readback, 255, 0, 0);

  NDIlib_video_frame_v2_t f{};
  Ndi::describeVideoFrame("RGBA", readback, kWidth, kHeight, 4 * kWidth, f);
  f.frame_rate_N = 60000;
  f.frame_rate_D = 1000;
  NDIlib_send_send_video_async_v2(sender, &f);

  NDIlib_send_destroy(sender);  // the synchronising call comes first
  std::free(readback);
  std::printf("  control: send_destroy before the free is clean\n");
}
}

int main()
{
  if(!NDIlib_initialize())
  {
    std::printf("  skip: the NDI runtime would not initialise on this machine\n");
    return 77;
  }

  const std::string name = "score-ndi-async-" + std::to_string(::getpid());
  NDIlib_send_create_t sendCfg{};
  sendCfg.p_ndi_name = name.c_str();
  sendCfg.clock_video = false;
  sendCfg.clock_audio = false;
  NDIlib_send_instance_t sender = NDIlib_send_create(&sendCfg);
  if(!sender)
  {
    std::printf("  skip: could not create an NDI sender\n");
    return 77;
  }
  const NDIlib_source_t* self = NDIlib_send_get_source_name(sender);
  if(!self)
  {
    std::printf("  skip: the sender reported no source descriptor\n");
    NDIlib_send_destroy(sender);
    return 77;
  }
  NDIlib_recv_create_v3_t recvCfg{};
  recvCfg.source_to_connect_to = *self;
  recvCfg.color_format = NDIlib_recv_color_format_RGBX_RGBA;
  recvCfg.bandwidth = NDIlib_recv_bandwidth_highest;
  recvCfg.allow_video_fields = false;
  NDIlib_recv_instance_t recv = NDIlib_recv_create_v3(&recvCfg);
  if(!recv)
  {
    std::printf("  skip: could not create an NDI receiver\n");
    NDIlib_send_destroy(sender);
    return 77;
  }

  std::printf("-- is the async buffer still read after the send returns? --\n");
  testAsyncOwnershipIsObservable(sender, recv);

  NDIlib_recv_destroy(recv);
  NDIlib_send_destroy(sender);

  std::printf("\n-- freeing a buffer the SDK still owns --\n");
  testFreeWhileOwned();

  std::printf("\n-- the ~OutputNode shutdown sequence, verbatim --\n");
  testShutdownOrderCorrected();
  testShutdownOrder();

  NDIlib_destroy();
  std::printf(
      "\nndi async send: %s (%d failure%s)\n", failures ? "FAILED" : "passed", failures,
      failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
