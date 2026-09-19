#pragma once

/**
 * @file WireEncodeRenderer.hpp
 * @brief Output renderer that converts to the NDI wire format on the GPU.
 *
 * The alternative is InvertYRenderer + sws_scale: read RGBA back off the GPU,
 * then convert it on the sender thread. That conversion costs 21.5 ms per frame
 * at 2160p against a 16.67 ms budget at 60 fps, so 4K UYVY simply cannot keep
 * up, and it moves twice as many bytes across the bus as it needs to.
 *
 * score::gfx already has an encoder per wire format -- the same ones the AJA and
 * DeckLink playout paths use -- so this drives whichever one the output's format
 * calls for (Ndi/WireEncode.hpp maps the names) and folds the Y-flip into the
 * same shader, replacing InvertYRenderer rather than sitting behind it.
 *
 * Two shapes come back:
 *
 *   - single-plane (UYVY, BGRA/BGRX): the encoder's output texture IS the
 *     framestore. Its own readback is switched off and this schedules one into
 *     whichever ReadbackPool buffer the output node currently owns -- the
 *     "direct-readback rung" that Gfx/tests/EncoderTester.cpp exercises. The
 *     bytes the GPU writes are the bytes handed to send_video_async.
 *
 *   - multi-plane (P216, NV12, I420, YV12): one readback per plane, in separate
 *     allocations, because that is what a QRhi readback does. NDI needs the
 *     planes adjacent in one buffer, so assembleInto() copies them there once
 *     the frame has completed. That copy is the cost of a planar format; the
 *     conversion itself still happens on the GPU.
 */

#include <Gfx/Graph/NodeRenderer.hpp>
#include <Gfx/Graph/OutputNode.hpp>
#include <Gfx/Graph/RenderList.hpp>
#include <Gfx/Graph/RenderState.hpp>
#include <Gfx/Graph/encoders/ColorSpaceOut.hpp>
#include <Ndi/NdiColorSpace.hpp>
#include <Ndi/WireEncode.hpp>

#include <QtGui/private/qrhi_p.h>

#include <memory>
#include <string>

namespace Ndi
{

class WireEncodeRenderer final : public score::gfx::OutputNodeRenderer
{
public:
  explicit WireEncodeRenderer(
      const score::gfx::Node& n, score::gfx::TextureRenderTarget rt,
      QRhiReadbackResult& readback, std::string format, ColorSpaceSetting colorSpace)
      : score::gfx::OutputNodeRenderer{n}
      , m_inputTarget{rt}
      , m_readback{&readback}
      , m_format{std::move(format)}
      , m_encoding{ndiEncoding(m_format)}
      , m_colorSpace{colorSpace}
  {
  }

  // The output node can replace both the QRhiTextureRenderTarget and the
  // QRhiRenderPassDescriptor behind this renderer's back -- a viewport resize
  // deleteLater()s them and installs fresh ones, and when the resize takes the
  // in-place fast path this renderer is not reconstructed. Anything reading a
  // stale snapshot dereferences freed memory, so re-adopt the node's live target
  // on every query. Same rule InvertYRenderer applies, for the same reason.
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
    // The encoders' own default is BT.709 FULL range, which is wrong at every
    // resolution, and the swscale path this replaced was BT.601 at every
    // resolution, which matches almost no receiver. Both put the wrong bytes on
    // the wire; the loopback and encode-renderer tests pin the right ones.
    //
    // sz is the input texture, i.e. the picture. A packed encoder's target is
    // narrower and a planar one's chroma plane is smaller -- neither of those
    // is what the automatic settings measure.
    const auto standard = resolveYuvStandard(m_colorSpace, sz.width(), sz.height());
    enc->init(
        *renderer.state.rhi, renderer.state, m_inputTarget.texture, sz.width(),
        sz.height(), ndiColorMatrixOut(standard));

    // Single-plane: we read the encoder's output texture into the output node's
    // pool ourselves, so the encoder must not also read it back into its own
    // buffer -- that would be a second full-frame transfer per frame, into
    // memory the pool's ownership states do not cover.
    //
    // Multi-plane: the encoder's own per-plane readbacks are exactly what
    // assembleInto() copies from, so they stay on.
    enc->setReadbackEnabled(m_encoding.needsAssembly());

    m_pictureSize = sz;
    m_encoder = std::move(enc);
  }

  void update(
      score::gfx::RenderList&, QRhiResourceUpdateBatch&, score::gfx::Edge*) override
  {
  }

  void finishFrame(
      score::gfx::RenderList& renderer, QRhiCommandBuffer& cb,
      QRhiResourceUpdateBatch*& res) override
  {
    if(!m_encoder || !m_readback)
      return;

    // exec() opens its own pass and, with readback disabled, ends it without
    // consuming a batch -- so the frame's pending batch has to be applied here
    // or its uploads are dropped.
    if(res)
    {
      cb.resourceUpdate(res);
      res = nullptr;
    }

    m_encoder->exec(*renderer.state.rhi, cb);

    if(m_encoding.needsAssembly())
      return;  // the planes come back in the encoder's own buffers

    if(auto* out = m_encoder->outputTexture())
    {
      auto* batch = renderer.state.rhi->nextResourceUpdateBatch();
      QRhiReadbackDescription rb(out);
      batch->readBackTexture(rb, m_readback);
      cb.resourceUpdate(batch);
    }
  }

  /// True when the frame still has to be assembled after the offscreen frame
  /// ends -- the node calls assembleInto() for those.
  bool needsAssembly() const noexcept
  {
    return m_encoder && m_encoding.needsAssembly();
  }

  /**
   * @brief Copy the encoder's planes into the send buffer, adjacent and tight.
   *
   * Call after QRhi::endOffscreenFrame(), which is when the plane readbacks
   * have landed. On any disagreement between what came back and what the wire
   * format says it should be, this empties the buffer rather than sending a
   * frame whose planes are at the wrong offsets: describeVideoFrame then
   * refuses it, because readbackStride() of an empty buffer is 0.
   */
  void assembleInto()
  {
    if(!m_encoder || !m_readback || !m_encoding.needsAssembly())
      return;

    const int w = m_pictureSize.width(), h = m_pictureSize.height();
    const int rowBytes = packedRowBytes(m_format, w);
    const size_t total = framestoreBytes(m_format, w, h);

    PlaneSource src[3]{};
    for(int i = 0; i < m_encoding.planeCount; i++)
    {
      const auto& spec = m_encoding.planes[i];
      const auto& rb = m_encoder->readback(spec.encoderPlane);
      const int rows = h / spec.heightDiv;
      const int tight = (w / spec.widthDiv) * spec.bytesPerTexel;
      if(rb.data.isEmpty() || rows <= 0 || tight <= 0)
      {
        m_readback->data.clear();
        return;
      }
      src[i] = PlaneSource{
          .data = reinterpret_cast<const uint8_t*>(rb.data.constData()),
          .srcStride = readbackStride(rb.data.size(), rows),
          .rowBytes = tight,
          .rows = rows};
    }

    if(size_t(m_readback->data.size()) != total)
      m_readback->data.resize(total);

    const size_t written = assembleFramestore(
        reinterpret_cast<uint8_t*>(m_readback->data.data()), total, src,
        m_encoding.planeCount);

    // The planes must fill the framestore exactly. If the two arithmetics --
    // this one, from the encoder's plane geometry, and VideoFrameFormat's, from
    // the format's row size and row count -- ever disagree, the frame is not
    // describable and must not go out.
    if(written != total || rowBytes <= 0)
    {
      m_readback->data.clear();
      return;
    }
    m_readback->pixelSize = QSize(w, framestoreRows(m_format, h));
  }

  void release(score::gfx::RenderList&) override
  {
    if(m_encoder)
    {
      m_encoder->release();
      m_encoder.reset();
    }
  }

  /// Point the next readback at another pool buffer.
  void updateReadback(QRhiReadbackResult& rb) { m_readback = &rb; }

  score::gfx::TextureRenderTarget m_inputTarget;

private:
  std::unique_ptr<score::gfx::GPUVideoEncoder> m_encoder;
  QRhiReadbackResult* m_readback{};
  std::string m_format;
  NdiEncoding m_encoding;
  ColorSpaceSetting m_colorSpace{ColorSpaceSetting::Rec709};
  QSize m_pictureSize;
};

}
