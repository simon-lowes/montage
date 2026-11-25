#include "OfxGl.h"

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QSurfaceFormat>
#include <QThread>
#include <atomic>
#include <condition_variable>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#ifdef __linux__
#include <dlfcn.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#endif

#ifndef GL_RGBA32F
#define GL_RGBA32F 0x8814
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_PROJECTION
#define GL_PROJECTION 0x1701
#endif
#ifndef GL_MODELVIEW
#define GL_MODELVIEW 0x1700
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#ifndef GL_PACK_ROW_LENGTH
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_PACK_SKIP_ROWS 0x0D03
#define GL_PACK_SKIP_PIXELS 0x0D04
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#endif
#ifndef GL_PACK_SWAP_BYTES
#define GL_PACK_SWAP_BYTES 0x0D00
#endif
#ifndef GL_UNPACK_SWAP_BYTES
#define GL_UNPACK_SWAP_BYTES 0x0CF0
#endif

namespace montage::ofx::gl {

namespace {

// The OpenGL 1.x calls a V1 overlay's projection needs (not in QOpenGLFunctions, which is the OpenGL ES 2 subset).
using MatrixMode = void (*)(unsigned);
using LoadIdentity = void (*)();
using Ortho = void (*)(double, double, double, double, double, double);
using LoadMatrixd = void (*)(const double*);

struct Worker {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    std::thread thread;
    std::thread::id id;
    QOffscreenSurface* surface = nullptr;  // (made on the GUI thread, used here)
    QOpenGLContext* context = nullptr;
    QOpenGLExtraFunctions* f = nullptr;
    MatrixMode matrixMode = nullptr;
    LoadIdentity loadIdentity = nullptr;
    Ortho ortho = nullptr;
    LoadMatrixd loadMatrix = nullptr;
    std::atomic<bool> ok{false};
    std::string why = "OpenGL for plugins was not started";  // (set before ok, read after it)
    // Textures shared with the GPU compositor's context: the compositor's context while it lives (shareMutex; this one
    // is made sharing with it when it is made later), whether this context copies between framebuffers (flip()), and
    // the two framebuffers it copies with.
    std::mutex shareMutex;
    QOpenGLContext* compositor = nullptr;
    std::atomic<bool> blits{false};
    unsigned copyFbo[2] = {0, 0};
};

// The thread waiting in run() for the job on the OpenGL thread, and what that job asks it to do meanwhile.
struct Caller {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>*> asks;
    bool done = false;
};
thread_local Caller* tCaller = nullptr;  // (on the OpenGL thread, during a job from run())

Worker& worker() {
    static Worker* w = new Worker;  // (never destroyed: its thread may still be waiting at exit)
    return *w;
}

void loop(Worker* w) {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(w->mutex);
            w->wake.wait(lock, [&] { return !w->jobs.empty(); });
            job = std::move(w->jobs.front());
            w->jobs.pop_front();
        }
        try {
            job();
        } catch (...) {  // (the OpenGL thread outlives any one job)
        }
    }
}

// Sets the context up on the OpenGL thread and checks it can draw into a float framebuffer.
void start(Worker* w) {
    if (!QOpenGLContext::supportsThreadedOpenGL()) {
        w->why = "OpenGL here cannot be used from a thread of its own";
        return;
    }
    w->context = new QOpenGLContext;
    w->context->setFormat(w->surface->format());
    bool created = false;
    {
        // (The compositor's context, where it was made first, stays while this one is made from it.)
        std::lock_guard<std::mutex> lock(w->shareMutex);
        if (w->compositor) w->context->setShareContext(w->compositor);
        created = w->context->create();
        if (!created && w->compositor) {  // (a context of its own, as before, where the two cannot share)
            w->context->setShareContext(nullptr);
            created = w->context->create();
        }
    }
    if (!created || !w->context->makeCurrent(w->surface)) {
        w->why = "No OpenGL context could be made for plugins";
        return;
    }
    w->f = w->context->extraFunctions();
    w->blits = w->context->format().majorVersion() >= 3;  // (glBlitFramebuffer)
    auto proc = [w](const char* name) { return w->context->getProcAddress(name); };
    w->matrixMode = reinterpret_cast<MatrixMode>(proc("glMatrixMode"));
    w->loadIdentity = reinterpret_cast<LoadIdentity>(proc("glLoadIdentity"));
    w->ortho = reinterpret_cast<Ortho>(proc("glOrtho"));
    w->loadMatrix = reinterpret_cast<LoadMatrixd>(proc("glLoadMatrixd"));
    Target t;
    if (!makeTarget(t, 4, 4)) {
        w->why = "OpenGL here cannot draw into a float framebuffer";
        return;
    }
    release(t);
    w->why.clear();
    w->ok = true;
}

}  // namespace

bool initialize(std::string* why) {
    Worker& w = worker();
    if (w.surface) {
        if (why) *why = w.why;
        return w.ok;
    }
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance()) || QThread::currentThread() != QCoreApplication::instance()->thread()) {
        if (why) *why = "OpenGL for plugins is set up on the GUI thread of a GUI application";
        return false;
    }
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);  // (plugins may draw with OpenGL 1.x calls)
    format.setAlphaBufferSize(8);
    w.surface = new QOffscreenSurface;
    w.surface->setFormat(format);
    w.surface->create();
    if (!w.surface->isValid()) {
        w.why = "No offscreen surface for OpenGL";
        if (why) *why = w.why;
        return false;
    }
    w.thread = std::thread(loop, &w);
    w.id = w.thread.get_id();
    w.thread.detach();
    std::promise<void> done;
    {
        std::lock_guard<std::mutex> lock(w.mutex);
        w.jobs.push_back([&] {
            start(&w);
            done.set_value();
        });
    }
    w.wake.notify_one();
    done.get_future().wait();
    if (why) *why = w.why;
    return w.ok;
}

bool available(std::string* why) {
    Worker& w = worker();
    const bool ok = w.ok;
    if (why) *why = ok ? std::string() : (w.surface ? w.why : std::string("OpenGL for plugins was not started"));
    return ok;
}

bool onThread() {
    Worker& w = worker();
    return w.ok && std::this_thread::get_id() == w.id;
}

QOpenGLContext* context() {
    Worker& w = worker();
    return w.ok ? w.context : nullptr;
}

void shareWith(QOpenGLContext* compositor) {
    Worker& w = worker();
    std::lock_guard<std::mutex> lock(w.shareMutex);
    w.compositor = compositor;
}

bool sharesWith(QOpenGLContext* other) {
    Worker& w = worker();
    return w.ok && w.blits && other && QOpenGLContext::areSharing(w.context, other);
}

namespace {
thread_local std::atomic<bool> tWaiting{false};
thread_local bool tServing = false;
}

const std::atomic<bool>* waitingFlag() { return &tWaiting; }
bool servingOpenGl() { return tServing; }

bool run(const std::function<void()>& fn, bool first) {
    Worker& w = worker();
    if (!w.ok) return false;
    if (onThread()) {
        fn();
        return true;
    }
    // The caller waits, doing what the job hands back to it (callBack) until it ends. Nothing thrown leaves either
    // side early: the job ends (and says so) whatever it threw, and an exception in what it handed back stays here,
    // so neither thread is left holding the other's stack.
    Caller caller;
    bool threw = false;
    {
        std::lock_guard<std::mutex> lock(w.mutex);
        auto job = [&] {
            tCaller = &caller;
            try {
                fn();
            } catch (...) {
                threw = true;
            }
            resetState();
            tCaller = nullptr;
            std::lock_guard<std::mutex> l(caller.mutex);
            caller.done = true;
            caller.wake.notify_all();
        };
        if (first) w.jobs.push_front(job);
        else w.jobs.push_back(job);
    }
    struct Waiting {
        Waiting() { tWaiting = true; }
        ~Waiting() { tWaiting = false; }
    } waiting;
    w.wake.notify_one();
    std::unique_lock<std::mutex> lock(caller.mutex);
    for (;;) {
        caller.wake.wait(lock, [&] { return caller.done || !caller.asks.empty(); });
        if (caller.asks.empty()) break;
        std::function<void()>* ask = caller.asks.front();
        lock.unlock();
        tWaiting = false;  // (doing the job's work here, not waiting)
        const bool serving = tServing;
        tServing = true;
        try {
            (*ask)();
        } catch (...) {
        }
        tServing = serving;
        tWaiting = true;
        lock.lock();
        caller.asks.pop_front();
        caller.wake.notify_all();
    }
    return !threw;
}

void callBack(const std::function<void()>& fn) {
    Caller* c = onThread() ? tCaller : nullptr;
    if (!c) {
        fn();
        return;
    }
    std::function<void()> ask = fn;
    std::unique_lock<std::mutex> lock(c->mutex);
    c->asks.push_back(&ask);
    c->wake.notify_all();
    c->wake.wait(lock, [&] { return std::find(c->asks.begin(), c->asks.end(), &ask) == c->asks.end(); });
}

namespace {

// Pixels moved between a texture and memory (`pack`: read from the texture) as plain rows, whatever a plug-in in this
// process left set on the context: rows as long as the picture's, nothing skipped, bytes in their order, and no buffer
// bound to read into or write from in memory's place. (Left set, a read wrote past the picture.)
void plainTransfers(Worker& w, bool pack) {
    QOpenGLExtraFunctions* f = w.f;
    const bool es = w.context->isOpenGLES();
    const QSurfaceFormat format = w.context->format();
    if (!es || format.majorVersion() >= 3) {
        f->glPixelStorei(pack ? GL_PACK_ROW_LENGTH : GL_UNPACK_ROW_LENGTH, 0);
        f->glPixelStorei(pack ? GL_PACK_SKIP_ROWS : GL_UNPACK_SKIP_ROWS, 0);
        f->glPixelStorei(pack ? GL_PACK_SKIP_PIXELS : GL_UNPACK_SKIP_PIXELS, 0);
    }
    if (!es) f->glPixelStorei(pack ? GL_PACK_SWAP_BYTES : GL_UNPACK_SWAP_BYTES, 0);
    if (es ? format.majorVersion() >= 3 : format.version() >= qMakePair(2, 1)) f->glBindBuffer(pack ? GL_PIXEL_PACK_BUFFER : GL_PIXEL_UNPACK_BUFFER, 0);
    f->glPixelStorei(pack ? GL_PACK_ALIGNMENT : GL_UNPACK_ALIGNMENT, 4);
}

}  // namespace

unsigned upload(const Image& img) {
    QOpenGLExtraFunctions* f = worker().f;
    if (!f || img.empty()) return 0;
    std::vector<float> rows(img.px.size());
    const size_t row = size_t(img.width) * 4;
    for (int y = 0; y < img.height; ++y) std::memcpy(rows.data() + size_t(y) * row, img.row(img.height - 1 - y), row * sizeof(float));
    GLuint tex = 0;
    f->glGenTextures(1, &tex);
    f->glBindTexture(GL_TEXTURE_2D, tex);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    plainTransfers(worker(), false);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, img.width, img.height, 0, GL_RGBA, GL_FLOAT, rows.data());
    f->glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

unsigned makeTexture(int width, int height, bool half) {
    QOpenGLExtraFunctions* f = worker().f;
    if (!f || width <= 0 || height <= 0) return 0;
    GLuint tex = 0;
    f->glGenTextures(1, &tex);
    f->glBindTexture(GL_TEXTURE_2D, tex);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    plainTransfers(worker(), false);  // (nothing is read from a buffer for it)
    f->glTexImage2D(GL_TEXTURE_2D, 0, half ? GL_RGBA16F : GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

namespace {

// `texture` as framebuffer `which`'s (0: read from, 1: drawn into) only colour attachment (0: none); whether it is
// complete then.
bool attach(Worker& w, int which, unsigned texture) {
    QOpenGLExtraFunctions* f = w.f;
    if (!w.copyFbo[0]) f->glGenFramebuffers(2, w.copyFbo);
    const GLenum target = which ? GL_DRAW_FRAMEBUFFER : GL_READ_FRAMEBUFFER;
    f->glBindFramebuffer(target, w.copyFbo[which]);
    f->glFramebufferTexture2D(target, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    return texture && f->glCheckFramebufferStatus(target) == GL_FRAMEBUFFER_COMPLETE;
}

// The framebuffers bound before a copy, bound again after it (a plug-in may ask for its input's texture with its
// output's framebuffer bound); neither texture stays held by the copy's framebuffers (the compositor's are its own to
// delete).
struct Bound {
    Worker& w;
    GLint read = 0, draw = 0;
    explicit Bound(Worker& worker) : w(worker) {
        w.f->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
        w.f->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    }
    ~Bound() {
        attach(w, 0, 0);
        attach(w, 1, 0);
        w.f->glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(read));
        w.f->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(draw));
    }
};

}  // namespace

namespace {
bool blit(unsigned from, unsigned to, int width, int height, bool reversed) {
    Worker& w = worker();
    QOpenGLExtraFunctions* f = w.f;
    if (!f || !w.blits || !from || !to || width <= 0 || height <= 0) return false;
    while (f->glGetError() != GL_NO_ERROR) {
    }
    bool complete = false;
    {
        Bound bound(w);
        complete = attach(w, 0, from) && attach(w, 1, to);
        if (complete) {
            const bool scissor = f->glIsEnabled(GL_SCISSOR_TEST);
            f->glDisable(GL_SCISSOR_TEST);
            f->glBlitFramebuffer(0, 0, width, height, 0, reversed ? height : 0, width, reversed ? 0 : height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            if (scissor) f->glEnable(GL_SCISSOR_TEST);
        }
    }
    return complete && f->glGetError() == GL_NO_ERROR;
}
}  // namespace

bool flip(unsigned from, unsigned to, int width, int height) { return blit(from, to, width, height, true); }
bool copy(unsigned from, unsigned to, int width, int height) { return blit(from, to, width, height, false); }

void readTexture(unsigned texture, int width, int height, bool topFirst, Image& out) {
    Worker& w = worker();
    QOpenGLExtraFunctions* f = w.f;
    out = Image();
    if (!f || !w.blits || !texture || width <= 0 || height <= 0) return;
    Bound bound(w);
    if (!attach(w, 0, texture)) return;
    out = Image(width, height);
    const size_t row = size_t(width) * 4;
    std::vector<float> rows(topFirst ? 0 : out.px.size());
    plainTransfers(w, true);
    f->glReadPixels(0, 0, width, height, GL_RGBA, GL_FLOAT, topFirst ? out.px.data() : rows.data());
    if (!topFirst)
        for (int y = 0; y < height; ++y) std::memcpy(out.row(height - 1 - y), rows.data() + size_t(y) * row, row * sizeof(float));
}

void writeTexture(unsigned texture, const Image& img) {
    QOpenGLExtraFunctions* f = worker().f;
    if (!f || !texture || img.empty()) return;
    f->glBindTexture(GL_TEXTURE_2D, texture);
    plainTransfers(worker(), false);
    f->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, img.width, img.height, GL_RGBA, GL_FLOAT, img.px.data());
    f->glBindTexture(GL_TEXTURE_2D, 0);
}

void finish() {
    if (QOpenGLExtraFunctions* f = worker().f) f->glFinish();
}

void destroyTexture(unsigned texture) {
    if (QOpenGLExtraFunctions* f = worker().f; f && texture) {
        GLuint t = texture;
        f->glDeleteTextures(1, &t);
    }
}

bool makeTarget(Target& t, int width, int height) {
    QOpenGLExtraFunctions* f = worker().f;
    if (!f || width <= 0 || height <= 0) return false;
    t.width = width, t.height = height;
    GLuint tex = 0, fbo = 0;
    f->glGenTextures(1, &tex);
    f->glBindTexture(GL_TEXTURE_2D, tex);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    plainTransfers(worker(), false);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    f->glGenFramebuffers(1, &fbo);
    f->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    const bool complete = f->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    f->glBindFramebuffer(GL_FRAMEBUFFER, worker().context->defaultFramebufferObject());
    t.texture = tex, t.fbo = fbo;
    if (!complete) release(t);
    return complete;
}

void bind(const Target& t) {
    if (QOpenGLExtraFunctions* f = worker().f) {
        f->glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        f->glViewport(0, 0, t.width, t.height);
    }
}

void clear() {
    if (QOpenGLExtraFunctions* f = worker().f) {
        f->glClearColor(0, 0, 0, 0);
        f->glClear(GL_COLOR_BUFFER_BIT);
    }
}

void read(const Target& t, Image& out) {
    QOpenGLExtraFunctions* f = worker().f;
    out = Image(t.width, t.height);
    if (!f || !t.fbo) return;
    std::vector<float> rows(out.px.size());
    f->glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    plainTransfers(worker(), true);
    f->glReadPixels(0, 0, t.width, t.height, GL_RGBA, GL_FLOAT, rows.data());
    const size_t row = size_t(t.width) * 4;
    for (int y = 0; y < t.height; ++y) std::memcpy(out.row(t.height - 1 - y), rows.data() + size_t(y) * row, row * sizeof(float));
}

QImage readOverlay(const Target& t) {
    Image px;
    read(t, px);
    QImage out(t.width, t.height, QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < t.height; ++y) {
        uchar* d = out.scanLine(y);
        const float* s = px.row(y);
        for (int x = 0; x < t.width * 4; ++x) d[x] = uchar(std::clamp(s[x], 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    return out;
}

void release(Target& t) {
    QOpenGLExtraFunctions* f = worker().f;
    if (f && t.fbo) {
        GLuint fbo = t.fbo;
        f->glDeleteFramebuffers(1, &fbo);
    }
    destroyTexture(t.texture);
    t = Target{};
}

void resetState() {
    Worker& w = worker();
    QOpenGLExtraFunctions* f = w.f;
    if (!f) return;
    f->glBindFramebuffer(GL_FRAMEBUFFER, w.context->defaultFramebufferObject());
    f->glUseProgram(0);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    f->glDisable(GL_BLEND);
    f->glDisable(GL_SCISSOR_TEST);
    f->glDisable(GL_DEPTH_TEST);
    if (w.matrixMode && w.loadIdentity) {
        w.matrixMode(GL_PROJECTION);
        w.loadIdentity();
        w.matrixMode(GL_MODELVIEW);
        w.loadIdentity();
    }
    while (f->glGetError() != GL_NO_ERROR) {
    }
}

void overlayProjection(int width, int height, const double m[6]) {
    Worker& w = worker();
    if (!w.matrixMode || !w.loadIdentity || !w.ortho || !w.loadMatrix) return;
    w.matrixMode(GL_PROJECTION);
    w.loadIdentity();
    w.ortho(0, width, height, 0, -1, 1);
    w.matrixMode(GL_MODELVIEW);
    const double mv[16] = {m[0], m[3], 0, 0, m[1], m[4], 0, 0, 0, 0, 1, 0, m[2], m[5], 0, 1};  // (column-major)
    w.loadMatrix(mv);
}

// ---- Pictures shared between processes -------------------------------------------------------------------------------

bool onePlaneExport(DmaBufExportQuery query, void* display, void* image, int& fourcc, unsigned long long& modifier) {
    if (!query) return false;
    int format = 0, planes = 0;
    if (!query(display, image, &format, &planes, nullptr) || planes != 1 || format == 0) return false;
    // (Room for four all the same: no format has more planes, should the driver's count change between the two calls.)
    unsigned long long modifiers[4] = {modifier, modifier, modifier, modifier};
    int again = 0;
    if (!query(display, image, &format, &again, modifiers) || again != 1 || format == 0) return false;
    fourcc = format;
    modifier = modifiers[0];
    return true;
}

#ifdef __linux__

namespace {

// EGL, as far as sharing a picture needs it, found in the running process: the context is EGL's where Qt's platform made
// it so (Wayland; X11 when asked), and then its library is loaded already. (No EGL header is needed to build this.)
using EglDisplay = void*;
using EglImage = void*;
using EglSync = void*;
constexpr int kEglExtensions = 0x3055, kEglNone = 0x3038, kEglWidth = 0x3057, kEglHeight = 0x3056;
constexpr unsigned kEglGlTexture2d = 0x30B1, kEglLinuxDmaBuf = 0x3270, kEglSyncNativeFence = 0x3144;
constexpr int kEglGlTextureLevel = 0x30BC, kEglFourcc = 0x3271, kEglPlane0Fd = 0x3272, kEglPlane0Offset = 0x3273, kEglPlane0Pitch = 0x3274;
constexpr int kEglPlane0ModifierLo = 0x3443, kEglPlane0ModifierHi = 0x3444;
constexpr unsigned long long kNoModifier = 0x00ffffffffffffffull;  // (DRM's "invalid": the driver's own layout)
constexpr int kFenceWaitMs = 5000;

struct Egl {
    bool probed = false;
    EglDisplay display = nullptr;
    void* (*currentContext)() = nullptr;
    int (*error)() = nullptr;
    EglImage (*createImage)(EglDisplay, void* context, unsigned target, void* buffer, const int* attributes) = nullptr;
    unsigned (*destroyImage)(EglDisplay, EglImage) = nullptr;
    unsigned (*exportQuery)(EglDisplay, EglImage, int* fourcc, int* planes, unsigned long long* modifiers) = nullptr;
    unsigned (*exportImage)(EglDisplay, EglImage, int* fds, int* strides, int* offsets) = nullptr;
    EglSync (*createSync)(EglDisplay, unsigned type, const int* attributes) = nullptr;
    unsigned (*destroySync)(EglDisplay, EglSync) = nullptr;
    int (*dupFence)(EglDisplay, EglSync) = nullptr;
    void (*imageTargetTexture)(unsigned target, void* image) = nullptr;
    bool modifiers = false;  // (an import may name the buffer's layout)
    bool exports = false, imports = false;
    std::string whyNoExport, whyNoImport;
};

std::mutex gEglMutex;
Egl gEgl;

// Looked up once, on the OpenGL thread with the context current.
void probeEgl(Egl& e) {
    e.probed = true;
    Worker& w = worker();
    const std::string notEgl = "OpenGL here is not through EGL";
    e.whyNoExport = e.whyNoImport = notEgl;
    using GetDisplay = EglDisplay (*)();
    using QueryString = const char* (*)(EglDisplay, int);
    using GetProc = void* (*)(const char*);
    const auto getDisplay = reinterpret_cast<GetDisplay>(dlsym(RTLD_DEFAULT, "eglGetCurrentDisplay"));
    const auto queryString = reinterpret_cast<QueryString>(dlsym(RTLD_DEFAULT, "eglQueryString"));
    const auto getProc = reinterpret_cast<GetProc>(dlsym(RTLD_DEFAULT, "eglGetProcAddress"));
    e.currentContext = reinterpret_cast<void* (*)()>(dlsym(RTLD_DEFAULT, "eglGetCurrentContext"));
    e.error = reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "eglGetError"));
    if (!getDisplay || !queryString || !getProc || !e.currentContext || !e.error) return;
    e.display = getDisplay();
    if (!e.display || !e.currentContext()) return;
    const char* list = queryString(e.display, kEglExtensions);
    const std::string extensions = std::string(" ") + (list ? list : "") + " ";
    auto has = [&](const char* name) { return extensions.find(std::string(" ") + name + " ") != std::string::npos; };
    auto proc = [&](const char* name) { return getProc(name); };
    e.createImage = reinterpret_cast<decltype(e.createImage)>(proc("eglCreateImageKHR"));
    e.destroyImage = reinterpret_cast<decltype(e.destroyImage)>(proc("eglDestroyImageKHR"));
    if (!has("EGL_KHR_image_base") || !e.createImage || !e.destroyImage) {
        e.whyNoExport = e.whyNoImport = "EGL here makes no images of buffers (EGL_KHR_image_base)";
        return;
    }
    // Out: a texture's storage as a DMA-BUF, with a fence where the driver gives one.
    e.exportQuery = reinterpret_cast<decltype(e.exportQuery)>(proc("eglExportDMABUFImageQueryMESA"));
    e.exportImage = reinterpret_cast<decltype(e.exportImage)>(proc("eglExportDMABUFImageMESA"));
    if (!has("EGL_KHR_gl_texture_2D_image")) e.whyNoExport = "EGL here makes no images of textures (EGL_KHR_gl_texture_2D_image)";
    else if (!has("EGL_MESA_image_dma_buf_export") || !e.exportQuery || !e.exportImage)
        e.whyNoExport = "the OpenGL driver does not export DMA-BUFs (EGL_MESA_image_dma_buf_export)";
    else e.exports = true, e.whyNoExport.clear();
    if (has("EGL_ANDROID_native_fence_sync") && has("EGL_KHR_fence_sync")) {
        e.createSync = reinterpret_cast<decltype(e.createSync)>(proc("eglCreateSyncKHR"));
        e.destroySync = reinterpret_cast<decltype(e.destroySync)>(proc("eglDestroySyncKHR"));
        e.dupFence = reinterpret_cast<decltype(e.dupFence)>(proc("eglDupNativeFenceFDANDROID"));
        if (!e.createSync || !e.destroySync || !e.dupFence) e.createSync = nullptr;
    }
    // In: a DMA-BUF as a texture.
    e.modifiers = has("EGL_EXT_image_dma_buf_import_modifiers");
    e.imageTargetTexture = reinterpret_cast<decltype(e.imageTargetTexture)>(w.context->getProcAddress("glEGLImageTargetTexture2DOES"));
    if (!has("EGL_EXT_image_dma_buf_import")) e.whyNoImport = "the OpenGL driver does not import DMA-BUFs (EGL_EXT_image_dma_buf_import)";
    else if (!w.context->hasExtension("GL_OES_EGL_image") || !e.imageTargetTexture)
        e.whyNoImport = "OpenGL here makes no textures of EGL's images (GL_OES_EGL_image)";
    else if (!w.blits) e.whyNoImport = "OpenGL here cannot read such a texture back";
    else e.imports = true, e.whyNoImport.clear();
}

// The probe's answers (made on the first call, on the OpenGL thread).
Egl eglNow() {
    {
        std::lock_guard<std::mutex> lock(gEglMutex);
        if (gEgl.probed) return gEgl;
    }
    Egl probe;
    // (Not from a thread the OpenGL thread waits for: asked again later.)
    const bool ran = !(servingOpenGl() && !onThread()) && run([&] { probeEgl(probe); });
    std::lock_guard<std::mutex> lock(gEglMutex);
    if (ran && probe.probed && !gEgl.probed) gEgl = probe;
    if (gEgl.probed) return gEgl;
    Egl none;
    none.whyNoExport = none.whyNoImport = "OpenGL for plugins is not running";
    return none;
}

}  // namespace

bool exportsDmaBuf(std::string* why) {
    const Egl e = eglNow();
    if (why) *why = e.whyNoExport;
    return e.exports;
}

bool importsDmaBuf(std::string* why) {
    const Egl e = eglNow();
    if (why) *why = e.whyNoImport;
    return e.imports;
}

bool exportDmaBuf(unsigned texture, int width, int height, DmaBuf& out, std::string* why) {
    auto no = [&](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };
    const Egl e = eglNow();
    QOpenGLExtraFunctions* f = worker().f;
    if (!e.exports || !f) return no(e.whyNoExport);
    if (!texture || width <= 0 || height <= 0) return no("there is no picture to share");
    const int level[] = {kEglGlTextureLevel, 0, kEglNone};
    const EglImage image = e.createImage(e.display, e.currentContext(), kEglGlTexture2d, reinterpret_cast<void*>(uintptr_t(texture)), level);
    if (!image) return no("EGL made no image of the picture's texture (error " + std::to_string(e.error()) + ")");
    // (The driver writes a descriptor, a stride and an offset for each plane: only one plane is exported, with room for
    // four all the same.)
    int fourcc = 0, fds[4] = {-1, -1, -1, -1}, strides[4] = {}, offsets[4] = {};
    unsigned long long modifier = kNoModifier;
    bool ok = onePlaneExport(e.exportQuery, e.display, image, fourcc, modifier);
    ok = ok && e.exportImage(e.display, image, fds, strides, offsets) && fds[0] >= 0 && strides[0] > 0 && offsets[0] >= 0;
    e.destroyImage(e.display, image);  // (the DMA-BUF keeps the storage)
    for (int i = 1; i < 4; ++i)
        if (fds[i] >= 0) ::close(fds[i]), ok = false;
    const int fd = fds[0], stride = strides[0], offset = offsets[0];
    if (!ok) {
        if (fd >= 0) ::close(fd);
        return no("the OpenGL driver did not export the picture's texture as a DMA-BUF of one plane");
    }
    // A fence for what is still being drawn into it, where the driver gives one; drawn to the end here where not.
    int fence = -1;
    if (e.createSync) {
        const int attributes[] = {kEglNone};
        if (const EglSync sync = e.createSync(e.display, kEglSyncNativeFence, attributes)) {
            f->glFlush();
            fence = e.dupFence(e.display, sync);
            e.destroySync(e.display, sync);
        }
    }
    if (fence < 0) {
        fence = -1;
        f->glFinish();
    }
    out = DmaBuf{};
    out.fd = fd;
    out.fence = fence;
    out.fourcc = unsigned(fourcc);
    out.stride = unsigned(stride);
    out.offset = unsigned(offset);
    out.modifier = modifier;
    out.width = width;
    out.height = height;
    return true;
}

namespace {
// How many bytes a pixel takes in the one-plane formats a picture is shared in: the half floats Montage's hosts export
// (AB4H), and the other four-channel layouts of 16 and of 8 or 10 bits a driver may name the same storage by. 0 for
// anything else, which is not taken.
unsigned bytesPerPixel(unsigned fourcc) {
    auto code = [](const char* c) { return unsigned(c[0]) | unsigned(c[1]) << 8 | unsigned(c[2]) << 16 | unsigned(c[3]) << 24; };
    for (const char* wide : {"AB4H", "XB4H", "AR4H", "XR4H", "AB48", "XB48", "AR48", "XR48"})
        if (fourcc == code(wide)) return 8;
    for (const char* narrow : {"AB24", "XB24", "AR24", "XR24", "RA24", "RX24", "BA24", "BX24", "AB30", "XB30", "AR30", "XR30"})
        if (fourcc == code(narrow)) return 4;
    return 0;
}
}  // namespace

bool dmaBufFits(const DmaBuf& buf, std::string* why) {
    auto no = [&](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };
    if (buf.fd < 0 || buf.width <= 0 || buf.height <= 0 || !buf.fourcc || !buf.stride) return no("the shared picture is not described");
    const unsigned pixel = bytesPerPixel(buf.fourcc);
    if (!pixel) return no("the shared picture is in a format Montage does not take");
    // A row holds its pixels at the format's size, and the rows from the offset on lie inside the buffer, where it
    // says how large it is. (Exact for rows laid one after another. A layout in tiles, which a modifier names, needs
    // no less than that, so this is the least such a buffer must hold: the driver that owns the layout checks the
    // rest when it takes the buffer.)
    if (uint64_t(buf.stride) < uint64_t(buf.width) * pixel || buf.stride > (1u << 30) || buf.offset > (1u << 30))
        return no("the shared picture's rows are not of its size");
    const off_t size = ::lseek(buf.fd, 0, SEEK_END);
    if (size >= 0 && uint64_t(buf.offset) + uint64_t(buf.stride) * uint64_t(buf.height) > uint64_t(size))
        return no("the shared picture is larger than its buffer");
    return true;
}

bool awaitDrawn(const DmaBuf& buf, std::string* why, int waitMs) {
    if (buf.fence < 0) return true;
    pollfd p{buf.fence, POLLIN, 0};
    int r = 0;
    do {
        r = ::poll(&p, 1, waitMs < 0 ? kFenceWaitMs : waitMs);
    } while (r < 0 && errno == EINTR);
    // (a fence signals by becoming readable; a descriptor that is closed, or no fence at all, says something else)
    if (r > 0 && (p.revents & POLLIN)) return true;
    if (why)
        *why = r > 0 ? std::string("the shared picture's fence cannot be waited for")
                     : "the shared picture was not drawn within " + std::to_string(std::max(1, (waitMs < 0 ? kFenceWaitMs : waitMs) / 1000)) + " s";
    return false;
}

unsigned importDmaBuf(const DmaBuf& buf, std::string* why) {
    auto no = [&](const std::string& reason) {
        if (why) *why = reason;
        return 0u;
    };
    const Egl e = eglNow();
    QOpenGLExtraFunctions* f = worker().f;
    if (!e.imports || !f) return no(e.whyNoImport);
    std::string bad;
    if (!dmaBufFits(buf, &bad)) return no(bad);
    // A layout in tiles (any but rows one after another, or the driver's own) is told to the driver with the picture;
    // a driver that cannot be told it would read the tiles as rows.
    if (!e.modifiers && buf.modifier != 0 && buf.modifier != kNoModifier)
        return no("the shared picture is laid out in tiles, which this driver cannot be told");
    if (!awaitDrawn(buf, &bad)) return no(bad);  // (at once when the caller has waited for it already)
    std::vector<int> a = {kEglWidth, buf.width, kEglHeight, buf.height, kEglFourcc, int(buf.fourcc), kEglPlane0Fd, buf.fd,
                          kEglPlane0Offset, int(buf.offset), kEglPlane0Pitch, int(buf.stride)};
    if (e.modifiers && buf.modifier != kNoModifier) {
        a.insert(a.end(), {kEglPlane0ModifierLo, int(unsigned(buf.modifier & 0xffffffffull)), kEglPlane0ModifierHi, int(unsigned(buf.modifier >> 32))});
    }
    a.push_back(kEglNone);
    const EglImage image = e.createImage(e.display, nullptr, kEglLinuxDmaBuf, nullptr, a.data());
    if (!image) return no("EGL did not take the shared picture (error " + std::to_string(e.error()) + ")");
    while (f->glGetError() != GL_NO_ERROR) {
    }
    GLuint tex = 0;
    f->glGenTextures(1, &tex);
    f->glBindTexture(GL_TEXTURE_2D, tex);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    e.imageTargetTexture(GL_TEXTURE_2D, image);
    const bool bound = f->glGetError() == GL_NO_ERROR;
    f->glBindTexture(GL_TEXTURE_2D, 0);
    e.destroyImage(e.display, image);  // (the texture keeps the buffer)
    if (!bound) {
        f->glDeleteTextures(1, &tex);
        return no("OpenGL made no texture of the shared picture");
    }
    return tex;
}

void closeDmaBuf(DmaBuf& buf) {
    if (buf.fd >= 0) ::close(buf.fd);
    if (buf.fence >= 0) ::close(buf.fence);
    buf.fd = buf.fence = -1;
}

namespace {

// The address of the socket under abstract name `name` (no file: it goes with its socket).
socklen_t postAddress(const std::string& name, sockaddr_un& a) {
    a = sockaddr_un{};
    a.sun_family = AF_UNIX;
    const size_t n = std::min(name.size(), sizeof a.sun_path - 2);
    std::memcpy(a.sun_path + 1, name.data(), n);
    return socklen_t(offsetof(sockaddr_un, sun_path) + 1 + n);
}

// One message taken from `post` (false: none within `waitMs`): the descriptors it carried, and who sent it.
bool takeMessage(int post, int waitMs, std::vector<int>& fds, long long& from) {
    fds.clear();
    from = -1;
    pollfd p{post, POLLIN, 0};
    int r = 0;
    do {
        r = ::poll(&p, 1, waitMs);
    } while (r < 0 && errno == EINTR);
    if (r <= 0) return false;
    char byte = 0;
    iovec io{&byte, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(4 * sizeof(int)) + CMSG_SPACE(sizeof(ucred))] = {};
    msghdr m{};
    m.msg_iov = &io;
    m.msg_iovlen = 1;
    m.msg_control = control;
    m.msg_controllen = sizeof control;
    if (::recvmsg(post, &m, MSG_CMSG_CLOEXEC | MSG_DONTWAIT) < 0) return false;
    for (cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
        if (c->cmsg_level != SOL_SOCKET) continue;
        if (c->cmsg_type == SCM_RIGHTS) {
            const size_t n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < n; ++i) {
                int fd = -1;
                std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof fd);
                fds.push_back(fd);
            }
        } else if (c->cmsg_type == SCM_CREDENTIALS) {
            ucred who{};
            std::memcpy(&who, CMSG_DATA(c), sizeof who);
            from = who.pid;
        }
    }
    if (m.msg_flags & MSG_CTRUNC) from = -1;  // (more came than is taken: not whole)
    return true;
}

}  // namespace

int heldAtOnce() {
    // (net.unix.max_dgram_qlen, 10 or 512 by default as the kernel goes: a socket takes one more before senders wait)
    static const int held = [] {
        int n = 512;
        if (FILE* f = std::fopen("/proc/sys/net/unix/max_dgram_qlen", "r")) {
            if (std::fscanf(f, "%d", &n) != 1) n = 512;
            std::fclose(f);
        }
        return std::clamp(n, 1, 1 << 16) + 1;
    }();
    return held;
}

int openDmaBufPost(const std::string& name, std::string* why) {
    auto no = [&](int s) {
        if (why) *why = std::string("no socket for the shared picture's descriptors (") + std::strerror(errno) + ")";
        if (s >= 0) ::close(s);
        return -1;
    };
    const int s = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return no(s);
    sockaddr_un a;
    const socklen_t length = postAddress(name, a);
    const int on = 1;  // (each message says which process sent it)
    if (::bind(s, reinterpret_cast<const sockaddr*>(&a), length) != 0 || ::setsockopt(s, SOL_SOCKET, SO_PASSCRED, &on, sizeof on) != 0) return no(s);
    return s;
}

void closeDmaBufPost(int post) {
    if (post < 0) return;
    // (descriptors sent and never taken go with their messages)
    ::close(post);
}

bool sendDmaBuf(const std::string& post, const DmaBuf& buf) {
    if (buf.fd < 0) return false;
    const int s = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return false;
    sockaddr_un a;
    const socklen_t length = postAddress(post, a);
    const int fds[2] = {buf.fd, buf.fence};
    const size_t n = buf.fence >= 0 ? 2 : 1;
    char byte = 1;
    iovec io{&byte, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(2 * sizeof(int))] = {};
    msghdr m{};
    m.msg_name = &a;
    m.msg_namelen = length;
    m.msg_iov = &io;
    m.msg_iovlen = 1;
    m.msg_control = control;
    m.msg_controllen = CMSG_SPACE(n * sizeof(int));
    cmsghdr* c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(n * sizeof(int));
    std::memcpy(CMSG_DATA(c), fds, n * sizeof(int));
    // (a post filled by another process is not waited on: the picture goes through shared memory instead)
    const bool sent = ::sendmsg(s, &m, MSG_NOSIGNAL | MSG_DONTWAIT) == 1;
    ::close(s);
    return sent;
}

bool receiveDmaBuf(int post, long long pid, bool fence, int waitMs, DmaBuf& buf, std::string* why) {
    buf.fd = buf.fence = -1;
    std::vector<int> fds;
    long long from = -1;
    bool whole = false;
    // The newest message is the one waited for (it was sent before the reply that said so); older ones were never
    // taken, and go. (As many as the socket holds at once are looked through, and one more, so the host's is among
    // them whatever another process sent first, and no more, whoever keeps sending.)
    const int most = heldAtOnce() + 1;
    int taken = 0;
    for (int wait = waitMs; taken < most && takeMessage(post, wait, fds, from); wait = 0, ++taken) {
        if (from == pid && fds.size() == (fence ? 2u : 1u)) {
            closeDmaBuf(buf);
            whole = true;
            buf.fd = fds[0];
            buf.fence = fence ? fds[1] : -1;
        } else {
            for (int fd : fds) ::close(fd);
        }
    }
    if (!whole && why) *why = "the shared picture's descriptors did not come whole";
    return whole;
}

#else  // (elsewhere a picture is shared through memory: IOSurface and DXGI handles are not written)

namespace {
const char* const kLinuxOnly = "pictures are shared from the GPU's memory on Linux only";
}

bool exportsDmaBuf(std::string* why) {
    if (why) *why = kLinuxOnly;
    return false;
}
bool importsDmaBuf(std::string* why) {
    if (why) *why = kLinuxOnly;
    return false;
}
bool exportDmaBuf(unsigned, int, int, DmaBuf&, std::string* why) {
    if (why) *why = kLinuxOnly;
    return false;
}
unsigned importDmaBuf(const DmaBuf&, std::string* why) {
    if (why) *why = kLinuxOnly;
    return 0;
}
void closeDmaBuf(DmaBuf& buf) { buf.fd = buf.fence = -1; }
int openDmaBufPost(const std::string&, std::string* why) {
    if (why) *why = kLinuxOnly;
    return -1;
}
void closeDmaBufPost(int) {}
bool sendDmaBuf(const std::string&, const DmaBuf&) { return false; }
bool receiveDmaBuf(int, long long, bool, int, DmaBuf& buf, std::string* why) {
    buf.fd = buf.fence = -1;
    if (why) *why = kLinuxOnly;
    return false;
}
int heldAtOnce() { return 0; }
bool dmaBufFits(const DmaBuf&, std::string* why) {
    if (why) *why = kLinuxOnly;
    return false;
}
bool awaitDrawn(const DmaBuf&, std::string* why, int) {
    if (why) *why = kLinuxOnly;
    return false;
}

#endif

}  // namespace montage::ofx::gl
