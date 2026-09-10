#pragma once

/**
 * @file UyvyEncodeRenderer.hpp
 * @brief Output renderer that converts to UYVY on the GPU.
 *
 * The alternative is InvertYRenderer + sws_scale: read RGBA back off the GPU,
 * then convert it to UYVY on the sender thread. That conversion costs 21.5 ms
 * per frame at 2160p against a 16.67 ms budget at 60 fps, so 4K UYVY simply
 * cannot keep up, and it moves twice as many bytes across the bus as it needs
 * to.
 *
 * score::gfx::UYVYEncoder already does the conversion in a fragment shader,
 * writing an RGBA8 target at HALF WIDTH whose texels are (U, Y0, V, Y1) -- the
 * UYVY memory layout, ready for the wire. It folds the Y-flip into the same
 * shader, so it replaces InvertYRenderer rather than sitting behind it.
 *
 * The encoder's own readback is switched off and this schedules its own instead,
 * into whichever ReadbackPool buffer the output node currently owns. That is the
 * "direct-readback rung" that Gfx/tests/EncoderTester.cpp exercises, and it is
 * what keeps the send path zero-copy: the bytes the GPU writes are the bytes
 * handed to send_video_async.
 */

#include <Gfx/Graph/NodeRenderer.hpp>
#include <Gfx/Graph/OutputNode.hpp>
#include <Gfx/Graph/RenderList.hpp>
#include <Gfx/Graph/RenderState.hpp>
#include <Gfx/Graph/encoders/UYVY.hpp>

#include <QtGui/private/qrhi_p.h>

#include <memory>

namespace Ndi
{

class UyvyEncodeRenderer final : public score::gfx::OutputNodeRenderer
{
public:
  explicit UyvyEncodeRenderer(
      const score::gfx::Node& n, score::gfx::TextureRenderTarget rt,
      QRhiReadbackResult& readback)
      : score::gfx::OutputNodeRenderer{n}
      , m_inputTarget{rt}
      , m_readback{&readback}
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

    // UYVY packs two pixels per macropixel, so the encoder's target is
    // width/2 wide. An odd width has no representation on the wire.
    if(sz.width() <= 0 || sz.height() <= 0 || (sz.width() % 2) != 0)
    {
      qWarning() << "Ndi::UyvyEncodeRenderer: UYVY needs an even width, got"
                 << sz.width() << "x" << sz.height() << "-- this output will not render";
      return;
    }

    auto enc = std::make_unique<score::gfx::UYVYEncoder>();
    enc->init(
        *renderer.state.rhi, renderer.state, m_inputTarget.texture, sz.width(),
        sz.height());
    // We read the encoder's output texture into the output node's pool instead,
    // so the encoder must not also read it back into its own single buffer:
    // that would be a second full-frame transfer per frame, into memory the
    // pool's ownership states do not cover.
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
    if(!m_encoder || !m_encoder->outputTexture() || !m_readback)
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

    auto* batch = renderer.state.rhi->nextResourceUpdateBatch();
    QRhiReadbackDescription rb(m_encoder->outputTexture());
    batch->readBackTexture(rb, m_readback);
    cb.resourceUpdate(batch);
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
  std::unique_ptr<score::gfx::UYVYEncoder> m_encoder;
  QRhiReadbackResult* m_readback{};
};

}
