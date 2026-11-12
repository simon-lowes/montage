#include "HdrSurface.h"

#include <QGuiApplication>

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
#include <array>
#include <cmath>
#include <cstring>

#if defined(MONTAGE_WAYLAND_COLOR)
#include "WaylandColor.h"  // (the class, whether or not it is used here)
#define MONTAGE_WAYLAND_COLOR_CLASS
#endif
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS) && QT_CONFIG(opengl)
#define MONTAGE_HDR_ENCODED  // (Linux: OpenGL, the picture written as BT.2100 PQ or HLG)
#include <QColorSpace>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <qpa/qplatformnativeinterface.h>
// Wayland's colour management: Montage's binding where it was built, else Qt's own (6.10.2 and later; 6.10.1's is
// the protocol's experimental forerunner).
#if !defined(MONTAGE_WAYLAND_COLOR) && QT_VERSION >= QT_VERSION_CHECK(6, 10, 2)
#define MONTAGE_QT_WAYLAND_COLOR
#endif
// Qt's word that it let a window's wl_surface go (made again when a window changes role, without a hide).
#if defined(MONTAGE_WAYLAND_COLOR) && __has_include(<qpa/qplatformwindow_p.h>)
#include <qpa/qplatformwindow_p.h>
#if QT_CONFIG(wayland)  // (the header has Qt's Wayland window only then)
#define MONTAGE_WAYLAND_SURFACE_SIGNALS
#endif
#endif
#else
#undef MONTAGE_WAYLAND_COLOR  // (no OpenGL: no surface to describe)
#endif

#if defined(Q_OS_WIN)
#include <windows.h>
#include <dxgi1_6.h>

#include <cwchar>

#include <vector>
#endif

namespace {
#include "shaders/HdrShaders.inc"
}
#endif

namespace montage {

#if defined(MONTAGE_HDR_VIEWER) && !defined(MONTAGE_WAYLAND_COLOR_CLASS)
class WaylandColor {};  // (not built: never made)
#endif

bool hdrViewerBuilt() {
#ifdef MONTAGE_HDR_VIEWER
    return true;
#else
    return false;
#endif
}

bool hdrViewerOnWaylandBuilt() {
#if defined(MONTAGE_HDR_VIEWER) && (defined(MONTAGE_WAYLAND_COLOR) || defined(MONTAGE_QT_WAYLAND_COLOR))
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

#if defined(MONTAGE_HDR_ENCODED)
// The last pass on Linux: the picture and overlay, drawn as linear light relative to SDR white in Rec. 709 primaries
// into a half-float texture, written as the BT.2100 signal the surface is described with (render/HdrView.h
// hdrSurfaceSignal, step for step): BT.2020 primaries, SDR white at 203 nits, PQ, or HLG on its 1000-nit reference
// display (tone.w 2). OpenGL is the RHI's backend there, so this shader is GLSL only, in the versions the baked ones
// carry, and takes the picture shader's interface (its uniform block, sampler and input), whose reflection it reuses:
// no shader tools are needed to build it.
QByteArray encodeGlsl(const QShaderVersion& v, const QByteArray& block) {
    const bool es = v.flags().testFlag(QShaderVersion::GlslEs);
    const bool modern = es ? v.version() >= 300 : v.version() >= 130;
    QByteArray m;
    {
        double d[9];
        primariesMatrix(Primaries::Bt709, Primaries::Bt2020, d);
        for (int c = 0; c < 3; ++c)  // (GLSL's matrices are given column by column)
            for (int r = 0; r < 3; ++r) m += (m.isEmpty() ? "" : ", ") + QByteArray::number(d[r * 3 + c], 'g', 9);
    }
    QByteArray src = "#version " + QByteArray::number(v.version()) + (es && v.version() >= 300 ? " es\n" : "\n");
    if (es) src += "precision highp float;\nprecision highp int;\n";
    src += "struct Params { mat4 clipCorrection; vec4 rect; vec4 tone; };\nuniform Params " + block + ";\n";
    src += es ? "uniform highp sampler2D tex;\n" : "uniform sampler2D tex;\n";
    src += modern ? "in vec2 v_uv;\nout vec4 fragColor;\n" : "varying vec2 v_uv;\n";
    src += "const mat3 toBt2020 = mat3(" + m + ");\n";
    src += "float pqCode(float nits) {\n"
           "    float y = pow(clamp(nits / 10000.0, 0.0, 1.0), 0.1593017578125);\n"
           "    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);\n"
           "}\n"
           "float hlgCode(float e) {\n"
           "    e = max(e, 0.0);\n"
           "    return e <= 1.0 / 12.0 ? sqrt(3.0 * e) : 0.17883277 * log(12.0 * e - 0.28466892) + 0.55991073;\n"
           "}\n"
           "void main() {\n";
    src += QByteArray("    vec3 l = max(toBt2020 * ") + (modern ? "texture" : "texture2D") + "(tex, v_uv).rgb, vec3(0.0));\n";
    src += "    vec3 e;\n"
           "    if (" + block + ".tone.w > 1.5) {\n"
           "        float yd = max(1e-12, dot(l, vec3(0.2627, 0.6780, 0.0593)) * 0.203);\n"
           "        vec3 s = l * (pow(yd, -1.0 / 6.0) * 0.203);\n"
           "        e = vec3(hlgCode(s.r), hlgCode(s.g), hlgCode(s.b));\n"
           "    } else {\n"
           "        vec3 n = l * 203.0;\n"
           "        e = vec3(pqCode(n.r), pqCode(n.g), pqCode(n.b));\n"
           "    }\n";
    src += QByteArray("    ") + (modern ? "fragColor" : "gl_FragColor") + " = vec4(clamp(e, 0.0, 1.0), 1.0);\n}\n";
    return src;
}

QShader encodeShader() {
    QShader s = shader(kPictureFragQsb, sizeof kPictureFragQsb);
    const QList<QShaderDescription::UniformBlock> blocks = s.description().uniformBlocks();
    const QByteArray block = blocks.isEmpty() || blocks.first().structName.isEmpty() ? QByteArrayLiteral("_27") : blocks.first().structName;
    for (const QShaderKey& key : s.availableShaders())
        if (key.source() == QShader::GlslShader) s.setShader(key, QShaderCode(encodeGlsl(key.sourceVersion(), block)));
    return s;
}

// What was read back from the GPU as an image, top row first (OpenGL gives the bottom row first); null if it is not
// in the format expected.
QImage readBack(const QRhiReadbackResult& r, QRhiTexture::Format rhiFormat, QImage::Format format, int bytesPerPixel, bool bottomFirst) {
    const int w = r.pixelSize.width(), h = r.pixelSize.height();
    if (r.format != rhiFormat || w <= 0 || h <= 0 || r.data.size() < qsizetype(w) * h * bytesPerPixel) return QImage();
    QImage img(w, h, format);
    if (img.isNull() || img.bytesPerLine() != qsizetype(w) * bytesPerPixel) return QImage();
    for (int y = 0; y < h; ++y)
        std::memcpy(img.scanLine(y), r.data.constData() + qsizetype(bottomFirst ? h - 1 - y : y) * w * bytesPerPixel,
                    size_t(w) * size_t(bytesPerPixel));
    return img;
}
#endif

#if defined(Q_OS_WIN)
// The display a window is on, as Windows reports it. (Qt asks about the display at the window's position, which for
// the surface, a window inside the main one, is the main display's.)
struct WinDisplay {
    bool known = false, hdr = false, whiteKnown = false;
    double maxNits = 0, sdrWhiteNits = 80;
};

WinDisplay windowsDisplay(HWND hwnd) {
    WinDisplay out;
    HMONITOR monitor = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (!monitor) return out;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return out;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; !out.known && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; !out.known && adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            IDXGIOutput6* output6 = nullptr;
            if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&output6)))) {
                DXGI_OUTPUT_DESC1 desc;
                if (SUCCEEDED(output6->GetDesc1(&desc)) && desc.Monitor == monitor) {
                    out.known = true;
                    out.hdr = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                    out.maxNits = desc.MaxLuminance;
                }
                output6->Release();
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    // SDR white (Settings' "SDR content brightness"), from the display configuration of this monitor's source.
    MONITORINFOEXW info{};
    info.cbSize = sizeof info;
    if (!out.known || !GetMonitorInfoW(monitor, &info)) return out;
    std::vector<DISPLAYCONFIG_PATH_INFO> pathInfo;
    std::vector<DISPLAYCONFIG_MODE_INFO> modeInfo;
    UINT32 paths = 0, modes = 0;
    LONG status = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 4 && status == ERROR_INSUFFICIENT_BUFFER; ++attempt) {  // (the displays may change between the calls)
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS) return out;
        pathInfo.resize(paths);
        modeInfo.resize(modes);
        status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, pathInfo.data(), &modes, modeInfo.data(), nullptr);
    }
    if (status != ERROR_SUCCESS) return out;
    for (UINT32 i = 0; i < paths; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof source;
        source.header.adapterId = pathInfo[i].sourceInfo.adapterId;
        source.header.id = pathInfo[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || wcscmp(source.viewGdiDeviceName, info.szDevice) != 0) continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof white;
        white.header.adapterId = pathInfo[i].targetInfo.adapterId;
        white.header.id = pathInfo[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0) {
            out.sdrWhiteNits = white.SDRWhiteLevel / 1000.0 * 80.0;  // (in thousandths of 80 nits)
            out.whiteKnown = true;
        }
        break;
    }
    return out;
}
#endif

}  // namespace

// The GPU side: the RHI, the swap chain and what is drawn with (the RHI is declared first so it goes last). On Linux
// the picture and overlay are drawn into `linear` (half floats), which the encode pass writes into the swap chain.
class HdrSurfaceGpu {
public:
#if defined(MONTAGE_HDR_ENCODED)
    std::unique_ptr<QOffscreenSurface> fallback;  // (OpenGL's, for when the window is not there; outlives the RHI)
#endif
    std::unique_ptr<QRhi> rhi;
    std::unique_ptr<QRhiSwapChain> sc;
    std::unique_ptr<QRhiRenderPassDescriptor> rp, linearRp;
    std::unique_ptr<QRhiBuffer> vbuf, ubufPicture, ubufOverlay, ubufEncode;
    std::unique_ptr<QRhiSampler> sampler;
    std::unique_ptr<QRhiTexture> picture, overlay, linear;
    std::unique_ptr<QRhiTextureRenderTarget> linearRt;
    std::unique_ptr<QRhiShaderResourceBindings> srbPicture, srbOverlay, srbEncode;
    std::unique_ptr<QRhiGraphicsPipeline> psPicture, psOverlay, psEncode;
    bool hasSwapChain = false, vbufUploaded = false, notExposed = false, newlyExposed = false;
    QSize pictureSize{1, 1}, overlaySize{1, 1}, linearSize{1, 1};
    QRhiReadbackResult readSignal, readLight;  // (tests: HdrSurface::keepNextFrame)

    ~HdrSurfaceGpu() {
        // Resources before the RHI that made them.
        psPicture.reset(), psOverlay.reset(), psEncode.reset(), srbPicture.reset(), srbOverlay.reset(), srbEncode.reset();
        linearRt.reset(), linear.reset();
        picture.reset(), overlay.reset(), sampler.reset(), vbuf.reset(), ubufPicture.reset(), ubufOverlay.reset(), ubufEncode.reset();
        linearRp.reset(), rp.reset(), sc.reset();
        rhi.reset();
    }

    QRhiShaderResourceBindings* bindings(QRhiBuffer* ubuf, QRhiTexture* tex) {
        QRhiShaderResourceBindings* srb = rhi->newShaderResourceBindings();
        srb->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, ubuf),
                          QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, tex, sampler.get())});
        srb->create();
        return srb;
    }

    QRhiGraphicsPipeline* pipeline(const QShader& fragment, QRhiShaderResourceBindings* srb, bool blend, QRhiRenderPassDescriptor* pass) {
        QRhiGraphicsPipeline* ps = rhi->newGraphicsPipeline();
        ps->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        ps->setShaderStages({{QRhiShaderStage::Vertex, shader(kViewVertQsb, sizeof kViewVertQsb)}, {QRhiShaderStage::Fragment, fragment}});
        QRhiVertexInputLayout layout;
        layout.setBindings({{2 * sizeof(float)}});
        layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
        ps->setVertexInputLayout(layout);
        ps->setShaderResourceBindings(srb);
        ps->setRenderPassDescriptor(pass);
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
        if (!ps->create()) {
            delete ps;
            return nullptr;
        }
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

    // The linear target at the swap chain's size (Linux), the encode pass pointed at it.
    bool resizeLinear(QSize size) {
        if (linear && linearRt && size == linearSize) return true;
        QRhiTexture* t = rhi->newTexture(QRhiTexture::RGBA16F, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
        if (!t->create()) {
            delete t;
            return false;
        }
        if (!linearRt) {
            linearRt.reset(rhi->newTextureRenderTarget({QRhiColorAttachment(t)}));
            linearRp.reset(linearRt->newCompatibleRenderPassDescriptor());
            linearRt->setRenderPassDescriptor(linearRp.get());
        } else {
            linearRt->setDescription(QRhiTextureRenderTargetDescription(QRhiColorAttachment(t)));
        }
        if (!linearRt->create()) {
            // (the target keeps the texture it was described with; the caller stops HDR)
            if (linear) linear.release()->deleteLater();
            linear.reset(t);
            linearSize = QSize();
            return false;
        }
        if (srbEncode) {
            srbEncode->setBindings({QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, ubufEncode.get()),
                                    QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, t, sampler.get())});
            srbEncode->create();
        }
        if (linear) linear.release()->deleteLater();
        linear.reset(t);
        linearSize = size;
        return true;
    }
};

void HdrSurface::setTestDisplay(double headroom) { gTestHeadroom = headroom; }

void HdrSurface::keepNextFrame() {
    keepFrame_ = true;
    keptSignal_ = QImage();
    keptLight_ = QImage();
    requestUpdate();
}

HdrSurface::HdrSurface(QWidget* target) : target_(target) {
    setFlag(Qt::WindowDoesNotAcceptFocus);  // the editor's shortcuts keep working: focus stays in the widgets
    displayTimer_.setInterval(1000);
    connect(&displayTimer_, &QTimer::timeout, this, [this] {
        if (!hdrActive_) return;
        if (encoded_) readDisplay();  // (Wayland: what the compositor said, first)
        if (gTestHeadroom <= 0 && !displayShowsHdr()) {
            stopHdr(problem_);
            return;
        }
        if (!encoded_) readDisplay();
    });
    connect(this, &QWindow::screenChanged, this, [this] {
        if (!gpu_ || !gpu_->sc || gTestHeadroom > 0) return;
        if (encoded_) {
            // (Wayland: the compositor tells the surface what it prefers on the new output, read every second)
            requestUpdate();
            return;
        }
        // Another display: HDR may have come or gone, and the swap chain is made again for it (macOS resets the layer's
        // colour space when the window changes screens).
        if (!displayShowsHdr()) {
            if (hdrActive_) stopHdr(problem_);
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
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        // A native window is needed: headless platforms (offscreen, minimal, VNC) have no swap chain to show HDR.
        const QString platform = QGuiApplication::platformName();
        if (platform != QLatin1String("cocoa") && platform != QLatin1String("windows") && platform != QLatin1String("direct2d"))
            return fail(tr("HDR viewing needs a desktop display (this one is \"%1\")").arg(platform));
#endif
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
#elif defined(MONTAGE_HDR_ENCODED)
        // Linux and the BSDs: on Wayland, through its colour management (X11 has no way to say a window is HDR). The
        // RHI draws with OpenGL there, into a 10-bit swap chain whose pixels are the BT.2100 signal the surface is
        // described with.
        const QString platform = QGuiApplication::platformName();
        if (!platform.startsWith(QLatin1String("wayland"))) {
            if (platform == QLatin1String("xcb")) return fail(tr("HDR viewing on Linux needs a Wayland session (this one is X11)"));
            return fail(tr("HDR viewing on Linux needs a Wayland session (this display is \"%1\")").arg(platform));
        }
#if defined(MONTAGE_WAYLAND_COLOR)
        QPlatformNativeInterface* native = QGuiApplication::platformNativeInterface();
        wayland_ = std::make_unique<WaylandColor>(native ? static_cast<wl_display*>(native->nativeResourceForIntegration("wl_display")) : nullptr);
        if (!wayland_->available()) return fail(tr("This compositor does not take HDR pictures (it has no Wayland colour management)"));
        if (const std::string why = wayland_->unsupported(); !why.empty())
            return fail(tr("This compositor's colour management cannot describe HDR pictures (%1)").arg(QString::fromStdString(why)));
#elif !defined(MONTAGE_QT_WAYLAND_COLOR)
        return fail(tr("This build cannot describe HDR pictures to Wayland (it needs wayland-protocols 1.41 or later, or Qt 6.10.2)"));
#endif
        QSurfaceFormat format = QSurfaceFormat::defaultFormat();
        format.setRedBufferSize(10);
        format.setGreenBufferSize(10);
        format.setBlueBufferSize(10);
        format.setAlphaBufferSize(2);
#if defined(MONTAGE_QT_WAYLAND_COLOR)
        // Qt describes the window (BT.2100 PQ, the protocol's default luminances; no mastering light).
        format.setColorSpace(QColorSpace(QColorSpace::Primaries::Bt2020, QColorSpace::TransferFunction::St2084));
#else
        // The window's format asks for no colour space, so Qt's own colour management (6.10.2 and later) leaves the
        // surface to Montage's: the protocol allows one colour management surface for each wl_surface.
        format.setColorSpace(QColorSpace());
#endif
        setSurfaceType(QSurface::OpenGLSurface);
        setFormat(format);
        create();
        g.fallback.reset(QRhiGles2InitParams::newFallbackSurface(format));
        QRhiGles2InitParams params;
        params.format = format;
        params.fallbackSurface = g.fallback.get();
        params.window = this;
        g.rhi.reset(QRhi::create(QRhi::OpenGLES2, &params));
        encoded_ = true;
#else
        return fail(tr("HDR viewing is available on macOS (EDR displays), Windows (with HDR switched on) and Wayland on Linux"));
#endif
    }
    if (!g.rhi) return fail(tr("No graphics device for HDR viewing"));
    if (!g.rhi->isTextureFormatSupported(QRhiTexture::RGBA16F)) return fail(tr("The graphics device has no half-float textures"));
    g.sc.reset(g.rhi->newSwapChain());
    g.sc->setWindow(this);
    if (gTestHeadroom <= 0) {
        if (!displayShowsHdr()) return fail(problem_);
        if (!encoded_) g.sc->setFormat(QRhiSwapChain::HDRExtendedSrgbLinear);
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
    QRhiRenderPassDescriptor* drawn = g.rp.get();
#if defined(MONTAGE_HDR_ENCODED)
    if (encoded_) {
        if (!g.rhi->isTextureFormatSupported(QRhiTexture::RGBA16F, QRhiTexture::RenderTarget) || !g.resizeLinear(QSize(1, 1)))
            return fail(tr("The graphics device cannot draw into half-float textures"));
        g.ubufEncode.reset(g.rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformBytes));
        g.ubufEncode->create();
        g.srbEncode.reset(g.bindings(g.ubufEncode.get(), g.linear.get()));
        g.psEncode.reset(g.pipeline(encodeShader(), g.srbEncode.get(), false, g.rp.get()));
        drawn = g.linearRp.get();
    }
#endif
    g.psPicture.reset(g.pipeline(shader(kPictureFragQsb, sizeof kPictureFragQsb), g.srbPicture.get(), false, drawn));
    g.psOverlay.reset(g.pipeline(shader(kOverlayFragQsb, sizeof kOverlayFragQsb), g.srbOverlay.get(), true, drawn));
    if (!g.psPicture || !g.psOverlay || (encoded_ && !g.psEncode)) return fail(tr("The graphics device could not make the HDR viewer's shaders"));
    hdrActive_ = true;
    problem_.clear();
    readDisplay();
    return true;
}

bool HdrSurface::displayShowsHdr() {
    if (!gpu_ || !gpu_->sc) return false;
    if (encoded_) {
        // Wayland: what the compositor prefers where the viewer is (with no surface to ask about yet, that the
        // compositor takes the description).
#if defined(MONTAGE_WAYLAND_COLOR)
        if (const WaylandDisplay d = waylandDisplay(); d.known ? d.headroom <= 1.0 : d.asked) {
            problem_ = tr("This display does not show HDR");
            return false;
        }
#elif defined(MONTAGE_QT_WAYLAND_COLOR)
        if (handle() && format().colorSpace() != requestedFormat().colorSpace()) {
            problem_ = tr("This compositor does not take HDR pictures (it has no Wayland colour management)");
            return false;
        }
#endif
        return true;
    }
    const bool qt = gpu_->sc->isFormatSupported(QRhiSwapChain::HDRExtendedSrgbLinear);
#if defined(Q_OS_WIN)
    // Both must agree: Windows about the viewer's own display, and Qt (which makes the swap chain) about the main one.
    // (the surface's own window: the monitor the viewer is on, not the one most of the main window is on)
    const WinDisplay d = windowsDisplay(reinterpret_cast<HWND>(winId()));
    if ((d.known && !d.hdr) || (!d.known && !qt)) {
        problem_ = tr("This display does not show HDR");
        return false;
    }
    if (!qt) {
        problem_ = tr("On Windows, HDR viewing also needs the main display to show HDR");
        return false;
    }
    return true;
#else
    if (!qt) problem_ = tr("This display does not show HDR");
    return qt;
#endif
}

HdrSurface::WaylandDisplay HdrSurface::waylandDisplay() {
    WaylandDisplay out;
#if defined(MONTAGE_WAYLAND_COLOR)
    if (!wayland_) return out;
    auto answered = [](const WaylandColor::Preferred& p) {
        WaylandDisplay d;
        d.asked = true;
        d.known = p.known;
        if (p.known) {
            d.headroom = hdrHeadroomFromDescription(p.referenceNits, p.maxNits, p.targetMaxNits);
            d.whiteNits = p.referenceNits;
        }
        return d;
    };
    QPlatformNativeInterface* native = QGuiApplication::platformNativeInterface();
    if (wayland_->surface()) {
        // The surface's own answer, read again first if the compositor said it changed (another output, its
        // brightness). It stands when it is HDR, and when it is SDR once the surface has had a frame up for a while.
        if (isVisible() && native && native->nativeResourceForWindow("surface", this) == wayland_->surface()) wayland_->dispatch();
        out = answered(wayland_->preferred());
        if (out.known && (out.headroom > 1.0 || (ownFrame_.isValid() && ownFrame_.elapsed() >= 2000))) return out;
    }
    // Else the viewer's top-level window's, once that is shown: it is on its output already, while a surface with no
    // frame yet is on none, and a compositor answers for such a surface with a default (its first output's, or sRGB)
    // until it is on one. (Also where the compositor never says what it prefers for the surface itself.)
    QWindow* top = target_ ? target_->window()->windowHandle() : nullptr;
    auto* surface = native && top && top->isExposed() ? static_cast<wl_surface*>(native->nativeResourceForWindow("surface", top)) : nullptr;
    if (surface) {
        const WaylandDisplay window = answered(wayland_->preferredFor(surface));
        // (the surface's own answer stays if the window's is not HDR either)
        if (!out.known || (window.known && window.headroom > 1.0)) out = window;
    }
#endif
    return out;
}

void HdrSurface::stopHdr(const QString& why) {
    if (!hdrActive_) return;
    hdrActive_ = false;
    problem_ = why.isEmpty() ? tr("This display does not show HDR") : why;
    displayTimer_.stop();
    emit displayChanged();
}

void HdrSurface::readDisplay() {
    // Asked at most once a second (it is not cheap; macOS changes the headroom with the brightness, and reads 1 until a
    // frame has been shown).
    if (displayRead_.isValid() && displayRead_.elapsed() < 1000) return;
    displayRead_.start();
    double h = 1, white = 0;
    float sc = 1;
    if (gTestHeadroom > 0) {
        h = gTestHeadroom;
    } else if (encoded_) {
#if defined(MONTAGE_WAYLAND_COLOR)
        // What the compositor prefers where the viewer is: its reference white (where the picture's 203 nits go) and
        // peak, asked here every second.
        const WaylandDisplay d = waylandDisplay();
        if (!d.known) return;
        h = d.headroom;
        white = d.whiteNits;
#else
        // Qt describes the surface and the compositor fits it to the display, which it does not say: nothing is rolled
        // off here, whatever the picture (PQ's whole range over its reference white).
        h = 10000.0 / 203;
#endif
    } else if (gpu_ && gpu_->sc && gpu_->hasSwapChain) {
#if defined(Q_OS_WIN)
        // The viewer's own display (Qt's answer is about the main one).
        const WinDisplay d = windowsDisplay(reinterpret_cast<HWND>(winId()));
        if (d.known && d.maxNits > 0) {
            // SDR white from the display configuration, else as Qt reads it, else Windows' default.
            white = d.sdrWhiteNits;
            if (!d.whiteKnown) {
                const QRhiSwapChainHdrInfo info = gpu_->sc->hdrInfo();
                white = info.sdrWhiteLevel > 0 ? info.sdrWhiteLevel : 80;
            }
            h = d.maxNits / white;
            sc = float(white / 80);
        } else
#endif
        {
            const QRhiSwapChainHdrInfo info = gpu_->sc->hdrInfo();
            if (info.limitsType == QRhiSwapChainHdrInfo::ColorComponentValue) {
                h = info.limits.colorComponentValue.maxColorComponentValue;  // macOS EDR: 1.0 is SDR white
            } else {
                // Windows scRGB: 1.0 is 80 nits; SDR white is the user's setting.
                white = info.sdrWhiteLevel > 0 ? info.sdrWhiteLevel : 80;
                h = info.limits.luminanceInNits.maxLuminance / white;
                sc = float(white / 80);
            }
        }
    } else {
        return;
    }
    h = std::max(1.0, std::isfinite(h) ? h : 1.0);
    if (std::fabs(h - headroom_) > 0.01 || sc != scale_ || white != whiteNits_) {
        headroom_ = h;
        scale_ = sc;
        whiteNits_ = white;
        emit displayChanged();
    }
}

int HdrSurface::bufferBits() const {
    if (!gpu_ || !gpu_->rhi) return 0;
    if (!encoded_) return 16;
#if defined(MONTAGE_HDR_ENCODED)
    if (const auto* h = static_cast<const QRhiGles2NativeHandles*>(gpu_->rhi->nativeHandles()); h && h->context)
        return h->context->format().redBufferSize();
#endif
    return 0;
}

Transfer HdrSurface::describedTransfer() const {
#if defined(MONTAGE_WAYLAND_COLOR)
    if (wayland_ && wayland_->described()) return wayland_->described()->transfer;
#elif defined(MONTAGE_QT_WAYLAND_COLOR)
    if (encoded_ && handle() && format().colorSpace() == requestedFormat().colorSpace()) return Transfer::Pq;
#endif
    return Transfer::Linear;
}

void HdrSurface::describeSurface() {
#if defined(MONTAGE_WAYLAND_COLOR)
    if (!wayland_ || !hdrActive_) return;
    QPlatformNativeInterface* native = QGuiApplication::platformNativeInterface();
    auto* surface = native ? static_cast<wl_surface*>(native->nativeResourceForWindow("surface", this)) : nullptr;
    if (!surface) return;
    if (surface != wayland_->surface()) {
        // A new wl_surface (the first, or made again when the window was shown again): its objects, and what the
        // compositor prefers for it.
#if defined(MONTAGE_WAYLAND_SURFACE_SIGNALS)
        // (let go when Qt lets the wl_surface go, so one made again at the same address is attached afresh)
        if (auto* w = nativeInterface<QNativeInterface::Private::QWaylandWindow>(); w && static_cast<QObject*>(w) != waylandWindow_.data()) {
            waylandWindow_ = w;
            connect(w, &QNativeInterface::Private::QWaylandWindow::surfaceDestroyed, this, [this] {
                if (wayland_) wayland_->detach();
            });
        }
#endif
        if (!wayland_->attach(surface)) {
            stopHdr(tr("The connection to the Wayland compositor failed"));
            return;
        }
        ownFrame_.invalidate();  // (no frame on this wl_surface yet)
        displayRead_.invalidate();
        readDisplay();
    }
    // PQ, or HLG for HLG pictures where the compositor takes it; the picture's light as shown on this display.
    WaylandColor::Description d;
    d.transfer = picture_ && picture_->transfer == Transfer::Hlg && wayland_->supportsHlg() ? Transfer::Hlg : Transfer::Pq;
    if (picture_) d.light = hdrSurfaceLight(*picture_, headroom_);
    else if (const WaylandColor::Description* now = wayland_->described()) d = *now;  // (nothing to show yet: as it is)
    if (const WaylandColor::Description* now = wayland_->described(); now && *now == d) return;
    std::string why;
    if (!wayland_->describe(d, &why))
        stopHdr(tr("The compositor did not take the HDR picture's description (%1)").arg(QString::fromStdString(why)));
#endif
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
        if (!g.hasSwapChain && g.rhi->isDeviceLost()) {
            stopHdr(tr("The graphics device was reset"));
            return;
        }
        // Wayland: the surface described, and what the compositor prefers for it read.
        if (g.hasSwapChain && encoded_) describeSurface();
        if (!hdrActive_) return;
        // Now inside the viewer: asked again (on Windows Qt makes an SDR swap chain where it sees an SDR display).
        if (g.hasSwapChain && gTestHeadroom <= 0 && !displayShowsHdr()) {
            stopHdr(problem_);
            return;
        }
        displayRead_.invalidate();
        readDisplay();
    }
    if (isExposed() && hdrActive_) {
        if (!displayTimer_.isActive()) displayTimer_.start();
    } else {
        displayTimer_.stop();
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
            if (static_cast<QPlatformSurfaceEvent*>(e)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
#if defined(MONTAGE_WAYLAND_COLOR)
                if (wayland_) wayland_->detach();
#endif
                if (gpu_ && gpu_->sc) {
                    gpu_->sc->destroy();
                    gpu_->hasSwapChain = false;
                }
            }
            break;
#if defined(MONTAGE_WAYLAND_COLOR)
        case QEvent::Hide:
            // (Qt lets the wl_surface go with the window hidden: shown again, the new one is described afresh)
            if (wayland_) wayland_->detach();
            break;
#endif
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
    // A swap chain that cannot be made again because the device is gone: back to SDR, as below.
    auto remade = [&] {
        g.hasSwapChain = g.sc->createOrResize();
        if (!g.hasSwapChain && g.rhi->isDeviceLost()) stopHdr(tr("The graphics device was reset"));
        return g.hasSwapChain;
    };
    if (g.sc->currentPixelSize() != g.sc->surfacePixelSize() || g.newlyExposed) {
        if (!remade()) return;
        g.newlyExposed = false;
    }
    readDisplay();
    // Linux: the surface described for this picture (applied with this frame's commit), and the linear target at the
    // swap chain's size.
    const bool encode = encoded_ && g.linearRt;
    if (encode) {
        describeSurface();
        if (!hdrActive_) return;
        if (!g.resizeLinear(g.sc->currentPixelSize())) {
            stopHdr(tr("The graphics device cannot draw into half-float textures"));
            return;
        }
    }
    QRhi::FrameOpResult r = g.rhi->beginFrame(g.sc.get());
    if (r == QRhi::FrameOpSwapChainOutOfDate) {
        if (!remade()) return;
        r = g.rhi->beginFrame(g.sc.get());
    }
    if (r == QRhi::FrameOpDeviceLost || r == QRhi::FrameOpError) {
        // (a driver reset or update, a GPU switch) The viewer goes back to its SDR picture; turning HDR on again starts over.
        stopHdr(tr("The graphics device was reset"));
        return;
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
        // (not copied: the picture is held until the frame is drawn)
        QRhiTextureSubresourceUploadDescription d(QByteArray::fromRawData(reinterpret_cast<const char*>(picture_->rgba.data()),
                                                                          qsizetype(size_t(size.width()) * size_t(size.height()) * 8)));
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
    if (encode) {
        // The whole window, the linear texture's first row where the backend puts it (OpenGL: at the bottom), and the
        // transfer the surface is described with.
        float data[24];
        std::memcpy(data, corr.constData(), 16 * sizeof(float));
        const float top = g.rhi->isYUpInFramebuffer() ? -1 : 1;
        data[16] = -1, data[17] = top, data[18] = 1, data[19] = -top;
        data[20] = data[21] = data[22] = 0;
        data[23] = describedTransfer() == Transfer::Hlg ? 2 : 1;
        u->updateDynamicBuffer(g.ubufEncode.get(), 0, kUniformBytes, data);
    }

    // (tests) This frame read back as its last pass ends: the swap chain's pixels, then the linear light under them.
    // (OpenGL, where the encode pass is: read as the frame ends)
    QRhiResourceUpdateBatch* kept = nullptr;
    if (encode && keepFrame_) {
        kept = g.rhi->nextResourceUpdateBatch();
        g.readSignal = QRhiReadbackResult();
        g.readLight = QRhiReadbackResult();
        kept->readBackTexture(QRhiReadbackDescription(), &g.readSignal);
        kept->readBackTexture(QRhiReadbackDescription(g.linear.get()), &g.readLight);
    }

    QRhiCommandBuffer* cb = g.sc->currentFrameCommandBuffer();
    const QSize px = g.sc->currentPixelSize();
    QRhiRenderTarget* target = encode ? static_cast<QRhiRenderTarget*>(g.linearRt.get()) : g.sc->currentFrameRenderTarget();
    cb->beginPass(target, Qt::black, {1.0f, 0}, u);
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
    if (encode) {
        cb->beginPass(g.sc->currentFrameRenderTarget(), Qt::black, {1.0f, 0});
        cb->setGraphicsPipeline(g.psEncode.get());
        cb->setViewport({0, 0, float(px.width()), float(px.height())});
        cb->setShaderResources(g.srbEncode.get());
        cb->setVertexInput(0, 1, &input);
        cb->draw(4);
        cb->endPass(kept);
    }
    if (g.rhi->endFrame(g.sc.get()) == QRhi::FrameOpDeviceLost) {
        stopHdr(tr("The graphics device was reset"));
        return;
    }
#if defined(MONTAGE_HDR_ENCODED)
    if (kept) {
        keepFrame_ = false;
        const bool bottomFirst = g.rhi->isYUpInFramebuffer();
        keptSignal_ = readBack(g.readSignal, QRhiTexture::RGBA8, QImage::Format_RGBA8888, 4, bottomFirst);
        keptLight_ = readBack(g.readLight, QRhiTexture::RGBA16F, QImage::Format_RGBA16FPx4, 8, bottomFirst);
    }
#endif
    if (!ownFrame_.isValid()) ownFrame_.start();
    if (++frames_ == 1) {
        // macOS reports the headroom once a frame is up: asked again straight after the first.
        QTimer::singleShot(100, this, [this] {
            displayRead_.invalidate();
            readDisplay();
        });
    }
}

#endif

}  // namespace montage
