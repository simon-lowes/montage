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
//
// A job sent in segments (render/SegmentedExport.h) is a job for each segment, rendered by whichever nodes are free,
// and a join job that waits for them all: the node that takes it checks each segment (its frames and the hash its node
// wrote), renders the sound once and joins the segments into the output. A segment that fails is tried again (twice)
// on its own, and one that does not check out at the join is rendered again alone. IMF and DCP packages are sent in
// segments the same way, the segments coding pictures and the join wrapping them.
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

struct ImfSettings;
struct DcpSettings;

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
    // A job sent in segments: a segment's join job (`group`) and its number (1 based) of `segments`; the join itself has
    // `join` and the number of its segments.
    std::string group;
    int segment = 0, segments = 0;
    bool join = false;
    // Two passes across segments: 1 = a segment's first pass (measuring), 2 = its second (waiting for every first).
    int pass = 0;
};
struct FarmNodeInfo {
    std::string name, host, state;  // state: idle, rendering, stopped
    std::string job;                // being rendered
    std::string updated;            // ISO 8601, the node's clock
    int64_t beat = 0;
    // The encoder it codes each hardware family with ("h264" -> "h264_nvenc"; "libx264" when it has none for it).
    std::map<std::string, std::string> encoders;
};

// The farm's folders made when missing; false with `error` when it cannot be used.
bool openFarm(const std::string& farm, std::string* error = nullptr);
// Sends a job: `project` as it is now, its sequence `sequence` exported with `settings` (its path where the nodes
// write, before their mapping). `id` gets the job's. With `segments` > 1, split into that many (at cuts near equal
// lengths; fewer for a short range) and a join job, whose id `id` gets; refused when the export cannot be split
// (exportSplits). The segments' files go in a hidden folder beside the output, removed once joined. Two passes
// (`twoPass`) send each segment's first pass as a job of its own; the second passes wait for all of them, and each
// takes its share of the bits by what its first pass measured (twoPassBitrates).
bool submitFarmJob(const std::string& farm, const std::string& name, const Project& project, Id sequence, const ExportSettings& settings,
                   int stems, std::string* id = nullptr, std::string* error = nullptr, int segments = 1);
// A mastering package (render/Imf.h Application #2E, render/Dcp.h) sent in `segments`: each segment's node codes its
// share of the pictures as JPEG 2000 codestreams into a hidden folder beside `parent` (where the package's folder is
// made, before the nodes' mapping), and the join's checks them, makes the sound and everything else and wraps them as
// they are (the picture track file as one node would have made it). `id` gets the join's. Refused for what cannot be
// made in segments (Application #5, supplemental packages, an encrypted DCP without a folder for its DKDM).
bool submitFarmImf(const std::string& farm, const std::string& name, const Project& project, Id sequence, const ImfSettings& settings,
                   const std::string& parent, int segments, std::string* id = nullptr, std::string* error = nullptr);
bool submitFarmDcp(const std::string& farm, const std::string& name, const Project& project, Id sequence, const DcpSettings& settings,
                   const std::string& parent, int segments, std::string* id = nullptr, std::string* error = nullptr);
std::vector<FarmJob> farmJobs(const std::string& farm);  // oldest first
std::vector<FarmNodeInfo> farmNodes(const std::string& farm);
// A waiting job is cancelled at once; a rendering one is asked to stop (its node looks every half second; a share's
// caching can delay its seeing the request). A join job's segments that have not ended are cancelled with it.
bool cancelFarmJob(const std::string& farm, const std::string& id, std::string* error = nullptr);
// A failed or cancelled job (or one a stopped node left) waits again; a join job's failed and cancelled segments too.
bool retryFarmJob(const std::string& farm, const std::string& id, std::string* error = nullptr);
// A job that is not rendering, with its copy of the project; the output stays. A join job's segments go with it (none
// may be rendering), and their files when the folder is reachable from here.
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
    // The encoder this node says it codes each hardware family with ("h264", "hevc"); a family left out is what it
    // finds (render nodes compare them: one export's hardware-encoded segments all go to nodes of the same kind).
    std::map<std::string, std::string> encoders;
};
// Runs a render node until stopped (or, `once`, out of work): the oldest waiting job claimed (a segment coded by a
// hardware encoder only when the first of its export's segments to be taken went to a node with the same encoder), its project read with
// the maps applied to every path in it and to the output, rendered with status and heartbeats every half second, then
// done, failed or cancelled. The render goes to a folder of the claim's own beside the output and is moved into place
// only while the node still holds the job. False with `error` only when the farm cannot be used. `rendered` counts the
// jobs it ended (done, failed or cancelled).
bool runFarmNode(const FarmNodeOptions& options, int* rendered = nullptr, std::string* error = nullptr);

}  // namespace montage
