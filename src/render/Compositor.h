// Montage — renders a sequence frame (video) and mixes sequence audio.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"

namespace montage {

struct RenderOptions {
    double scale = 1.0;        // output size relative to the sequence (preview resolution)
    bool highQuality = false;  // bicubic scaling for export
    bool useProxies = false;
    int depth = 0;             // nesting depth (internal)
    bool captions = false;     // draw the visible caption track over the program
};

// Composites all video tracks of `seq` at timeline frame `t`.
// The result is (seq.width*scale) x (seq.height*scale), transparent where
// nothing is on screen; call flattenOver() to put it over black.
Image renderSequenceFrame(const Project& p, const Sequence& seq, FrameTime t, const RenderOptions& o);

// Same, flattened over black — what the program monitor and exports show.
Image renderProgramFrame(const Project& p, const Sequence& seq, FrameTime t, const RenderOptions& o);

// Maps between a clip's own frame (u, v as fractions of its media frame, as
// masks use) and the sequence frame (pixels) at timeline frame t, following
// the clip's transform. False if the clip has no picture.
bool clipFrameToSequence(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, double u, double v,
                         double& x, double& y);
bool sequenceToClipFrame(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, double x, double y,
                         double& u, double& v);
// The size of the clip's own frame in its media's pixels.
bool clipFrameSize(const Project& p, const Sequence& seq, const Clip& c, double& w, double& h);

// Draws the caption on screen at frame t from `track` over `img` (the whole frame).
void drawCaption(Image& img, const CaptionTrack& track, FrameTime t);

// Raw (unprocessed) frame of a media item at `seconds`, fitted into w x h; for the source monitor.
Image renderMediaFrame(const Project& p, const MediaItem& m, double seconds, int w, int h);

// Renders a title / colour / gradient generator at the given size.
Image renderGenerator(const Effect& g, FrameTime t, int w, int h, double scale);

// ---------------------------------------------------------------------------
// Audio

struct MeterLevels {
    float peakL = 0, peakR = 0;
};

class AudioMixer {
public:
    AudioMixer();
    ~AudioMixer();
    // Mixes `frames` stereo frames starting at sample `start` (at seq.sampleRate)
    // into `out` (interleaved, overwritten). Per-track peak levels go to `trackLevels`.
    void mix(const Project& p, const Sequence& seq, int64_t start, int frames, float* out,
             std::vector<MeterLevels>* trackLevels = nullptr);
    // Drop filter state (call after seeking).
    void reset();
    // Real-time mode: media whose audio is not decoded yet plays as silence
    // instead of blocking until decoding finishes.
    void setNonBlocking(bool on) { nonBlocking_ = on; }

    struct State;  // per clip/effect DSP state
    static bool ensurePlugin(State& st, const Effect& e, double sr);

private:
    // `rate` overrides the sequence's sample rate (nested sequences mix at the outer rate).
    void mixInto(const Project& p, const Sequence& seq, int64_t start, int frames, float* out,
                 std::vector<MeterLevels>* trackLevels, int depth, int rate = 0);
    // Runs an effect chain over an interleaved stereo block; DSP state is kept per (owner, effect).
    void processChain(const std::vector<Effect>& chain, Id owner, FrameTime lt, double sr, float* buf, int frames);
    // Sums a track's clips over [start, start + frames) into trackBuf; false if none plays.
    bool mixTrackClips(const Project& p, const Sequence& seq, const Track& track, int64_t start, int frames, double sr,
                       int depth, float* trackBuf);
    // Latency (samples) of a chain's plugins, loading them if needed; and the largest in a sequence.
    int chainLatency(const std::vector<Effect>& chain, Id owner, double sr);
    int maxLatency(const Sequence& seq, double sr);
    void resetLocked();
    int64_t nextStart_ = -1;  // where the next contiguous block starts
    std::map<std::pair<Id, Id>, std::unique_ptr<State>> states_;
    std::mutex m_;
    bool nonBlocking_ = false;
};

}  // namespace montage
