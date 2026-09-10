#pragma once

/**
 * @file ReadbackPool.hpp
 * @brief The readback buffer ownership state machine, on its own.
 *
 * TESTABILITY SEAM (added by review, no behaviour change): the states and the
 * transitions below were lifted verbatim out of Ndi::OutputNode so that they can
 * be driven from a test without a GPU, a renderer or the NDI SDK. Every atomic,
 * every memory order and every branch is the same as it was inline in
 * OutputNode::render() and OutputNode::senderThreadFunc(); the only change is
 * that the code now has a name. tests/ReadbackPoolTest.cpp drives it.
 *
 * A frame handed to send_video_async stays owned by the SDK until the next send,
 * so the renderer must never target a buffer that is still spoken for; the
 * states below are what enforce that, and they are what let the RGBA path send
 * the readback itself with no copy at all.
 *
 *   Free     - the renderer may read back into it
 *   Queued   - readback done, waiting for the sender
 *   Sending  - the sender is converting/submitting from it
 *   InFlight - handed to the SDK, still being read
 */

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace Ndi
{
enum class Buf : std::uint8_t
{
  Free,
  Queued,
  Sending,
  InFlight
};

/**
 * @brief Ownership of the readback buffers, shared between render and sender.
 *
 * @tparam N how many buffers. The production value is 4: at most one Queued, one
 *         Sending and one InFlight, so a Free one always exists -- that claim is
 *         what tests/ReadbackPoolTest.cpp is mostly about.
 */
template <int N>
struct ReadbackPool
{
  static constexpr int kBuffers = N;

  std::atomic<Buf> m_bufState[kBuffers]{};
  int m_readbackIndex{0};  // Next buffer for readback

  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_frameReady{false};
  std::atomic<int> m_sendIndex{-1};  // Buffer ready for sender
  std::atomic<int> m_inFlight{-1};   // Buffer the SDK is still reading

  /// Render thread: hand the buffer just read back to the sender.
  void queueForSend()
  {
    // Signal sender thread with current readback buffer
    {
      std::lock_guard lock(m_mutex);
      // A frame the sender never got to is dropped, not queued behind this
      // one: for a live output the newest frame is the only one worth
      // sending. Give its buffer back.
      const int stale = m_sendIndex.load(std::memory_order_relaxed);
      if(stale >= 0 && m_bufState[stale].load(std::memory_order_relaxed) == Buf::Queued)
        m_bufState[stale].store(Buf::Free, std::memory_order_release);

      m_bufState[m_readbackIndex].store(Buf::Queued, std::memory_order_release);
      m_sendIndex = m_readbackIndex;
      m_frameReady = true;
    }
    m_cv.notify_one();
  }

  /// Render thread: the buffer the next readback should target.
  int advanceToFreeBuffer()
  {
    // Advance to the next Free buffer, stepping over any the sender or the SDK
    // still owns. kBuffers guarantees this finds one.
    for(int i = 1; i <= kBuffers; i++)
    {
      const int cand = (m_readbackIndex + i) % kBuffers;
      if(m_bufState[cand].load(std::memory_order_acquire) == Buf::Free)
      {
        m_readbackIndex = cand;
        break;
      }
    }
    return m_readbackIndex;
  }

  /// Sender thread: block until there is something to send. -1 to go round again
  /// (or to stop, which the caller's while(m_running) sees).
  int acquireForSending()
  {
    std::unique_lock lock(m_mutex);
    m_cv.wait(lock, [this] { return m_frameReady.load() || !m_running.load(); });

    if(!m_running)
      return -1;

    int idx = m_sendIndex.load();
    if(idx >= 0)
    {
      m_frameReady = false;
      m_sendIndex = -1;
      m_bufState[idx].store(Buf::Sending, std::memory_order_release);
      return idx;
    }
    m_frameReady = false;
    return -1;
  }

  /// Sender thread: the frame could not be described, so nothing was sent and
  /// the SDK still holds whatever it held before.
  void releaseUnsent(int idx)
  {
    m_bufState[idx].store(Buf::Free, std::memory_order_release);
  }

  /// Sender thread: idx has just been handed to send_video_async, which is the
  /// synchronising event for the buffer handed to the previous one.
  void markSent(int idx)
  {
    const int prev = m_inFlight.exchange(idx, std::memory_order_acq_rel);
    m_bufState[idx].store(Buf::InFlight, std::memory_order_release);
    if(prev >= 0)
      m_bufState[prev].store(Buf::Free, std::memory_order_release);
  }

  /// Put every buffer back to Free and forget any in-flight frame.
  ///
  /// A render-list rebuild destroys the renderer and builds a new one, which
  /// reads back into buffer 0 again -- but the pool outlives it. Without this,
  /// the first render() after a rebuild reads into buffer 0 while the leftover
  /// m_readbackIndex queues some other buffer holding a pre-rebuild frame, and
  /// the freshly started sender wakes on a stale m_frameReady and sends it.
  /// Call before starting the sender thread.
  void reset()
  {
    std::lock_guard lock(m_mutex);
    for(auto& st : m_bufState)
      st.store(Buf::Free, std::memory_order_release);
    m_readbackIndex = 0;
    m_sendIndex = -1;
    m_inFlight = -1;
    m_frameReady = false;
  }

  void requestStop()
  {
    // The flag MUST be stored under the same mutex the waiter holds while it
    // evaluates the predicate. Storing it outside leaves a window between the
    // sender deciding the predicate is false and registering inside
    // pthread_cond_wait: a notify that lands there has no waiter, is dropped,
    // and the sender then sleeps forever on an already-true predicate while
    // join() blocks behind it. stopRendering() runs on every render-list
    // rebuild, so this is a live-edit hang, not just a shutdown one.
    {
      std::lock_guard lock(m_mutex);
      m_running = false;
    }
    m_cv.notify_all();
  }
};
}
