// Montage — remote rendering (Resolve's Remote Rendering, Media Encoder's watch folders and Deadline's job folders): a
// render farm is a folder every computer can reach (a network share). The app's Render Queue and `montage-cli farm
// submit` put jobs in it, each the project as it was with the export's settings; `montage-cli node <farm>` on any
// computer with Montage takes the waiting jobs one at a time and renders them, writing where the job says (through its
// own path mapping, for media mounted elsewhere), and anyone can follow, cancel, retry or remove them.
//
// The folder: jobs/<id>/job.json (name, sequence, settings, stems, who sent it) and project.json (the project, its
// paths absolute); a node claims a job by creating jobs/<id>/claim (only one creation can succeed) and writes
// status.json (state, progress, a heartbeat count) as it renders; a cancel file asks it to stop. nodes/<name>.json is
// each node's heartbeat. Nothing relies on the computers' clocks agreeing: a node that stops answering is noticed by
// its heartbeat count not changing for a minute of the watcher's own time, and its job is taken back for another.
// (Jobs are taken oldest first by the sending computer's clock, which only orders them.)
#pragma once

#include <QJsonObject>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/Model.h"
#include "render/Exporter.h"

namespace montage {

// An export's settings as JSON and back (every field; what is missing keeps its default). False with `error` when a
// value has the wrong type.
QJsonObject exportSettingsToJson(const ExportSettings& s);
bool exportSettingsFromJson(const QJsonObject& o, ExportSettings& s, std::string* error = nullptr);

// One render as the queue and the farm's nodes make it: the export, then its stems (StemGroups, 0 none) in the second
// half of the progress. `keptUnsigned` as exportSequence's (the export kept though it failed, its stems not made).
bool renderExport(const Project& p, Id sequence, const ExportSettings& s, int stems, const ExportProgress& progress,
                  const std::atomic<bool>* cancel, std::string* error, std::string* encoder = nullptr,
                  bool* keptUnsigned = nullptr);

// Path mapping: a path starting with `from` (a folder; "\" and "/" alike, letters' case kept) is given `to` in its
// place, its separators made `to`'s. The first map that fits is used.
struct FarmPathMap {
    std::string from, to;
};
std::string mapFarmPath(const std::string& path, const std::vector<FarmPathMap>& maps);
// "from=to" (the last "=" splits, so a Windows path's own are kept); false when it is not that.
bool parseFarmPathMap(const std::string& text, FarmPathMap& map);

enum class FarmState { Waiting, Rendering, Done, Failed, Cancelled };
const char* farmStateName(FarmState s);  // "waiting", "rendering", ...

struct FarmJob {
    std::string id;  // sortable by when it was sent: yyyyMMdd-HHmmsszzz-xxxxxx
    std::string name, output, submittedBy, submitted;  // submitted: ISO 8601, the sender's clock
    FarmState state = FarmState::Waiting;
    double progress = 0;
    std::string node;   // who renders or rendered it
    std::string error;  // a failure's
    std::string encoder;
    std::string started, finished;  // ISO 8601, the node's clock
    int64_t beat = 0;   // the node's heartbeat count while rendering
    bool cancelRequested = false;  // asked to stop, not stopped yet
    std::string claim;  // who holds it (a node's token for this run), while claimed
};
struct FarmNodeInfo {
    std::string name, host, state;  // state: idle, rendering, stopped
    std::string job;                // being rendered
    std::string updated;            // ISO 8601, the node's clock
    int64_t beat = 0;
};

// The farm's folders made when missing; false with `error` when it cannot be used.
bool openFarm(const std::string& farm, std::string* error = nullptr);
// Sends a job: `project` as it is now, its sequence `sequence` exported with `settings` (its path where the nodes
// write, before their mapping). `id` gets the job's.
bool submitFarmJob(const std::string& farm, const std::string& name, const Project& project, Id sequence, const ExportSettings& settings,
                   int stems, std::string* id = nullptr, std::string* error = nullptr);
std::vector<FarmJob> farmJobs(const std::string& farm);  // oldest first
std::vector<FarmNodeInfo> farmNodes(const std::string& farm);
// A waiting job is cancelled at once; a rendering one is asked to stop (its node looks every half second; a share's
// caching can delay its seeing the request).
bool cancelFarmJob(const std::string& farm, const std::string& id, std::string* error = nullptr);
// A failed or cancelled job (or one a stopped node left) waits again.
bool retryFarmJob(const std::string& farm, const std::string& id, std::string* error = nullptr);
// A job that is not rendering, with its copy of the project; the output stays.
bool removeFarmJob(const std::string& farm, const std::string& id, std::string* error = nullptr);

// Notices heartbeats that stop: `stale` is true once the same beat has been seen for `ms` of this computer's time.
class FarmHeartbeats {
public:
    explicit FarmHeartbeats(int64_t ms = 60000) : ms_(ms) {}
    bool stale(const std::string& key, int64_t beat);
    void forget(const std::string& key) { seen_.erase(key); }

private:
    struct Seen {
        int64_t beat = -1;
        int64_t since = 0;
    };
    int64_t ms_;
    std::map<std::string, Seen> seen_;
};

// A job taken back from a node that stopped answering, to wait for another: only while `claim` (FarmJob::claim, as
// seen when it was judged silent) still holds it, and only one watcher's taking succeeds.
bool reclaimFarmJob(const std::string& farm, const std::string& id, const std::string& claim, std::string* error = nullptr);

struct FarmNodeOptions {
    std::string farm;
    std::string name;  // "" = this computer's name
    std::vector<FarmPathMap> maps;
    bool once = false;      // ends when no job is waiting
    int pollMs = 2000;      // between looks for work
    int64_t staleMs = 60000;  // a node silent this long has its job taken back
    std::function<bool()> stopped;  // asked often: true ends the node (its job, half done, waits for another)
    std::function<void(const std::string&)> log;
};
// Runs a render node until stopped (or, `once`, out of work): the oldest waiting job claimed, its project read with
// the maps applied to every path in it and to the output, rendered with status and heartbeats every half second, then
// done, failed or cancelled. The render goes to a folder of the claim's own beside the output and is moved into place
// only while the node still holds the job. False with `error` only when the farm cannot be used. `rendered` counts the
// jobs it ended (done, failed or cancelled).
bool runFarmNode(const FarmNodeOptions& options, int* rendered = nullptr, std::string* error = nullptr);

}  // namespace montage
