// Montage — renders a sequence frame (video) and mixes sequence audio.
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Stereo.h"
#include "core/Ambisonics.h"
#include "core/Model.h"
#include "media/Image.h"

class QImage;

namespace montage {

// What kept a frame from being rendered in full: an effect that takes seconds a frame left out of a view that cannot wait,
// or one whose model is missing or failed. Such a frame is not final: the render cache does not keep it, and an export
// fails with the reason.
struct RenderIssues {
    std::atomic<bool> incomplete{false};
    void note(const std::string& why) {
        std::lock_guard lock(m_);
        if (!incomplete.exchange(true)) reason_ = why;
    }
    std::string reason() const {
        std::lock_guard lock(m_);
        return reason_;
    }

private:
    mutable std::mutex m_;
    std::string reason_;
};

struct RenderOptions {
    double scale = 1.0;        // output size relative to the sequence (preview resolution)
    bool highQuality = false;  // bicubic scaling for export
    bool useProxies = false;
    bool offlineProxies = false;  // an offline file plays its proxy (viewing; deliveries show it offline)
    int depth = 0;             // nesting depth (internal)
    bool captions = false;     // draw the visible caption track over the program
    // A trim pass (render/TrimPasses.h, the id of one of the sequence's targets): program frames as that display shows
    // them, in its space (displaySpace converts from there); "" = the master.
    std::string trimTarget;
    std::string displaySpace;  // program frames converted to this space (ColorSpace.h id) for viewing; "" = as is
    int soloVideoTrack = -1;   // render only this video track (a multicam angle); not passed to nested sequences
    int eye = 0;               // stereoscopic 3D: the eye rendered (0 left, 1 right)
    // How renderProgramFrame shows a stereoscopic sequence's two eyes (render/Stereo.h); flat sequences ignore it.
    StereoView stereoView = StereoView::Left;
    // Effects that take seconds a frame (Motion Deblur) run in deliveries (highQuality) and when this is set (rendering
    // previews ahead); otherwise a result already made for the frame is used, else the frame goes without them.
    bool slowEffects = false;
    // Program frames drawn on the GPU (render/GpuCompositor.h) where it can draw them, the CPU otherwise.
    bool gpu = false;
    RenderIssues* issues = nullptr;               // told when a frame is not rendered in full
    const std::atomic<bool>* cancel = nullptr;    // stops a slow effect part way (the frame is then not final)
};

// Composites all video tracks of `seq` at timeline frame `t`.
// The result is (seq.width*scale) x (seq.height*scale), transparent where
// nothing is on screen; call flattenOver() to put it over black.
Image renderSequenceFrame(const Project& p, const Sequence& seq, FrameTime t, const RenderOptions& o);

// The clip's effects that take part, in order: its own that are on and outside its grade graph or in a node of it that
// is on and feeds the output, then the effects of the shared nodes it shows there.
std::vector<const Effect*> liveEffects(const Sequence& seq, const Clip& c);

// The video tracks a Track Matte Key hides at t (they are its mattes, not seen themselves).
std::vector<bool> hiddenMatteTracks(const Sequence& seq, FrameTime t);

// Same, flattened over black — what the program monitor and exports show.
Image renderProgramFrame(const Project& p, const Sequence& seq, FrameTime t, const RenderOptions& o);
// Same as 8-bit RGBA (as toRgba8 makes it), for showing: from the GPU without a float frame in between when o.gpu.
QImage renderProgramFrameRgba8(const Project& p, const Sequence& seq, FrameTime t, const RenderOptions& o);

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
// Captions are styled in SDR; `space` converts them into the picture's colour space.
struct ColorSpace;
void drawCaption(Image& img, const CaptionTrack& track, FrameTime t, const ColorSpace* space = nullptr);

// Burn-ins (timecode, clip name, text, watermark) drawn over frame t of `seq`
// in `img` (the whole frame, in colour space `space`; null = Rec.709).
// `watermark` is the loaded logo (or null). The timecode shown is frame `timecode` at `seq`'s rate (t when negative),
// drop-frame at 29.97 and 59.94 unless `dropFrame` is false.
struct BurnIn;
void drawBurnIns(Image& img, const Project& p, const Sequence& seq, FrameTime t, const BurnIn& b, const QImage* watermark,
                 const ColorSpace* space = nullptr, FrameTime timecode = -1, bool dropFrame = true);

// Raw (unprocessed) frame of a media item at `seconds`, fitted into w x h; for the source monitor.
Image renderMediaFrame(const Project& p, const MediaItem& m, double seconds, int w, int h);

// Shot matching. The picture at timeline frame t as Match Colour compares it
// (Rec.709, at most 480 px wide), to use as the reference:
Image colourReferenceFrame(const Project& p, const Sequence& s, FrameTime t);
// Grades picture clips to `reference`: each one's own media frame at timeline
// frame `at` (its middle frame when `at` is outside it) is matched into a
// Color Correct put first in its effects, replacing an earlier match. Returns
// how many clips were matched.
int matchClipColour(Project& p, Sequence& s, const std::vector<Id>& clips, const Image& reference, FrameTime at);
// Film scans, read from the clip's own picture at `at` (or its first frame): Steady on Perforations' reference (where
// the perforations are), and Film Negative's base (set by hand from the frame's rebate). False with `error` when the
// frame has none.
bool takePerforationReference(const Project& p, const Sequence& s, const Clip& c, Effect& e, FrameTime at, std::string* error = nullptr);
bool takeFilmBase(const Project& p, const Sequence& s, const Clip& c, Effect& e, FrameTime at, std::string* error = nullptr);
// Keys a green or blue screen out of a clip: its Keyer (added first if it has none) set to the screen's colour, read
// from the clip's picture at `at` (or its middle). False with `error` if no screen colour stands out.
bool keyScreen(Project& p, Sequence& s, Id clip, FrameTime at, std::string* error = nullptr);

// Renders a title / colour / gradient generator at the given size.
// A generator's picture at clip-local frame t; titles animate in and out over
// the clip's `duration` (frames at `fps`).
Image renderGenerator(const Effect& g, FrameTime t, int w, int h, double scale, FrameTime duration = 0, double fps = 30);

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
    // The same in the sequence's own layout (core/Surround.h): `out` gets
    // layoutChannels(seq.audioLayout) interleaved channels. mix() gives the
    // stereo fold-down of a surround mix, for listening.
    void mixLayout(const Project& p, const Sequence& seq, int64_t start, int frames, float* out);
    // Mixes only the audio tracks whose entry is true (stems); empty = all.
    void setTrackMask(std::vector<bool> mask) { mask_ = std::move(mask); }
    // Audio tracks heard only through their LFE send (ADM objects: their position is the object's, their LFE send
    // stays in the bed); empty = none. Only in surround mixes.
    void setLfeOnly(std::vector<bool> tracks) { lfeOnly_ = std::move(tracks); }
    // Drop filter state (call after seeking).
    void reset();
    // Real-time mode: media whose audio is not decoded yet plays as silence
    // instead of blocking until decoding finishes.
    void setNonBlocking(bool on) {
        nonBlocking_ = on;
        if (keyMixer_) keyMixer_->setNonBlocking(on);
    }
    // An offline file's proxy plays its sound, when it has sound (playback; deliveries leave it silent).
    void setOfflineProxies(bool on) {
        offlineProxies_ = on;
        if (keyMixer_) keyMixer_->setOfflineProxies(on);
    }
    // How mix() lets an ambisonic sequence be heard: binaurally for headphones, or through two virtual cardioids for
    // speakers (the default, as downmixToStereo folds it: exports, stems; playback sets the monitoring choice).
    void setAmbisonicBinaural(bool on) { ambisonicBinaural_ = on; }

    struct State;  // per clip/effect DSP state
    static bool ensurePlugin(State& st, const Effect& e, double sr);

private:
    // `rate` overrides the sequence's sample rate (nested sequences mix at the outer rate);
    // `onlyTrack` mixes that audio track alone (a multicam clip's audio angle).
    // `channels` is 2, or the sequence layout's count for a surround mix (depth 0 only; an ambisonic sequence nested in
    // an ambisonic mix is mixed as its field).
    void mixInto(const Project& p, const Sequence& seq, int64_t start, int frames, float* out,
                 std::vector<MeterLevels>* trackLevels, int depth, int rate = 0, int onlyTrack = -1, int channels = 2);
    // Runs an effect chain over an interleaved stereo block; DSP state is kept per (owner, effect).
    void processChain(const std::vector<Effect>& chain, Id owner, FrameTime lt, double sr, float* buf, int frames);
    // Sums a track's clips over [start, start + frames) into trackBuf; false if none plays. With `trackField` (a
    // surround mix, `fieldChannels` wide), ambisonic clips (and nested ambisonic sequences) add their turned field
    // there (an ambisonic mix's channels, or decoded to a speaker layout's) over [fieldStart, fieldStart + frames)
    // instead of being heard as stereo: the field passes no stereo inserts, so it is read where the mix after them is
    // (the master's latency ahead) rather than as far ahead as the track's sound.
    bool mixTrackClips(const Project& p, const Sequence& seq, const Track& track, int64_t start, int frames, double sr,
                       int depth, float* trackBuf, float* trackField = nullptr, int64_t fieldStart = 0, int fieldChannels = 0);
    // Latency (samples) of a chain's plugins, loading them if needed; and the largest in a sequence.
    int chainLatency(const std::vector<Effect>& chain, Id owner, double sr);
    int maxLatency(const Sequence& seq, double sr);
    void resetLocked();
    // Sidechain keys (a compressor or gate listening to another audio track): that track's signal over the block being
    // processed, before its fader and whether or not it is muted, from a mixer of its own (so the state of its clips'
    // effects never runs twice); computed once per track and block (fed ahead by its own inserts' latency), so a
    // consumer fed ahead by plugin latency downstream hears it that much early. Null when there is no such track.
    // `at` is the timeline sample the consumer's input is at (the chain's input, less the latency of the plugins before
    // it in the chain), so a key lines up with what it is keying.
    const float* keySignal(Id track, int frames, int64_t at);
    struct KeyContext {
        const Project* p = nullptr;
        const Sequence* seq = nullptr;
        int64_t at = 0;  // the timeline sample the chain being run starts at
        double sr = 0;
        int depth = 0;
    };
    KeyContext key_;
    std::unique_ptr<AudioMixer> keyMixer_;
    struct KeyBuffer {
        int64_t block = -1;
        std::vector<float> samples;
    };
    std::map<std::pair<Id, int64_t>, KeyBuffer> keyBufs_;  // by track and where it is heard from, this block
    int64_t keyBlock_ = 0;  // counts the top-level blocks mixed
    int64_t nextStart_ = -1;  // where the next contiguous block starts
    std::map<std::pair<Id, Id>, std::unique_ptr<State>> states_;
    std::mutex m_;
    bool nonBlocking_ = false;
    bool offlineProxies_ = false;
    std::vector<bool> mask_, lfeOnly_;
    bool ambisonicBinaural_ = false;
    struct Binaural {
        std::unique_ptr<AmbisonicBinaural> renderer;
        std::shared_ptr<const Hrirs> hrirs;  // what it was made with (null: the spherical head)
        uint64_t generation = 0;  // of the HRTF chosen (media/Sofa.h) when it was last checked
    };
    // The renderer in `slot`, made for the order, rate and HRTF chosen now.
    AmbisonicBinaural& binauralFor(Binaural& slot, int order, double sr);
    std::map<Id, Binaural> binaural_;  // per ambisonic clip heard binaurally
    Binaural monitor_;                  // an ambisonic sequence heard binaurally
};

}  // namespace montage
