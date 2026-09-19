#pragma once

/**
 * @file WireEncodeRenderer.hpp
 * @brief Output renderer that converts to the NDI wire format on the GPU.
 *
 * The alternative is InvertYRenderer + sws_scale: read RGBA back off the GPU,
 * then convert on the sender thread. That costs 21.5 ms a frame at 2160p
 * against a 16.67 ms budget, so 4K UYVY cannot keep up, and it moves twice the
 * bytes across the bus.
 *
 * score::gfx has an encoder per wire format, so this drives whichever the
 * output's format calls for and folds the Y-flip into the same shader,
 * replacing InvertYRenderer rather than sitting behind it.
 *
 * Every encoder is asked for its contiguous-framestore form, so its output
 * texture IS the framestore. The encoder's own readback is switched off and
 * this schedules one into whichever ReadbackPool buffer the output node
 * currently owns: the bytes the GPU writes are the bytes handed to
 * send_video_async.
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

    // We read the encoder's output texture into the output node's pool
    // ourselves, so the encoder must not also read it into its own buffer:
    // that is a second full-frame transfer, into memory the pool's ownership
    // states do not cover.
    enc->setReadbackEnabled(false);

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

    if(auto* out = m_encoder->outputTexture())
    {
      auto* batch = renderer.state.rhi->nextResourceUpdateBatch();
      QRhiReadbackDescription rb(out);
      batch->readBackTexture(rb, m_readback);
      cb.resourceUpdate(batch);
    }
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
};

}
