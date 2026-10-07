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
};

// Composites all video tracks of `seq` at timeline frame `t`.
// The result is (seq.width*scale) x (seq.height*scale), transparent where
// nothing is on screen; call flattenOver() to put it over black.
Image renderSequenceFrame(const Project& p, const Sequence& seq, FrameTime t, const RenderOptions& o);

// Same, flattened over black — what the program monitor and exports show.
Image renderProgramFrame(const Project& p, const Sequence& seq, FrameTime t, const RenderOptions& o);

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

    struct State;  // per clip/effect DSP state

private:
    void mixInto(const Project& p, const Sequence& seq, int64_t start, int frames, float* out,
                 std::vector<MeterLevels>* trackLevels, int depth);
    std::map<std::pair<Id, Id>, std::unique_ptr<State>> states_;
    std::mutex m_;
};

}  // namespace montage
