#pragma once

/**
 * @file WireEncodeRenderer.hpp
 * @brief Output renderer that converts to the NDI wire format on the GPU and
 *        reads it back into the buffer that is sent.
 *
 * score::gfx has an encoder per wire format, so this drives whichever the
 * output's format calls for and folds the Y-flip into the same shader. Every
 * encoder is asked for its contiguous-framestore form, so its output texture
 * IS the framestore.
 *
 * The bytes then go from that texture into one of the output node's send
 * slots, which is the memory handed to send_video_async, with no copy in
 * between where the backend allows it (Gfx/Graph/RhiTextureReadback.hpp):
 *
 *  - Vulkan, D3D12: the slot's pages are imported once and the GPU copies the
 *    texture into them. The bytes are there when endOffscreenFrame returns.
 *  - OpenGL, D3D11: the copy lands in a driver buffer and finishing it costs a
 *    fence wait and a memcpy. Finishing a frame's copy at the start of the
 *    next frame rather than right after it leaves the GPU a whole frame period
 *    to complete it, so the wait is over before it starts and the render
 *    thread never stalls on the GPU.
 *  - anything else, or a slot the backend cannot import: QRhi's own readback
 *    into the slot's QRhiReadbackResult, which is what gets sent then.
 */

#include <Gfx/Graph/NodeRenderer.hpp>
#include <Gfx/Graph/OutputNode.hpp>
#include <Gfx/Graph/RenderList.hpp>
#include <Gfx/Graph/RenderState.hpp>
#include <Gfx/Graph/RhiTextureReadback.hpp>
#include <Gfx/Graph/encoders/ColorSpaceOut.hpp>
#include <Ndi/NdiColorSpace.hpp>
#include <Ndi/ReadbackPool.hpp>
#include <Ndi/WireEncode.hpp>

#include <QtGui/private/qrhi_p.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Ndi
{

/**
 * @brief One buffer of the output node's readback pool.
 *
 * `host` is what the SDK reads when the GPU could write it directly. It is
 * allocated by the renderer the first time the direct path is taken, and
 * then kept for the output's whole life, so a backend that never takes that
 * path costs no memory and steady-state frames allocate nothing. `readback`
 * is QRhi's destination when the GPU could not write `host`.
 * `data`, `size` and `stride` describe whichever of the two holds the frame;
 * the renderer sets them before the slot is queued for sending.
 */
struct SendSlot
{
  /// Alignment and size granularity of `host`: D3D12 needs 64 KiB, Vulkan
  /// the device's import alignment, 4 KiB on every known implementation.
  static constexpr std::size_t kAlignment = 65536;

  // D3D12 imports `host` with OpenExistingHeapFromAddress, which accepts
  // nothing but VirtualAlloc regions. VirtualAlloc places a region on the
  // 64 KiB allocation granularity, so kAlignment comes with it.
  static std::uint8_t* allocateHost(std::size_t bytes) noexcept
  {
#if defined(_WIN32)
    return static_cast<std::uint8_t*>(
        ::VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    return static_cast<std::uint8_t*>(
        ::operator new(bytes, std::align_val_t{kAlignment}, std::nothrow));
#endif
  }

  struct HostFree
  {
    void operator()(std::uint8_t* p) const noexcept
    {
#if defined(_WIN32)
      ::VirtualFree(p, 0, MEM_RELEASE);
#else
      ::operator delete(p, std::align_val_t{kAlignment});
#endif
    }
  };

  std::unique_ptr<std::uint8_t, HostFree> host;
  std::size_t capacity{};
  QRhiReadbackResult readback;

  const std::uint8_t* data{};
  int size{};
  int stride{};

  /// Makes `host` hold at least @p bytes, rounded up to kAlignment. A buffer
  /// that is already large enough is kept as it is: the SDK may still be
  /// reading it. False when the memory could not be had.
  bool allocate(std::size_t bytes) noexcept
  {
    const std::size_t want = (bytes + kAlignment - 1) / kAlignment * kAlignment;
    if(host && capacity >= want)
      return true;
    host.reset(allocateHost(want));
    capacity = host ? want : 0;
    return bool(host);
  }
};

class WireEncodeRenderer final : public score::gfx::OutputNodeRenderer
{
public:
  explicit WireEncodeRenderer(
      const score::gfx::Node& n, score::gfx::TextureRenderTarget rt,
      std::span<SendSlot> slots, std::string format, ColorSpaceSetting colorSpace)
      : score::gfx::OutputNodeRenderer{n}
      , m_inputTarget{rt}
      , m_slots{slots}
      , m_format{std::move(format)}
      , m_colorSpace{colorSpace}
  {
  }

  // The output node can replace both the QRhiTextureRenderTarget and the
  // QRhiRenderPassDescriptor behind this renderer's back -- a viewport resize
  // deleteLater()s them and installs fresh ones, and when the resize takes the
  // in-place fast path this renderer is not reconstructed. Anything reading a
  // stale snapshot dereferences freed memory, so re-adopt the node's live target
  // on every query.
  score::gfx::TextureRenderTarget
  renderTargetForInput(const score::gfx::Port& p) override
  {
    if(auto* out = dynamic_cast<const score::gfx::OutputNode*>(&this->node))
    {
      auto cur = out->currentRenderTarget();
      if(cur.renderTarget && cur.renderPass)
        m_inputTarget = cur;
    }
    return m_inputTarget;
  }

  void init(score::gfx::RenderList& renderer, QRhiResourceUpdateBatch& res) override
  {
    if(auto* out = dynamic_cast<const score::gfx::OutputNode*>(&this->node))
    {
      auto cur = out->currentRenderTarget();
      if(cur.renderTarget && cur.renderPass)
        m_inputTarget = cur;
    }

    if(!m_inputTarget.texture)
      return;

    const auto sz = m_inputTarget.texture->pixelSize();
    const auto* wire = findWireFormat(m_format);
    if(!wire)
      return;

    // Two pixels per macropixel or per chroma site: a subsampled format has no
    // representation for an odd size, and the 4:2:0 ones halve the height too.
    if(sz.width() <= 0 || sz.height() <= 0
       || (wire->widthMustBeEven && (sz.width() % 2) != 0)
       || (wire->heightMustBeEven && (sz.height() % 2) != 0))
    {
      qWarning() << "Ndi::WireEncodeRenderer:" << QString::fromStdString(m_format)
                 << "cannot express" << sz.width() << "x" << sz.height()
                 << "-- this output will not render";
      return;
    }

    auto enc = makeNdiEncoder(m_format);
    if(!enc)
    {
      qWarning() << "Ndi::WireEncodeRenderer: no GPU encoder for"
                 << QString::fromStdString(m_format);
      return;
    }

    // The matrix is a policy, not a fact: NDI signals it nowhere for SDR, and
    // the receivers that matter all apply Rec.709 whatever the size. The
    // setting decides; Ndi/NdiColorSpace.hpp carries the measurements and the
    // citation.
    //
    // sz is the input texture, i.e. the picture. A packed encoder's target is
    // narrower and a planar one's chroma plane is smaller -- neither of those
    // is what the automatic settings measure.
    const auto standard = resolveYuvStandard(m_colorSpace, sz.width(), sz.height());
    auto& rhi = *renderer.state.rhi;
    enc->init(
        rhi, renderer.state, m_inputTarget.texture, sz.width(), sz.height(),
        ndiColorMatrixOut(standard));

    // The readback is scheduled here, into the slot the output node names, so
    // the encoder must not also read its texture back into a buffer of its own.
    enc->setReadbackEnabled(false);

    auto* tex = enc->outputTexture();
    const std::size_t framestore = framestoreBytes(m_format, sz.width(), sz.height());
    const std::size_t texBytes
        = tex ? std::size_t(tex->pixelSize().width()) * tex->pixelSize().height()
                    * (tex->format() == QRhiTexture::R8 ? 1 : 4)
              : 0;
    if(!tex || framestore == 0 || texBytes != framestore)
    {
      qWarning() << "Ndi::WireEncodeRenderer:" << QString::fromStdString(m_format)
                 << "encoder output is" << texBytes << "bytes, the NDI framestore"
                 << framestore << "-- this output will not send";
      enc->release();
      return;
    }

    m_encoder = std::move(enc);
    m_width = sz.width();
    m_height = sz.height();
    m_stride = packedRowBytes(m_format, m_width);
    createTargets(rhi, *tex, framestore);
  }

  void update(
      score::gfx::RenderList&, QRhiResourceUpdateBatch&, score::gfx::Edge*) override
  {
  }

  void finishFrame(
      score::gfx::RenderList& renderer, QRhiCommandBuffer& cb,
      QRhiResourceUpdateBatch*& res) override
  {
    m_recorded = -1;

    // The frame's pending batch has to be applied here or its uploads are
    // dropped: exec() opens its own pass and, with readback disabled, ends it
    // without consuming one, and a frame nobody receives runs no pass at all.
    if(res)
    {
      cb.resourceUpdate(res);
      res = nullptr;
    }

    if(!m_encoder || m_slot < 0 || m_slot >= int(m_slots.size()) || !m_active)
      return;

    auto& rhi = *renderer.state.rhi;
    m_encoder->exec(rhi, cb);

    auto* out = m_encoder->outputTexture();
    if(!m_targets.empty())
    {
      if(score::gfx::readbackTextureToHost(rhi, cb, *out, *m_targets[m_slot]))
      {
        m_recorded = m_slot;
        return;
      }
      // Nothing about the texture changes between frames, so a refusal now is
      // a refusal forever: read back through QRhi from here on.
      qWarning() << "Ndi::WireEncodeRenderer: direct readback refused, using "
                    "QRhi's readback from now on";
      destroyTargets();
    }

    auto* batch = rhi.nextResourceUpdateBatch();
    batch->readBackTexture(QRhiReadbackDescription{out}, &m_slots[m_slot].readback);
    cb.resourceUpdate(batch);
    m_recorded = m_slot;
  }

  void release(score::gfx::RenderList&) override
  {
    destroyTargets();
    if(m_encoder)
    {
      m_encoder->release();
      m_encoder.reset();
    }
  }

  /// Render thread, before the frame: whether the frame should be encoded and
  /// read back at all, and into which slot.
  void prepare(bool active, int slot) noexcept
  {
    m_active = active;
    m_slot = slot;
  }

  /// Render thread, right after endOffscreenFrame: the slot that now holds a
  /// complete frame, or -1 when there is none yet -- either nothing was
  /// recorded, or the copy is finished by takeDeferred() at the next frame.
  int takeCompleted() noexcept
  {
    const int idx = std::exchange(m_recorded, -1);
    if(idx < 0)
      return -1;
    if(m_targets.empty())
      return describeQRhiReadback(idx) ? idx : -1;
    if(m_deferredFinish)
    {
      // A copy left pending must still be finished before its target records
      // again; the output node collects it every frame, so this only guards.
      if(m_deferred >= 0)
        finishHost(m_deferred);
      m_deferred = idx;
      return -1;
    }
    return finishHost(idx) ? idx : -1;
  }

  /// Render thread, before the next frame is recorded: complete the copy that
  /// takeCompleted() left pending, and return its slot, or -1.
  int takeDeferred() noexcept
  {
    const int idx = std::exchange(m_deferred, -1);
    if(idx < 0)
      return -1;
    return finishHost(idx) ? idx : -1;
  }

  /// Whether the frames go straight from the GPU into the send slots.
  bool readsIntoSlots() const noexcept { return !m_targets.empty(); }

  /// Whether a frame's slot is only complete at the start of the next frame.
  bool finishesOneFrameLate() const noexcept
  {
    return !m_targets.empty() && m_deferredFinish;
  }

  score::gfx::TextureRenderTarget m_inputTarget;

private:
  void createTargets(QRhi& rhi, QRhiTexture& tex, std::size_t framestore)
  {
    destroyTargets();

    const auto path = score::gfx::readbackPath(rhi);
    if(path == score::gfx::ReadbackPath::Unsupported
       || qEnvironmentVariable("SCORE_GFX_DIRECT_READBACK").toLower() == "never")
      return;

    const std::size_t align = score::gfx::readbackHostMemoryAlignment(rhi);
    if(align == 0 || SendSlot::kAlignment % align != 0
       || !score::gfx::readbackTextureSupported(rhi, tex, framestore))
      return;

    for(auto& slot : m_slots)
    {
      if(!slot.allocate(framestore))
        break;
      auto* t = score::gfx::createReadbackTarget(rhi, slot.host.get(), slot.capacity);
      if(!t)
        break;
      m_targets.push_back(t);
    }
    if(m_targets.size() != m_slots.size())
    {
      destroyTargets();
      return;
    }
    m_rhi = &rhi;
    m_deferredFinish = path != score::gfx::ReadbackPath::Import;
    qDebug() << "Ndi::WireEncodeRenderer:" << QString::fromStdString(m_format)
             << "reads back straight into the send buffers"
             << (m_deferredFinish ? "(finished one frame later)" : "");
  }

  void destroyTargets() noexcept
  {
    for(auto* t : m_targets)
      score::gfx::destroyReadbackTarget(t);
    m_targets.clear();
    m_deferred = -1;
  }

  bool finishHost(int idx) noexcept
  {
    if(!m_rhi || idx >= int(m_targets.size()))
      return false;
    if(!score::gfx::finishReadbackToHost(*m_rhi, *m_targets[idx]))
      return false;
    auto& slot = m_slots[idx];
    slot.data = slot.host.get();
    slot.stride = m_stride;
    slot.size = m_stride * framestoreRows(m_format, m_height);
    return true;
  }

  bool describeQRhiReadback(int idx) noexcept
  {
    auto& slot = m_slots[idx];
    const int size = int(slot.readback.data.size());
    const int rows = framestoreRows(m_format, m_height);
    if(size <= 0 || rows <= 0)
      return false;
    // The backend may pad rows, so the stride comes from what came back.
    slot.data = reinterpret_cast<const std::uint8_t*>(slot.readback.data.constData());
    slot.size = size;
    slot.stride = readbackStride(size, rows);
    return true;
  }

  std::unique_ptr<score::gfx::GPUVideoEncoder> m_encoder;
  std::span<SendSlot> m_slots;
  std::vector<score::gfx::ReadbackTarget*> m_targets;
  QRhi* m_rhi{};
  std::string m_format;
  ColorSpaceSetting m_colorSpace{ColorSpaceSetting::Rec709};
  int m_width{}, m_height{}, m_stride{};

  bool m_active{};
  bool m_deferredFinish{};
  int m_slot{-1};
  int m_recorded{-1};
  int m_deferred{-1};
};

/**
 * @brief The output node's side of one frame, before it is recorded.
 *
 * Queues the slot whose copy was left for this frame to finish, THEN picks the
 * Free slot this frame reads back into. In that order, the pending slot is
 * already Queued when the pool looks for a Free one, so it cannot be picked
 * again while its bytes are still on their way; four slots then always leave
 * one Free (tests/ReadbackPoolTest.cpp checks every interleaving).
 *
 * @p active is false when nobody receives the output: nothing is encoded or
 * read back then.
 */
template <int N>
void beginSendFrame(WireEncodeRenderer& r, ReadbackPool<N>& pool, bool active)
{
  if(const int done = r.takeDeferred(); done >= 0)
    pool.queueForSend(done);
  r.prepare(active, pool.advanceToFreeBuffer());
}

/// The output node's side of one frame, after endOffscreenFrame.
template <int N>
void endSendFrame(WireEncodeRenderer& r, ReadbackPool<N>& pool)
{
  if(const int done = r.takeCompleted(); done >= 0)
    pool.queueForSend(done);
}

}
