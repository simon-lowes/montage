#include "GpuCompositor.h"

#include <QImage>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>

#include "Processing.h"

namespace montage::gpu {

namespace {

std::mutex gStatsMutex;
Stats gStats;

bool logFallbacks() {
    static const bool on = [] {
        const char* v = std::getenv("MONTAGE_GPU_LOG");
        return v && *v && std::string(v) != "0";
    }();
    return on;
}

}  // namespace

Stats stats() {
    std::lock_guard lock(gStatsMutex);
    return gStats;
}

void resetStats() {
    std::lock_guard lock(gStatsMutex);
    gStats = {};
}

void countFallback(const std::string& why) {
    {
        std::lock_guard lock(gStatsMutex);
        ++gStats.cpuFrames;
        gStats.lastFallback = why;
    }
    if (logFallbacks()) std::fprintf(stderr, "montage gpu: frame on the CPU: %s\n", why.c_str());
}

void countLayer(bool onGpu) {
    std::lock_guard lock(gStatsMutex);
    ++(onGpu ? gStats.gpuLayers : gStats.cpuLayers);
}

}  // namespace montage::gpu

#ifndef MONTAGE_WITH_GPU

namespace montage::gpu {

static const char* const kNoGpu = "this build has no GPU compositor (it needs Qt 6.6 or later)";

bool initialize(std::string* why) {
    if (why) *why = kNoGpu;
    return false;
}
bool available(std::string* why) { return initialize(why); }
std::string deviceName() { return {}; }
int maxTextureSize() { return 0; }
void shutdown() {}
bool execute(const Frame&, Image*, QImage*) { return false; }

}  // namespace montage::gpu

#else

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QThread>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <rhi/qrhi.h>
#include <shared_mutex>
#include <thread>

namespace montage::gpu {

namespace {

#include "shaders/GpuShaders.inc"

QShader shader(const unsigned char* data, size_t size) {
    return QShader::fromSerialized(QByteArray::fromRawData(reinterpret_cast<const char*>(data), qsizetype(size)));
}

constexpr int kUniformBytes = 16 * 16;  // vec4 p[16]

// The GPU: one thread owns the RHI and does all its work, one frame at a time (the RHI is not thread safe, and
// frames are asked for from several render threads).
class Device {
public:
    ~Device() { stop(); }

    bool start(std::string* why);
    void stop();
    bool ready() const { return ready_; }
    const std::string& problem() const { return problem_; }
    const std::string& name() const { return name_; }
    int maxTextureSize() const { return maxTexture_; }

    // fn run on the GPU's thread, waited for.
    template <typename F>
    auto run(F fn) -> decltype(fn()) {
        auto task = std::make_shared<std::packaged_task<decltype(fn())()>>(std::move(fn));
        auto result = task->get_future();
        {
            std::lock_guard lock(queueMutex_);
            queue_.push_back([task] { (*task)(); });
        }
        queueCv_.notify_one();
        return result.get();
    }

    bool draw(const Frame& frame, Image* out, QImage* out8);

private:
    void loop();
    bool create();  // on the GPU's thread
    void destroy();

    struct Target {
        QRhiTexture* tex = nullptr;
        QRhiTextureRenderTarget* rt = nullptr;
        QRhiRenderPassDescriptor* rp = nullptr;
    };
    QRhiTexture* texture(QRhiTexture::Format format, int w, int h, bool target);
    void release(QRhiTexture* tex);  // back to the pool, for later passes of the frame
    Target* target(QRhiTexture* tex);  // (null, and the frame failed, when the device cannot draw into it)
    QRhiTexture* lutTexture(const std::shared_ptr<const Lut3D>& lut, QRhiResourceUpdateBatch* u, QRhiTexture** table1d);
    QRhiBuffer* uniforms(QRhiResourceUpdateBatch* u, const float (&p)[64]);
    void pass(QRhiGraphicsPipeline* ps, QRhiTexture* into, QRhiResourceUpdateBatch* u,
              const std::vector<std::pair<int, QRhiTexture*>>& textures, QRhiBuffer* ubuf, QRhiResourceUpdateBatch* after = nullptr);
    QRhiTexture* layer(const Layer& l, QRhiResourceUpdateBatch*& u);
    void endFrame();

    std::thread thread_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<std::function<void()>> queue_;
    bool quit_ = false;

    QOffscreenSurface* fallback_ = nullptr;  // (OpenGL; made on the GUI thread)
    QSurfaceFormat glFormat_;                // (OpenGL 3.3: float and 3D textures; the surface may not say so)
    QRhi::Implementation backend_ = QRhi::OpenGLES2;
    bool software_ = false;
    bool ready_ = false;
    std::string problem_, name_;
    int maxTexture_ = 0;

    QRhi* rhi_ = nullptr;
    QRhiCommandBuffer* cb_ = nullptr;
    QRhiSampler* sampler_ = nullptr;
    QRhiTexture *dummy2d_ = nullptr, *dummy3d_ = nullptr, *dummy16_ = nullptr, *dummyR_ = nullptr;
    QRhiShaderResourceBindings* layout_ = nullptr;
    QRhiGraphicsPipeline *colorPs_ = nullptr, *compositePs_ = nullptr, *outputPs_ = nullptr, *output8Ps_ = nullptr;
    QRhiRenderPassDescriptor *rpFloat_ = nullptr, *rp8_ = nullptr;
    QRhiTexture *rpFloatTex_ = nullptr, *rp8Tex_ = nullptr;  // (the small targets they were made from)
    // Textures kept between frames (by format and size), lent out within one. Only let go between frames: a pass
    // recorded earlier in a frame may still read one given back.
    struct Pooled {
        QRhiTexture* tex;
        bool target, used;
        long lastFrame;
        size_t bytes;
    };
    std::vector<Pooled> pool_;
    long frameNo_ = 0;
    bool failed_ = false;  // a pass of this frame could not be drawn
    std::map<QRhiTexture*, Target> targets_;
    std::vector<QRhiBuffer*> ubufs_;
    size_t ubufUsed_ = 0;
    std::vector<QRhiShaderResourceBindings*> frameSrbs_;
    // LUTs uploaded, by the LUT (kept while it lives).
    struct LutTex {
        std::weak_ptr<const Lut3D> lut;
        QRhiTexture* tex;
        bool is1D;
    };
    std::map<const Lut3D*, LutTex> luts_;
};

bool Device::start(std::string* why) {
    if (ready_) return true;
    if (!problem_.empty()) {
        if (why) *why = problem_;
        return false;
    }
    const QByteArray choice = qgetenv("MONTAGE_GPU").toLower();
    if (choice == "off" || choice == "0" || choice == "none") {
        problem_ = "switched off (MONTAGE_GPU=off)";
        if (why) *why = problem_;
        return false;
    }
#if defined(Q_OS_MACOS)
    backend_ = QRhi::Metal;
#elif defined(Q_OS_WIN)
    backend_ = QRhi::D3D11;
#else
    backend_ = QRhi::OpenGLES2;
#endif
    if (choice == "opengl" || choice == "gl") backend_ = QRhi::OpenGLES2;
    else if (choice == "metal") backend_ = QRhi::Metal;
    else if (choice == "d3d11") backend_ = QRhi::D3D11;
    else if (choice == "d3d12") backend_ = QRhi::D3D12;
    else if (choice == "software") software_ = true;
    if (backend_ == QRhi::OpenGLES2) {
        if (!qGuiApp || QThread::currentThread() != qGuiApp->thread()) {
            if (why) *why = "not set up (OpenGL is set up from the GUI thread)";
            return false;  // (not remembered: initialize() from the GUI thread can still)
        }
        QSurfaceFormat format = QSurfaceFormat::defaultFormat();
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        glFormat_ = format;
        fallback_ = QRhiGles2InitParams::newFallbackSurface(format);
    }
    thread_ = std::thread([this] { loop(); });
    const bool ok = run([this] { return create(); });
    if (!ok) {
        stop();
        if (problem_.empty()) problem_ = "no graphics device";
    }
    ready_ = ok;
    if (why && !ok) *why = problem_;
    return ok;
}

void Device::stop() {
    if (thread_.joinable()) {
        run([this] {
            destroy();
            return 0;
        });
        {
            std::lock_guard lock(queueMutex_);
            quit_ = true;
        }
        queueCv_.notify_one();
        thread_.join();
        quit_ = false;
    }
    delete fallback_;
    fallback_ = nullptr;
    ready_ = false;
}

void Device::loop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(queueMutex_);
            queueCv_.wait(lock, [this] { return quit_ || !queue_.empty(); });
            if (queue_.empty()) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        job();
    }
}

bool Device::create() {
    const QRhi::Flags flags = software_ ? QRhi::PreferSoftwareRenderer : QRhi::Flags();
    switch (backend_) {
        case QRhi::OpenGLES2: {
            QRhiGles2InitParams params;
            params.fallbackSurface = fallback_;
            params.format = glFormat_;
            rhi_ = QRhi::create(QRhi::OpenGLES2, &params, flags);
            break;
        }
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS)
        case QRhi::Metal: {
            QRhiMetalInitParams params;
            rhi_ = QRhi::create(QRhi::Metal, &params, flags);
            break;
        }
#endif
#if defined(Q_OS_WIN)
        case QRhi::D3D11: {
            QRhiD3D11InitParams params;
            rhi_ = QRhi::create(QRhi::D3D11, &params, flags);
            break;
        }
        case QRhi::D3D12: {
            QRhiD3D12InitParams params;
            rhi_ = QRhi::create(QRhi::D3D12, &params, flags);
            break;
        }
#endif
        default: break;
    }
    if (!rhi_) {
        problem_ = "no graphics device for the GPU compositor";
        return false;
    }
    using T = QRhiTexture;
    if (!rhi_->isTextureFormatSupported(T::RGBA32F, T::RenderTarget) || !rhi_->isTextureFormatSupported(T::RG16) ||
        !rhi_->isTextureFormatSupported(T::R32F) ||
        !rhi_->isFeatureSupported(QRhi::ThreeDimensionalTextures)) {
        problem_ = "the graphics device lacks float render targets, 16-bit textures or 3D textures";
        return false;
    }
    name_ = std::string(rhi_->backendName()) + ", " + rhi_->driverInfo().deviceName.toStdString();
    sampler_ = rhi_->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None, QRhiSampler::ClampToEdge,
                                QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
    sampler_->create();
    dummy2d_ = rhi_->newTexture(T::RGBA32F, QSize(1, 1));
    dummy3d_ = rhi_->newTexture(T::RGBA32F, 2, 2, 2, 1, T::ThreeDimensional);
    dummy16_ = rhi_->newTexture(T::RG16, QSize(2, 1));
    dummyR_ = rhi_->newTexture(T::R32F, QSize(1, 1));
    if (!dummy2d_->create() || !dummy3d_->create() || !dummy16_->create() || !dummyR_->create()) {
        problem_ = "the graphics device cannot make the compositor's textures";
        return false;
    }
    // One layout for every pipeline: uniforms at 0, textures at 1 to 5 (gpucolor.frag uses them all).
    using B = QRhiShaderResourceBinding;
    layout_ = rhi_->newShaderResourceBindings();
    QRhiBuffer* ub = rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformBytes);
    ub->create();
    ubufs_.push_back(ub);
    layout_->setBindings({B::uniformBuffer(0, B::FragmentStage, ub), B::sampledTexture(1, B::FragmentStage, dummy2d_, sampler_),
                          B::sampledTexture(2, B::FragmentStage, dummy16_, sampler_), B::sampledTexture(3, B::FragmentStage, dummy3d_, sampler_),
                          B::sampledTexture(4, B::FragmentStage, dummy2d_, sampler_), B::sampledTexture(5, B::FragmentStage, dummyR_, sampler_)});
    layout_->create();
    // Render pass descriptors for float and 8-bit targets, from small targets of each.
    rpFloatTex_ = rhi_->newTexture(T::RGBA32F, QSize(1, 1), 1, T::RenderTarget);
    rp8Tex_ = rhi_->newTexture(T::RGBA8, QSize(1, 1), 1, T::RenderTarget);
    const Target* f = rpFloatTex_->create() ? target(rpFloatTex_) : nullptr;
    const Target* b = rp8Tex_->create() ? target(rp8Tex_) : nullptr;
    if (!f || !b) {
        problem_ = "the graphics device cannot draw into float textures";
        return false;
    }
    rpFloat_ = f->rp;
    rp8_ = b->rp;
    maxTexture_ = rhi_->resourceLimit(QRhi::TextureSizeMax);
    const QShader vert = shader(kQuadVertQsb, sizeof kQuadVertQsb);
    auto pipeline = [&](const unsigned char* frag, size_t size, QRhiRenderPassDescriptor* rp) {
        QRhiGraphicsPipeline* ps = rhi_->newGraphicsPipeline();
        ps->setTopology(QRhiGraphicsPipeline::Triangles);
        ps->setShaderStages({{QRhiShaderStage::Vertex, vert}, {QRhiShaderStage::Fragment, shader(frag, size)}});
        ps->setVertexInputLayout({});
        ps->setShaderResourceBindings(layout_);
        ps->setRenderPassDescriptor(rp);
        return ps->create() ? ps : (delete ps, nullptr);
    };
    colorPs_ = pipeline(kColorFragQsb, sizeof kColorFragQsb, rpFloat_);
    compositePs_ = pipeline(kCompositeFragQsb, sizeof kCompositeFragQsb, rpFloat_);
    outputPs_ = pipeline(kOutputFragQsb, sizeof kOutputFragQsb, rpFloat_);
    output8Ps_ = pipeline(kOutputFragQsb, sizeof kOutputFragQsb, rp8_);
    if (!colorPs_ || !compositePs_ || !outputPs_ || !output8Ps_) {
        problem_ = "the graphics device cannot build the compositor's shaders";
        return false;
    }
    return true;
}

void Device::destroy() {
    for (auto& [lut, t] : luts_) delete t.tex;
    luts_.clear();
    for (auto& [tex, t] : targets_) delete t.rt, delete t.rp;
    targets_.clear();
    for (Pooled& p : pool_) delete p.tex;
    pool_.clear();
    for (QRhiBuffer* b : ubufs_) delete b;
    ubufs_.clear();
    delete colorPs_, delete compositePs_, delete outputPs_, delete output8Ps_;
    colorPs_ = compositePs_ = outputPs_ = output8Ps_ = nullptr;
    rpFloat_ = rp8_ = nullptr;  // (the targets' own)
    delete layout_, layout_ = nullptr;
    delete dummy2d_, delete dummy3d_, delete dummy16_, delete dummyR_, delete rpFloatTex_, delete rp8Tex_;
    dummy2d_ = dummy3d_ = dummy16_ = dummyR_ = rpFloatTex_ = rp8Tex_ = nullptr;
    delete sampler_, sampler_ = nullptr;
    delete rhi_, rhi_ = nullptr;
}

QRhiTexture* Device::texture(QRhiTexture::Format format, int w, int h, bool isTarget) {
    for (Pooled& p : pool_)
        if (!p.used && p.target == isTarget && p.tex->format() == format && p.tex->pixelSize() == QSize(w, h)) {
            p.used = true;
            p.lastFrame = frameNo_;
            return p.tex;
        }
    QRhiTexture* t = rhi_->newTexture(format, QSize(w, h), 1, isTarget ? QRhiTexture::RenderTarget : QRhiTexture::Flags());
    if (!t->create()) {
        delete t;
        failed_ = true;
        return nullptr;
    }
    const size_t texel = format == QRhiTexture::RGBA32F ? 16 : 4;  // (RGBA8, RG16 and R32F alike)
    pool_.push_back({t, isTarget, true, frameNo_, size_t(w) * size_t(h) * texel});
    return t;
}

void Device::release(QRhiTexture* tex) {
    for (Pooled& p : pool_)
        if (p.tex == tex) p.used = false;
}

Device::Target* Device::target(QRhiTexture* tex) {
    Target& t = targets_[tex];
    if (!t.rt) {
        t.tex = tex;
        t.rt = rhi_->newTextureRenderTarget({QRhiColorAttachment(tex)});
        t.rp = t.rt->newCompatibleRenderPassDescriptor();
        t.rt->setRenderPassDescriptor(t.rp);
        if (!t.rt->create()) {
            delete t.rt, delete t.rp;
            targets_.erase(tex);
            failed_ = true;
            return nullptr;
        }
    }
    return &t;
}

QRhiTexture* Device::lutTexture(const std::shared_ptr<const Lut3D>& lut, QRhiResourceUpdateBatch* u, QRhiTexture** table1d) {
    *table1d = dummy2d_;
    if (!lut || lut->size < 2) return dummy3d_;
    // Forget LUTs that are gone.
    for (auto it = luts_.begin(); it != luts_.end();)
        if (it->second.lut.expired()) {
            delete it->second.tex;
            it = luts_.erase(it);
        } else {
            ++it;
        }
    auto it = luts_.find(lut.get());
    if (it == luts_.end()) {
        const int n = lut->size;
        const size_t texels = lut->is1D ? size_t(n) : size_t(n) * n * n;
        std::vector<float> rgba(texels * 4, 1.0f);
        for (size_t i = 0; i < texels && i * 3 + 2 < lut->data.size(); ++i)
            for (int c = 0; c < 3; ++c) rgba[i * 4 + size_t(c)] = lut->data[i * 3 + size_t(c)];
        QRhiTexture* t = lut->is1D ? rhi_->newTexture(QRhiTexture::RGBA32F, QSize(n, 1))
                                   : rhi_->newTexture(QRhiTexture::RGBA32F, n, n, n, 1, QRhiTexture::ThreeDimensional);
        if (!t->create()) {
            delete t;
            return nullptr;
        }
        QVarLengthArray<QRhiTextureUploadEntry, 1> entries;
        const quint32 slice = quint32(size_t(n) * (lut->is1D ? 1 : size_t(n)) * 16);
        for (int z = 0; z < (lut->is1D ? 1 : n); ++z) {
            QRhiTextureSubresourceUploadDescription d(rgba.data() + size_t(z) * slice / 4, slice);
            entries.append(QRhiTextureUploadEntry(z, 0, d));
        }
        QRhiTextureUploadDescription desc;
        desc.setEntries(entries.cbegin(), entries.cend());
        u->uploadTexture(t, desc);
        it = luts_.emplace(lut.get(), LutTex{lut, t, lut->is1D}).first;
    }
    if (it->second.is1D) {
        *table1d = it->second.tex;
        return dummy3d_;
    }
    return it->second.tex;
}

QRhiBuffer* Device::uniforms(QRhiResourceUpdateBatch* u, const float (&p)[64]) {
    if (ubufUsed_ >= ubufs_.size()) {
        QRhiBuffer* b = rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformBytes);
        b->create();
        ubufs_.push_back(b);
    }
    QRhiBuffer* b = ubufs_[ubufUsed_++];
    u->updateDynamicBuffer(b, 0, kUniformBytes, p);
    return b;
}

void Device::pass(QRhiGraphicsPipeline* ps, QRhiTexture* into, QRhiResourceUpdateBatch* u,
                  const std::vector<std::pair<int, QRhiTexture*>>& textures, QRhiBuffer* ubuf, QRhiResourceUpdateBatch* after) {
    using B = QRhiShaderResourceBinding;
    QRhiTexture* bound[6] = {nullptr, dummy2d_, dummy16_, dummy3d_, dummy2d_, dummyR_};
    for (const auto& [binding, tex] : textures) bound[binding] = tex;
    QRhiShaderResourceBindings* srb = rhi_->newShaderResourceBindings();
    srb->setBindings({B::uniformBuffer(0, B::FragmentStage, ubuf), B::sampledTexture(1, B::FragmentStage, bound[1], sampler_),
                      B::sampledTexture(2, B::FragmentStage, bound[2], sampler_), B::sampledTexture(3, B::FragmentStage, bound[3], sampler_),
                      B::sampledTexture(4, B::FragmentStage, bound[4], sampler_), B::sampledTexture(5, B::FragmentStage, bound[5], sampler_)});
    srb->create();
    frameSrbs_.push_back(srb);
    const Target* t = target(into);
    if (!t) {
        u->release();
        if (after) after->release();
        return;
    }
    cb_->beginPass(t->rt, Qt::transparent, {1.0f, 0}, u);
    cb_->setGraphicsPipeline(ps);
    const QSize size = into->pixelSize();
    cb_->setViewport({0, 0, float(size.width()), float(size.height())});
    cb_->setShaderResources(srb);
    cb_->draw(3);
    cb_->endPass(after);
}

// A layer's picture at its source size, through its colour steps (null when it could not be made).
QRhiTexture* Device::layer(const Layer& l, QRhiResourceUpdateBatch*& u) {
    using T = QRhiTexture;
    QRhiTexture* cur = nullptr;
    float p[64] = {};
    if (l.frame) {
        const int w = l.frame->width, h = l.frame->height;
        QRhiTexture* raw = texture(T::RG16, w * 2, h, false);
        cur = texture(T::RGBA32F, w, h, true);
        if (!raw || !cur) return nullptr;
        QRhiTextureSubresourceUploadDescription d(l.frame->px.data(), quint32(size_t(w) * h * 8));
        d.setDataStride(quint32(w) * 8);
        u->uploadTexture(raw, QRhiTextureUploadEntry(0, 0, d));
        QRhiTexture* table = nullptr;
        QRhiTexture* lut = lutTexture(l.input, u, &table);
        if (!lut) return nullptr;
        p[0] = 0;  // unpack
        if (l.input) {
            p[1] = float(l.input->size), p[2] = l.input->is1D ? 1 : 0, p[3] = 1;  // (clamped first, as convertColor does)
            for (int i = 0; i < 3; ++i) p[4 + i] = l.input->domainMin[i], p[8 + i] = l.input->domainMax[i];
            p[12] = 1, p[13] = 1;
        }
        QRhiBuffer* ub = uniforms(u, p);
        pass(colorPs_, cur, u, {{2, raw}, {3, lut}, {4, table}}, ub);
        u = rhi_->nextResourceUpdateBatch();
        release(raw);
    } else {
        const int w = l.image.width, h = l.image.height;
        if (w <= 0 || h <= 0) return nullptr;
        cur = texture(T::RGBA32F, w, h, true);
        if (!cur) return nullptr;
        QRhiTextureSubresourceUploadDescription d(l.image.px.data(), quint32(size_t(w) * h * 16));
        u->uploadTexture(cur, QRhiTextureUploadEntry(0, 0, d));
    }
    std::vector<QRhiTexture*> marked;  // pictures kept for a Mix
    // A grade graph's slots, each let go after the last step that reads it.
    std::vector<QRhiTexture*> kept;
    std::vector<int> lastUse;
    for (size_t i = 0; i < l.ops.size(); ++i) {
        const ColorOp& op = l.ops[i];
        auto use = [&](int s, bool read) {
            if (s < 0) return;
            if (size_t(s) >= kept.size()) kept.resize(size_t(s) + 1, nullptr), lastUse.resize(size_t(s) + 1, -1);
            if (read) lastUse[size_t(s)] = int(i);
        };
        if (op.kind == ColorOp::Store) use(op.slot, false);
        if (op.kind == ColorOp::Load) use(op.slot, true);
        if (op.kind == ColorOp::Pass) {
            for (int k : op.in) use(k, true);
            use(op.out, false);
        }
    }
    auto keep = [&](QRhiTexture* t) {
        return std::find(marked.begin(), marked.end(), t) != marked.end() || std::find(kept.begin(), kept.end(), t) != kept.end();
    };
    auto sweep = [&](size_t i) {
        for (size_t k = 0; k < kept.size(); ++k)
            if (kept[k] && lastUse[k] <= int(i)) {
                QRhiTexture* t = kept[k];
                kept[k] = nullptr;
                if (t != cur && !keep(t)) release(t);
            }
    };
    const QSize base = cur->pixelSize();
    for (size_t i = 0; i < l.ops.size(); ++i) {
        const ColorOp& op = l.ops[i];
        if (op.kind == ColorOp::Mark) {
            marked.push_back(cur);
            continue;
        }
        if (op.kind == ColorOp::Store || op.kind == ColorOp::Load) {
            if (op.kind == ColorOp::Store) {
                kept[size_t(op.slot)] = cur;
            } else {
                QRhiTexture* t = kept[size_t(op.slot)];
                if (!t) return nullptr;
                if (!keep(cur)) release(cur);
                cur = t;
            }
            sweep(i);
            continue;
        }
        if (op.kind == ColorOp::Pass) {
            QRhiTexture* made = texture(T::RGBA32F, base.width(), base.height(), true);
            if (!made) return nullptr;
            std::fill(p, p + 64, 0.0f);
            p[0] = float(op.passMode);
            std::copy(op.params, op.params + 32, p + 4);
            std::vector<std::pair<int, QRhiTexture*>> textures;
            const int binding[3] = {1, 4, 5};
            for (int k = 0; k < 3; ++k)
                if (op.in[k] >= 0) {
                    if (!kept[size_t(op.in[k])]) return nullptr;
                    textures.push_back({binding[k], kept[size_t(op.in[k])]});
                }
            QRhiTexture* m = nullptr;  // (a node's drawn shape)
            if (op.masked) {
                std::copy(op.maskParams, op.maskParams + 12, p + 52);
                if (!op.mask.empty() && op.in[2] < 0) {
                    if (op.mask.size() != size_t(base.width()) * size_t(base.height())) return nullptr;
                    m = texture(T::R32F, base.width(), base.height(), false);
                    if (!m) return nullptr;
                    QRhiTextureSubresourceUploadDescription d(op.mask.data(), quint32(op.mask.size() * sizeof(float)));
                    u->uploadTexture(m, QRhiTextureUploadEntry(0, 0, d));
                    textures.push_back({5, m});
                }
            }
            QRhiBuffer* ub = uniforms(u, p);
            pass(colorPs_, made, u, textures, ub);
            u = rhi_->nextResourceUpdateBatch();
            if (m) release(m);
            kept[size_t(op.out)] = made;
            sweep(i);
            continue;
        }
        const QSize size = cur->pixelSize();
        QRhiTexture* next = texture(T::RGBA32F, size.width(), size.height(), true);
        if (!next) return nullptr;
        std::fill(p, p + 64, 0.0f);
        std::vector<std::pair<int, QRhiTexture*>> textures = {{1, cur}};
        QRhiTexture* input = nullptr;  // (a Mix's: the picture marked)
        if (op.kind == ColorOp::Mix) {
            if (marked.empty()) return nullptr;
            input = marked.back();
            marked.pop_back();
            p[0] = 10;
            textures = {{1, input}, {4, cur}};
        } else if (op.kind == ColorOp::Lut) {
            QRhiTexture* table = nullptr;
            QRhiTexture* lut = lutTexture(op.lut, u, &table);
            if (!lut || !op.lut) return nullptr;
            p[0] = 1, p[1] = float(op.lut->size), p[2] = op.lut->is1D ? 1 : 0, p[3] = op.clampFirst ? 1 : 0;
            for (int i = 0; i < 3; ++i) p[4 + i] = op.lut->domainMin[i], p[8 + i] = op.lut->domainMax[i];
            p[12] = op.strength;
            textures.push_back({3, lut});
            textures.push_back({4, table});
        } else if (op.kind != ColorOp::Mix) {
            // (gpucolor.frag's modes: Color Correct 2 ... Levels 9; the keyers 18 to 20)
            p[0] = op.kind >= ColorOp::ChromaKey ? float(18 + int(op.kind) - int(ColorOp::ChromaKey)) : float(int(op.kind) + 1);
            std::copy(op.params, op.params + 32, p + 4);  // from p[1]
            const int rows = op.tableWidth > 0 ? int(op.table.size() / (size_t(op.tableWidth) * 4)) : 0;
            if (rows > 0) {
                QRhiTexture* table = texture(T::RGBA32F, op.tableWidth, rows, false);
                if (!table) return nullptr;
                QRhiTextureSubresourceUploadDescription d(op.table.data(), quint32(op.table.size() * sizeof(float)));
                u->uploadTexture(table, QRhiTextureUploadEntry(0, 0, d));
                textures.push_back({4, table});
            }
        }
        if (op.kind == ColorOp::ScreenKey && (op.params[10] != 0 || op.params[11] != 0)) {
            // Its matte first, shrunk or grown and softened as render/Processing's screenKey does (gpucolor.frag's
            // 21 to 23), then read from the table.
            QRhiTexture* matte = texture(T::RGBA32F, size.width(), size.height(), true);
            if (!matte) return nullptr;
            float q[64] = {};
            std::copy(p, p + 64, q);
            q[0] = 21;
            pass(colorPs_, matte, u, {{1, cur}}, uniforms(u, q));
            u = rhi_->nextResourceUpdateBatch();
            auto step = [&](int mode, int radius, bool maximum, bool column) {
                QRhiTexture* made = texture(T::RGBA32F, size.width(), size.height(), true);
                if (!made) return false;
                std::fill(q, q + 64, 0.0f);
                q[0] = float(mode), q[4] = float(radius), q[5] = maximum ? 1 : 0, q[6] = column ? 1 : 0;
                pass(colorPs_, made, u, {{1, matte}}, uniforms(u, q));
                u = rhi_->nextResourceUpdateBatch();
                release(matte);
                matte = made;
                return true;
            };
            const int shrink = int(op.params[10]), blur = int(op.params[11]);
            bool made = true;
            if (shrink != 0) made = step(22, std::abs(shrink), shrink < 0, false) && step(22, std::abs(shrink), shrink < 0, true);
            for (int k = 0; made && blur > 0 && k < 3; ++k) made = step(23, blur, false, false) && step(23, blur, false, true);
            if (!made) {
                release(matte);
                return nullptr;
            }
            textures.push_back({4, matte});  // (let go after the pass, as tables are)
        }
        if (op.masked) {
            p[48] = 1;  // p[12].x
            std::copy(op.maskParams, op.maskParams + 12, p + 52);
            if (!op.mask.empty()) {
                if (op.mask.size() != size_t(size.width()) * size_t(size.height())) return nullptr;
                QRhiTexture* m = texture(T::R32F, size.width(), size.height(), false);
                if (!m) return nullptr;
                QRhiTextureSubresourceUploadDescription d(op.mask.data(), quint32(op.mask.size() * sizeof(float)));
                u->uploadTexture(m, QRhiTextureUploadEntry(0, 0, d));
                textures.push_back({5, m});
            }
        }
        QRhiBuffer* ub = uniforms(u, p);
        pass(colorPs_, next, u, textures, ub);
        u = rhi_->nextResourceUpdateBatch();
        if (!keep(cur)) release(cur);
        if (input && !keep(input)) release(input);
        for (const auto& [binding, tex] : textures)
            if ((binding == 4 && op.kind != ColorOp::Lut && op.kind != ColorOp::Mix) || binding == 5) release(tex);
        cur = next;
    }
    for (QRhiTexture* t : kept)
        if (t && t != cur) release(t);
    return cur;
}

void Device::endFrame() {
    for (QRhiShaderResourceBindings* s : frameSrbs_) delete s;
    frameSrbs_.clear();
    for (Pooled& p : pool_) p.used = false;
    ubufUsed_ = 0;
    // The frame is finished (offscreen frames wait for the GPU), so textures can go now: those not used for a few
    // frames, then the least recently used while the pool holds more than its budget.
    constexpr long kIdleFrames = 8;
    constexpr size_t kBudget = size_t(768) << 20;
    auto drop = [&](std::vector<Pooled>::iterator it) {
        if (auto tg = targets_.find(it->tex); tg != targets_.end()) {
            delete tg->second.rt, delete tg->second.rp;
            targets_.erase(tg);
        }
        delete it->tex;
        return pool_.erase(it);
    };
    for (auto it = pool_.begin(); it != pool_.end();) it = frameNo_ - it->lastFrame > kIdleFrames ? drop(it) : it + 1;
    size_t bytes = 0;
    for (const Pooled& p : pool_) bytes += p.bytes;
    while (bytes > kBudget && !pool_.empty()) {
        auto oldest = std::min_element(pool_.begin(), pool_.end(), [](const Pooled& a, const Pooled& b) { return a.lastFrame < b.lastFrame; });
        bytes -= oldest->bytes;
        drop(oldest);
    }
    ++frameNo_;
    failed_ = false;
}

bool Device::draw(const Frame& frame, Image* out, QImage* out8) {
    using T = QRhiTexture;
    const int W = frame.width, H = frame.height;
    const int maxSize = maxTexture_;
    if (W <= 0 || H <= 0 || W > maxSize || H > maxSize) return false;
    for (const Item& it : frame.items)
        for (const Layer* l : {&it.a, it.b ? &*it.b : nullptr})
            if (l && ((l->frame && l->frame->width * 2 > maxSize) || l->image.width > maxSize || l->image.height > maxSize)) return false;
    if (rhi_->beginOffscreenFrame(&cb_) != QRhi::FrameOpSuccess) return false;
    bool ok = true;
    QRhiResourceUpdateBatch* u = rhi_->nextResourceUpdateBatch();
    QRhiTexture* canvas = nullptr;
    float p[64] = {};
    auto place = [&](float* q, const Layer* l, QRhiTexture* tex) {
        if (!l || !tex) return;  // (kind 0: nothing)
        const Placement& g = l->place;
        q[0] = g.full ? 1 : 2, q[1] = g.scale, q[2] = g.opacity, q[3] = g.axis ? 1 : 0;
        q[4] = g.cx, q[5] = g.cy, q[6] = g.cosr, q[7] = g.sinr;
        q[8] = g.sx, q[9] = g.sy, q[10] = g.au, q[11] = g.av;
        q[12] = g.u0, q[13] = g.u1, q[14] = g.v0, q[15] = g.v1;
        q[16] = g.kx, q[17] = g.ky, q[18] = g.px, q[19] = g.py;
    };
    for (const Item& it : frame.items) {
        QRhiTexture* a = layer(it.a, u);
        QRhiTexture* b = it.b ? layer(*it.b, u) : nullptr;
        if ((!a && (it.a.frame || !it.a.image.empty())) || (it.b && !b && (it.b->frame || !it.b->image.empty()))) {
            ok = false;
            break;
        }
        QRhiTexture* next = texture(T::RGBA32F, W, H, true);
        if (!next) {
            ok = false;
            break;
        }
        std::fill(p, p + 64, 0.0f);
        p[0] = canvas ? 1 : 0;
        p[1] = !canvas && it.blendMode == 0 ? 1 : 0;  // the first normal layer is the canvas (as the CPU moves it in)
        p[2] = float(it.blendMode);
        p[3] = it.b ? it.dissolve : -1;
        place(p + 4, &it.a, a);
        if (it.b) place(p + 24, &*it.b, b);
        QRhiBuffer* ub = uniforms(u, p);
        pass(compositePs_, next, u, {{1, canvas ? canvas : dummy2d_}, {2, a ? a : dummy2d_}, {3, b ? b : dummy2d_}}, ub);
        u = rhi_->nextResourceUpdateBatch();
        for (QRhiTexture* t : {canvas, a, b})
            if (t) release(t);
        canvas = next;
    }
    QRhiReadbackResult result;
    if (ok) {
        QRhiTexture* final = texture(out8 ? T::RGBA8 : T::RGBA32F, W, H, true);
        QRhiTexture* table = nullptr;
        QRhiTexture* lut = final ? lutTexture(frame.display, u, &table) : nullptr;
        if (!final || !lut) {
            ok = false;
        } else {
            std::fill(p, p + 64, 0.0f);
            p[0] = frame.flatten ? 1 : 0, p[1] = frame.display ? 1 : 0, p[2] = out8 ? 1 : 0, p[3] = frame.display ? float(frame.display->size) : 0;
            p[4] = canvas ? 1 : 0;  // (nothing drawn: transparent)
            QRhiBuffer* ub = uniforms(u, p);
            QRhiResourceUpdateBatch* rb = rhi_->nextResourceUpdateBatch();
            rb->readBackTexture({final}, &result);
            pass(out8 ? output8Ps_ : outputPs_, final, u, {{1, canvas ? canvas : dummy2d_}, {3, lut}}, ub, rb);
            u = nullptr;
        }
    }
    if (u) u->release();
    if (rhi_->endOffscreenFrame() != QRhi::FrameOpSuccess || failed_) ok = false;
    endFrame();
    if (!ok) return false;
    const size_t bytes = size_t(W) * H * (out8 ? 4 : 16);
    if (size_t(result.data.size()) < bytes) return false;
    if (out8) {
        *out8 = QImage(W, H, QImage::Format_RGBA8888);
        for (int y = 0; y < H; ++y) std::memcpy(out8->scanLine(y), result.data.constData() + size_t(y) * W * 4, size_t(W) * 4);
    } else {
        *out = Image(W, H, Image::Uninitialized{});
        std::memcpy(out->px.data(), result.data.constData(), bytes);
    }
    return true;
}

// Frames hold it shared while they draw, so shutdown() waits for them.
std::shared_mutex gDeviceMutex;
std::unique_ptr<Device> gDevice;

}  // namespace

bool initialize(std::string* why) {
    std::unique_lock lock(gDeviceMutex);
    if (!gDevice) {
        gDevice = std::make_unique<Device>();
        // Gone before the application is (an OpenGL context outliving it crashes at exit).
        static bool once = [] {
            qAddPostRoutine(shutdown);
            return true;
        }();
        (void)once;
    }
    return gDevice->start(why);
}

bool available(std::string* why) {
    std::shared_lock lock(gDeviceMutex);
    if (gDevice && gDevice->ready()) return true;
    if (why) *why = gDevice && !gDevice->problem().empty() ? gDevice->problem() : "not set up";
    return false;
}

std::string deviceName() {
    std::shared_lock lock(gDeviceMutex);
    return gDevice && gDevice->ready() ? gDevice->name() : std::string();
}

int maxTextureSize() {
    std::shared_lock lock(gDeviceMutex);
    return gDevice && gDevice->ready() ? gDevice->maxTextureSize() : 0;
}

void shutdown() {
    std::unique_lock lock(gDeviceMutex);
    gDevice.reset();
}

bool execute(const Frame& frame, Image* out, QImage* out8) {
    std::shared_lock lock(gDeviceMutex);
    if (!gDevice || !gDevice->ready()) return false;
    Device* d = gDevice.get();
    const bool ok = d->run([&] { return d->draw(frame, out, out8); });
    if (ok) {
        std::lock_guard statsLock(gStatsMutex);
        ++gStats.gpuFrames;
    }
    return ok;
}

}  // namespace montage::gpu

#endif
