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
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#ifndef GL_RGBA32F
#define GL_RGBA32F 0x8814
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
    if (!w->context->create() || !w->context->makeCurrent(w->surface)) {
        w->why = "No OpenGL context could be made for plugins";
        return;
    }
    w->f = w->context->extraFunctions();
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
    f->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, img.width, img.height, 0, GL_RGBA, GL_FLOAT, rows.data());
    f->glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
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
    f->glPixelStorei(GL_PACK_ALIGNMENT, 4);
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

}  // namespace montage::ofx::gl
