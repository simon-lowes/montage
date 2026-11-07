// Montage's side of the plug-in hosts (audio/PluginHost.h): the host processes, the stand-in audio instance, the
// failure book and the supervisor that restarts failed hosts.
#include "PluginHost.h"
#include "PluginHostInternal.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <algorithm>
#include <functional>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <thread>

#include "render/Ofx.h"

namespace montage::plugins {

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Settings

namespace {
struct Settings {
    std::mutex m;
    HostingSettings s;
    std::string exe;
    std::function<void(const HostingSettings&)> changed;
};
Settings& settings() {
    static Settings* s = new Settings;  // (never destroyed: hosts may be used while the program exits)
    return *s;
}
std::atomic<int> gIsolation{int(Isolation::PerPlugin)};
std::atomic<int> gMissedBlocks{16};
std::atomic<bool> gSilence{false};
std::atomic<int> gExportTimeoutMs{30000};
std::atomic<int> gMemoryLimitMb{4096};
std::atomic<int> gAutoRestarts{2};
std::atomic<bool> gInHost{false};
thread_local bool tRealtime = false;
}  // namespace

const char* isolationName(Isolation i) {
    switch (i) {
        case Isolation::PerPlugin: return "plugin";
        case Isolation::PerVendor: return "vendor";
        case Isolation::Shared: return "shared";
        case Isolation::InProcess: return "off";
    }
    return "plugin";
}

std::optional<Isolation> isolationFromName(std::string_view name) {
    for (Isolation i : {Isolation::PerPlugin, Isolation::PerVendor, Isolation::Shared, Isolation::InProcess})
        if (name == isolationName(i)) return i;
    return std::nullopt;
}

void setHosting(const HostingSettings& s) {
    std::lock_guard lock(settings().m);
    settings().s = s;
    gIsolation = int(s.isolation);
    gMissedBlocks = std::max(1, s.missedBlocks);
    gSilence = s.bypassWithSilence;
    gExportTimeoutMs = std::max(100, s.exportTimeoutMs);
    gMemoryLimitMb = std::max(0, s.memoryLimitMb);
    gAutoRestarts = std::max(0, s.autoRestarts);
}

void setHostingAndSave(const HostingSettings& s) {
    setHosting(s);
    std::function<void(const HostingSettings&)> save;
    {
        std::lock_guard lock(settings().m);
        save = settings().changed;
    }
    if (save) save(s);
}

void onHostingChanged(std::function<void(const HostingSettings&)> save) {
    std::lock_guard lock(settings().m);
    settings().changed = std::move(save);
}

HostingSettings hosting() {
    std::lock_guard lock(settings().m);
    return settings().s;
}

void setHostExecutable(const std::string& path) {
    std::lock_guard lock(settings().m);
    settings().exe = path;
}

std::string hostExecutable() {
    {
        std::lock_guard lock(settings().m);
        if (!settings().exe.empty()) return settings().exe;
    }
    const QString env = qEnvironmentVariable("MONTAGE_PLUGIN_HOST");
    if (!env.isEmpty()) return env.toStdString();
    if (!QCoreApplication::instance()) return {};
#ifdef _WIN32
    const QString name = QStringLiteral("montage-pluginhost.exe");
#else
    const QString name = QStringLiteral("montage-pluginhost");
#endif
    return QDir(QCoreApplication::applicationDirPath()).filePath(name).toStdString();
}

bool hostAvailable() {
    const std::string exe = hostExecutable();
    if (exe.empty()) return false;
    const QFileInfo fi(QString::fromStdString(exe));
    return fi.isFile() && fi.isExecutable();
}

void setRunningInHost(bool on) { gInHost = on; }
bool runningInHost() { return gInHost.load(); }

bool isolating() {
    if (gInHost.load() || Isolation(gIsolation.load()) == Isolation::InProcess) return false;
    static std::atomic<bool> warned{false};
    if (hostAvailable()) return true;
    if (!warned.exchange(true))
        std::fprintf(stderr, "montage: the plug-in host (%s) is missing: plug-ins run in Montage's own process\n", hostExecutable().c_str());
    return false;
}

RenderModeScope::RenderModeScope(bool realtime) : previous_(tRealtime) { tRealtime = realtime; }
RenderModeScope::~RenderModeScope() { tRealtime = previous_; }
bool renderingRealtime() { return tRealtime; }

namespace remote {

Isolation currentIsolation() { return Isolation(gIsolation.load()); }

std::string hostKey(Isolation grouping, const std::string& id, const std::string& maker, bool ofx) {
    const std::string prefix = ofx ? "ofx:" : "";
    switch (grouping) {
        case Isolation::PerVendor: return prefix + "vendor:" + maker;
        case Isolation::Shared: return prefix + "all";
        default: return prefix + "plugin:" + id;
    }
}

// ---------------------------------------------------------------------------
// Channels and hosts

bool Channel::create(std::string* error) {
    const std::string name = ipc::uniqueName("c");
    mem = ipc::SharedMemory::create(name, wire::kChannelBytes, error);
    if (!mem) return false;
    toHost = ipc::Bell::create(name + "h", error);
    toClient = ipc::Bell::create(name + "c", error);
    if (!toHost || !toClient) return false;
    h = wire::header(mem->data());
    h->magic = wire::kMagic;
    h->version = wire::kVersion;
    return true;
}

void Channel::unlinkNames() {
    if (mem) mem->unlinkName();
    if (toHost) toHost->unlinkName();
    if (toClient) toClient->unlinkName();
}

bool Host::alive() {
    if (dead_.load(std::memory_order_acquire)) return false;
    if (proc && proc->alive()) return true;
    dead_.store(true, std::memory_order_release);
    return false;
}

std::string Host::howItEnded() {
    if (alive()) return {};
    const int code = proc ? proc->exitCode() : -1;
    if (code == wire::kExitTooMuchMemory) return "used more memory than the limit (" + std::to_string(gMemoryLimitMb.load()) + " MB)";
    if (code == wire::kExitLostMontage) return "lost touch with Montage";
    const std::string how = proc ? proc->howItEnded() : std::string("ended");
    if (how.rfind("ended by signal", 0) == 0) return "crashed (" + how.substr(9) + ")";
    if (code == 0) return "ended";
    return "crashed (" + how + ")";
}

void Host::kill() {
    if (proc) proc->kill();
    dead_.store(true, std::memory_order_release);
}

Host::~Host() {
    if (alive()) {
        std::lock_guard lock(controlM);
        control.h->command = wire::Quit;
        call(control, *this, 300'000);
    }
    // (the process, if it still runs, is ended by its own destructor)
}

namespace {
std::mutex gPoolM;
std::map<std::string, std::weak_ptr<Host>>& pool() {
    static auto* p = new std::map<std::string, std::weak_ptr<Host>>;
    return *p;
}
}  // namespace

std::shared_ptr<Host> hostFor(const std::string& key, bool gpu, std::string* error) {
    std::lock_guard lock(gPoolM);
    if (auto h = pool()[key].lock(); h && h->alive()) return h;
    const std::string exe = hostExecutable();
    auto host = std::make_shared<Host>();
    host->key = key;
    if (!host->control.create(error)) return nullptr;
    std::vector<std::string> args{"--control", host->control.mem->name(), "--parent", std::to_string(ipc::ownPid()),
                                  "--memory-limit-mb", std::to_string(gMemoryLimitMb.load())};
    if (gpu) args.push_back("--gpu");
    // The same windowing system as Montage's (or none, as in the command-line tool).
    QString platform = QStringLiteral("offscreen");
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance())) platform = QGuiApplication::platformName();
    if (platform.isEmpty()) platform = QStringLiteral("offscreen");
    args.push_back("-platform");
    args.push_back(platform.toStdString());
    host->proc = ipc::Process::spawn(exe, args, error);
    if (!host->proc) return nullptr;
    host->control.h->command = wire::Nop;
    const Result r = call(host->control, *host, 20'000'000);
    if (r != Result::Ok) {
        if (error) *error = r == Result::Dead ? "the plug-in host " + host->howItEnded() + " while starting" : "the plug-in host did not start";
        host->kill();
        return nullptr;
    }
    host->control.unlinkNames();
    pool()[key] = host;
    return host;
}

bool collectPending(Channel& c) {
    if (!c.pending) return true;
    const uint32_t now = c.h->toClient.load(std::memory_order_acquire);
    if (c.h->replySeq != c.pending) return false;
    c.seen = now;
    c.pending = 0;
    return true;
}

Result call(Channel& c, Host& host, int64_t timeoutUs, const std::function<void(Channel&)>* callback) {
    wire::Header* h = c.h;
    const int64_t end = ipc::nowUs() + timeoutUs;
    // A reply still out from a message not waited for: the host takes the next one only after it.
    while (c.pending && !collectPending(c)) {
        const int64_t left = end - ipc::nowUs();
        if (left <= 0) return Result::Timeout;
        if (!c.toClient->wait(&h->toClient, h->toClient.load(std::memory_order_acquire), std::min<int64_t>(left, 2000)) && !host.alive())
            return Result::Dead;
    }
    if (++c.seq == 0) ++c.seq;
    h->seq = c.seq;
    h->toHost.store(++c.bumps, std::memory_order_release);
    c.toHost->ring(&h->toHost);
    for (;;) {
        const uint32_t now = h->toClient.load(std::memory_order_acquire);
        if (now != c.seen) {
            c.seen = now;
            if (h->replySeq != c.seq) continue;  // (a reply to a message given up on)
            if (h->replyKind == wire::Callback) {
                if (callback) (*callback)(c);
                else h->status = 0;
                h->command = wire::CallbackReply;
                h->toHost.store(++c.bumps, std::memory_order_release);
                c.toHost->ring(&h->toHost);
                continue;
            }
            return Result::Ok;
        }
        const int64_t left = end - ipc::nowUs();
        if (left <= 0) {
            c.pending = c.seq;
            return Result::Timeout;
        }
        if (!c.toClient->wait(&h->toClient, now, std::min<int64_t>(left, 2000)) && !host.alive()) {
            // (it may have answered just before it ended)
            if (h->toClient.load(std::memory_order_acquire) != c.seen && h->replySeq == c.seq && h->replyKind == wire::Final) {
                c.seen = h->toClient.load(std::memory_order_acquire);
                return Result::Ok;
            }
            return Result::Dead;
        }
    }
}

Attached attach(Host& host, Channel& chan, const std::string& kind, const std::string& json) {
    Attached a;
    std::lock_guard lock(host.controlM);
    const bool alone = host.instances == 0;  // (a shared host may end for another plug-in's doing)
    Channel& k = host.control;
    const std::string msg = kind + "\n" + chan.mem->name() + "\n" + json;
    if (msg.size() > wire::kPayload) {
        a.error = "the description is too long";
        return a;
    }
    std::memcpy(wire::payload(k.base()), msg.data(), msg.size());
    k.h->payloadBytes = msg.size();
    k.h->command = wire::Attach;
    const Result r = call(k, host, kLoadTimeoutUs);
    if (r == Result::Dead) {
        a.hostDied = alone;
        a.error = "its host " + host.howItEnded() + " while loading it";
        return a;
    }
    if (r == Result::Timeout) {
        a.timedOut = true;
        host.kill();
        a.error = "it did not finish loading within " + std::to_string(kLoadTimeoutUs / 1'000'000) + " seconds";
        return a;
    }
    if (!k.h->status) {
        a.error.assign(wire::payload(k.base()), size_t(std::min<uint64_t>(k.h->payloadBytes, wire::kPayload)));
        if (a.error.empty()) a.error = "the plug-in could not be loaded in its host";
        return a;
    }
    a.ok = true;
    a.flags = k.h->resultFlags;
    a.latency = k.h->latency;
    chan.unlinkNames();
    ++host.instances;
    return a;
}

bool sendPayload(Channel& c, Host& host, const std::string& data, int64_t timeoutUs, Result* r) {
    *r = Result::Ok;
    if (data.size() <= wire::kPayload) {
        std::memcpy(wire::payload(c.base()), data.data(), data.size());
        c.h->payloadBytes = data.size();
        c.h->flags &= ~wire::kPayloadInBlob;
        return true;
    }
    for (size_t off = 0; off < data.size(); off += wire::kPayload) {
        const size_t n = std::min(wire::kPayload, data.size() - off);
        std::memcpy(wire::payload(c.base()), data.data() + off, n);
        c.h->payloadBytes = n;
        c.h->command = wire::PutBlob;
        c.h->argI = int64_t(off);
        c.h->argU = data.size();
        c.h->paramCount = 0;
        if ((*r = call(c, host, timeoutUs)) != Result::Ok || !c.h->status) return false;
    }
    c.h->payloadBytes = 0;
    c.h->flags |= wire::kPayloadInBlob;
    return true;
}

bool receiveBlob(Channel& c, Host& host, int64_t size, std::string& out, int64_t timeoutUs, Result* r) {
    *r = Result::Ok;
    out.assign(wire::payload(c.base()), size_t(std::min<uint64_t>(uint64_t(std::max<int64_t>(0, size)), c.h->payloadBytes)));
    while (int64_t(out.size()) < size) {
        c.h->command = wire::FetchBlob;
        c.h->argI = int64_t(out.size());
        c.h->paramCount = 0;
        if ((*r = call(c, host, timeoutUs)) != Result::Ok || c.h->payloadBytes == 0) return false;
        out.append(wire::payload(c.base()), size_t(std::min<uint64_t>(c.h->payloadBytes, wire::kPayload)));
    }
    return true;
}

// ---------------------------------------------------------------------------
// The failure book

namespace {
struct Book {
    std::mutex m;
    std::map<std::string, HostFailure> byId;
    std::set<std::string> held, exhausted;
    std::map<std::string, int> autoUsed;
    std::atomic<uint64_t> serial{0}, offlineSerial{0};
    std::string lastOffline;
};
Book& book() {
    static Book* b = new Book;
    return *b;
}
int64_t epochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
}  // namespace

void recordFailure(const std::string& id, const std::string& name, const std::string& vendor, const std::string& kind,
                   const std::string& reason, bool offline, bool countsAsNew, bool offlineNoted) {
    Book& b = book();
    {
        std::lock_guard lock(b.m);
        HostFailure& f = b.byId[id];
        const int64_t now = epochMs();
        if (f.count == 0) f.firstMs = now;
        f.pluginId = id;
        f.name = name.empty() ? id : name;
        f.vendor = vendor;
        f.kind = kind;
        f.reason = reason;
        f.lastMs = now;
        f.offline = offline;
        if (countsAsNew) ++f.count;
        if (offline && !offlineNoted) {
            std::string why = reason;
            if (const size_t cut = why.rfind("; bypassed"); cut != std::string::npos) why.erase(cut);
            b.lastOffline = f.name + " " + why;
            ++b.offlineSerial;
        }
    }
    ++b.serial;
    std::fprintf(stderr, "montage: plug-in %s %s\n", name.c_str(), reason.c_str());
}

void noteOfflineFailure(const std::string& name, const std::string& why) {
    Book& b = book();
    {
        std::lock_guard lock(b.m);
        b.lastOffline = name + " " + why;
    }
    ++b.offlineSerial;
}

void noteRestart(const std::string& id) {
    Book& b = book();
    {
        std::lock_guard lock(b.m);
        if (auto it = b.byId.find(id); it != b.byId.end()) ++it->second.restarts;
    }
    ++b.serial;
}

bool pluginHeldBypassed(const std::string& id) {
    std::lock_guard lock(book().m);
    return book().held.count(id) > 0;
}

int autoRestartsLeft(const std::string& id) {
    std::lock_guard lock(book().m);
    return std::max(0, gAutoRestarts.load() - book().autoUsed[id]);
}

void useAutoRestart(const std::string& id) {
    std::lock_guard lock(book().m);
    ++book().autoUsed[id];
}

void markExhausted(const std::string& id) {
    {
        std::lock_guard lock(book().m);
        book().exhausted.insert(id);
    }
    ++book().serial;
}

bool exhausted(const std::string& id) {
    std::lock_guard lock(book().m);
    return book().exhausted.count(id) > 0;
}

}  // namespace remote

using namespace remote;

// ---------------------------------------------------------------------------
// The stand-in instance

namespace {

struct Link {
    std::shared_ptr<Host> host;
    Channel chan;
    uint32_t flags = 0;
    int latency = 0;
    bool attached = false;
    ~Link() {
        if (!host) return;
        // The instance in the host goes too (not when the host has failed: then it went with it).
        if (attached && host->alive() && chan.h) {
            chan.h->command = wire::Destroy;
            chan.h->paramCount = 0;
            call(chan, *host, 1'000'000);
        }
        if (attached) --host->instances;
    }
};

enum Failure { NoFailure = 0, Crashed = 1, Hung = 2 };

// What a stand-in shares with the supervisor: its failure, a restarted link waiting for it, and what a restart needs.
struct Core {
    Descriptor d;
    Isolation grouping = Isolation::PerPlugin;
    std::atomic<Link*> next{nullptr};
    std::atomic<Link*> retired{nullptr};
    std::atomic<int> state{0};  // 0 running, 1 bypassed, 2 a restart being made
    std::atomic<int> failure{NoFailure};
    std::atomic<bool> failedOffline{false};
    std::atomic<bool> lastRenderOffline{false};  // (whether its last block was an export's: a host found ended between blocks)
    std::atomic<int> missed{0};
    std::atomic<bool> attention{false};
    std::atomic<bool> restartAsked{false};
    std::atomic<int64_t> hostPid{0};
    std::weak_ptr<Host> host;  // (the supervisor's copy, under memM)
    // Remembered for a restart.
    std::mutex memM;
    std::string savedState;
    bool hasState = false;
    double rate = 0;
    int maxFrames = 0;
    bool active = false;
    int offline = -1, mainChannels = 0;
    std::vector<uint32_t> ids;  // sorted
    std::unique_ptr<std::atomic<double>[]> values;
    std::unique_ptr<std::atomic<bool>[]> set;
    void remember(uint32_t id, double v) {
        const auto it = std::lower_bound(ids.begin(), ids.end(), id);
        if (it == ids.end() || *it != id) return;
        const size_t i = size_t(it - ids.begin());
        values[i].store(v, std::memory_order_relaxed);
        set[i].store(true, std::memory_order_release);
    }
    std::optional<double> remembered(uint32_t id) const {
        const auto it = std::lower_bound(ids.begin(), ids.end(), id);
        if (it == ids.end() || *it != id) return std::nullopt;
        const size_t i = size_t(it - ids.begin());
        if (!set[i].load(std::memory_order_acquire)) return std::nullopt;
        return values[i].load(std::memory_order_relaxed);
    }
    ~Core() {
        delete next.exchange(nullptr);
        delete retired.exchange(nullptr);
    }
};

// The supervisor: looks after failed stand-ins off the audio thread (records, ends hung hosts, restarts).
struct Supervisor {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::weak_ptr<Core>> cores;
    bool started = false;
    void add(const std::shared_ptr<Core>& c) {
        std::lock_guard lock(m);
        cores.erase(std::remove_if(cores.begin(), cores.end(), [](const auto& w) { return w.expired(); }), cores.end());
        cores.push_back(c);
        if (!started) {
            started = true;
            std::thread([this] { run(); }).detach();
        }
    }
    std::vector<std::shared_ptr<Core>> live() {
        std::lock_guard lock(m);
        std::vector<std::shared_ptr<Core>> out;
        for (const auto& w : cores)
            if (auto c = w.lock()) out.push_back(c);
        return out;
    }
    void wake() { cv.notify_all(); }
    void run();
    void pass();
};
Supervisor& supervisor() {
    static Supervisor* s = new Supervisor;
    return *s;
}

int64_t timeoutFor(int frames, double rate) {
    if (!tRealtime) return int64_t(gExportTimeoutMs.load()) * 1000;
    const double blockUs = rate > 0 ? double(frames) * 1e6 / rate : 10000;
    return std::max<int64_t>(10'000, int64_t(2 * blockUs));
}

// Builds a link for `core` in a (new) host and gives it what the instance had: offline mode, channels, activation,
// state and parameters.
std::unique_ptr<Link> buildLink(Core& core, std::string* error) {
    std::string key = hostKey(core.grouping, core.d.id, core.d.vendor.empty() ? core.d.path : core.d.vendor, false);
    auto host = hostFor(key, false, error);
    if (!host) return nullptr;
    auto link = std::make_unique<Link>();
    link->host = host;
    if (!link->chan.create(error)) return nullptr;
    const Attached a = attach(*host, link->chan, "audio", descriptorsToJson({core.d}));
    if (!a.ok) {
        if (error) *error = a.error;
        return nullptr;
    }
    link->attached = true;
    link->flags = a.flags;
    link->latency = a.latency;
    Channel& c = link->chan;
    auto run = [&](uint32_t cmd, int64_t timeout) {
        c.h->command = cmd;
        c.h->paramCount = 0;
        return call(c, *host, timeout) == Result::Ok;
    };
    std::lock_guard lock(core.memM);
    if (core.offline >= 0) {
        c.h->argI = core.offline;
        if (!run(wire::SetOffline, kControlTimeoutUs)) return nullptr;
    }
    if (core.mainChannels > 0) {
        c.h->argI = core.mainChannels;
        if (!run(wire::SetMainChannels, kControlTimeoutUs)) return nullptr;
    }
    if (core.active) {
        c.h->argD = core.rate;
        c.h->argI = core.maxFrames;
        if (!run(wire::Activate, kLoadTimeoutUs) || !c.h->status) {
            if (error) *error = "it could not be activated again";
            return nullptr;
        }
    }
    if (core.hasState) {
        Result r;
        if (!sendPayload(c, *host, core.savedState, kControlTimeoutUs, &r)) return nullptr;
        c.h->argU = core.savedState.size();
        c.h->command = wire::LoadState;
        c.h->paramCount = 0;
        if (call(c, *host, kControlTimeoutUs) != Result::Ok) return nullptr;
        c.h->flags = 0;
    }
    // The parameters as they were last set (automation included).
    uint32_t n = 0;
    for (size_t i = 0; i < core.ids.size(); ++i) {
        if (!core.set[i].load()) continue;
        c.h->params[n++] = {core.ids[i], 0, core.values[i].load()};
        if (n == uint32_t(wire::kMaxParamChanges) || i + 1 == core.ids.size()) {
            c.h->paramCount = n;
            c.h->command = wire::Flush;
            if (call(c, *host, kControlTimeoutUs) != Result::Ok) return nullptr;
            n = 0;
        }
    }
    if (n > 0) {
        c.h->paramCount = n;
        c.h->command = wire::Flush;
        if (call(c, *host, kControlTimeoutUs) != Result::Ok) return nullptr;
    }
    link->latency = c.h->latency;
    core.host = host;
    core.hostPid = host->proc ? host->proc->pid() : 0;
    return link;
}

void Supervisor::run() {
    for (;;) {
        {
            std::unique_lock lock(m);
            cv.wait_for(lock, 20ms);
        }
        pass();
    }
}

// Records a stand-in's failure (and ends its host, when hung). `same`: another instance of the plug-in failed with it,
// in the same host (one failure, not two).
void reportFailure(Core& core, bool same) {
    std::shared_ptr<Host> host;
    {
        std::lock_guard lock(core.memM);
        host = core.host.lock();
    }
    const bool offline = core.failedOffline.load();
    std::string kind = "crash", reason;
    if (core.failure == Hung) {
        kind = "hang";
        reason = offline ? "did not answer within " + std::to_string(gExportTimeoutMs.load() / 1000) + " s and was stopped"
                         : "stopped responding (" + std::to_string(core.missed.load()) + " audio blocks late in a row) and was stopped";
        if (host) host->kill();
    } else {
        const std::string how = host ? host->howItEnded() : std::string("ended");
        if (how.rfind("used more memory", 0) == 0) kind = "memory";
        reason = how.empty() ? "stopped answering" : how;
    }
    recordFailure(core.d.id, core.d.name, core.d.vendor, kind, reason + "; bypassed", offline, !same, offline);
}

void Supervisor::pass() {
    std::set<std::string> restartedNow;
    for (const std::shared_ptr<Core>& core : live()) {
        delete core->retired.exchange(nullptr);
        if (core->state.load() == 0) {
            // A host that ended while its instances were idle (no block to notice it by).
            std::shared_ptr<Host> host;
            {
                std::lock_guard lock(core->memM);
                host = core->host.lock();
            }
            int expected = 0;
            if (host && !host->alive() && core->state.compare_exchange_strong(expected, 1)) {
                core->failure = Crashed;
                core->failedOffline = core->lastRenderOffline.load();
                core->attention = true;
            }
        }
        if (core->attention.exchange(false)) reportFailure(*core, restartedNow.count(core->d.id) > 0);
        if (core->state.load() != 1) continue;
        const bool asked = core->restartAsked.exchange(false);
        if (pluginHeldBypassed(core->d.id) && !asked) continue;
        if (!asked && !restartedNow.count(core->d.id)) {
            if (autoRestartsLeft(core->d.id) <= 0) continue;
            useAutoRestart(core->d.id);
        }
        restartedNow.insert(core->d.id);
        core->state = 2;
        std::string err;
        if (auto link = buildLink(*core, &err)) {
            delete core->next.exchange(link.release());
            noteRestart(core->d.id);
        } else {
            core->state = 1;
            recordFailure(core->d.id, core->d.name, core->d.vendor, "crash", "could not be started again: " + err, false, false);
        }
    }
}

class RemoteInstance : public Instance {
public:
    RemoteInstance(std::shared_ptr<Core> core, std::unique_ptr<Link> link) : core_(std::move(core)), link_(std::move(link)) {
        hasSidechain_ = link_->flags & wire::kHasSidechain;
        hasEditor_ = link_->flags & wire::kHasEditor;
        resizable_ = link_->flags & wire::kEditorResizable;
        latency_ = link_->latency;
    }

    ~RemoteInstance() override {
        // (a failure the supervisor has not seen yet, as when an export stops at it, is still recorded)
        if (core_->attention.exchange(false)) reportFailure(*core_, false);
        if (editorOpen_) closeEditor();
        link_.reset();  // (its instance in the host goes with it)
    }

    bool activate(double sampleRate, int maxFrames) override {
        takeNext();
        {
            std::lock_guard lock(core_->memM);
            core_->rate = sampleRate;
            core_->maxFrames = std::min(maxFrames, wire::kMaxFrames);
            core_->active = true;
            delayChannels_ = std::clamp(core_->mainChannels > 0 ? core_->mainChannels : 2, 2, wire::kMaxChannels);
        }
        rate_ = sampleRate;
        prepareDelay();
        if (!usable()) return true;  // (bypassed: it comes back activated)
        Channel& c = link_->chan;
        c.h->argD = sampleRate;
        c.h->argI = std::min(maxFrames, wire::kMaxFrames);
        if (!send(wire::Activate, kLoadTimeoutUs)) return true;
        latency_ = c.h->latency;
        prepareDelay();
        return c.h->status != 0;
    }

    void process(float* const* channels, int numChannels, int frames) override {
        takeNext();
        if (frames <= 0) return;
        for (int off = 0; off < frames; off += wire::kMaxFrames) processChunk(channels, numChannels, off, std::min(wire::kMaxFrames, frames - off));
        key_ = nullptr;
    }

    std::vector<ParamInfo> parameters() override {
        takeNext();
        if (!usable() || !send(wire::Parameters, kControlTimeoutUs)) return paramsCache_;
        const char* p = wire::payload(link_->chan.base());
        const char* end = p + std::min<uint64_t>(link_->chan.h->payloadBytes, wire::kPayload);
        std::vector<ParamInfo> out;
        auto take = [&](void* dst, size_t n) {
            if (size_t(end - p) < n) return false;
            std::memcpy(dst, p, n);
            p += n;
            return true;
        };
        uint32_t count = 0;
        if (!take(&count, 4)) return paramsCache_;
        for (uint32_t i = 0; i < count; ++i) {
            ParamInfo pi;
            uint8_t flags = 0;
            uint16_t len = 0;
            if (!take(&pi.id, 4) || !take(&pi.min, 8) || !take(&pi.max, 8) || !take(&pi.def, 8) || !take(&flags, 1) || !take(&len, 2)) break;
            if (size_t(end - p) < len) break;
            pi.name.assign(p, len);
            p += len;
            pi.automatable = flags & 1;
            pi.stepped = flags & 2;
            out.push_back(std::move(pi));
        }
        paramsCache_ = out;
        return out;
    }

    double parameter(uint32_t id) override {
        takeNext();
        if (usable()) {
            link_->chan.h->argU = id;
            if (send(wire::GetParameter, kControlTimeoutUs)) return link_->chan.h->resultD;
        }
        for (int i = queued_ - 1; i >= 0; --i)
            if (queue_[size_t(i)].id == id) return queue_[size_t(i)].value;
        return core_->remembered(id).value_or(0.0);
    }

    void setParameter(uint32_t id, double value) override {
        core_->remember(id, value);
        if (queued_ == wire::kMaxParamChanges) {
            takeNext();
            if (usable()) send(wire::Flush, kControlTimeoutUs);
            queued_ = 0;
        }
        queue_[size_t(queued_++)] = {id, 0, value};
    }

    std::string saveState() override {
        takeNext();
        if (usable() && send(wire::SaveState, kControlTimeoutUs)) {
            std::string s;
            Result r;
            if (receiveBlob(link_->chan, *link_->host, link_->chan.h->resultI, s, kControlTimeoutUs, &r)) {
                std::lock_guard lock(core_->memM);
                core_->savedState = s;
                core_->hasState = true;
                return s;
            }
            noteResult(r);
        }
        // (bypassed: the last state it had, so the project keeps its settings)
        std::lock_guard lock(core_->memM);
        return core_->savedState;
    }

    bool loadState(const std::string& state) override {
        takeNext();
        {
            std::lock_guard lock(core_->memM);
            core_->savedState = state;
            core_->hasState = true;
        }
        if (!usable()) return true;
        Channel& c = link_->chan;
        Result r;
        if (!sendPayload(c, *link_->host, state, kControlTimeoutUs, &r)) {
            noteResult(r);
            return false;
        }
        c.h->argU = state.size();
        const bool ok = send(wire::LoadState, kControlTimeoutUs) && c.h->status;
        c.h->flags = 0;
        if (ok) latency_ = c.h->latency;
        return ok;
    }

    int latencySamples() override {
        takeNext();
        return latency_;
    }
    bool hasSidechain() override { return hasSidechain_; }
    void setSidechain(const float* const* channels, int numChannels) override {
        key_ = channels;
        keyChannels_ = channels ? numChannels : 0;
    }
    void enableSidechain(bool on) override { keyEnable_ = on ? 1 : 0; }

    void reset() override {
        takeNext();
        for (auto& line : delay_) std::fill(line.begin(), line.end(), 0.0f);
        if (usable()) send(wire::Reset, kControlTimeoutUs);
    }

    void setTransport(int64_t samplePosition, bool playing, double bpm, int beatsPerBar) override {
        transport_ = true;
        position_ = samplePosition;
        playing_ = playing;
        bpm_ = bpm;
        beatsPerBar_ = beatsPerBar;
    }

    void setOffline(bool offline) override {
        takeNext();
        {
            std::lock_guard lock(core_->memM);
            core_->offline = offline ? 1 : 0;
        }
        if (!usable()) return;
        link_->chan.h->argI = offline ? 1 : 0;
        send(wire::SetOffline, kControlTimeoutUs);
    }

    bool setMainChannels(int channels) override {
        takeNext();
        {
            std::lock_guard lock(core_->memM);
            core_->mainChannels = channels;
        }
        if (!usable()) return false;
        link_->chan.h->argI = channels;
        return send(wire::SetMainChannels, kControlTimeoutUs) && link_->chan.h->status;
    }

    bool hasEditor() override { return hasEditor_; }

    bool openEditor(void* parent, EditorListener* listener, int& width, int& height) override {
        takeNext();
        if (!usable()) return false;
#ifdef __APPLE__
        // (macOS: a view of another process cannot be a parent; the editor opens in a window of the host's own)
        const uint64_t parentId = 0;
        if (!parent) return false;
#else
        if (!parent) return false;
        const uint64_t parentId = uint64_t(reinterpret_cast<uintptr_t>(parent));
#endif
        Channel& c = link_->chan;
        c.h->argU = parentId;
        if (!send(wire::OpenEditor, kLoadTimeoutUs) || !c.h->status) return false;
        width = int(c.h->resultI);
        height = int(c.h->resultI2);
        resizable_ = c.h->resultFlags & wire::kEditorResizable;
        ownWindow_ = c.h->resultFlags & wire::kOwnWindow;
        listener_ = listener;
        editorOpen_ = true;
        deliverEvents();
        return true;
    }

    void closeEditor() override {
        takeNext();
        if (!editorOpen_) return;
        editorOpen_ = false;
        if (usable()) send(wire::CloseEditor, kControlTimeoutUs);
        listener_ = nullptr;
    }

    bool editorResizable() override { return resizable_; }
    bool editorInOwnWindow() override { return ownWindow_; }

    void setEditorSize(int width, int height) override {
        takeNext();
        if (!editorOpen_ || !usable()) return;
        link_->chan.h->argI = width;
        link_->chan.h->argI2 = height;
        send(wire::SetEditorSize, kControlTimeoutUs);
    }

    void idle() override {
        takeNext();
        if (!editorOpen_) return;
        if (!usable()) {
            // The host failed: its window went with it.
            editorOpen_ = false;
            if (EditorListener* l = std::exchange(listener_, nullptr)) l->editorClosed();
            return;
        }
        if (send(wire::EditorIdle, kControlTimeoutUs)) deliverEvents();
    }

    int64_t hostPid() const { return core_->hostPid.load(); }
    bool isBypassed() const { return core_->state.load() != 0; }

private:
    bool usable() const { return link_ && core_->state.load(std::memory_order_acquire) == 0; }

    // A restarted link from the supervisor, taken up here (the thread using the instance).
    void takeNext() {
        if (Link* n = core_->next.exchange(nullptr, std::memory_order_acq_rel)) {
            Link* old = link_.release();
            link_.reset(n);
            if (old) delete core_->retired.exchange(old);  // (freed by the supervisor; the one before, here, is rare)
            latency_ = n->latency;
            keySent_ = -1;
            offlineNoted_ = false;
            core_->missed = 0;
            core_->failure = NoFailure;
            core_->state.store(0, std::memory_order_release);
            if (editorOpen_) {  // (its window went with the old host)
                editorOpen_ = false;
                if (EditorListener* l = std::exchange(listener_, nullptr)) l->editorClosed();
            }
        }
    }

    // (`rendering`: in the middle of processing; a failure on a control call, from the UI thread, is no export's)
    void fail(Failure f, bool rendering = false) {
        int expected = 0;
        if (!core_->state.compare_exchange_strong(expected, 1)) return;
        core_->failure = f;
        core_->failedOffline = rendering && !tRealtime;
        if (rendering && !tRealtime) {
            offlineNoted_ = true;
            // (an export checks for failures between frames: it hears of this one before it finishes)
            std::string how = f == Hung ? "did not answer within " + std::to_string(gExportTimeoutMs.load() / 1000) + " s and was stopped"
                                        : link_->host->howItEnded();
            noteOfflineFailure(core_->d.name.empty() ? core_->d.id : core_->d.name, how.empty() ? "stopped answering" : how);
        }
        core_->attention = true;
    }

    void noteResult(Result r) {
        if (r == Result::Dead) fail(Crashed);
        else if (r == Result::Timeout) fail(Hung);
    }

    // Sends a command with the queued parameter changes (they go either way: a late reply still applies them).
    bool send(uint32_t cmd, int64_t timeoutUs) {
        Channel& c = link_->chan;
        std::memcpy(c.h->params, queue_.data(), size_t(queued_) * sizeof(wire::ParamChange));
        c.h->paramCount = uint32_t(queued_);
        c.h->command = cmd;
        c.h->keyEnable = -1;
        const Result r = call(c, *link_->host, timeoutUs);
        if (r != Result::Dead) queued_ = 0;
        if (r == Result::Ok) return true;
        noteResult(r);
        return false;
    }

    static constexpr size_t kLatencyHeadroom = 8192;
    void prepareDelay() {
        // The input, delayed by the plug-in's latency, is what a bypassed instance gives (so the sound stays where
        // the mixer's delay compensation expects it).
        const int nch = delayChannels_;
        // (room for a whole block after the latency: a block the host did not give back is delayed after the fact;
        // and for the latency to grow while it plays, where nothing may be allocated, as a plug-in's may after a
        // state is loaded or a parameter changes)
        const size_t cap = size_t(std::max(0, latency_)) + size_t(wire::kMaxFrames) + 1 + kLatencyHeadroom;
        if (delay_.size() == size_t(nch) && delay_[0].size() >= cap) return;
        delay_.assign(size_t(nch), std::vector<float>(cap, 0.0f));
        delayPos_ = 0;
    }

    // Keeps the input's history (when the plug-in has latency) and, when `bypass`, gives it out delayed.
    void delayLine(float* const* channels, int numChannels, int off, int frames, bool bypass) {
        const bool silence = gSilence.load(std::memory_order_relaxed);
        if (delay_.empty() || latency_ <= 0 || size_t(latency_) + size_t(frames) >= delay_[0].size()) {
            if (bypass && silence)
                for (int c = 0; c < numChannels; ++c) std::fill(channels[c] + off, channels[c] + off + frames, 0.0f);
            return;
        }
        const size_t cap = delay_[0].size();
        const size_t lat = size_t(latency_);
        size_t pos = delayPos_;
        for (int c = 0; c < numChannels; ++c) {
            pos = delayPos_;
            float* io = channels[c] + off;
            std::vector<float>* line = c < int(delay_.size()) ? &delay_[size_t(c)] : nullptr;
            for (int i = 0; i < frames; ++i) {
                const float in = io[i];
                if (line) {
                    (*line)[pos] = in;
                    if (bypass) io[i] = silence ? 0.0f : (*line)[(pos + cap - lat) % cap];
                } else if (bypass && silence) {
                    io[i] = 0.0f;
                }
                if (++pos == cap) pos = 0;
            }
        }
        delayPos_ = pos;
    }

    void processChunk(float* const* channels, int numChannels, int off, int frames) {
        core_->lastRenderOffline.store(!tRealtime, std::memory_order_relaxed);
        if (!usable()) {
            if (!tRealtime && !offlineNoted_ && !pluginHeldBypassed(core_->d.id)) {
                // An export would leave it out: it stops instead, naming it (a plug-in the user disabled after it
                // failed stays out, as asked).
                offlineNoted_ = true;
                std::string how = core_->failure == Hung ? std::string("stopped responding") : link_ ? link_->host->howItEnded() : std::string();
                noteOfflineFailure(core_->d.name.empty() ? core_->d.id : core_->d.name,
                                   (how.empty() ? std::string("failed") : how) + " in its plug-in host and is bypassed");
            }
            delayLine(channels, numChannels, off, frames, true);
            queued_ = 0;  // (remembered for the restart)
            return;
        }
        Channel& c = link_->chan;
        wire::Header* h = c.h;
        if (c.pending && !collectPending(c)) {
            // Still busy with a block it was late with.
            delayLine(channels, numChannels, off, frames, true);
            missedBlock();
            return;
        }
        const int nch = std::min(numChannels, wire::kMaxChannels);
        for (int ch = 0; ch < nch; ++ch) std::memcpy(wire::audio(c.base(), ch), channels[ch] + off, size_t(frames) * sizeof(float));
        const int kch = key_ ? std::min(keyChannels_, wire::kMaxChannels) : 0;
        for (int ch = 0; ch < kch; ++ch)
            std::memcpy(wire::audio(c.base(), wire::kMaxChannels + ch), key_[ch] + off, size_t(frames) * sizeof(float));
        h->frames = frames;
        h->channels = nch;
        h->keyChannels = kch;
        h->keyEnable = keyEnable_ != keySent_ ? keyEnable_ : -1;
        h->flags = 0;
        if (transport_) {
            h->flags |= wire::kTransport | (playing_ ? uint32_t(wire::kPlaying) : 0u);
            h->position = position_;
            h->bpm = bpm_;
            h->beatsPerBar = beatsPerBar_;
        }
        std::memcpy(h->params, queue_.data(), size_t(queued_) * sizeof(wire::ParamChange));
        h->paramCount = uint32_t(queued_);
        h->command = wire::Process;
        delayLine(channels, numChannels, off, frames, false);  // (the input's history)
        const Result r = call(c, *link_->host, timeoutFor(frames, rate_));
        if (r != Result::Dead) {
            queued_ = 0;
            keySent_ = keyEnable_;
            transport_ = false;
        }
        if (r == Result::Ok) {
            for (int ch = 0; ch < nch; ++ch) std::memcpy(channels[ch] + off, wire::audio(c.base(), ch), size_t(frames) * sizeof(float));
            latency_ = h->latency;
            core_->missed = 0;
            return;
        }
        bypassAfterTheFact(channels, numChannels, off, frames);
        if (r == Result::Dead) fail(Crashed, true);
        else if (!tRealtime) fail(Hung, true);
        else missedBlock();
    }

    // The block's output when the host did not give it back (the history already holds its input).
    void bypassAfterTheFact(float* const* channels, int numChannels, int off, int frames) {
        const bool silence = gSilence.load(std::memory_order_relaxed);
        if (delay_.empty() || latency_ <= 0 || size_t(latency_) + size_t(frames) >= delay_[0].size()) {
            if (silence)
                for (int c = 0; c < numChannels; ++c) std::fill(channels[c] + off, channels[c] + off + frames, 0.0f);
            return;
        }
        const size_t cap = delay_[0].size(), lat = size_t(latency_);
        const size_t start = (delayPos_ + cap - size_t(frames)) % cap;  // where this block's input went
        for (int c = 0; c < numChannels; ++c) {
            float* io = channels[c] + off;
            const std::vector<float>* line = c < int(delay_.size()) ? &delay_[size_t(c)] : nullptr;
            for (int i = 0; i < frames; ++i) {
                if (silence) io[i] = 0.0f;
                else if (line) io[i] = (*line)[(start + size_t(i) + cap - lat) % cap];
            }
        }
    }

    void missedBlock() {
        const int n = ++core_->missed;
        if (n >= gMissedBlocks.load(std::memory_order_relaxed)) fail(Hung);
    }

    void deliverEvents() {
        Channel& c = link_->chan;
        const char* p = wire::payload(c.base());
        const size_t n = size_t(std::min<uint64_t>(c.h->payloadBytes, wire::kPayload)) / wire::kEventBytes;
        // (copied first: a listener may call back into the instance)
        std::vector<char> events(p, p + n * wire::kEventBytes);
        for (size_t i = 0; i < n && listener_; ++i) {
            const char* e = events.data() + i * wire::kEventBytes;
            uint8_t kind = uint8_t(e[0]);
            uint32_t id;
            double value;
            int32_t w, hgt;
            std::memcpy(&id, e + 1, 4);
            std::memcpy(&value, e + 5, 8);
            std::memcpy(&w, e + 13, 4);
            std::memcpy(&hgt, e + 17, 4);
            switch (kind) {
                case wire::EvParameter: listener_->editorParameter(id, value); break;
                case wire::EvGestureBegin: listener_->editorGesture(id, true); break;
                case wire::EvGestureEnd: listener_->editorGesture(id, false); break;
                case wire::EvResize: listener_->editorResize(w, hgt); break;
                case wire::EvClosed:
                    editorOpen_ = false;
                    if (EditorListener* l = std::exchange(listener_, nullptr)) l->editorClosed();
                    break;
                default: break;
            }
        }
    }

    std::shared_ptr<Core> core_;
    std::unique_ptr<Link> link_;
    std::array<wire::ParamChange, wire::kMaxParamChanges> queue_{};
    int queued_ = 0;
    const float* const* key_ = nullptr;
    int keyChannels_ = 0;
    int keyEnable_ = 0, keySent_ = -1;
    bool transport_ = false, playing_ = false;
    int64_t position_ = 0;
    double bpm_ = 120;
    int beatsPerBar_ = 4;
    double rate_ = 48000;
    int latency_ = 0;
    bool hasSidechain_ = false, hasEditor_ = false, resizable_ = false, ownWindow_ = false;
    bool editorOpen_ = false;
    bool offlineNoted_ = false;  // an export was told it failed
    EditorListener* listener_ = nullptr;
    std::vector<ParamInfo> paramsCache_;
    std::vector<std::vector<float>> delay_;
    size_t delayPos_ = 0;
    int delayChannels_ = 2;
};

}  // namespace

std::unique_ptr<Instance> instantiateIsolated(const Descriptor& d, Isolation grouping, std::string* error) {
    if (grouping == Isolation::InProcess) grouping = Isolation::PerPlugin;
    if (!hostAvailable()) {
        if (error) *error = "the plug-in host program is missing";
        return nullptr;
    }
    auto core = std::make_shared<Core>();
    core->d = d;
    core->grouping = grouping;
    const std::string key = hostKey(grouping, d.id, d.vendor.empty() ? d.path : d.vendor, false);
    std::string err;
    auto host = hostFor(key, false, &err);
    if (!host) {
        if (error) *error = err;
        return nullptr;
    }
    auto link = std::make_unique<Link>();
    link->host = host;
    if (!link->chan.create(&err)) {
        if (error) *error = err;
        return nullptr;
    }
    const Attached a = attach(*host, link->chan, "audio", descriptorsToJson({d}));
    if (!a.ok) {
        if (a.hostDied) {
            // As a crash while scanning: blocked until the file changes or the user tries it again.
            Registry::instance().blockAfterCrash(d.format, d.path, "crashed while loading: " + a.error);
            recordFailure(d.id, d.name, d.vendor, "load", a.error + "; blocked", !tRealtime);
        } else if (a.timedOut) {
            recordFailure(d.id, d.name, d.vendor, "hang", a.error, !tRealtime);
        }
        if (error) *error = a.error;
        return nullptr;
    }
    link->attached = true;
    link->flags = a.flags;
    link->latency = a.latency;
    core->host = host;
    core->hostPid = host->proc ? host->proc->pid() : 0;
    auto inst = std::make_unique<RemoteInstance>(core, std::move(link));
    // The parameters it has, for remembering their values across a restart.
    std::vector<uint32_t> ids;
    for (const ParamInfo& p : inst->parameters()) ids.push_back(p.id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    core->ids = ids;
    core->values = std::make_unique<std::atomic<double>[]>(ids.size());
    core->set = std::make_unique<std::atomic<bool>[]>(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) core->set[i] = false;
    supervisor().add(core);
    return inst;
}

int64_t hostProcessOf(Instance* inst) {
    auto* r = dynamic_cast<RemoteInstance*>(inst);
    return r ? r->hostPid() : 0;
}

bool bypassed(Instance* inst) {
    auto* r = dynamic_cast<RemoteInstance*>(inst);
    return r && r->isBypassed();
}

std::vector<HostFailure> hostFailures() {
    std::map<std::string, int> bypassedNow;
    for (const auto& c : supervisor().live())
        if (c->state.load() != 0) ++bypassedNow[c->d.id];
    std::vector<HostFailure> out;
    {
        std::lock_guard lock(book().m);
        for (auto [id, f] : book().byId) {
            f.bypassedNow = std::max(bypassedNow[id], book().exhausted.count(id) || book().held.count(id) ? 1 : 0);
            f.disabled = book().held.count(id) > 0;
            out.push_back(std::move(f));
        }
    }
    std::sort(out.begin(), out.end(), [](const HostFailure& a, const HostFailure& b) { return a.lastMs > b.lastMs; });
    return out;
}

uint64_t hostFailureSerial() { return book().serial.load(); }
uint64_t offlineFailureSerial() { return book().offlineSerial.load(); }

std::string lastOfflineFailure() {
    std::lock_guard lock(book().m);
    return book().lastOffline;
}

bool restartPlugin(const std::string& id) {
    bool any = false, wasHeld = false;
    {
        std::lock_guard lock(book().m);
        wasHeld = book().held.erase(id) > 0;
        any = book().exhausted.erase(id) > 0 || wasHeld;
        book().autoUsed[id] = 0;
    }
    // (disabled from the failure bar: Restart is the user taking it back, so it is offered for new effects again)
    if (wasHeld) Registry::instance().setPluginDisabled(id, false);
    for (const auto& c : supervisor().live())
        if (c->d.id == id && c->state.load() == 1) {
            c->restartAsked = true;
            any = true;
        }
    ++book().serial;
    supervisor().wake();
    return any;
}

void disablePluginAfterFailure(const std::string& id) {
    {
        std::lock_guard lock(book().m);
        book().held.insert(id);
    }
    Registry::instance().setPluginDisabled(id, true);
    ++book().serial;
}

void clearHostFailures() {
    {
        std::lock_guard lock(book().m);
        book().byId.clear();
        book().held.clear();
        book().exhausted.clear();
        book().autoUsed.clear();
        book().lastOffline.clear();
    }
    ++book().serial;
}

std::vector<HostInfo> runningHosts() {
    std::vector<HostInfo> out;
    std::lock_guard lock(gPoolM);
    for (const auto& [key, w] : pool())
        if (auto h = w.lock(); h && h->alive()) out.push_back({key, h->proc ? h->proc->pid() : 0, h->instances.load()});
    return out;
}

}  // namespace montage::plugins
