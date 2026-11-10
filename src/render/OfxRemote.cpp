#include "OfxRemote.h"

#include <algorithm>
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
    ~Link() {
        if (attached && host && host->alive()) {
            chan.h->command = wire::Destroy;
            call(chan, *host, 1'000'000);
        }
        if (attached && host) --host->instances;
    }
    // Room for `bytes` in the arena (a new, larger block when needed; the host maps it at its next look).
    bool room(size_t bytes, std::string* error = nullptr) {
        if (arena && arena->size() >= bytes) return true;
        const size_t size = std::max(bytes, arena ? arena->size() * 2 : size_t(16) << 20);
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

std::unique_ptr<Link> takeLink(const std::string& key, std::string* error) {
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
    auto host = hostFor(key, gpu, error);
    if (!host) return nullptr;
    auto link = std::make_unique<Link>();
    link->host = host;
    if (!link->chan.create(error)) return nullptr;
    const Attached a = attach(*host, link->chan, "ofx", {});
    if (!a.ok) {
        if (error) *error = a.error;
        return nullptr;
    }
    link->attached = true;
    return link;
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

}  // namespace

bool isolated() { return plugins::isolating(); }

void releaseIdle() {
    std::map<std::string, std::vector<std::unique_ptr<Link>>> gone;
    {
        std::lock_guard lock(gLinksM);
        gone.swap(idleLinks());
    }
}

bool render(Kind kind, const Effect& e, const PluginDesc& d, double t, double progress, double scale, int w, int h, const Image* a,
            const Image* b, Image& out, const FrameFetch* fetch, std::string* error, const ColorSpace* working) {
    if (pluginHeldBypassed(d.id) || exhausted(d.id)) {
        if (error) *error = d.label + " is bypassed after failing in its plug-in host (Restart brings it back)";
        return false;
    }
    const std::string key = hostKey(currentIsolation(), d.id, d.bundle.empty() ? d.binary : d.bundle, true);
    std::string err;
    std::unique_ptr<Link> link = takeLink(key, &err);
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
    // (during playback a picture is waited for 5 s at most, not an export's 30: a hung plug-in holds the viewer no
    // longer than that, each time)
    const int64_t timeout = int64_t(renderingRealtime() ? std::min(hosting().exportTimeoutMs, 5000) : hosting().exportTimeoutMs) * 1000;
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
    const bool hung = r == Result::Timeout;
    std::string how = hung ? "did not answer within " + std::to_string(timeout / 1'000'000) + " s and was stopped" : link->host->howItEnded();
    if (how.empty()) how = "stopped answering";
    link->host->kill();
    // (Renders on other threads lost the same host: the first of them counts the failure and uses the restart.)
    if (link->host->failureTaken.exchange(true)) {
        if (error) *error = d.label + " " + how;
        return false;
    }
    const bool restart = autoRestartsLeft(d.id) > 0;
    if (restart) useAutoRestart(d.id);
    else markExhausted(d.id);
    const std::string kindName = hung ? "hang" : how.rfind("used more memory", 0) == 0 ? "memory" : "crash";
    recordFailure(d.id, d.label, std::string(), kindName, how + "; bypassed", !renderingRealtime());
    if (restart) noteRestart(d.id);
    if (error) *error = d.label + " " + how;
    return false;
}

}  // namespace montage::ofx::remote
