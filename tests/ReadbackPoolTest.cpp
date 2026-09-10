// Drives Ndi::ReadbackPool -- the ownership state machine that lets the RGBA
// path send a readback buffer with no copy -- without a GPU, a renderer or the
// NDI SDK.
//
// The thing being checked is the claim the pool is built on: "a frame handed to
// send_video_async stays owned by the SDK until the next send, so the renderer
// must never target a buffer that is still spoken for". Two ways:
//
//  * a model check (single-threaded, deterministic) that replays the render and
//    sender step sequences and asserts, at every point where the renderer picks
//    a buffer, that the buffer it picked was Free;
//  * a stress run with a real render thread, a real sender thread and a model of
//    the SDK's asynchronous reader that holds a buffer from one send to the next
//    exactly as the SDK contract says it does. The renderer writes a distinct
//    pattern per frame, so a buffer handed over while the renderer can still
//    reach it shows up as content changing under the reader -- and, under
//    ThreadSanitizer, as a data race on the buffer bytes.
//
// The pool is templated on the buffer count so the same checks can be run at
// N = 3 as well as the production N = 4. That is deliberate: the "four buffers
// is enough" claim is only a comment in the source, and a test that passes at
// N = 4 and fails at N = 3 is what turns it into a checked property.
#include <Ndi/ReadbackPool.hpp>

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <thread>
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

using Ndi::Buf;

namespace
{
const char* name(Buf b)
{
  switch(b)
  {
    case Buf::Free:
      return "Free";
    case Buf::Queued:
      return "Queued";
    case Buf::Sending:
      return "Sending";
    case Buf::InFlight:
      return "InFlight";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// 1. The states the pool starts in.
//
// createRenderer() binds the renderer to m_readback[0] and m_readbackIndex is 0,
// so buffer 0 has to be Free before the first render or the very first frame is
// read back into a buffer that is not ours.
// ---------------------------------------------------------------------------
void testInitialState()
{
  // Heap, not stack: std::mutex has a trivial destructor, so ThreadSanitizer
  // cannot tell two successive stack-allocated pools at the same address apart
  // and reports the second lock as a double lock of the first.
  auto owned = std::make_unique<Ndi::ReadbackPool<4>>();
  auto& pool = *owned;
  for(int i = 0; i < 4; i++)
    CHECK(
        pool.m_bufState[i].load() == Buf::Free, "buffer %d starts %s, not Free", i,
        name(pool.m_bufState[i].load()));
  CHECK(pool.m_readbackIndex == 0, "first readback must target buffer 0");
  CHECK(pool.m_sendIndex.load() == -1, "nothing may be queued before the first render");
  CHECK(pool.m_inFlight.load() == -1, "nothing may be in flight before the first send");
  CHECK(!pool.m_frameReady.load(), "no frame may be ready before the first render");
  std::printf("  ok initial states\n");
}

// ---------------------------------------------------------------------------
// 2. Model check.
//
// The render thread and the sender thread are each reduced to the sequence of
// pool calls they make, and every interleaving of those sequences is explored
// depth-first. At each step the model tracks, independently of the pool, which
// buffer the SDK is actually still reading -- per the send_video_async contract,
// that is the buffer handed to the most recent send, until the send after it --
// and asserts:
//
//   INV1  the buffer the renderer is about to read back into is Free
//   INV2  the buffer the renderer is about to read back into is not the one the
//         SDK is still reading  (the bug class that has recurred here)
//   INV3  the buffer the sender is converting from is not one the renderer is
//         about to overwrite
//   INV4  markSent never frees the buffer it has just marked InFlight, which is
//         what happens if the same index is sent twice in a row
// ---------------------------------------------------------------------------
template <int N>
struct Model
{
  // Where each thread is in its own step sequence.
  //   render:  0 = about to read back into m_readbackIndex, then queue, then advance
  //   sender:  0 = idle/waiting, 1 = holding idx (Sending), 2 = sent
  struct State
  {
    Ndi::ReadbackPool<N>* pool;
    int senderStage{0};
    int senderIdx{-1};
    int sdkReading{-1};  // what the SDK is still reading, tracked outside the pool
    int rendererTarget{0};
  };

  int violations{0};
  int statesSeen{0};
  int depthLimit{14};
  char firstViolation[256]{};

  void fail(const char* what, const Ndi::ReadbackPool<N>& p, int a, int b)
  {
    if(violations == 0)
      std::snprintf(
          firstViolation, sizeof(firstViolation),
          "%s (buffers=%d, a=%d[%s] b=%d[%s], readbackIndex=%d, sendIndex=%d, "
          "inFlight=%d)",
          what, N, a, a >= 0 ? name(p.m_bufState[a].load()) : "-", b,
          b >= 0 ? name(p.m_bufState[b].load()) : "-", p.m_readbackIndex,
          p.m_sendIndex.load(), p.m_inFlight.load());
    ++violations;
  }

  // A snapshot lets the search restore the world after exploring a branch.
  struct Snapshot
  {
    Buf state[N];
    int readbackIndex, senderStage, senderIdx, sdkReading, rendererTarget;
    int sendIndex, inFlight;
    bool frameReady;
  };

  Snapshot save(const State& s)
  {
    Snapshot sn{};
    for(int i = 0; i < N; i++)
      sn.state[i] = s.pool->m_bufState[i].load();
    sn.readbackIndex = s.pool->m_readbackIndex;
    sn.sendIndex = s.pool->m_sendIndex.load();
    sn.inFlight = s.pool->m_inFlight.load();
    sn.frameReady = s.pool->m_frameReady.load();
    sn.senderStage = s.senderStage;
    sn.senderIdx = s.senderIdx;
    sn.sdkReading = s.sdkReading;
    sn.rendererTarget = s.rendererTarget;
    return sn;
  }
  void restore(State& s, const Snapshot& sn)
  {
    for(int i = 0; i < N; i++)
      s.pool->m_bufState[i].store(sn.state[i]);
    s.pool->m_readbackIndex = sn.readbackIndex;
    s.pool->m_sendIndex.store(sn.sendIndex);
    s.pool->m_inFlight.store(sn.inFlight);
    s.pool->m_frameReady.store(sn.frameReady);
    s.senderStage = sn.senderStage;
    s.senderIdx = sn.senderIdx;
    s.sdkReading = sn.sdkReading;
    s.rendererTarget = sn.rendererTarget;
  }

  /// One whole render(): fill the current buffer, queue it, pick the next.
  void stepRender(State& s)
  {
    auto& p = *s.pool;

    // The GPU writes into the buffer the previous advance chose.
    const int target = s.rendererTarget;
    if(p.m_bufState[target].load() != Buf::Free)
      fail("renderer read back into a buffer that is not Free", p, target, -1);
    if(target == s.sdkReading)
      fail("renderer read back into a buffer the SDK is still reading", p, target, -1);
    if(s.senderStage == 1 && target == s.senderIdx)
      fail("renderer read back into the buffer the sender is converting", p, target, -1);

    p.queueForSend();
    s.rendererTarget = p.advanceToFreeBuffer();
    if(p.m_bufState[s.rendererTarget].load() != Buf::Free)
      fail(
          "advanceToFreeBuffer handed back a buffer that is not Free", p,
          s.rendererTarget, -1);
  }

  void stepSenderTake(State& s)
  {
    auto& p = *s.pool;
    const int idx = p.acquireForSending();
    if(idx < 0)
      return;
    s.senderIdx = idx;
    s.senderStage = 1;
    // describeVideoFrame reads the readback here.
    if(idx == s.rendererTarget)
      fail("sender is converting from the buffer the renderer will overwrite", p, idx, -1);
  }

  void stepSenderSend(State& s)
  {
    auto& p = *s.pool;
    const int idx = s.senderIdx;
    const int before = p.m_inFlight.load();
    // send_video_async: the SDK releases what it had and takes idx.
    s.sdkReading = idx;
    p.markSent(idx);
    if(before == idx)
      fail("markSent freed the buffer it just marked InFlight", p, idx, before);
    if(p.m_bufState[idx].load() != Buf::InFlight)
      fail("buffer is not InFlight after markSent", p, idx, -1);
    s.senderStage = 0;
    s.senderIdx = -1;
  }

  void explore(State& s, int depth)
  {
    ++statesSeen;
    if(depth >= depthLimit || violations > 0)
      return;

    // Branch 1: the render thread runs a frame.
    {
      const auto sn = save(s);
      stepRender(s);
      explore(s, depth + 1);
      restore(s, sn);
    }
    // Branch 2: the sender thread takes the next step it can.
    {
      const auto sn = save(s);
      if(s.senderStage == 0)
      {
        if(s.pool->m_frameReady.load())
        {
          stepSenderTake(s);
          explore(s, depth + 1);
        }
      }
      else
      {
        stepSenderSend(s);
        explore(s, depth + 1);
      }
      restore(s, sn);
    }
  }
};

template <int N>
int modelCheck(const char* label, bool expectClean)
{
  auto owned = std::make_unique<Ndi::ReadbackPool<N>>();
  auto& pool = *owned;
  pool.m_running = true;
  Model<N> m;
  typename Model<N>::State s{&pool};
  m.explore(s, 0);
  std::printf(
      "  %-28s N=%d  %d interleavings, %d violation%s%s\n", label, N, m.statesSeen,
      m.violations, m.violations == 1 ? "" : "s",
      m.violations ? "" : "  <- invariants hold");
  if(m.violations)
    std::printf("      first: %s\n", m.firstViolation);
  if(expectClean)
    CHECK(m.violations == 0, "%s: the ownership invariants do not hold at N=%d", label, N);
  return m.violations;
}

// ---------------------------------------------------------------------------
// 3. What the fallback in advanceToFreeBuffer() does when nothing is Free.
//
// The loop runs i = 1..kBuffers, so its last candidate is m_readbackIndex
// itself; when no buffer is Free it takes no branch at all and m_readbackIndex
// is left as it was. The renderer is then pointed at a buffer that is Queued,
// Sending or InFlight -- silently. This pins that behaviour so a change to
// kBuffers, or a new state that can leak, cannot make it happen unnoticed.
// ---------------------------------------------------------------------------
void testStarvationFallback()
{
  auto owned = std::make_unique<Ndi::ReadbackPool<4>>();
  auto& pool = *owned;
  pool.m_readbackIndex = 1;
  pool.m_bufState[0].store(Buf::InFlight);
  pool.m_bufState[1].store(Buf::Queued);
  pool.m_bufState[2].store(Buf::Sending);
  pool.m_bufState[3].store(Buf::InFlight);

  const int got = pool.advanceToFreeBuffer();
  const Buf st = pool.m_bufState[got].load();
  std::printf(
      "  advanceToFreeBuffer with nothing Free -> buffer %d, state %s (silent reuse)\n",
      got, name(st));
  // This is a pin, not a demand. The model check above says the state below
  // cannot be reached at N=4, so asserting Free here would fail on an input the
  // pool never sees. What is worth recording is that if it ever were reached --
  // one more state that can leak, or kBuffers dropped to 3 -- the renderer is
  // handed a buffer that is still spoken for and nothing says so. The model
  // check at N=3 is the guard; this is the failure mode it guards against.
  CHECK(
      got == pool.m_readbackIndex && st != Buf::Free,
      "advanceToFreeBuffer's no-Free-buffer fallback changed; it used to leave "
      "m_readbackIndex alone, which silently reuses a buffer in state %s",
      name(st));
}

// ---------------------------------------------------------------------------
// 3b. A render-list rebuild.
//
// Graph::createRenderList calls stopRendering() on every output, tears the
// render list down, rebuilds it -- which calls OutputNode::createRenderer, and
// that binds the new InvertYRenderer to m_readback[0], hardcoded -- and then
// calls startRendering() again. That happens on every live edit and every
// backend switch, so it is a routine path, not a corner.
//
// Nothing resets the pool across it. m_readbackIndex keeps whatever value it had
// when rendering stopped, while the renderer is now writing into buffer 0. The
// first render() after a rebuild therefore reads back into buffer 0 and queues
// m_readbackIndex, which is a different buffer holding a frame from before the
// rebuild -- or nothing at all.
// ---------------------------------------------------------------------------
void testRestartAfterRebuild()
{
  auto owned = std::make_unique<Ndi::ReadbackPool<4>>();
  auto& pool = *owned;
  pool.m_running = true;

  // Run a few frames so m_readbackIndex walks away from 0, the way it does in
  // any session that has been rendering.
  int rendererTarget = 0;
  for(int f = 0; f < 3; f++)
  {
    pool.queueForSend();
    const int idx = pool.acquireForSending();
    if(idx >= 0)
      pool.markSent(idx);
    rendererTarget = pool.advanceToFreeBuffer();
  }
  const int beforeStop = pool.m_readbackIndex;
  std::printf(
      "  after 3 frames: m_readbackIndex=%d, renderer bound to %d\n", beforeStop,
      rendererTarget);

  // stopRendering(): join the sender, nothing else.
  pool.requestStop();

  // The rebuild. createRenderer constructs the InvertYRenderer over
  // m_readback[0], so from here the GPU writes buffer 0 whatever the pool says.
  const int reboundTo = 0;
  pool.m_running = true;

  // The first render() after the rebuild: the readback lands in `reboundTo`, and
  // this is the buffer the pool then hands to the sender.
  pool.queueForSend();
  const int queued = pool.m_sendIndex.load();
  std::printf(
      "  first render after the rebuild: readback landed in %d, pool queued %d\n",
      reboundTo, queued);
  CHECK(
      queued == reboundTo,
      "the rebuild rebinds the renderer to m_readback[0] but leaves "
      "m_readbackIndex at %d, so the first frame after every render-list rebuild "
      "is sent from buffer %d while the new frame is in buffer %d",
      beforeStop, queued, reboundTo);

  // And whatever the sender was left mid-way through is still there.
  std::printf(
      "  leftover across the stop: frameReady=%d sendIndex=%d inFlight=%d, states "
      "%d%d%d%d\n",
      (int)pool.m_frameReady.load(), pool.m_sendIndex.load(), pool.m_inFlight.load(),
      (int)pool.m_bufState[0].load(), (int)pool.m_bufState[1].load(),
      (int)pool.m_bufState[2].load(), (int)pool.m_bufState[3].load());

  pool.requestStop();
}

// ---------------------------------------------------------------------------
// 4. Stress: a real render thread, a real sender thread, and a model of the
//    SDK's reader that holds a buffer from one send to the next.
// ---------------------------------------------------------------------------
constexpr int kPayload = 4096;

struct SdkModel
{
  // Holds the buffer it was handed until the next send, reading it the whole
  // time, which is what NDIlib_send_send_video_async_v2 documents.
  std::vector<uint8_t>* buffers{};
  std::atomic<int> pending{-1};
  std::atomic<int> acked{-2};
  std::atomic<bool> alive{true};
  std::atomic<int> corruptions{0};
  std::thread reader;

  void start()
  {
    reader = std::thread([this] {
      int cur = -1;
      while(alive.load(std::memory_order_acquire))
      {
        const int next = pending.load(std::memory_order_acquire);
        if(next == cur)
        {
          std::this_thread::yield();
          continue;
        }
        cur = next;
        acked.store(cur, std::memory_order_release);
        if(cur < 0)
          continue;
        // Read it for as long as we own it, and check nobody rewrites it.
        auto& b = buffers[cur];
        const uint8_t first = b[0];
        while(pending.load(std::memory_order_acquire) == cur
              && alive.load(std::memory_order_acquire))
        {
          for(int i = 0; i < kPayload; i += 512)
          {
            if(b[i] != first)
            {
              corruptions.fetch_add(1);
              break;
            }
          }
        }
      }
    });
  }

  /// The synchronising call: returns only once the reader has let go of the
  /// buffer it had, so a caller that frees the previous buffer afterwards is
  /// within the contract.
  void send(int idx)
  {
    pending.store(idx, std::memory_order_release);
    while(acked.load(std::memory_order_acquire) != idx && alive.load())
      std::this_thread::yield();
  }

  void stop()
  {
    alive.store(false, std::memory_order_release);
    pending.store(-1, std::memory_order_release);
    if(reader.joinable())
      reader.join();
  }
};

template <int N>
void testStress(const char* label, int frames)
{
  auto owned = std::make_unique<Ndi::ReadbackPool<N>>();
  auto& pool = *owned;
  std::vector<uint8_t> buffers[N];
  for(auto& b : buffers)
    b.assign(kPayload, 0);

  SdkModel sdk;
  sdk.buffers = buffers;
  sdk.start();

  std::atomic<int> senderRead{0};
  std::atomic<int> sent{0};
  std::atomic<bool> senderTouchedWrongBuffer{false};

  pool.m_running = true;
  std::thread sender([&] {
    std::mt19937 rng(1234);
    while(pool.m_running)
    {
      const int idx = pool.acquireForSending();
      if(idx < 0)
        continue;

      // describeVideoFrame: reads the readback (sws_scale, or nothing at all on
      // the RGBA path where the readback bytes go straight out).
      auto& b = buffers[idx];
      const uint8_t first = b[0];
      for(int i = 0; i < kPayload; i += 256)
        if(b[i] != first)
          senderTouchedWrongBuffer.store(true);
      senderRead.fetch_add(1);
      if(rng() % 8 == 0)
        std::this_thread::yield();

      sdk.send(idx);
      pool.markSent(idx);
      sent.fetch_add(1);
    }
  });

  // Run until the sender has actually got through `frames` sends, so the number
  // of handovers exercised does not depend on how the two threads happen to be
  // scheduled. Drop-oldest means most rendered frames never get sent.
  std::mt19937 rng(4321);
  int rendered = 0;
  for(; sent.load() < frames && rendered < frames * 200; rendered++)
  {
    // The GPU readback writes the buffer the last advance chose.
    std::memset(buffers[pool.m_readbackIndex].data(), uint8_t(rendered & 0xff), kPayload);
    pool.queueForSend();
    pool.advanceToFreeBuffer();
    if(rng() % 4 == 0)
      std::this_thread::yield();
  }

  pool.requestStop();
  if(sender.joinable())
    sender.join();
  sdk.stop();

  std::printf(
      "  %-28s N=%d  %d rendered, %d sent, %d corruptions\n", label, N, rendered,
      sent.load(), sdk.corruptions.load());
  CHECK(
      sdk.corruptions.load() == 0,
      "%s: N=%d, the renderer rewrote a buffer %d time(s) while the SDK model was "
      "still reading it",
      label, N, sdk.corruptions.load());
  CHECK(
      !senderTouchedWrongBuffer.load(),
      "%s: N=%d, the buffer changed under the sender while it was converting", label, N);
  CHECK(sent.load() > 0, "%s: N=%d, nothing was ever sent", label, N);
}

// ---------------------------------------------------------------------------
// 5. requestStop() writes the predicate without holding the mutex.
//
// senderThreadFunc waits on `m_frameReady || !m_running`. requestStop() -- which
// is what OutputNode::stopRendering() and ~OutputNode do -- stores
// m_running = false and calls notify_one() without taking m_mutex. Between the
// waiter evaluating the predicate as false and registering itself on the
// condition variable, the waiter holds only m_mutex, which requestStop() does
// not want; a notify_one() that lands in that window has no waiter to wake and
// is dropped. The waiter then sleeps on a predicate that is already true, and
// join() never returns.
//
// The window is a handful of instructions wide, so racing for it is unreliable.
// Instead pthread_cond_wait is interposed: the hook parks the waiter exactly
// there -- predicate already evaluated false, m_mutex held, not yet registered
// -- while requestStop() runs, then lets it descend. That is the real
// ReadbackPool code on both sides; only the scheduling is forced.
// ---------------------------------------------------------------------------
std::atomic<bool> g_holdInWindow{false};
std::atomic<bool> g_reachedWindow{false};

/// Runs one shutdown, with the waiter parked in the pre-registration window or
/// not. Returns true if the waiter came back.
bool runShutdown(bool park)
{
  using namespace std::chrono;
  auto owned = std::make_unique<Ndi::ReadbackPool<4>>();
  auto& pool = *owned;
  pool.m_running = true;
  pool.m_frameReady = false;

  std::atomic<bool> returned{false};
  g_reachedWindow = false;
  g_holdInWindow = park;

  std::thread waiter([&] {
    pool.acquireForSending();
    returned.store(true, std::memory_order_release);
  });

  if(park)
  {
    while(!g_reachedWindow.load(std::memory_order_acquire))
      std::this_thread::yield();
  }
  else
  {
    // Give the waiter time to get all the way into the wait.
    std::this_thread::sleep_for(milliseconds(20));
  }

  pool.requestStop();
  g_holdInWindow.store(false, std::memory_order_release);

  const auto deadline = steady_clock::now() + milliseconds(500);
  while(!returned.load(std::memory_order_acquire) && steady_clock::now() < deadline)
    std::this_thread::yield();

  const bool ok = returned.load(std::memory_order_acquire);
  if(!ok)
  {
    // Rescue the thread so this reports instead of hanging. This notify is the
    // one requestStop() dropped.
    for(int k = 0; k < 500 && !returned.load(); k++)
    {
      pool.m_cv.notify_all();
      std::this_thread::sleep_for(milliseconds(1));
    }
  }
  waiter.join();
  return ok;
}

void testStopWakeup()
{
  // Control first: with the waiter fully inside the wait, requestStop() works.
  // Without this, a hang below could just as well be the hook's fault.
  const bool control = runShutdown(false);
  std::printf(
      "  control (waiter already registered):      %s\n",
      control ? "stop woke it" : "did not come back");
  CHECK(control, "requestStop() failed to wake a waiter that was already registered; "
                 "the rest of this test cannot be trusted");

  const bool parked = runShutdown(true);
  std::printf(
      "  waiter parked pre-registration:           %s\n",
      parked ? "stop woke it" : "notify dropped, join() would hang");
  CHECK(
      parked,
      "requestStop() stored m_running without holding m_mutex; the notify was lost "
      "and the sender thread slept on an already-true predicate, so "
      "OutputNode::stopRendering() and ~OutputNode hang in join() forever");
}
}

// The hook. Placed in the executable so it takes precedence over libc for every
// pthread_cond_wait in the process, including the one std::condition_variable
// makes from inside libstdc++.
extern "C" int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m)
{
  if(g_holdInWindow.load(std::memory_order_acquire))
  {
    g_reachedWindow.store(true, std::memory_order_release);
    while(g_holdInWindow.load(std::memory_order_acquire))
      std::this_thread::yield();
  }
  using fn_t = int (*)(pthread_cond_t*, pthread_mutex_t*);
  // Under a sanitizer, go through the sanitizer's own interceptor rather than
  // straight to libc: RTLD_NEXT would skip it, the sanitizer would never see the
  // mutex being released, and every later lock of it would be reported as a
  // double lock.
  static fn_t real = [] {
    if(auto* tsan = dlsym(RTLD_DEFAULT, "__interceptor_pthread_cond_wait"))
      return (fn_t)tsan;
    return (fn_t)dlsym(RTLD_NEXT, "pthread_cond_wait");
  }();
  return real(c, m);
}

namespace
{
}

int main(int argc, char** argv)
{
  const bool quick = argc > 1 && std::strcmp(argv[1], "--quick") == 0;

  // Validation of the stress harness itself. A harness that has never caught an
  // aliased buffer is not evidence that there is none, so this mode runs the
  // same threads against a three-buffer pool, where the model check says the
  // renderer does get handed a buffer that is still spoken for. It has to see
  // corruption -- and under ThreadSanitizer, a data race on the buffer bytes.
  if(argc > 1 && std::strcmp(argv[1], "--prove-instrument") == 0)
  {
    std::printf("-- proving the stress harness can see an aliased buffer --\n");
    testStress<3>("three-buffer pool", 3000);
    std::printf(
        "%s: the harness %s corruption at N=3\n",
        failures ? "as expected" : "PROBLEM", failures ? "saw" : "did not see");
    return failures ? 0 : 1;
  }

  testInitialState();

  std::printf("\n-- model check (all interleavings of render and sender) --\n");
  modelCheck<4>("production pool", true);
  // Same check with one buffer fewer. It has to find a violation: if it does
  // not, this test cannot distinguish a pool that is safe from a pool whose
  // invariants are never exercised, and it is worth nothing at N=4 either.
  const int atThree = modelCheck<3>("one buffer fewer", false);
  CHECK(
      atThree > 0,
      "the model check found no violation at N=3, so it is not actually checking "
      "anything at N=4 either");

  std::printf("\n-- starvation fallback --\n");
  testStarvationFallback();

  std::printf("\n-- render-list rebuild --\n");
  testRestartAfterRebuild();

  std::printf("\n-- stress (render thread + sender thread + SDK reader) --\n");
  testStress<4>("production pool", quick ? 300 : 3000);

  std::printf("\n-- shutdown wakeup --\n");
  testStopWakeup();

  std::printf(
      "\nndi readback pool: %s (%d failure%s)\n", failures ? "FAILED" : "passed",
      failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
