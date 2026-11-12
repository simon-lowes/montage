// montage-pluginhost --control <name> --parent <pid> [--memory-limit-mb N] [--gpu] [-platform <name>]
//
// Runs plug-ins for Montage in a process of their own (audio/PluginHost.h), so one that crashes, hangs or runs away
// with memory ends only this process. Montage starts it with the name of a control channel (shared memory); each
// plug-in instance then gets a channel of its own, served by a thread here. Editors and their windows are looked
// after on the main thread, which runs Qt's event loop. It ends when Montage does.
#include <QGuiApplication>
#include <QImage>
#include <QMetaObject>
#include <QPainter>
#include <QTimer>
#include <QWindow>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/PluginHost.h"
#include "audio/PluginIpc.h"
#include "audio/PluginWire.h"
#include "audio/Plugins.h"
#include "core/ProjectIO.h"
#include "render/ColorSpace.h"
#include "render/Ofx.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#endif

using namespace montage;
using namespace montage::plugins;

namespace {

int64_t gMemoryLimit = 0;  // bytes, 0: none

void checkMemory() {
    if (gMemoryLimit > 0 && ipc::ownResidentBytes() > gMemoryLimit) std::_Exit(wire::kExitTooMuchMemory);
}

// The channels Montage waits on, for a crash to be told at once: a crashed process can take seconds to end (a core
// dump being written, a crash reporter), and Montage would count those as late blocks. Written from a signal handler,
// so fixed slots of atomics.
struct CrashSlot {
    std::atomic<wire::Header*> h{nullptr};
    std::atomic<ipc::Bell*> bell{nullptr};
};
CrashSlot gCrashSlots[512];

void watchForCrash(wire::Header* h, ipc::Bell* bell) {
    for (CrashSlot& s : gCrashSlots) {
        wire::Header* none = nullptr;
        if (s.h.compare_exchange_strong(none, h)) {
            s.bell.store(bell);
            return;
        }
    }
}

void unwatchForCrash(wire::Header* h) {
    for (CrashSlot& s : gCrashSlots)
        if (s.h.load() == h) {
            s.bell.store(nullptr);
            s.h.store(nullptr);
        }
}

void forgetLoadingUnlessHere();

// (async-signal-safe: atomics, and a futex wake, sem_post or SetEvent)
void tellMontageOfCrash(uint32_t code) {
    forgetLoadingUnlessHere();
    for (CrashSlot& s : gCrashSlots) {
        wire::Header* h = s.h.load();
        if (!h) continue;
        h->crashed.store(code ? code : 1, std::memory_order_release);
        h->toClient.fetch_add(1, std::memory_order_acq_rel);
        if (ipc::Bell* b = s.bell.load()) b->ring(&h->toClient);
    }
}

#ifdef _WIN32
LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* e) {
    tellMontageOfCrash(e && e->ExceptionRecord ? uint32_t(e->ExceptionRecord->ExceptionCode) : 1);
    return EXCEPTION_EXECUTE_HANDLER;  // (ends the process; no error box, see SetErrorMode)
}
void onAbort(int) {
    tellMontageOfCrash(3);  // (abort's exit code)
    std::_Exit(3);
}
#else
void onFatalSignal(int sig) {
    tellMontageOfCrash(uint32_t(sig));
    std::signal(sig, SIG_DFL);
    raise(sig);  // (ends it as the signal would have)
}

// A thread's own stack for the handler, so a stack overflow reaches it too.
struct AltStack {
    std::unique_ptr<char[]> mem;
    AltStack() {
        const size_t size = 64 * 1024;
        mem.reset(new char[size]);
        stack_t st{};
        st.ss_sp = mem.get();
        st.ss_size = size;
        if (sigaltstack(&st, nullptr) != 0) mem.reset();
    }
    ~AltStack() {
        if (!mem) return;
        stack_t st{};
        st.ss_flags = SS_DISABLE;
        sigaltstack(&st, nullptr);
    }
};
#endif

void catchCrashes() {
#ifdef _WIN32
    SetUnhandledExceptionFilter(onUnhandledException);
    std::signal(SIGABRT, onAbort);
#else
    // No core dumps: the plug-in's crash is reported by Montage, and a dump only keeps the process from ending.
    rlimit none{0, 0};
    setrlimit(RLIMIT_CORE, &none);
#if defined(__linux__)
    prctl(PR_SET_DUMPABLE, 0);  // (a core_pattern piped to a collector does not heed RLIMIT_CORE)
#endif
    struct sigaction sa {};
    sa.sa_handler = onFatalSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_ONSTACK | SA_NODEFER;
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS}) sigaction(sig, &sa, nullptr);
#endif
}

// The control channel's header, where the plug-in being loaded is noted (Montage reads it if the host ends then), and
// the thread loading it.
std::atomic<wire::Header*> gControl{nullptr};
std::atomic<int> gLoader{0};
std::atomic<int> gThreads{0};
thread_local int tThread = 0;
thread_local bool tServing = false;  // (one of the host's own threads serving a channel)
int thisThread() {
    if (!tThread) tThread = ++gThreads;
    return tThread;
}

// A crash on another of the host's channel threads than the one loading (a render of a plug-in already loaded) is
// not the loading one's: its name is taken back. A crash on a thread of the plug-ins' own (or the main thread, which
// a load may hand work to) leaves it. (In the crash handler, on the crashing thread: atomics and its thread-locals.)
void forgetLoadingUnlessHere() {
    wire::Header* h = gControl.load(std::memory_order_acquire);
    if (h && tServing && (tThread == 0 || gLoader.load(std::memory_order_acquire) != tThread)) h->loading[0] = '\0';
}

void noteLoading(const std::string& id, bool loading) {
    wire::Header* h = gControl.load(std::memory_order_acquire);
    if (!h) return;
    if (!loading) {
        int me = thisThread();
        // (only the thread that noted it takes it back: another may have begun loading meanwhile)
        if (gLoader.compare_exchange_strong(me, 0, std::memory_order_acq_rel)) h->loading[0] = '\0';
        return;
    }
    const size_t n = std::min(id.size(), sizeof h->loading - 1);
    if (n == 0) return;
    gLoader.store(thisThread(), std::memory_order_release);
    // (the first byte last: Montage, reading after the host ended, never sees half a name)
    h->loading[0] = '\0';
    std::memcpy(h->loading + 1, id.data() + 1, n - 1);
    h->loading[n] = '\0';
    std::atomic_thread_fence(std::memory_order_release);
    h->loading[0] = id[0];
}

// The host's end of a channel.
struct Channel {
    std::unique_ptr<ipc::SharedMemory> mem;
    std::unique_ptr<ipc::Bell> toHost, toClient;
    wire::Header* h = nullptr;
    uint32_t seen = 0, bumps = 0;
    bool open(const std::string& name, std::string* error) {
        mem = ipc::SharedMemory::open(name, wire::kChannelBytes, error);
        if (!mem) return false;
        toHost = ipc::Bell::open(name + "h", error);
        toClient = ipc::Bell::open(name + "c", error);
        if (!toHost || !toClient) return false;
        h = wire::header(mem->data());
        if (h->magic != wire::kMagic || h->version != wire::kVersion) {
            if (error) *error = "the channel is from another version of Montage";
            return false;
        }
        // (a channel is new when opened here: a message Montage sent before then is still to be read)
        seen = 0;
        bumps = h->toClient.load(std::memory_order_acquire);
        watchForCrash(h, toClient.get());
        return true;
    }
    Channel() = default;
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    ~Channel() {
        if (h) unwatchForCrash(h);
    }
    void* base() const { return mem->data(); }
    char* payload() const { return wire::payload(mem->data()); }
    // The next message.
    void next() {
        for (;;) {
            const uint32_t now = h->toHost.load(std::memory_order_acquire);
            if (now != seen) {
                seen = now;
                return;
            }
            toHost->wait(&h->toHost, now, 200'000);
        }
    }
    void reply(uint32_t kind = wire::Final) {
        h->replyKind = kind;
        h->replySeq = h->seq;
        h->toClient.store(++bumps, std::memory_order_release);
        toClient->ring(&h->toClient);
    }
    void replyText(bool ok, const std::string& text) {
        h->status = ok ? 1 : 0;
        const size_t n = std::min(text.size(), wire::kPayload);
        std::memcpy(payload(), text.data(), n);
        h->payloadBytes = n;
        reply();
    }
};

// A block for one channel's audio, ending at a page the plug-in cannot touch: a plug-in that writes well past its
// buffer crashes here at once instead of quietly spoiling memory.
class GuardedBuffer {
public:
    GuardedBuffer() {
#ifdef _WIN32
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        page_ = size_t(si.dwPageSize);
#else
        page_ = size_t(sysconf(_SC_PAGESIZE));
#endif
        body_ = wire::alignUp(size_t(wire::kMaxFrames) * sizeof(float), page_);
#ifdef _WIN32
        base_ = static_cast<char*>(VirtualAlloc(nullptr, body_ + page_, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        DWORD old = 0;
        if (base_) VirtualProtect(base_ + body_, page_, PAGE_NOACCESS, &old);
#else
        void* p = mmap(nullptr, body_ + page_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        base_ = p == MAP_FAILED ? nullptr : static_cast<char*>(p);
        if (base_) mprotect(base_ + body_, page_, PROT_NONE);
#endif
    }
    ~GuardedBuffer() {
#ifdef _WIN32
        if (base_) VirtualFree(base_, 0, MEM_RELEASE);
#else
        if (base_) munmap(base_, body_ + page_);
#endif
    }
    GuardedBuffer(const GuardedBuffer&) = delete;
    GuardedBuffer& operator=(const GuardedBuffer&) = delete;
    // Room for `frames` samples, ending (64-byte aligned) just before the guard page.
    float* forFrames(int frames) const {
        return reinterpret_cast<float*>(base_ + body_ - wire::alignUp(size_t(frames) * sizeof(float), 64));
    }

private:
    char* base_ = nullptr;
    size_t page_ = 4096, body_ = 0;
};

// What an editor says, kept for Montage's next look.
struct Events : EditorListener {
    std::mutex m;
    std::vector<char> bytes;
    QWindow* window = nullptr;  // a window of the host's own (main thread)
    void push(uint8_t kind, uint32_t id, double value, int32_t w, int32_t h) {
        std::lock_guard lock(m);
        if (bytes.size() + wire::kEventBytes > wire::kPayload) return;
        char e[wire::kEventBytes];
        e[0] = char(kind);
        std::memcpy(e + 1, &id, 4);
        std::memcpy(e + 5, &value, 8);
        std::memcpy(e + 13, &w, 4);
        std::memcpy(e + 17, &h, 4);
        bytes.insert(bytes.end(), e, e + wire::kEventBytes);
    }
    void editorParameter(uint32_t id, double value) override { push(wire::EvParameter, id, value, 0, 0); }
    void editorGesture(uint32_t id, bool begin) override { push(begin ? wire::EvGestureBegin : wire::EvGestureEnd, id, 0, 0, 0); }
    void editorResize(int width, int height) override {
        push(wire::EvResize, 0, 0, width, height);
        if (window) window->resize(std::max(1, width), std::max(1, height));
    }
    void editorClosed() override { push(wire::EvClosed, 0, 0, 0, 0); }
    // Into the reply's payload.
    void drain(Channel& c) {
        std::lock_guard lock(m);
        const size_t n = std::min(bytes.size(), wire::kPayload);
        std::memcpy(c.payload(), bytes.data(), n);
        c.h->payloadBytes = n;
        bytes.clear();
    }
};

struct AudioInstance;

// A window of the host's own for an editor (macOS, where Montage's view cannot be its parent). Closing it closes the
// editor and tells Montage.
class EditorWindow : public QWindow {
public:
    explicit EditorWindow(AudioInstance* owner) : owner_(owner) {}
    bool event(QEvent* e) override;

private:
    AudioInstance* owner_;
};

struct AudioInstance {
    Descriptor d;
    std::unique_ptr<Instance> inst;
    std::mutex m;  // (the main thread idles the editor while this instance's thread runs it)
    Channel chan;
    std::vector<std::unique_ptr<GuardedBuffer>> main, key;
    std::string blob;  // a state coming in parts, or going out in parts
    uint64_t inBytes = 0;  // the payload that came with the message being served
    Events events;
    bool editorOpen = false;
    int keyEnabled = -1;
    uint32_t calls = 0;
};

bool EditorWindow::event(QEvent* e) {
    if (e->type() == QEvent::Close && owner_) {
        std::lock_guard lock(owner_->m);
        if (owner_->editorOpen) {
            owner_->inst->closeEditor();
            owner_->editorOpen = false;
            owner_->events.window = nullptr;
            owner_->events.editorClosed();
        }
        owner_ = nullptr;
        deleteLater();
    }
    return QWindow::event(e);
}

std::mutex gInstancesM;
std::vector<std::shared_ptr<AudioInstance>> gInstances;

template <typename F>
void onMainThread(F&& f) {
    QMetaObject::invokeMethod(qApp, std::forward<F>(f), Qt::BlockingQueuedConnection);
}

void applyParams(AudioInstance& a) {
    wire::Header* h = a.chan.h;
    const uint32_t n = std::min<uint32_t>(h->paramCount, wire::kMaxParamChanges);
    for (uint32_t i = 0; i < n; ++i) a.inst->setParameter(h->params[i].id, h->params[i].value);
    h->paramCount = 0;
}

std::string payloadOrBlob(AudioInstance& a) {
    wire::Header* h = a.chan.h;
    if (h->flags & wire::kPayloadInBlob) return a.blob;
    return std::string(a.chan.payload(), size_t(a.inBytes));
}

void process(AudioInstance& a) {
    wire::Header* h = a.chan.h;
    Instance& inst = *a.inst;
    if (h->keyEnable >= 0 && h->keyEnable != a.keyEnabled) {
        a.keyEnabled = h->keyEnable;
        inst.enableSidechain(h->keyEnable != 0);
    }
    if (h->flags & wire::kTransport) inst.setTransport(h->position, (h->flags & wire::kPlaying) != 0, h->bpm, h->beatsPerBar);
    const int frames = std::clamp(h->frames, 0, wire::kMaxFrames);
    const int nch = std::clamp(h->channels, 0, wire::kMaxChannels);
    const int kch = std::clamp(h->keyChannels, 0, wire::kMaxChannels);
    while (int(a.main.size()) < nch) a.main.push_back(std::make_unique<GuardedBuffer>());
    while (int(a.key.size()) < kch) a.key.push_back(std::make_unique<GuardedBuffer>());
    float* ch[wire::kMaxChannels];
    const float* k[wire::kMaxChannels];
    for (int c = 0; c < nch; ++c) {
        ch[c] = a.main[size_t(c)]->forFrames(frames);
        std::memcpy(ch[c], wire::audio(a.chan.base(), c), size_t(frames) * sizeof(float));
    }
    for (int c = 0; c < kch; ++c) {
        float* kp = a.key[size_t(c)]->forFrames(frames);
        std::memcpy(kp, wire::audio(a.chan.base(), wire::kMaxChannels + c), size_t(frames) * sizeof(float));
        k[c] = kp;
    }
    const bool keyed = inst.hasSidechain();
    if (keyed) inst.setSidechain(kch > 0 ? k : nullptr, kch);
    inst.process(ch, nch, frames);
    if (keyed) inst.setSidechain(nullptr, 0);
    for (int c = 0; c < nch; ++c) std::memcpy(wire::audio(a.chan.base(), c), ch[c], size_t(frames) * sizeof(float));
    if (++a.calls % 8 == 0) checkMemory();
}

void serveAudio(std::shared_ptr<AudioInstance> a) {
    tServing = true;
#ifndef _WIN32
    AltStack alt;
#endif
    Channel& c = a->chan;
    wire::Header* h = c.h;
    for (;;) {
        c.next();
        const uint32_t cmd = h->command;
        bool done = false;
        {
            std::lock_guard lock(a->m);
            applyParams(*a);
            a->inBytes = std::min<uint64_t>(h->payloadBytes, wire::kPayload);  // (what came with the message)
            h->status = 1;
            h->payloadBytes = 0;
            switch (cmd) {
                case wire::Activate: h->status = a->inst->activate(h->argD, int(h->argI)) ? 1 : 0; break;
                case wire::Process: process(*a); break;
                case wire::Parameters: {
                    std::string out;
                    const auto params = a->inst->parameters();
                    const uint32_t n = uint32_t(params.size());
                    out.append(reinterpret_cast<const char*>(&n), 4);
                    for (const ParamInfo& p : params) {
                        const uint8_t flags = (p.automatable ? 1 : 0) | (p.stepped ? 2 : 0);
                        const uint16_t len = uint16_t(std::min<size_t>(p.name.size(), 65535));
                        out.append(reinterpret_cast<const char*>(&p.id), 4);
                        out.append(reinterpret_cast<const char*>(&p.min), 8);
                        out.append(reinterpret_cast<const char*>(&p.max), 8);
                        out.append(reinterpret_cast<const char*>(&p.def), 8);
                        out.append(reinterpret_cast<const char*>(&flags), 1);
                        out.append(reinterpret_cast<const char*>(&len), 2);
                        out.append(p.name.data(), len);
                    }
                    const size_t bytes = std::min(out.size(), wire::kPayload);
                    std::memcpy(c.payload(), out.data(), bytes);
                    h->payloadBytes = bytes;
                    break;
                }
                case wire::GetParameter: h->resultD = a->inst->parameter(uint32_t(h->argU)); break;
                case wire::Flush: break;
                case wire::SaveState: {
                    a->blob = a->inst->saveState();
                    h->resultI = int64_t(a->blob.size());
                    const size_t n = std::min(a->blob.size(), wire::kPayload);
                    std::memcpy(c.payload(), a->blob.data(), n);
                    h->payloadBytes = n;
                    break;
                }
                case wire::FetchBlob: {
                    const size_t off = size_t(std::max<int64_t>(0, h->argI));
                    const size_t n = off < a->blob.size() ? std::min(a->blob.size() - off, wire::kPayload) : 0;
                    std::memcpy(c.payload(), a->blob.data() + off, n);
                    h->payloadBytes = n;
                    break;
                }
                case wire::PutBlob: {
                    if (h->argI == 0) a->blob.clear();
                    a->blob.resize(size_t(h->argI));
                    a->blob.append(c.payload(), size_t(a->inBytes));
                    break;
                }
                case wire::LoadState: h->status = a->inst->loadState(payloadOrBlob(*a)) ? 1 : 0; break;
                case wire::Reset: a->inst->reset(); break;
                case wire::SetOffline: a->inst->setOffline(h->argI != 0); break;
                case wire::SetMainChannels: h->status = a->inst->setMainChannels(int(h->argI)) ? 1 : 0; break;
                case wire::Destroy: done = true; break;
                default: break;
            }
        }
        // The editor, on the main thread.
        if (cmd == wire::OpenEditor) {
            const uint64_t parentId = h->argU;
            int w = 400, hh = 300;
            bool ok = false;
            bool own = false;
            onMainThread([&] {
                std::lock_guard lock(a->m);
                void* parent = reinterpret_cast<void*>(uintptr_t(parentId));
                EditorWindow* win = nullptr;
                if (!parentId) {
                    win = new EditorWindow(a.get());
                    win->setTitle(QString::fromStdString(a->d.name));
                    win->create();
                    parent = reinterpret_cast<void*>(win->winId());
                    a->events.window = win;
                }
                ok = a->inst->openEditor(parent, &a->events, w, hh);
                a->editorOpen = ok;
                if (win) {
                    if (ok) {
                        win->resize(std::max(1, w), std::max(1, hh));
                        win->show();
                        own = true;
                    } else {
                        a->events.window = nullptr;
                        delete win;
                    }
                }
            });
            h->status = ok ? 1 : 0;
            h->resultI = w;
            h->resultI2 = hh;
            h->resultFlags = (a->inst->editorResizable() ? wire::kEditorResizable : 0) | (own ? wire::kOwnWindow : 0);
            a->events.drain(c);
        } else if (cmd == wire::EditorIdle) {
            onMainThread([&] {
                std::lock_guard lock(a->m);
                if (a->editorOpen) a->inst->idle();
            });
            a->events.drain(c);
        } else if (cmd == wire::CloseEditor || (done && a->editorOpen)) {
            onMainThread([&] {
                std::lock_guard lock(a->m);
                if (a->editorOpen) a->inst->closeEditor();
                a->editorOpen = false;
                if (QWindow* w = std::exchange(a->events.window, nullptr)) w->deleteLater();
            });
        } else if (cmd == wire::SetEditorSize) {
            const int w = int(h->argI), hh = int(h->argI2);
            onMainThread([&] {
                std::lock_guard lock(a->m);
                if (a->editorOpen) a->inst->setEditorSize(w, hh);
                if (a->events.window) a->events.window->resize(std::max(1, w), std::max(1, hh));
            });
        }
        if (done) {
            {
                std::lock_guard lock(gInstancesM);
                gInstances.erase(std::remove(gInstances.begin(), gInstances.end(), a), gInstances.end());
            }
            std::lock_guard lock(a->m);
            a->inst.reset();
        }
        {
            std::lock_guard lock(a->m);
            h->latency = a->inst ? a->inst->latencySamples() : 0;
        }
        c.reply();
        if (done) return;
    }
}

// ---- OpenFX ----

struct OfxChannel {
    Channel chan;
    std::unique_ptr<ipc::SharedMemory> arena;
    uint32_t arenaGen = 0;
    std::string blob;
    std::string out;  // a reply longer than the payload, fetched in parts
    std::unique_ptr<ofx::Interact> controls;  // (an overlay's channel) its on-screen controls
    std::mutex questions;  // (one question to Montage at a time, whichever render thread asks)
    bool mapArena() {
        wire::Header* h = chan.h;
        if (arena && arenaGen == h->arenaGen) return true;
        arena.reset();
        std::string err;
        const std::string name(h->arenaName, strnlen(h->arenaName, sizeof h->arenaName));
        arena = ipc::SharedMemory::open(name, size_t(h->arenaSize), &err);
        if (!arena) return false;
        arenaGen = h->arenaGen;
        return true;
    }
    float* at(size_t offsetBytes) const { return reinterpret_cast<float*>(static_cast<char*>(arena->data()) + offsetBytes); }
};

// Asks Montage for a picture (a frame at another time, or what an input shows) in the middle of a render.
bool ask(OfxChannel& o, int32_t kind, double t, const std::string& connection, Image& out) {
    std::lock_guard lock(o.questions);
    wire::Header* h = o.chan.h;
    h->resultI = kind;
    h->resultD = t;
    const size_t n = std::min(connection.size(), wire::kPayload);
    std::memcpy(o.chan.payload(), connection.data(), n);
    h->payloadBytes = n;
    o.chan.reply(wire::Callback);
    o.chan.next();
    if (h->command != wire::CallbackReply || !h->status || !o.mapArena()) return false;
    const int w = int(h->argI2), hh = int(h->argI3);
    const size_t off = size_t(h->argI);
    if (w <= 0 || hh <= 0 || off + size_t(w) * size_t(hh) * 16 > o.arena->size()) return false;
    out = Image(w, hh, Image::Uninitialized{});
    std::memcpy(out.px.data(), o.at(off), size_t(w) * size_t(hh) * 16);
    return true;
}

// A V1 (OpenGL) overlay drawn in a host started without --gpu: its OpenGL is started then, on the main thread, for
// drawing only (renders stay on the CPU, as asked).
void ensureOpenGl() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!ofx::gpuAvailable()) QMetaObject::invokeMethod(qApp, [] { ofx::initializeGpu(); }, Qt::BlockingQueuedConnection);
    });
}

// The on-screen controls' messages (wire::OfxOverlayOpen...): the view and effect, then the control's answer. A draw
// leaves the overlay in the arena as one premultiplied picture at the device's pixels.
void serveOverlay(OfxChannel& o, uint32_t cmd) {
    Channel& c = o.chan;
    wire::Header* h = c.h;
    const std::string msg = (h->flags & wire::kPayloadInBlob) ? o.blob : std::string(c.payload(), size_t(h->payloadBytes));
    o.mapArena();  // (at every message: Montage lets go of a new arena's name once this has answered)
    h->resultFlags = 0;
    h->resultI = h->resultI2 = 0;
    h->resultD = -1;  // (a draw: how long the control took, in seconds)
    wire::OverlayArgs a{};
    const size_t at = sizeof a;
    const size_t s1 = msg.size() > at ? msg.find('\0', at) : std::string::npos;
    const size_t s2 = s1 == std::string::npos ? s1 : msg.find('\0', s1 + 1);
    Effect e;
    if (s2 == std::string::npos || !effectFromJsonString(msg.substr(at, s1 - at), e)) {
        c.replyText(false, "the on-screen controls' message did not come through");
        return;
    }
    std::memcpy(&a, msg.data(), sizeof a);
    for (const ofx::PluginDesc& d : ofx::descriptionsFromJson(msg.substr(s1 + 1, s2 - s1 - 1))) {
        ofx::Registry::instance().adopt(d);
        if (cmd == wire::OfxOverlayOpen && d.overlay == 1) ensureOpenGl();
    }
    const std::string text = msg.substr(s2 + 1);
    ofx::View v;
    v.width = std::clamp(a.width, 0, 16384);
    v.height = std::clamp(a.height, 0, 16384);
    for (int k = 0; k < 6; ++k) v.m[k] = std::isfinite(a.m[k]) ? a.m[k] : 0;
    v.renderScale = std::isfinite(a.renderScale) && a.renderScale > 0 ? a.renderScale : 1;
    v.frameW = std::isfinite(a.frameW) ? a.frameW : 0;
    v.frameH = std::isfinite(a.frameH) ? a.frameH : 0;
    v.pixelRatio = std::isfinite(a.pixelRatio) ? std::clamp(a.pixelRatio, 0.25, 8.0) : 1;
    const double t = std::isfinite(a.time) ? a.time : 0;
    if (cmd == wire::OfxOverlayOpen) {
        std::string error;
        o.controls = ofx::Interact::open(e, &error);
        if (!o.controls && error.empty()) error = "the plug-in has no on-screen controls";
        c.replyText(o.controls != nullptr, error);
        return;
    }
    if (!o.controls || o.controls->failed()) {
        h->resultFlags = wire::kControlFailed;
        c.replyText(false, "the on-screen controls are not open");
        return;
    }
    bool ok = false;
    std::string error;
    Effect edited;
    bool changed = false;
    if (cmd == wire::OfxOverlayDraw) {
        ofx::Overlay drawn;
        const int64_t began = ipc::nowUs();
        ok = o.controls->draw(e, t, v, drawn);
        h->resultD = double(ipc::nowUs() - began) / 1e6;
        if (ok && drawn.ops.empty() && drawn.gl.isNull()) {
            // Nothing drawn (an OpenGL control where the host has no OpenGL draws nothing, as in Montage): no picture.
        } else if (ok) {
            const int pw = std::clamp(int(std::lround(v.width * v.pixelRatio)), 1, 16384);
            const int ph = std::clamp(int(std::lround(v.height * v.pixelRatio)), 1, 16384);
            const size_t bytes = size_t(pw) * size_t(ph) * 4;
            if (!o.mapArena() || bytes > o.arena->size()) {
                ok = false;
                error = "the overlay is larger than Montage made room for";
            } else {
                // Painted as Montage paints a control in its own process, into the shared block itself.
                QImage img(static_cast<uchar*>(o.arena->data()), pw, ph, pw * 4, QImage::Format_RGBA8888_Premultiplied);
                img.fill(Qt::transparent);
                img.setDevicePixelRatio(v.pixelRatio);
                {
                    QPainter p(&img);
                    ofx::paintOverlay(p, drawn, v);
                }
                h->resultI = pw;
                h->resultI2 = ph;
            }
        }
    } else if (cmd == wire::OfxOverlayPen) {
        const auto kind = a.pen == 0 ? ofx::Interact::Pen::Down : a.pen == 2 ? ofx::Interact::Pen::Up : ofx::Interact::Pen::Move;
        const double px = std::isfinite(a.x) ? a.x : 0, py = std::isfinite(a.y) ? a.y : 0;
        ok = o.controls->pen(kind, e, t, v, px, py, std::isfinite(a.pressure) ? a.pressure : 1, edited, changed);
    } else if (cmd == wire::OfxOverlayKey) {
        ok = o.controls->key(a.down != 0, a.keysym, text, e, t, v, edited, changed);
    } else if (cmd == wire::OfxOverlayFocus) {
        o.controls->focus(a.down != 0, e, t, v);
        ok = true;
    }
    if (o.controls->failed()) h->resultFlags |= wire::kControlFailed;
    if (changed) {
        // The edited effect back (Montage takes from it what the control set).
        o.out = effectToJsonString(edited);
        const size_t n = std::min(o.out.size(), wire::kPayload);
        std::memcpy(c.payload(), o.out.data(), n);
        h->payloadBytes = n;
        h->resultI = int64_t(o.out.size());
        h->resultFlags |= wire::kEdited;
        h->status = ok ? 1 : 0;
        c.reply();
        return;
    }
    c.replyText(ok, error);
}

void serveOfx(std::shared_ptr<OfxChannel> o) {
    tServing = true;
#ifndef _WIN32
    AltStack alt;
#endif
    Channel& c = o->chan;
    wire::Header* h = c.h;
    for (;;) {
        c.next();
        const uint32_t cmd = h->command;
        h->payloadBytes = std::min<uint64_t>(h->payloadBytes, wire::kPayload);
        if (cmd == wire::PutBlob) {
            if (h->argI == 0) o->blob.clear();
            o->blob.resize(size_t(h->argI));
            o->blob.append(c.payload(), size_t(h->payloadBytes));
            h->status = 1;
            h->payloadBytes = 0;
            c.reply();
            continue;
        }
        if (cmd == wire::Destroy) {
            o->controls.reset();
            h->status = 1;
            c.reply();
            return;
        }
        if (cmd == wire::FetchBlob) {
            const size_t off = size_t(std::max<int64_t>(0, h->argI));
            const size_t n = off < o->out.size() ? std::min(o->out.size() - off, wire::kPayload) : 0;
            std::memcpy(c.payload(), o->out.data() + off, n);
            h->payloadBytes = n;
            h->status = 1;
            c.reply();
            continue;
        }
        if (cmd >= wire::OfxOverlayOpen && cmd <= wire::OfxOverlayFocus) {
            serveOverlay(*o, cmd);
            checkMemory();
            continue;
        }
        if (cmd != wire::OfxRender) {
            h->status = 1;
            h->payloadBytes = 0;
            c.reply();
            continue;
        }
        // Everything the request says, read before any question to Montage reuses the header.
        const std::string msg = (h->flags & wire::kPayloadInBlob) ? o->blob : std::string(c.payload(), size_t(h->payloadBytes));
        const uint32_t flags = h->flags;
        const int kind = int(h->argI), w = int(h->argI2), hh = int(h->argI3);
        const double t = h->argD, progress = h->argD2, scale = h->argD3;
        const uint64_t empties = h->argU;
        const size_t a = msg.find('\0'), b = a == std::string::npos ? a : msg.find('\0', a + 1);
        std::string error;
        bool ok = false;
        Image out;
        Effect e;
        if (b == std::string::npos || !effectFromJsonString(msg.substr(0, a), e)) {
            error = "the effect did not come through";
        } else if (!o->mapArena()) {
            error = "the pictures did not come through";
        } else {
            const auto descs = ofx::descriptionsFromJson(msg.substr(a + 1, b - a - 1));
            for (const ofx::PluginDesc& d : descs) ofx::Registry::instance().adopt(d);
            const std::string spaceId = msg.substr(b + 1);
            const ColorSpace* space = spaceId.empty() ? nullptr : findColorSpace(spaceId);
            const size_t slot = size_t(std::max(0, w)) * size_t(std::max(0, hh)) * 16;
            const int images = kind == 1 ? 2 : kind == 0 ? 1 : 0;
            auto picture = [&](int i) {
                Image img(w, hh, Image::Uninitialized{});
                std::memcpy(img.px.data(), o->at(size_t(i) * slot), slot);
                return img;
            };
            std::vector<float> mask;
            if (flags & wire::kHasMask) {
                mask.resize(size_t(w) * size_t(hh));
                std::memcpy(mask.data(), o->at(size_t(images) * slot), mask.size() * sizeof(float));
            }
            ofx::FrameFetch fetch;
            if (flags & wire::kHasFetch) fetch = [&](double at, Image& img) { return ask(*o, wire::FetchFrame, at, {}, img); };
            std::unique_ptr<ofx::InputScope> inputs;
            if (flags & wire::kHasInputs)
                inputs = std::make_unique<ofx::InputScope>(
                    [&](const std::string& connection, double at, Image& img) { return ask(*o, wire::ResolveInput, at, connection, img); });
            ofx::OwnMaskScope ownMask((flags & wire::kHasMask) ? &mask : nullptr);
            if (kind == 0) {
                out = picture(0);
                ok = ofx::applyEffect(e, t, out, scale, fetch, &error, space);
            } else if (kind == 1) {
                const Image from = (empties & 1) ? Image() : picture(0);
                const Image to = (empties & 2) ? Image() : picture(1);
                ok = ofx::applyTransition(e, t, progress, from, to, out, scale, &error, space);
            } else {
                ok = ofx::renderGenerator(e, t, w, hh, scale, out, &error, space);
            }
            // The picture back, after the inputs (and the mask).
            if (ok) {
                const size_t outOff = size_t(images) * slot + ((flags & wire::kHasMask) ? size_t(w) * size_t(hh) * sizeof(float) : 0);
                const size_t bytes = size_t(out.width) * size_t(out.height) * 16;
                if (!o->mapArena() || outOff + bytes > o->arena->size()) {
                    ok = false;
                    error = "the picture is larger than Montage made room for";
                } else {
                    if (bytes) std::memcpy(o->at(outOff), out.px.data(), bytes);
                    h->resultI = out.width;
                    h->resultI2 = out.height;
                }
            }
        }
        checkMemory();
        c.replyText(ok, error);
    }
}

// The control channel: Montage asks for instances here.
void serveControl(std::string name) {
    tServing = true;
#ifndef _WIN32
    AltStack alt;
#endif
    Channel c;
    std::string err;
    if (!c.open(name, &err)) {
        std::fprintf(stderr, "montage-pluginhost: %s\n", err.c_str());
        std::_Exit(2);
    }
    wire::Header* h = c.h;
    gControl.store(h, std::memory_order_release);
    for (;;) {
        c.next();
        switch (h->command) {
            case wire::Nop: c.replyText(true, {}); break;
            case wire::Quit:
                c.replyText(true, {});
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                std::_Exit(0);
            case wire::Attach: {
                const std::string msg(c.payload(), size_t(std::min<uint64_t>(h->payloadBytes, wire::kPayload)));
                const size_t a = msg.find('\n'), b = a == std::string::npos ? a : msg.find('\n', a + 1);
                if (b == std::string::npos) {
                    c.replyText(false, "a malformed request");
                    break;
                }
                const std::string kind = msg.substr(0, a), channel = msg.substr(a + 1, b - a - 1), json = msg.substr(b + 1);
                if (kind == "ofx") {
                    auto o = std::make_shared<OfxChannel>();
                    if (!o->chan.open(channel, &err)) {
                        c.replyText(false, err);
                        break;
                    }
                    std::thread([o] { serveOfx(o); }).detach();
                    h->resultFlags = 0;
                    h->latency = 0;
                    c.replyText(true, {});
                    break;
                }
                const auto descs = descriptorsFromJson(json, &err);
                if (descs.empty()) {
                    c.replyText(false, err.empty() ? "no plug-in was named" : err);
                    break;
                }
                auto inst = std::make_shared<AudioInstance>();
                inst->d = descs[0];
                if (!inst->chan.open(channel, &err)) {
                    c.replyText(false, err);
                    break;
                }
                noteLoading(descs[0].id, true);
                inst->inst = instantiateHere(descs[0], &err);
                noteLoading({}, false);
                if (!inst->inst) {
                    c.replyText(false, err.empty() ? "the plug-in could not be loaded" : err);
                    break;
                }
                h->resultFlags = (inst->inst->hasSidechain() ? wire::kHasSidechain : 0) | (inst->inst->hasEditor() ? wire::kHasEditor : 0) |
                                 (inst->inst->editorResizable() ? wire::kEditorResizable : 0);
                h->latency = inst->inst->latencySamples();
                {
                    std::lock_guard lock(gInstancesM);
                    gInstances.push_back(inst);
                }
                std::thread([inst] { serveAudio(inst); }).detach();
                c.replyText(true, {});
                break;
            }
            default: c.replyText(false, "unknown request"); break;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string control;
    int64_t parent = 0;
    bool gpu = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--control" && i + 1 < argc) control = argv[++i];
        else if (a == "--parent" && i + 1 < argc) parent = std::atoll(argv[++i]);
        else if (a == "--memory-limit-mb" && i + 1 < argc) gMemoryLimit = std::atoll(argv[++i]) * 1024 * 1024;
        else if (a == "--gpu") gpu = true;
    }
    if (control.empty() || parent <= 0) {
        std::fprintf(stderr, "usage: montage-pluginhost --control <name> --parent <pid> [--memory-limit-mb N] [--gpu]\n"
                             "Started by Montage to run plug-ins in a process of their own.\n");
        return 2;
    }
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);  // (a crash ends it quietly; Montage reports it)
#endif
    catchCrashes();
    if (!ipc::processRunning(parent)) return wire::kExitLostMontage;
    setRunningInHost(true);
    ofx::setLoadWatcher(noteLoading);
    QGuiApplication app(argc, argv);
    if (gpu) ofx::initializeGpu();
    else ofx::setGpuRendering(false);  // (OpenGL may start later for drawing controls: renders stay on the CPU)
    // Montage gone, or too much memory: end.
    std::thread([parent] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (!ipc::processRunning(parent)) std::_Exit(wire::kExitLostMontage);
            checkMemory();
        }
    }).detach();
    std::thread([control] { serveControl(control); }).detach();
    // Open editors are idled here, about 30 times a second, whatever Montage asks.
    QTimer idle;
    QObject::connect(&idle, &QTimer::timeout, [] {
        std::vector<std::shared_ptr<AudioInstance>> list;
        {
            std::lock_guard lock(gInstancesM);
            list = gInstances;
        }
        for (const auto& a : list) {
            std::lock_guard lock(a->m);
            if (a->editorOpen && a->inst) a->inst->idle();
        }
    });
    idle.start(33);
    return app.exec();
}
