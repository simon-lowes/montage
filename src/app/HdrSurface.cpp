#include "HdrSurface.h"

#ifdef MONTAGE_HDR_VIEWER
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDropEvent>
#include <QExposeEvent>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QWheelEvent>
#include <QWidget>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
#include "shaders/HdrShaders.inc"
}
#endif

namespace montage {

bool hdrViewerBuilt() {
#ifdef MONTAGE_HDR_VIEWER
    return true;
#else
    return false;
#endif
}

#ifdef MONTAGE_HDR_VIEWER

namespace {

double gTestHeadroom = 0;

QShader shader(const unsigned char* data, size_t size) {
    return QShader::fromSerialized(QByteArray::fromRawData(reinterpret_cast<const char*>(data), qsizetype(size)));
}

// The uniform block the shaders share: the clip-space correction, the rectangle drawn and the tone values.
constexpr quint32 kUniformBytes = 96;

}  // namespace

// The GPU side: the RHI, the swap chain and what is drawn with (the RHI is declared first so it goes last).
class HdrSurfaceGpu {
public:
    std::unique_ptr<QRhi> rhi;
    std::unique_ptr<QRhiSwapChain> sc;
    std::unique_ptr<QRhiRenderPassDescriptor> rp;
    std::unique_ptr<QRhiBuffer> vbuf, ubufPicture, ubufOverlay;
    std::unique_ptr<QRhiSampler> sampler;
    std::unique_ptr<QRhiTexture> picture, overlay;
    std::unique_ptr<QRhiShaderResourceBindings> srbPicture, srbOverlay;
    std::unique_ptr<QRhiGraphicsPipeline> psPicture, psOverlay;
    bool hasSwapChain = false, vbufUploaded = false, notExposed = false, newlyExposed = false;
    QSize pictureSize{1, 1}, overlaySize{1, 1};

    ~HdrSurfaceGpu() {
        // Resources before the RHI that made them.
        psPicture.reset(), psOverlay.reset(), srbPicture.reset(), srbOverlay.reset();
        picture.reset(), overlay.reset(), sampler.reset(), vbuf.reset(), ubufPicture.reset(), ubufOverlay.reset();
        rp.reset(), sc.reset();
        rhi.reset();
    }

    QRhiShaderResourceBindings* bindings(QRhiBuffer* ubuf, QRhiTexture* tex) {
        QRhiShaderResourceBindings* srb = rhi->newShaderResourceBindings();
        srb->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, ubuf),
                          QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, tex, sampler.get())});
        srb->create();
        return srb;
    }

    QRhiGraphicsPipeline* pipeline(const QShader& fragment, QRhiShaderResourceBindings* srb, bool blend) {
        QRhiGraphicsPipeline* ps = rhi->newGraphicsPipeline();
        ps->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        ps->setShaderStages({{QRhiShaderStage::Vertex, shader(kViewVertQsb, sizeof kViewVertQsb)}, {QRhiShaderStage::Fragment, fragment}});
        QRhiVertexInputLayout layout;
        layout.setBindings({{2 * sizeof(float)}});
        layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
        ps->setVertexInputLayout(layout);
        ps->setShaderResourceBindings(srb);
        ps->setRenderPassDescriptor(rp.get());
        if (blend) {
            // The overlay is premultiplied.
            QRhiGraphicsPipeline::TargetBlend b;
            b.enable = true;
            b.srcColor = QRhiGraphicsPipeline::One;
            b.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            b.srcAlpha = QRhiGraphicsPipeline::One;
            b.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            ps->setTargetBlends({b});
        }
        ps->create();
        return ps;
    }

    // A texture of a new size, its bindings pointed at it (the old one released once the GPU is done with it).
    void resize(std::unique_ptr<QRhiTexture>& tex, std::unique_ptr<QRhiShaderResourceBindings>& srb, QRhiBuffer* ubuf,
                QRhiTexture::Format format, QSize size) {
        QRhiTexture* t = rhi->newTexture(format, size);
        t->create();
        srb->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, ubuf),
                          QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, t, sampler.get())});
        srb->create();
        if (tex) tex.release()->deleteLater();
        tex.reset(t);
    }
};

void HdrSurface::setTestDisplay(double headroom) { gTestHeadroom = headroom; }

HdrSurface::HdrSurface(QWidget* target) : target_(target) {
    setFlag(Qt::WindowDoesNotAcceptFocus);  // the editor's shortcuts keep working: focus stays in the widgets
    connect(this, &QWindow::screenChanged, this, [this] {
        if (!gpu_ || !gpu_->sc || gTestHeadroom > 0) return;
        // Another display: HDR may have come or gone, and the swap chain is made again for it (macOS resets the layer's
        // colour space when the window changes screens).
        const bool hdr = gpu_->sc->isFormatSupported(QRhiSwapChain::HDRExtendedSrgbLinear);
        if (!hdr) {
            if (hdrActive_) {
                hdrActive_ = false;
                emit displayChanged();
            }
            return;
        }
        if (gpu_->hasSwapChain) {
            gpu_->sc->destroy();
            gpu_->hasSwapChain = false;
        }
        if (isExposed()) gpu_->hasSwapChain = gpu_->sc->createOrResize();
        displayRead_.invalidate();
        readDisplay();
        requestUpdate();
    });
}

HdrSurface::~HdrSurface() = default;

bool HdrSurface::probe(QString* why) {
    auto fail = [&](const QString& reason) {
        if (why) *why = reason;
        gpu_.reset();
        hdrActive_ = false;
        return false;
    };
    if (gpu_ && gpu_->rhi) return hdrActive_;
    gpu_ = std::make_unique<HdrSurfaceGpu>();
    HdrSurfaceGpu& g = *gpu_;
    if (gTestHeadroom > 0) {
        setSurfaceType(QSurface::RasterSurface);
        create();
        QRhiNullInitParams params;
        g.rhi.reset(QRhi::create(QRhi::Null, &params));
    } else {
#if defined(Q_OS_MACOS)
        setSurfaceType(QSurface::MetalSurface);
        create();
        QRhiMetalInitParams params;
        g.rhi.reset(QRhi::create(QRhi::Metal, &params));
#elif defined(Q_OS_WIN)
        setSurfaceType(QSurface::Direct3DSurface);
        create();
        QRhiD3D11InitParams params;
        g.rhi.reset(QRhi::create(QRhi::D3D11, &params));
#else
        return fail(tr("HDR viewing is available on macOS (EDR displays) and Windows (with HDR switched on)"));
#endif
    }
    if (!g.rhi) return fail(tr("No graphics device for HDR viewing"));
    if (!g.rhi->isTextureFormatSupported(QRhiTexture::RGBA16F)) return fail(tr("The graphics device has no half-float textures"));
    g.sc.reset(g.rhi->newSwapChain());
    g.sc->setWindow(this);
    if (gTestHeadroom <= 0) {
        if (!g.sc->isFormatSupported(QRhiSwapChain::HDRExtendedSrgbLinear)) return fail(tr("This display does not show HDR"));
        g.sc->setFormat(QRhiSwapChain::HDRExtendedSrgbLinear);
    }
    g.rp.reset(g.sc->newCompatibleRenderPassDescriptor());
    g.sc->setRenderPassDescriptor(g.rp.get());

    g.vbuf.reset(g.rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, 8 * sizeof(float)));
    g.vbuf->create();
    g.ubufPicture.reset(g.rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformBytes));
    g.ubufPicture->create();
    g.ubufOverlay.reset(g.rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformBytes));
    g.ubufOverlay->create();
    g.sampler.reset(g.rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge,
                                      QRhiSampler::ClampToEdge));
    g.sampler->create();
    g.picture.reset(g.rhi->newTexture(QRhiTexture::RGBA16F, g.pictureSize));
    g.picture->create();
    g.overlay.reset(g.rhi->newTexture(QRhiTexture::RGBA8, g.overlaySize));
    g.overlay->create();
    g.srbPicture.reset(g.bindings(g.ubufPicture.get(), g.picture.get()));
    g.srbOverlay.reset(g.bindings(g.ubufOverlay.get(), g.overlay.get()));
    g.psPicture.reset(g.pipeline(shader(kPictureFragQsb, sizeof kPictureFragQsb), g.srbPicture.get(), false));
    g.psOverlay.reset(g.pipeline(shader(kOverlayFragQsb, sizeof kOverlayFragQsb), g.srbOverlay.get(), true));
    hdrActive_ = true;
    readDisplay();
    return true;
}

void HdrSurface::readDisplay() {
    // Asked at most once a second (it is not cheap; macOS changes the headroom with the brightness, and reads 1 until a
    // frame has been shown).
    if (displayRead_.isValid() && displayRead_.elapsed() < 1000) return;
    displayRead_.start();
    double h = 1;
    float sc = 1;
    if (gTestHeadroom > 0) {
        h = gTestHeadroom;
    } else if (gpu_ && gpu_->sc && gpu_->hasSwapChain) {
        const QRhiSwapChainHdrInfo info = gpu_->sc->hdrInfo();
        if (info.limitsType == QRhiSwapChainHdrInfo::ColorComponentValue) {
            h = info.limits.colorComponentValue.maxColorComponentValue;  // macOS EDR: 1.0 is SDR white
        } else {
            // Windows scRGB: 1.0 is 80 nits; SDR white is the user's setting.
            const double white = info.sdrWhiteLevel > 0 ? info.sdrWhiteLevel : 80;
            h = info.limits.luminanceInNits.maxLuminance / white;
            sc = float(white / 80);
        }
    } else {
        return;
    }
    h = std::max(1.0, std::isfinite(h) ? h : 1.0);
    if (std::fabs(h - headroom_) > 0.01 || sc != scale_) {
        headroom_ = h;
        scale_ = sc;
        emit displayChanged();
    }
}

void HdrSurface::setPicture(HdrPicturePtr picture, const QRectF& rect) {
    picture_ = std::move(picture);
    rect_ = rect;
    pictureDirty_ = true;
    requestUpdate();
}

void HdrSurface::setOverlay(const QImage& overlay) {
    overlay_ = overlay;
    overlayDirty_ = true;
    requestUpdate();
}

void HdrSurface::exposeEvent(QExposeEvent*) {
    if (!gpu_ || !gpu_->rhi) return;
    HdrSurfaceGpu& g = *gpu_;
    if (isExposed() && !g.hasSwapChain) {
        g.hasSwapChain = g.sc->createOrResize();
        displayRead_.invalidate();
        readDisplay();
    }
    const QSize size = g.hasSwapChain ? g.sc->surfacePixelSize() : QSize();
    if ((!isExposed() || size.isEmpty()) && g.hasSwapChain) g.notExposed = true;
    if (isExposed() && g.notExposed && !size.isEmpty()) {
        g.notExposed = false;
        g.newlyExposed = true;
    }
    if (isExposed() && !size.isEmpty()) render();
}

bool HdrSurface::event(QEvent* e) {
    switch (e->type()) {
        case QEvent::UpdateRequest:
            render();
            return true;
        case QEvent::PlatformSurface:
            if (static_cast<QPlatformSurfaceEvent*>(e)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed && gpu_ &&
                gpu_->sc) {
                gpu_->sc->destroy();
                gpu_->hasSwapChain = false;
            }
            break;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        case QEvent::Wheel:
        case QEvent::DragEnter:
        case QEvent::DragMove:
        case QEvent::DragLeave:
        case QEvent::Drop:
            if (forward(e)) return true;
            break;
        default:
            break;
    }
    return QWindow::event(e);
}

bool HdrSurface::forward(QEvent* e) {
    QWidget* t = target_;
    if (!t) return false;
    // The surface covers the widget exactly, so positions in one are positions in the other.
    switch (e->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove: {
            auto* m = static_cast<QMouseEvent*>(e);
            if (m->type() == QEvent::MouseMove && m->buttons() == Qt::NoButton && !t->hasMouseTracking()) return true;
            QMouseEvent copy(m->type(), m->position(), QPointF(t->mapTo(t->window(), m->position().toPoint())), m->globalPosition(), m->button(),
                             m->buttons(), m->modifiers(), m->pointingDevice());
            if (m->type() == QEvent::MouseButtonPress) t->setFocus(Qt::MouseFocusReason);
            QCoreApplication::sendEvent(t, &copy);
            if (m->type() == QEvent::MouseButtonPress && m->button() == Qt::RightButton && t) {
                QContextMenuEvent menu(QContextMenuEvent::Mouse, m->position().toPoint(), m->globalPosition().toPoint(), m->modifiers());
                QCoreApplication::sendEvent(t, &menu);
            }
            if (target_) setCursor(target_->cursor());
            return true;
        }
        case QEvent::Wheel: {
            auto* w = static_cast<QWheelEvent*>(e);
            QWheelEvent copy(w->position(), w->globalPosition(), w->pixelDelta(), w->angleDelta(), w->buttons(), w->modifiers(), w->phase(),
                             w->inverted(), w->source(), w->pointingDevice());
            QCoreApplication::sendEvent(t, &copy);
            e->setAccepted(copy.isAccepted());
            return true;
        }
        case QEvent::DragEnter: {
            auto* d = static_cast<QDragEnterEvent*>(e);
            QDragEnterEvent copy(d->position().toPoint(), d->possibleActions(), d->mimeData(), d->buttons(), d->modifiers());
            QCoreApplication::sendEvent(t, &copy);
            d->setDropAction(copy.dropAction());
            d->setAccepted(copy.isAccepted());
            return true;
        }
        case QEvent::DragMove: {
            auto* d = static_cast<QDragMoveEvent*>(e);
            QDragMoveEvent copy(d->position().toPoint(), d->possibleActions(), d->mimeData(), d->buttons(), d->modifiers());
            QCoreApplication::sendEvent(t, &copy);
            d->setDropAction(copy.dropAction());
            d->setAccepted(copy.isAccepted());
            return true;
        }
        case QEvent::DragLeave: {
            QDragLeaveEvent copy;
            QCoreApplication::sendEvent(t, &copy);
            return true;
        }
        case QEvent::Drop: {
            auto* d = static_cast<QDropEvent*>(e);
            QDropEvent copy(d->position(), d->possibleActions(), d->mimeData(), d->buttons(), d->modifiers());
            QCoreApplication::sendEvent(t, &copy);
            d->setDropAction(copy.dropAction());
            d->setAccepted(copy.isAccepted());
            return true;
        }
        default:
            return false;
    }
}

void HdrSurface::render() {
    if (!gpu_ || !gpu_->rhi || !gpu_->hasSwapChain || gpu_->notExposed) return;
    HdrSurfaceGpu& g = *gpu_;
    if (g.sc->currentPixelSize() != g.sc->surfacePixelSize() || g.newlyExposed) {
        g.hasSwapChain = g.sc->createOrResize();
        if (!g.hasSwapChain) return;
        g.newlyExposed = false;
    }
    readDisplay();
    QRhi::FrameOpResult r = g.rhi->beginFrame(g.sc.get());
    if (r == QRhi::FrameOpSwapChainOutOfDate) {
        g.hasSwapChain = g.sc->createOrResize();
        if (!g.hasSwapChain) return;
        r = g.rhi->beginFrame(g.sc.get());
    }
    if (r != QRhi::FrameOpSuccess) {
        requestUpdate();
        return;
    }
    QRhiResourceUpdateBatch* u = g.rhi->nextResourceUpdateBatch();
    if (!g.vbufUploaded) {
        static const float quad[8] = {0, 0, 1, 0, 0, 1, 1, 1};  // a strip: top left, top right, bottom left, bottom right
        u->uploadStaticBuffer(g.vbuf.get(), quad);
        g.vbufUploaded = true;
    }
    if (pictureDirty_ && picture_ && picture_->width > 0 && picture_->height > 0 &&
        picture_->rgba.size() >= size_t(picture_->width) * size_t(picture_->height) * 4) {
        const QSize size(picture_->width, picture_->height);
        if (size != g.pictureSize) {
            g.resize(g.picture, g.srbPicture, g.ubufPicture.get(), QRhiTexture::RGBA16F, size);
            g.pictureSize = size;
        }
        QRhiTextureSubresourceUploadDescription d(picture_->rgba.data(), quint32(size_t(size.width()) * size_t(size.height()) * 8));
        u->uploadTexture(g.picture.get(), QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, d)));
        pictureDirty_ = false;
    }
    if (overlayDirty_ && !overlay_.isNull()) {
        const QImage img = overlay_.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        if (img.size() != g.overlaySize) {
            g.resize(g.overlay, g.srbOverlay, g.ubufOverlay.get(), QRhiTexture::RGBA8, img.size());
            g.overlaySize = img.size();
        }
        u->uploadTexture(g.overlay.get(), img);
        overlayDirty_ = false;
    }
    // Rectangles from the window's coordinates to clip space (y up), and the tone values.
    const QMatrix4x4 corr = g.rhi->clipSpaceCorrMatrix();
    const double w = std::max(1, width()), h = std::max(1, height());
    auto uniforms = [&](QRhiBuffer* b, const QRectF& rect) {
        float data[24];
        std::memcpy(data, corr.constData(), 16 * sizeof(float));
        data[16] = float(2 * rect.left() / w - 1);
        data[17] = float(1 - 2 * rect.top() / h);
        data[18] = float(2 * rect.right() / w - 1);
        data[19] = float(1 - 2 * rect.bottom() / h);
        data[20] = float(headroom_);
        data[21] = float(picture_ ? picture_->contentPeak : 1);
        data[22] = scale_;
        data[23] = 0;
        u->updateDynamicBuffer(b, 0, kUniformBytes, data);
    };
    uniforms(g.ubufPicture.get(), rect_);
    uniforms(g.ubufOverlay.get(), QRectF(0, 0, w, h));

    QRhiCommandBuffer* cb = g.sc->currentFrameCommandBuffer();
    const QSize px = g.sc->currentPixelSize();
    cb->beginPass(g.sc->currentFrameRenderTarget(), Qt::black, {1.0f, 0}, u);
    const QRhiCommandBuffer::VertexInput input(g.vbuf.get(), 0);
    if (picture_ && !rect_.isEmpty()) {
        cb->setGraphicsPipeline(g.psPicture.get());
        cb->setViewport({0, 0, float(px.width()), float(px.height())});
        cb->setShaderResources(g.srbPicture.get());
        cb->setVertexInput(0, 1, &input);
        cb->draw(4);
    }
    if (!overlay_.isNull()) {
        cb->setGraphicsPipeline(g.psOverlay.get());
        cb->setViewport({0, 0, float(px.width()), float(px.height())});
        cb->setShaderResources(g.srbOverlay.get());
        cb->setVertexInput(0, 1, &input);
        cb->draw(4);
    }
    cb->endPass();
    g.rhi->endFrame(g.sc.get());
    ++frames_;
}

#endif

}  // namespace montage
