// Montage — the client side of the plug-in host protocol (audio/PluginHost.h), shared by the audio stand-in instance
// (audio/PluginHost.cpp) and OpenFX's (render/OfxRemote.cpp). Internal.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "audio/PluginHost.h"
#include "audio/PluginIpc.h"
#include "audio/PluginWire.h"

namespace montage::plugins::remote {

// One channel's shared block and its two wake-ups, Montage's side.
struct Channel {
    std::unique_ptr<ipc::SharedMemory> mem;
    std::unique_ptr<ipc::Bell> toHost, toClient;
    wire::Header* h = nullptr;
    uint32_t seq = 0;      // the last message's
    uint32_t bumps = 0;    // toHost's value
    uint32_t seen = 0;     // toClient's value last looked at
    uint32_t pending = 0;  // a message whose reply was not waited for to the end (0: none)
    bool create(std::string* error);
    void unlinkNames();
    void* base() const { return mem ? mem->data() : nullptr; }
};

// A montage-pluginhost process.
class Host {
public:
    ~Host();
    std::string key;
    bool gpu = false;  // started with --gpu (OpenFX renders on the GPU): not shared with a host started without
    std::unique_ptr<ipc::Process> proc;
    Channel control;
    std::mutex controlM;
    bool alive();
    // How it ended ("crashed (signal 11)", "used more than 4096 MB"...); "" while it runs.
    std::string howItEnded();
    // What the host said as it crashed (signal or exception code; 0: it did not), told before the process ends.
    uint32_t crashCode() const;
    void kill();
    // Whether Montage stopped it (kill: for a hang, memory, a failed attach), its failure recorded by whoever did.
    bool stoppedByMontage() const { return killed_.load(std::memory_order_acquire); }
    std::atomic<int> instances{0};
    // Set by the first to report this host's end (renders on several threads see it end together; one failure).
    std::atomic<bool> failureTaken{false};
    // The plug-in it was loading when it ended ("" when it was loading none): that one alone is to blame.
    std::string loading() const;

private:
    std::atomic<bool> dead_{false}, killed_{false};
};

// The running host for a key, or a new one (null with why when none can start).
std::shared_ptr<Host> hostFor(const std::string& key, bool gpu, std::string* error);

enum class Result { Ok, Timeout, Dead };
// Sends the message written in `c` (command and arguments) and waits up to `timeoutUs` for its reply. A host's
// question in the middle (OpenFX) goes to `callback`, which writes the answer's fields; the wait then goes on.
// Neither allocates nor locks (without a callback).
Result call(Channel& c, Host& host, int64_t timeoutUs, const std::function<void(Channel&)>* callback = nullptr);
// Whether the reply to a message not waited for to the end has come (then the channel is free again).
bool collectPending(Channel& c);

// Has the host open `chan` and make an instance of `kind` ("audio" or "ofx") from `json` in it.
struct Attached {
    bool ok = false;
    bool hostDied = false;  // it crashed while loading, in a host with nothing else in it (so it was this plug-in)
    bool timedOut = false;  // it did not finish loading (slow, or a busy machine: not a reason to block it)
    uint32_t flags = 0;
    int latency = 0;
    std::string error;
};
// `id` is the plug-in's: a host that ends while loading it (as its control block says) is its doing, whatever else the
// host held; one that ends while loading another plug-in is not.
Attached attach(Host& host, Channel& chan, const std::string& kind, const std::string& json, const std::string& id);

// Puts `data` in the channel's payload, through PutBlob when longer (true when it fits or went).
bool sendPayload(Channel& c, Host& host, const std::string& data, int64_t timeoutUs, Result* r);
// Reads a reply's blob of `size` bytes (the first part already in the payload).
bool receiveBlob(Channel& c, Host& host, int64_t size, std::string& out, int64_t timeoutUs, Result* r);

// The host key for a plug-in under a grouping ("ofx:" in front for OpenFX: video plug-ins have hosts of their own);
// `instance` names the instance for a host of its own (PerInstance).
std::string hostKey(Isolation grouping, const std::string& id, const std::string& maker, bool ofx, const std::string& instance = {});
Isolation currentIsolation();

// Records a failure (any thread but the audio thread).
// (`offlineNoted`: noteOfflineFailure already told the export.)
void recordFailure(const std::string& id, const std::string& name, const std::string& vendor, const std::string& kind,
                   const std::string& reason, bool offline, bool countsAsNew = true, bool offlineNoted = false);
// Tells an export running now that a plug-in failed, at once (the thread rendering it, not the audio thread).
void noteOfflineFailure(const std::string& name, const std::string& why);
void noteRestart(const std::string& id);
bool pluginHeldBypassed(const std::string& id);  // disabled after failing
int autoRestartsLeft(const std::string& id);
void useAutoRestart(const std::string& id);
// (OpenFX, which has no stand-in to hold) failed past its restarts: bypassed until Restart. `id` is the plug-in's, or
// "<id>#<effect id>" for an instance in a host of its own (Restart of the plug-in brings those back too).
void markExhausted(const std::string& id);
bool exhausted(const std::string& id);

constexpr int64_t kControlTimeoutUs = 10'000'000;
constexpr int64_t kLoadTimeoutUs = 30'000'000;

}  // namespace montage::plugins::remote
