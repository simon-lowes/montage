// Montage — remote grading: a client's Montage, or a second colourist's, with its own copy of the media, follows the
// colourist's work live and talks back. Where Remote Review (render/RemoteReview.h) sends the picture, this sends the
// work: the project as it changes (every edit the colourist makes, the grade's effects and nodes, keyframes, colour
// groups, as patches to the project file's JSON, a few hundred bytes for a grade change), the playhead and whether it
// plays, and the viewer's state (its exposure check, the wipe and its reference picture, the scope shown). The client
// renders every frame itself from its own media, at full quality on its own calibrated display or reference output;
// where it lacks a clip's media it shows Remote Review's stream instead (needsStream). A hash of the project every two
// seconds catches drift, and the client then asks for the whole project again.
//
// Two SRT connections, both encrypted (AES-256) with the session's passphrase, which SRT never sends: the work on the
// colourist's port, from the colourist to the client, and the client's notes and pointers on the next port back (the
// notes land as markers, as Remote Review's do). The colourist's Montage listens, the client calls; one client at a
// time, who can join again after a dropped connection and is brought up to date.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/Model.h"
#include "render/RemoteReview.h"

namespace montage::remote {

struct GradeSettings {
    // The colourist's: where to listen ("srt://0.0.0.0:9100"); the client's: the colourist's computer ("srt://host:9100").
    // The notes come back on the next port.
    std::string address = "srt://0.0.0.0:9100";
    std::string passphrase;  // 10 to 79 characters
    int latencyMs = 120;
    std::string name;  // the client's, shown with its notes and pointers
};
// Why the settings cannot be used ("" when they can).
std::string checkGradeSettings(const GradeSettings& s);
// Whether this Montage's FFmpeg has SRT (and why not).
bool gradingAvailable(std::string* why = nullptr);

// What the colourist is looking at.
struct ViewState {
    Id sequence = 0;
    FrameTime playhead = 0;
    bool playing = false;
    double speed = 1;   // while playing (shuttle speeds; negative backwards)
    std::string viewer;  // the app's viewer state as JSON text: exposure check, wipe, scope ("" none)
    std::string compare;  // the wipe's reference picture (PNG or JPEG bytes; "" none)
};
// Where the playhead is `ageMs` after a view arrived: the frame it showed, moved on at its speed while playing.
FrameTime followPlayhead(const ViewState& v, Rational fps, int64_t ageMs);

// A point the client shows the colourist on the picture: x and y from 0 to 1 across it.
struct Pointer {
    Id sequence = 0;
    FrameTime frame = 0;
    double x = 0.5, y = 0.5;
    std::string author;
};

// The colourist's end.
class GradeHost {
public:
    GradeHost();
    ~GradeHost();
    GradeHost(const GradeHost&) = delete;
    GradeHost& operator=(const GradeHost&) = delete;

    bool start(const GradeSettings& settings, std::string* error = nullptr);
    void stop();
    bool running() const;
    // The project as it is now; sent as a patch to the last one sent (changes come in a burst while a control is
    // dragged; the latest is sent, ten times a second at most).
    void publish(const Project& p);
    void setView(const ViewState& v);
    std::vector<output::RemoteMarker> takeNotes();
    std::vector<Pointer> takePointers();

    struct Status {
        bool connected = false;  // a client follows
        std::string client;      // its name
        uint64_t revision = 0;   // projects published
        int64_t snapshots = 0, patches = 0, views = 0, bytes = 0;  // sent since started
        int64_t resyncs = 0;     // whole projects sent again because a client found drift
        int notes = 0, pointers = 0;
        int port = 0, notesPort = 0;
        std::string error;
    };
    Status status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// The client's end.
class GradeClient {
public:
    GradeClient();
    ~GradeClient();
    GradeClient(const GradeClient&) = delete;
    GradeClient& operator=(const GradeClient&) = delete;

    bool start(const GradeSettings& settings, std::string* error = nullptr);
    void stop();
    bool running() const;
    // The colourist's project, when it has changed since the last call (its revision in `revision`).
    bool takeProject(Project& out, uint64_t* revision = nullptr, std::string* error = nullptr);
    // The colourist's view, and how long ago it came (ms); false before the first.
    bool view(ViewState& out, int64_t* ageMs = nullptr) const;
    void sendNote(const output::RemoteMarker& m);
    void sendPointer(const Pointer& p);

    struct Status {
        bool connected = false;
        uint64_t revision = 0;  // the project's, as last received
        int64_t snapshots = 0, patches = 0, checks = 0, mismatches = 0;
        std::string error;
    };
    Status status() const;
    // (Testing: changes its copy of the project as a lost patch would, so the next check finds drift.)
    void damageForTest();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// The patch that turns one JSON document into another: [{"p": [key or index, ...], "v": value} or {"p": [...],
// "x": 1} (removed)]; arrays that change length are sent whole.
std::string jsonPatch(const std::string& from, const std::string& to);
bool applyJsonPatch(const std::string& doc, const std::string& patch, std::string& out);
// SHA-256 of a JSON document as compact text (keys in order), in hex.
std::string jsonHash(const std::string& json);

// The client's own media in place of the colourist's: each media item it also has (by id) keeps its files.
void keepLocalMedia(const Project& local, Project& received);
// Whether frame `t` of `s` shows a picture from media the client lacks (`missing`), which then comes from the stream.
bool needsStream(const Project& p, const Sequence& s, FrameTime t, const std::function<bool(Id media)>& missing);

}  // namespace montage::remote
