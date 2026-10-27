// Montage — hosting ARA 2 plug-ins (Audio Random Access, Celemony's open API: third_party/ara), which read a clip's
// whole sound themselves rather than being fed it block by block: pitch and timing editors, analysers and the like.
//
// A clip effect "ara" (audio/PluginEffect.h makes one for a plug-in the scanner found ARA in, over VST3 or CLAP)
// opens an ARA document for the clip's source: a musical context (the effect's tempo and bar, 4/4 at 120 bpm unless
// set), a region sequence (the track), an audio source (the clip's media sound, read sample-exact from any thread at
// the rate Montage plays it), an audio modification and a playback region over the whole source, playing at the
// source's own time. The plug-in renders it as a playback renderer and the result replaces the source, as the other
// whole-source effects do (audio/SpeechCleanup.h): cached, and the same in playback, export, Render and Replace and on
// the render farm. The clip's trim, slip and speed then apply to what the plug-in made.
//
// The document's state (what was analysed, edits made in the plug-in's editor) is kept with the project as an ARA
// archive in the effect ("archive"), restored each time the document is opened. Content the plug-in found (notes,
// tempo, bar signatures, chords) is read back through content readers.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Plugins.h"
#include "core/Model.h"
#include "media/Decoder.h"

namespace montage::ara {

struct Note {
    double start = 0, duration = 0;  // seconds in the source
    double frequency = 0;            // Hz (0: unknown)
    int pitch = -1;                  // MIDI note number (-1: unknown)
    float volume = 0;                // 0..1
};
struct TempoEntry {
    double time = 0;     // seconds
    double quarter = 0;  // quarter notes from the start
};
struct BarSignature {
    int numerator = 4, denominator = 4;
    double position = 0;  // quarter notes
};
struct Chord {
    double position = 0, duration = 0;  // quarter notes
    int root = -1, bass = -1;           // circle-of-fifths index from C (ARA's), -1: none
    std::string name;
};
struct Content {
    std::vector<Note> notes;
    std::vector<TempoEntry> tempo;
    std::vector<BarSignature> bars;
    std::vector<Chord> chords;
    bool analysed = false;  // the plug-in reported its analysis of the source complete
};

// The musical context Montage describes to the plug-in: a steady tempo and bar.
struct Timeline {
    double bpm = 120;
    int beatsPerBar = 4;
};
Timeline timelineOf(const Effect& e);

enum Role : uint32_t { kPlaybackRenderer = 1, kEditorRenderer = 2, kEditorView = 4, kAllRoles = 7 };

// One ARA document for one source, with one plug-in instance bound to it.
class Session {
public:
    // `archive`: a stored document (Session::archive(); empty: a new one). `sourceId` and `modificationId` name the
    // audio source and modification persistently (the archive refers to them).
    static std::unique_ptr<Session> open(const plugins::Descriptor& d, const std::string& archive, AudioBufferPtr source,
                                         const std::string& sourceId, const std::string& modificationId,
                                         const Timeline& timeline, uint32_t roles, std::string* error = nullptr);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    plugins::Instance* instance();
    const plugins::Descriptor& descriptor() const;
    // Renders the whole source through the plug-in (as a playback renderer): the result is as long as the source, at
    // its rate, with its channels. False when the plug-in was not bound as a renderer or it was cancelled.
    bool render(AudioBuffer& out, const std::atomic<bool>* cancel = nullptr);
    // Asks the plug-in to analyse the source for what it can find, waits for it (up to `timeoutMs`), and reads back
    // what it found.
    bool analyse(Content& content, int timeoutMs = 60000, const std::atomic<bool>* cancel = nullptr);
    // What the plug-in has found so far, without asking for an analysis.
    Content content();
    // The document's state, to store with the project.
    std::string archive();
    // Changes the plug-in reported to the document (edits in its editor) since the last call.
    bool changed();
    // Reported again at the next changed() (its archive could not be stored).
    void stillChanged();
    // Main thread housekeeping while an editor is open (model updates).
    void idle();

private:
    struct Impl;
    explicit Session(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> d_;
};

// What the effect's archive calls its audio modification (kept in the effect, so copies and moved projects keep it)
// and its audio source: the modification's, with the sound's length, rate and channels, so the same sound is the
// same source wherever its file is (relinked, a proxy, another computer) and other sound is not.
std::string modificationIdFor(const Effect& e);
std::string sourceIdFor(const Effect& e, const AudioBuffer& source);

// The whole-source effect (audio/SpeechCleanup.cpp): `source` through the plug-in the effect names (its state and
// parameters), its document restored from the effect's archive, rendered offline. False (with `error`) when the
// plug-in is missing or fails.
bool renderEffect(const Effect& e, const AudioBufferPtr& source, AudioBuffer& out, const std::atomic<bool>* cancel = nullptr,
                  std::string* error = nullptr);

// The clip's media sound as the mixer plays it to its source effects: MediaPool's key and the decoded buffer at the
// sequence's rate (false with `error` when the clip has no media sound). With `upTo`, the sound as it reaches that
// effect: through the clip's source effects before it (worked out now), as rendering gives it the sound, so the plug-in
// edits what it renders, under the same source id; the key then names them too.
bool clipSource(const Project& p, const Sequence& seq, const Clip& c, std::string& key, AudioBufferPtr& buffer,
                std::string* error = nullptr, const Effect* upTo = nullptr);

// Sessions kept open for effects (the editor): one per effect id, bound as editor view and editor renderer with its
// playback region selected. `sourceKey` and `source` name and hold the clip's media sound (MediaPool audio key and
// buffer). A session is reused while the effect's archive is the one it was opened from or last saved
// (noteArchiveSaved).
std::shared_ptr<Session> liveSession(const Effect& e, const std::string& sourceKey, const AudioBufferPtr& source,
                                     std::string* error = nullptr);
std::shared_ptr<Session> findLiveSession(Id effect);
// The session's document was stored in its effect as `archive` (base64, as the effect keeps it).
void noteArchiveSaved(Id effect, const std::string& archive);
// Closes the sessions of effects no longer in the project (a clip deleted) or whose archive is no longer the
// session's (an undo, a reset), ending their playback regions.
void dropSessionsNotIn(const Project& p);
void dropAllSessions();

// Diagnostics (tests): playback regions alive in all documents; whole sources rendered by plug-ins so far; and, while logging, what the plug-ins read from each
// audio source (by source id): the source as they saw it (interleaved, NaN where nothing was read) and how many reads.
int liveRegions();
int rendersDone();
struct ReadLog {
    std::vector<float> seen;
    int channels = 0;
    int reads = 0;
};
void logReads(bool on);
std::vector<std::pair<std::string, ReadLog>> readsLogged();

}  // namespace montage::ara
