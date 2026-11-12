#include "OfxRemote.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "audio/PluginHostInternal.h"
#include "core/ProjectIO.h"
#include "render/ColorSpace.h"

namespace montage::ofx::remote {

using namespace montage::plugins;
using namespace montage::plugins::remote;

namespace {

// A channel to an OpenFX host, with the shared block its pictures go through. One render at a time uses one; renders
// on several threads take several.
struct Link {
    std::shared_ptr<Host> host;
    Channel chan;
    std::unique_ptr<ipc::SharedMemory> arena;
    bool attached = false;
    bool hung = false;  // (a message still out that will not be answered: no Destroy is waited for)
    bool rendered = false;  // (it has given a picture: its host has the plug-in loaded)
    ~Link() {
        if (attached && host && host->alive() && !hung) {
            chan.h->command = wire::Destroy;
            call(chan, *host, 1'000'000);
        }
        if (attached && host) --host->instances;
    }
    // Room for `bytes` in the arena (a new, larger block when needed; the host maps it at its next look). A quarter
    // more than asked, not twice: the block's pages are all reserved when it is made, the old one's until it goes.
    bool room(size_t bytes, std::string* error = nullptr) {
        if (arena && arena->size() >= bytes) return true;
        const size_t ahead = std::max(bytes + bytes / 4, arena ? arena->size() + arena->size() / 4 : size_t(0));
        const size_t size = std::max(ahead, size_t(16) << 20);
        auto next = ipc::SharedMemory::create(ipc::uniqueName("a"), size, error);
        if (!next && size > bytes) next = ipc::SharedMemory::create(ipc::uniqueName("a"), bytes, error);  // (no room to grow ahead)
        if (!next) return false;
        arena = std::move(next);
        std::memset(chan.h->arenaName, 0, sizeof chan.h->arenaName);
        std::memcpy(chan.h->arenaName, arena->name().data(), std::min(arena->name().size(), sizeof chan.h->arenaName - 1));
        chan.h->arenaSize = arena->size();
        ++chan.h->arenaGen;
        return true;
    }
    char* at(size_t off) const { return static_cast<char*>(arena->data()) + off; }
};

std::mutex gLinksM;
std::map<std::string, std::vector<std::unique_ptr<Link>>>& idleLinks() {
    static auto* m = new std::map<std::string, std::vector<std::unique_ptr<Link>>>;
    return *m;
}

// A new channel to the host for `key` (started when none runs), for plug-in `id`.
template <typename L>
std::unique_ptr<L> newLink(const std::string& key, bool gpu, const std::string& id, std::string* error) {
    auto host = hostFor(key, gpu, error);
    if (!host) return nullptr;
    auto link = std::make_unique<L>();
    link->host = host;
    if (!link->chan.create(error)) return nullptr;
    const Attached a = attach(*host, link->chan, "ofx", {}, id);
    if (!a.ok) {
        if (error) *error = a.error;
        return nullptr;
    }
    link->attached = true;
    return link;
}

std::unique_ptr<Link> takeLink(const std::string& key, const std::string& id, std::string* error) {
    const bool gpu = gpuRendering() && gpuAvailable();
    {
        std::lock_guard lock(gLinksM);
        auto& v = idleLinks()[key];
        while (!v.empty()) {
            std::unique_ptr<Link> l = std::move(v.back());
            v.pop_back();
            if (l->host->alive() && l->host->gpu == gpu) return l;
        }
    }
    return newLink<Link>(key, gpu, id, error);
}

void giveBack(const std::string& key, std::unique_ptr<Link> l) {
    if (!l || !l->host->alive()) return;
    std::lock_guard lock(gLinksM);
    auto& v = idleLinks()[key];
    if (v.size() < 8) v.push_back(std::move(l));
}

// The effect as the host gets it: parameters driven by expressions (which read the project, here) as their values at
// the frame rendered.
Effect baked(const Effect& e, double t) {
    Effect out = e;
    for (auto& [key, p] : out.params)
        if (!p.expr.empty()) p = Param(p.atTime(t));
    return out;
}

// The host key for an effect's plug-in under the grouping now (its own host, for one each instance).
std::string keyFor(const Effect& e, const PluginDesc& d) {
    return hostKey(currentIsolation(), d.id, d.bundle.empty() ? d.binary : d.bundle, true, std::to_string(e.id));
}

// How long playback waits for a picture: 5 s for a plug-in not yet seen to render, or on a channel that has not rendered
// yet (a host just started loads it first), then eight times its slowest render seen, scaled to this picture's size,
// at least 1 s: a plug-in that renders in milliseconds and then hangs holds the viewer for a second, not five, each
// time. (An export waits as it is set to.)
std::mutex gSlowestM;
std::map<std::string, double>& slowest() {  // (microseconds a pixel, by plug-in)
    static auto* m = new std::map<std::string, double>;
    return *m;
}
int64_t playbackWaitUs(const std::string& id, int64_t pixels, int64_t limitUs) {
    std::lock_guard lock(gSlowestM);
    const auto it = slowest().find(id);
    if (it == slowest().end()) return limitUs;
    return std::clamp<int64_t>(int64_t(8 * it->second * double(std::max<int64_t>(1, pixels))), 1'000'000, limitUs);
}
void noteRender(const std::string& id, int64_t pixels, int64_t us) {
    std::lock_guard lock(gSlowestM);
    double& s = slowest()[id];
    s = std::max(s, double(us) / double(std::max<int64_t>(1, pixels)));
}

// What is bypassed past its restarts: the plug-in, or with a host for each instance, that instance alone.
std::string bypassKey(const Effect& e, const PluginDesc& d) {
    return currentIsolation() == Isolation::PerInstance ? d.id + "#" + std::to_string(e.id) : d.id;
}
bool bypassedNow(const Effect& e, const PluginDesc& d) { return pluginHeldBypassed(d.id) || exhausted(d.id) || exhausted(bypassKey(e, d)); }

// A host that crashed or did not answer while working for plug-in `d`: ended, the failure recorded once (renders on
// several threads lose a host together) against the plug-in to blame, and a restart used (bypassed past them). What
// happened, in words, after the plug-in's name.
std::string hostFailed(Host& host, const PluginDesc& d, const std::string& bypass, Result r, int64_t timeoutUs) {
    const bool hung = r == Result::Timeout;
    std::string how = hung ? "did not answer within " + std::to_string(std::max<int64_t>(1, timeoutUs / 1'000'000)) + " s and was stopped"
                           : host.howItEnded();
    if (how.empty()) how = "stopped answering";
    // (the plug-in the host was loading when it ended, as it said: that one alone is blamed and blocked)
    const std::string loading = hung ? std::string() : host.loading();
    host.kill();
    PluginDesc blamed = d;
    const bool other = !loading.empty() && !host.wasLoading(d.id);
    if (other && !ofx::Registry::instance().find(loading, blamed)) blamed = PluginDesc{}, blamed.id = loading, blamed.label = loading;
    const std::string words = other ? "was stopped with its plug-in host, which " + how + " while loading " + blamed.label : how;
    if (host.failureTaken.exchange(true)) return words;
    if (!loading.empty()) {
        if (!blamed.binary.empty()) ofx::Registry::instance().blockAfterCrash(blamed.binary, "Crashed its plug-in host while loading");
        recordFailure(blamed.id, blamed.label, std::string(), "load", how + " while loading it; blocked", !renderingRealtime());
        return words;
    }
    const bool restart = autoRestartsLeft(d.id) > 0;
    if (restart) useAutoRestart(d.id);
    else markExhausted(bypass);
    const std::string kindName = hung ? "hang" : how.rfind("used more memory", 0) == 0 ? "memory" : "crash";
    recordFailure(d.id, d.label, std::string(), kindName, how + "; bypassed", !renderingRealtime());
    if (restart) noteRestart(d.id);
    return words;
}

}  // namespace

bool isolated() { return plugins::isolating(); }

void releaseIdle() {
    drainClosedControls();
    std::map<std::string, std::vector<std::unique_ptr<Link>>> gone;
    {
        std::lock_guard lock(gLinksM);
        gone.swap(idleLinks());
    }
}

bool render(Kind kind, const Effect& e, const PluginDesc& d, double t, double progress, double scale, int w, int h, const Image* a,
            const Image* b, Image& out, const FrameFetch* fetch, std::string* error, const ColorSpace* working) {
    if (bypassedNow(e, d)) {
        if (error) *error = d.label + " is bypassed after failing in its plug-in host (Restart brings it back)";
        return false;
    }
    const std::string key = keyFor(e, d);
    std::string err;
    std::unique_ptr<Link> link = takeLink(key, d.id, &err);
    if (!link) {
        if (error) *error = d.label + " could not start in a plug-in host: " + err;
        return false;
    }
    Channel& c = link->chan;
    wire::Header* hd = c.h;
    // The pictures in, the mask after them, then room for the picture back and for answers.
    const size_t slot = size_t(w) * size_t(h) * 16;
    const int images = kind == Kind::Transition ? 2 : kind == Kind::Effect ? 1 : 0;
    const std::vector<float>* mask = currentOwnMask();
    const bool withMask = mask && mask->size() == size_t(w) * size_t(h) && kind == Kind::Effect;
    const size_t maskBytes = withMask ? size_t(w) * size_t(h) * sizeof(float) : 0;
    const size_t outOff = size_t(images) * slot + maskBytes;
    const size_t answerOff = outOff + slot;
    if (std::string why; !link->room(answerOff + slot, &why)) {
        if (error) *error = "no memory to share the pictures with the plug-in host: " + why;
        giveBack(key, std::move(link));
        return false;
    }
    if (a) std::memcpy(link->at(0), a->px.data(), slot);
    if (b) std::memcpy(link->at(slot), b->px.data(), slot);
    if (withMask) std::memcpy(link->at(size_t(images) * slot), mask->data(), maskBytes);
    const std::string msg = effectToJsonString(baked(e, t)) + std::string(1, '\0') + descriptionsToJson({d}) + std::string(1, '\0') +
                            (working ? working->id : std::string());
    // (during playback a picture is waited for 5 s at most, not an export's 30, and once the plug-in has rendered, a
    // few times its slowest: a hung plug-in holds the viewer no longer than that, each time)
    const int64_t exportUs = int64_t(hosting().exportTimeoutMs) * 1000;
    const int64_t pixels = int64_t(w) * int64_t(h);
    const int64_t playbackUs = std::min<int64_t>(exportUs, 5'000'000);
    const int64_t timeout = !renderingRealtime() ? exportUs : link->rendered ? playbackWaitUs(d.id, pixels, playbackUs) : playbackUs;
    const int64_t began = ipc::nowUs();
    Result r = Result::Ok;
    if (sendPayload(c, *link->host, msg, timeout, &r)) {
        const InputResolver& inputs = currentInputs();
        uint32_t flags = hd->flags & wire::kPayloadInBlob;
        if (fetch && *fetch) flags |= wire::kHasFetch;
        if (inputs) flags |= wire::kHasInputs;
        if (withMask) flags |= wire::kHasMask;
        hd->flags = flags;
        hd->argI = int64_t(kind);
        hd->argI2 = w;
        hd->argI3 = h;
        hd->argD = t;
        hd->argD2 = progress;
        hd->argD3 = scale;
        hd->argU = 0;
        hd->paramCount = 0;
        hd->command = wire::OfxRender;
        // The host's questions while it renders: a frame at another time, or what an input shows.
        const std::function<void(Channel&)> answer = [&](Channel& ch) {
            const int32_t what = int32_t(ch.h->resultI);
            const double at = ch.h->resultD;
            const std::string connection(wire::payload(ch.base()), size_t(std::min<uint64_t>(ch.h->payloadBytes, wire::kPayload)));
            Image pic;
            bool ok = false;
            if (what == wire::FetchFrame && fetch && *fetch) ok = (*fetch)(at, pic);
            else if (what == wire::ResolveInput && inputs) ok = inputs(connection, at, pic);
            const size_t bytes = size_t(std::max(0, pic.width)) * size_t(std::max(0, pic.height)) * 16;
            ok = ok && !pic.empty() && link->room(answerOff + bytes);
            if (ok) std::memcpy(link->at(answerOff), pic.px.data(), bytes);
            ch.h->status = ok ? 1 : 0;
            ch.h->argI = int64_t(answerOff);
            ch.h->argI2 = pic.width;
            ch.h->argI3 = pic.height;
        };
        r = call(c, *link->host, timeout, &answer);
    }
    if (r == Result::Ok) {
        hd->flags = 0;
        link->arena->unlinkName();  // (the host has it mapped now)
        if (hd->status) {
            noteRender(d.id, pixels, ipc::nowUs() - began);
            link->rendered = true;
        }
        if (!hd->status) {
            if (error) *error = std::string(wire::payload(c.base()), size_t(std::min<uint64_t>(hd->payloadBytes, wire::kPayload)));
            giveBack(key, std::move(link));
            return false;
        }
        // (the size the host gives is not trusted: negative or past 64K a side is refused before anything is made)
        const int64_t rw = hd->resultI, rh = hd->resultI2;
        const bool sane = rw >= 0 && rh >= 0 && rw <= 65536 && rh <= 65536;
        const int ow = sane ? int(rw) : 0, oh = sane ? int(rh) : 0;
        const size_t bytes = size_t(ow) * size_t(oh) * 16;
        if (!sane || outOff + bytes > link->arena->size()) {
            if (error) *error = d.label + " gave back a picture of the wrong size";
            return false;
        }
        out = Image(ow, oh, Image::Uninitialized{});
        if (bytes) std::memcpy(out.px.data(), link->at(outOff), bytes);
        giveBack(key, std::move(link));
        return true;
    }
    // It crashed or did not answer: bypassed this time, named, and started again for the next render (a few times).
    const std::string how = hostFailed(*link->host, d, bypassKey(e, d), r, timeout);
    if (error) *error = d.label + " " + how;
    return false;
}

// ---- On-screen controls ----

struct Controls::Link : remote::Link {
    // (left by controls closed while a reply was still out: drainClosedControls)
    int64_t sentUs = 0;
    PluginDesc desc;
    std::string bypass;
};

namespace {
constexpr int64_t kDeadlineUs = int64_t(Controls::kDeadlineMs) * 1000;
constexpr int64_t kHangUs = int64_t(Controls::kHangMs) * 1000;
constexpr uint32_t kPartOut = 0xffffffffu;  // (pending: a part of a long message; the command itself never went)
constexpr int kUnchanged = 100;              // (send: a draw not sent, its picture the last one)
int devicePixels(int size, double ratio) { return std::clamp(int(std::lround(size * ratio)), 1, 16384); }

// Channels of controls closed while a reply was still out. Each is let go once the reply comes (the host's controls
// then closed as usual), or its host is ended as hung once the reply has not come for kHangMs.
std::mutex gClosedM;
std::vector<std::unique_ptr<Controls::Link>>& closedControls() {
    static auto* v = new std::vector<std::unique_ptr<Controls::Link>>;
    return *v;
}
}  // namespace

bool drainClosedControls() {
    std::vector<std::unique_ptr<Controls::Link>> done;
    bool left = false;
    {
        std::lock_guard lock(gClosedM);
        auto& v = closedControls();
        for (auto it = v.begin(); it != v.end();) {
            Controls::Link& l = **it;
            const bool ended = l.chan.h->crashed.load(std::memory_order_acquire) || !l.host->alive();
            if (!ended && !collectPending(l.chan)) {
                if (ipc::nowUs() - l.sentUs <= kHangUs) {
                    ++it;
                    continue;
                }
                hostFailed(*l.host, l.desc, l.bypass, Result::Timeout, kHangUs);
            }
            l.hung = ended || l.chan.pending != 0;  // (answered: its controls are closed in the host as usual)
            done.push_back(std::move(*it));
            it = v.erase(it);
        }
        left = !v.empty();
    }
    // (let go here, outside the lock: one that answered waits for its Destroy)
    return left;
}

std::unique_ptr<Controls> Controls::open(const Effect& e, const PluginDesc& d, std::string* error) {
    drainClosedControls();
    if (d.overlay == 0) return nullptr;
    if (bypassedNow(e, d)) {
        if (error) *error = d.label + " is bypassed after failing in its plug-in host (Restart brings it back)";
        return nullptr;
    }
    std::string err;
    // (the host the effect renders in: its controls are one more channel to it)
    auto link = newLink<Link>(keyFor(e, d), gpuRendering() && gpuAvailable(), d.id, &err);
    if (!link || !link->room(16 << 20, &err)) {
        if (error) *error = d.label + "'s on-screen controls could not start in its plug-in host: " + err;
        return nullptr;
    }
    std::unique_ptr<Controls> c(new Controls);
    c->link_ = std::move(link);
    c->desc_ = d;
    c->bypass_ = bypassKey(e, d);
    const int r = c->send(wire::OfxOverlayOpen, e, 0, View{}, {}, {}, kHangUs);
    if (r != int(Result::Ok) || !c->link_->chan.h->status) {
        if (error) {
            if (r != int(Result::Ok)) *error = d.label + "'s on-screen controls " + c->failure_;
            else *error = std::string(wire::payload(c->link_->chan.base()), size_t(std::min<uint64_t>(c->link_->chan.h->payloadBytes, wire::kPayload)));
            if (error->empty()) *error = d.label + "'s on-screen controls could not start";
        }
        return nullptr;
    }
    return c;
}

Controls::~Controls() {
    if (!link_ || failed_ || !pendingCmd_) return;
    // (a reply still out: the channel is kept until it comes, so the host's controls are closed then)
    link_->sentUs = sentUs_;
    link_->desc = desc_;
    link_->bypass = bypass_;
    std::lock_guard lock(gClosedM);
    closedControls().push_back(std::move(link_));
}

bool Controls::waiting() const { return pendingCmd_ != 0 && !failed_; }
bool Controls::unresponsive() const { return waiting() && ipc::nowUs() - sentUs_ >= kDeadlineUs; }

int Controls::send(uint32_t cmd, const Effect& e, double t, const View& v, const std::function<void(void*)>& args, const std::string& text,
                   int64_t timeoutUs) {
    if (failed_) return int(Result::Dead);
    Channel& c = link_->chan;
    Host& host = *link_->host;
    wire::OverlayArgs a{};
    a.width = v.width, a.height = v.height;
    for (int k = 0; k < 6; ++k) a.m[k] = v.m[k];
    a.renderScale = v.renderScale, a.frameW = v.frameW, a.frameH = v.frameH;
    a.pixelRatio = v.pixelRatio > 0 ? v.pixelRatio : 1;
    a.time = t;
    if (args) args(&a);
    const Effect baking = baked(e, t);
    std::string msg(reinterpret_cast<const char*>(&a), sizeof a);
    msg += effectToJsonString(baking);
    msg += '\0';
    if (cmd == wire::OfxOverlayOpen) msg += descriptionsToJson({desc_});  // (the host keeps it from then on)
    msg += '\0';
    msg += text;
    // (controls drawn without waiting are not asked again for the picture they gave last: nothing has changed)
    if (cmd == wire::OfxOverlayDraw && slowDraws_ && msg == drawnMsg_) return kUnchanged;
    sent_ = baking;
    if (cmd == wire::OfxOverlayDraw) {
        // Room for the picture back, at the device's pixels.
        std::string why;
        const size_t bytes = size_t(devicePixels(v.width, a.pixelRatio)) * size_t(devicePixels(v.height, a.pixelRatio)) * 4;
        if (!link_->room(bytes, &why)) {
            failed_ = true;
            failure_ = "could not share their picture: " + why;
            return int(Result::Dead);
        }
    }
    Result r = Result::Ok;
    const int64_t wait = timeoutUs > 0 ? timeoutUs : kDeadlineUs;
    const int64_t began = ipc::nowUs();
    bool sent = false;
    if (sendPayload(c, host, msg, wait, &r)) {
        c.h->command = cmd;
        c.h->paramCount = 0;
        sent = true;
        // (controls slow to draw are not waited for: the picture is taken at a later paint, the last one shown)
        r = call(c, host, cmd == wire::OfxOverlayDraw && slowDraws_ ? 1 : wait);
    } else if (r == Result::Ok) {
        c.h->resultFlags = 0;  // (the host refused a part of the message: not taken, status 0)
        return int(r);
    }
    if (r == Result::Timeout) {
        // (its last picture stays up; the reply is taken when it comes, or the host is ended as hung)
        pendingCmd_ = sent ? cmd : kPartOut;
        pendingRatio_ = a.pixelRatio;
        if (pendingCmd_ == wire::OfxOverlayDraw) pendingDraw_ = msg;
        sentUs_ = began;
        if (sent && cmd == wire::OfxOverlayDraw) slowDraws_ = true;
        if (timeoutUs >= kHangUs) fail(int(r));
        return int(r);
    }
    if (r == Result::Dead) {
        fail(int(r));
        return int(r);
    }
    link_->arena->unlinkName();  // (the host has it mapped now)
    if (cmd == wire::OfxOverlayDraw) drawnMsg_ = std::move(msg);
    if (c.h->resultFlags & wire::kControlFailed) {
        failed_ = true;
        failure_ = "stopped working";
    }
    return int(r);
}

void Controls::fail(int result) {
    failed_ = true;
    const std::string how = hostFailed(*link_->host, desc_, bypass_, Result(result), kHangUs);
    failure_ = how;
    link_->hung = true;
}

bool Controls::settle(int64_t waitUs) {
    if (failed_) return false;
    if (!pendingCmd_) return true;
    Channel& c = link_->chan;
    const int64_t end = ipc::nowUs() + waitUs;
    while (!collectPending(c)) {
        if (c.h->crashed.load(std::memory_order_acquire) || !link_->host->alive()) {
            fail(int(Result::Dead));
            return false;
        }
        const int64_t now = ipc::nowUs();
        if (now - sentUs_ > kHangUs) {
            fail(int(Result::Timeout));
            return false;
        }
        if (now >= end) return false;
        c.toClient->wait(&c.h->toClient, c.h->toClient.load(std::memory_order_acquire), std::min<int64_t>(end - now, 2000));
    }
    const uint32_t cmd = std::exchange(pendingCmd_, 0u);
    if (cmd == kPartOut) return true;
    link_->arena->unlinkName();
    if (c.h->resultFlags & wire::kControlFailed) {
        failed_ = true;
        failure_ = "stopped working";
        return false;
    }
    if (cmd == wire::OfxOverlayDraw) {
        if (!c.h->status || !takeImage(pendingRatio_)) last_ = QImage();
        drawnMsg_ = std::move(pendingDraw_);
        // (drawn in time again, as the host timed it: draws are waited for again)
        if (c.h->resultD >= 0 && c.h->resultD * 1e6 < double(kDeadlineUs) / 2) slowDraws_ = false;
    } else if (cmd == wire::OfxOverlayPen || cmd == wire::OfxOverlayKey) {
        // What the control set in a late reply, kept for the next event (or lateEdits) to hand on.
        Effect set;
        bool changed = false;
        if (readEdited(set, changed) && changed) {
            for (auto& [key, p] : set.params) late_.params[key] = p;
            for (auto& [key, v] : set.strings) late_.strings[key] = v;
            lateEdits_ = true;
        }
        noteBlobOut();
    }
    return !failed_;
}

void Controls::noteBlobOut() {
    // (a part of a long answer still out: waited for as any message is, and the host ended if it never comes)
    if (failed_ || pendingCmd_ || !link_->chan.pending) return;
    pendingCmd_ = kPartOut;
    sentUs_ = ipc::nowUs();
}

bool Controls::takeLate(Effect& into) {
    if (!lateEdits_) return false;
    for (auto& [key, p] : late_.params) into.params[key] = p;
    for (auto& [key, v] : late_.strings) into.strings[key] = v;
    late_ = Effect{};
    lateEdits_ = false;
    return true;
}

bool Controls::lateEdits(Effect& changes) {
    settle();
    changes = Effect{};
    return takeLate(changes);
}

bool Controls::takeImage(double ratio) {
    const wire::Header* h = link_->chan.h;
    // (what the host says is not trusted: a size outside the arena is refused)
    const int64_t w = h->resultI, hh = h->resultI2;
    if (w == 0 && hh == 0) {  // (the control drew nothing)
        last_ = QImage();
        return true;
    }
    if (w <= 0 || hh <= 0 || w > 16384 || hh > 16384 || size_t(w) * size_t(hh) * 4 > link_->arena->size()) return false;
    QImage img(int(w), int(hh), QImage::Format_RGBA8888_Premultiplied);
    if (img.isNull()) return false;
    const auto* src = static_cast<const uchar*>(link_->arena->data());
    for (int y = 0; y < int(hh); ++y) std::memcpy(img.scanLine(y), src + size_t(y) * size_t(w) * 4, size_t(w) * 4);
    img.setDevicePixelRatio(ratio);
    last_ = std::move(img);
    return true;
}

bool Controls::draw(const Effect& e, double t, const View& v, Overlay& out) {
    drainClosedControls();
    out = Overlay{};
    if (settle()) {
        const int r = send(wire::OfxOverlayDraw, e, t, v, {}, {});
        if (r == int(Result::Ok) && (!link_->chan.h->status || !takeImage(v.pixelRatio > 0 ? v.pixelRatio : 1))) last_ = QImage();
    }
    if (!failed_) out.gl = last_;
    return !failed_;
}

bool Controls::readEdited(Effect& edited, bool& changed) {
    Channel& c = link_->chan;
    if (!(c.h->resultFlags & wire::kEdited)) return true;
    std::string json;
    Result r = Result::Ok;
    if (!receiveBlob(c, *link_->host, c.h->resultI, json, kDeadlineUs, &r)) {
        if (r == Result::Dead) fail(int(r));
        return false;
    }
    Effect back;
    if (!effectFromJsonString(json, back)) return false;
    // Only what the control set is taken: a parameter driven by an expression went as its value at the frame, and
    // keeps its expression unless the control changed it. (Both sides compared as JSON gives them back.)
    Effect sent;
    if (!effectFromJsonString(effectToJsonString(sent_), sent)) return false;
    for (const auto& [key, p] : back.params) {
        const auto it = sent.params.find(key);
        if (it == sent.params.end() || !(it->second == p)) {
            edited.params[key] = p;
            changed = true;
        }
    }
    for (const auto& [key, v] : back.strings) {
        const auto it = sent.strings.find(key);
        if (it == sent.strings.end() || it->second != v) {
            edited.strings[key] = v;
            changed = true;
        }
    }
    return true;
}

bool Controls::pen(int kind, const Effect& e, double t, const View& v, double vx, double vy, double pressure, Effect& edited, bool& changed) {
    edited = e;
    changed = false;
    if (!settle(kDeadlineUs)) return false;  // (not responding: the pen is not sent)
    changed = takeLate(edited);  // (what a late reply set goes first, and with the effect the control is sent)
    const int r = send(wire::OfxOverlayPen, edited, t, v, [&](void* p) {
        auto* a = static_cast<wire::OverlayArgs*>(p);
        a->pen = kind;
        a->x = vx, a->y = vy, a->pressure = pressure;
    });
    if (r != int(Result::Ok)) return false;
    const bool took = link_->chan.h->status == 1;
    bool more = false;
    readEdited(edited, more);
    noteBlobOut();
    changed = changed || more;
    return took;
}

bool Controls::key(bool down, int keysym, const std::string& text, const Effect& e, double t, const View& v, Effect& edited, bool& changed) {
    edited = e;
    changed = false;
    if (!settle(kDeadlineUs)) return false;
    changed = takeLate(edited);
    const int r = send(wire::OfxOverlayKey, edited, t, v, [&](void* p) {
        auto* a = static_cast<wire::OverlayArgs*>(p);
        a->keysym = keysym;
        a->down = down ? 1 : 0;
    }, text);
    if (r != int(Result::Ok)) return false;
    const bool took = link_->chan.h->status == 1;
    bool more = false;
    readEdited(edited, more);
    noteBlobOut();
    changed = changed || more;
    return took;
}

void Controls::focus(bool gained, const Effect& e, double t, const View& v) {
    if (!settle()) return;
    send(wire::OfxOverlayFocus, e, t, v, [&](void* p) { static_cast<wire::OverlayArgs*>(p)->down = gained ? 1 : 0; });
}

}  // namespace montage::ofx::remote
