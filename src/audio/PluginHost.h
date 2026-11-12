// Montage — plug-ins in host processes of their own, so a plug-in that crashes, hangs or runs away with memory is
// bypassed instead of ending the session.
//
// Audio plug-ins (CLAP, VST3, LV2, Audio Units) and OpenFX video plug-ins run in montage-pluginhost processes:
// instantiate() (audio/Plugins.h) gives a stand-in Instance whose calls travel to the host, and ofx::applyEffect and
// its kin send their pictures there. Audio goes one block at a time through shared memory (audio/PluginIpc.h), with no
// allocation or lock on the audio thread; parameter changes are queued and go with the next block, and the
// sidechain, transport, latency, state, offline mode and channel requests travel beside it. Output is the same, bit
// for bit, as the plug-in gives in Montage's own process.
//
// How plug-ins are grouped into hosts is a setting: each plug-in in a host of its own (the default: the instances of
// one plug-in share it, and a fault in one plug-in reaches no other), each instance in a host of its own, one host for
// each maker, all plug-ins in one host, or none (in Montage's own process). ARA plug-ins always run in Montage's
// process, as ARA shares the document. A host notes which plug-in it is loading, so one that crashes while loading in
// a shared host is blocked alone, and the others in that host are started again as if nothing of theirs failed.
//
// When a host fails — it crashes, it does not answer for a set number of audio blocks in a row during playback, or it
// uses more memory than the limit — every instance in it is bypassed (its input passes, delayed by its latency, or
// silence: a setting), the failure is recorded for the bar in the main window and for MCP (which plug-in, when, how
// often), and the host is started again with each instance's last saved state and parameters (up to a set number of
// times a session; then it stays bypassed until Restart). Playback carries on throughout. An export instead waits for
// a slow plug-in (up to a time limit) and stops, naming it, when one fails. A plug-in that crashes its host while
// loading goes on the scanner's blocklist, as one that crashes the scan does.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "audio/Plugins.h"

namespace montage::plugins {

// (PerInstance: each instance in a host of its own, so a fault in one reaches not even another instance of the same
// plug-in; the most memory.)
enum class Isolation { PerPlugin, PerVendor, Shared, InProcess, PerInstance };
const char* isolationName(Isolation i);  // "plugin", "vendor", "shared", "off", "instance"
std::optional<Isolation> isolationFromName(std::string_view name);

struct HostingSettings {
    Isolation isolation = Isolation::PerPlugin;
    // Audio blocks in a row a host may miss its deadline for during playback before it counts as hung (each missed
    // block is bypassed meanwhile). A block's deadline is twice its length, at least 10 ms.
    int missedBlocks = 16;
    bool bypassWithSilence = false;  // a bypassed plug-in gives silence instead of its input
    int exportTimeoutMs = 30000;     // how long an export (or any offline render) waits for one block or picture
    int memoryLimitMb = 4096;        // a host using more than this is stopped (0: no limit)
    int autoRestarts = 2;            // restarts a session for each plug-in before it stays bypassed until Restart
    bool operator==(const HostingSettings&) const = default;
};
void setHosting(const HostingSettings& s);
HostingSettings hosting();
// Sets them and has them saved by what onHostingChanged gave (the app keeps them in its settings).
void setHostingAndSave(const HostingSettings& s);
void onHostingChanged(std::function<void(const HostingSettings&)> save);

// The host program (default: montage-pluginhost beside the running program, or MONTAGE_PLUGIN_HOST). Without it,
// plug-ins run in Montage's own process.
void setHostExecutable(const std::string& path);
std::string hostExecutable();
bool hostAvailable();
// Set in montage-pluginhost itself: everything it loads runs in its own process.
void setRunningInHost(bool on);
bool runningInHost();
// Whether plug-ins made now go to a host (the setting, a host program to run, and not inside a host).
bool isolating();

// Whether the audio processed on this thread is live playback (a block a host cannot give back in time is then
// bypassed) or an offline render, the default (it waits). The mixer sets it.
class RenderModeScope {
public:
    explicit RenderModeScope(bool realtime);
    ~RenderModeScope();
    RenderModeScope(const RenderModeScope&) = delete;
    RenderModeScope& operator=(const RenderModeScope&) = delete;

private:
    bool previous_;
};
bool renderingRealtime();

// An export marks its thread with this for as long as it renders. A plug-in that fails in its host, or is found
// bypassed, by an offline render on a thread so marked stops that export (offlineFailureSerial() goes up). Met by any
// other offline render, such as a background analysis with a mixer of its own, it is recorded for the main window's
// bar and nothing more: an export running beside it carries on, as its own mixer has lost nothing.
class ExportScope {
public:
    ExportScope();
    ~ExportScope();
    ExportScope(const ExportScope&) = delete;
    ExportScope& operator=(const ExportScope&) = delete;

private:
    bool previous_;
};
bool renderingForExport();

// An audio plug-in in a host process of the given grouping, whatever the setting (null with why when it cannot start).
std::unique_ptr<Instance> instantiateIsolated(const Descriptor& d, Isolation grouping, std::string* error = nullptr);
// The host process an instance runs in (0: Montage's own process).
int64_t hostProcessOf(Instance* inst);
// Whether an instance is bypassed after its host failed.
bool bypassed(Instance* inst);

// ---- Failures ----
struct HostFailure {
    std::string pluginId, name, vendor;
    std::string kind;    // "crash", "hang", "memory" or "load" (it crashed its host while loading: blocked)
    std::string reason;  // in words
    int64_t firstMs = 0, lastMs = 0;  // since the epoch
    int count = 0;       // failures this session
    int restarts = 0;    // hosts started again for it
    int bypassedNow = 0; // its instances bypassed at the moment
    bool offline = false;  // the last failure happened in an export
    bool disabled = false; // turned off after failing
};
std::vector<HostFailure> hostFailures();
// Goes up with each failure, restart or change of the list (the main window's bar watches it).
uint64_t hostFailureSerial();
// Goes up with each failure during an export's render (ExportScope; an OpenFX plug-in's during any offline render);
// lastOfflineFailure() says what it was.
uint64_t offlineFailureSerial();
std::string lastOfflineFailure();
// Starts the plug-in's bypassed instances again, in fresh hosts, with their last saved states (false: none bypassed).
bool restartPlugin(const std::string& id);
// Keeps the plug-in's instances bypassed and turns it off in the plug-in list (it stays in projects, bypassed).
void disablePluginAfterFailure(const std::string& id);
void clearHostFailures();

struct HostInfo {
    std::string key;  // what it hosts: "plugin:<id>", "vendor:<maker>", "all"
    int64_t pid = 0;
    int instances = 0;
};
std::vector<HostInfo> runningHosts();

}  // namespace montage::plugins
