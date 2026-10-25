// Montage — the monitoring section (a mixing room's monitor controller): what playback sends to the
// speakers and to talent's headphones, after the mix and never in an export or bounce. A speaker set maps the
// speakers of a layout (or headphones) to the output device's channels, each with a trim (0.1 dB steps) and a delay in
// samples for speakers at different distances; bass management sends the mains' lows and the LFE to the subwoofer
// through fourth-order Linkwitz-Riley crossovers; Listen As hears the programme as a smaller layout (mixed in that
// layout, as an export to it is), as stereo (the stereo fold-down exports use) or binaurally (panned into a
// third-order field and heard through the SOFA head, core/Ambisonics.h); Dim, Mono, Mute and one speaker alone;
// monitor solos (in place, after the fader, before it) that leave the project's own solos alone; a reference level
// set from band-limited pink noise and a sound level meter's reading; and a cue mix for the talent (chosen tracks,
// the ADR beeps, the programme and a talkback microphone) on other channels of the device.
#pragma once

#include <QJsonObject>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/Adr.h"
#include "core/Model.h"
#include "core/Surround.h"
#include "render/AudioFx.h"
#include "render/Compositor.h"

namespace montage {

struct MonitorSpeaker {
    int output = -1;    // the device channel it is on (from 0); -1 not connected
    double trimDb = 0;  // in 0.1 dB steps
    int delay = 0;      // samples, 0 to kMaxMonitorDelay
    bool operator==(const MonitorSpeaker&) const = default;
};
constexpr int kMaxMonitorDelay = 9600;  // 200 ms: some 68 m of air
constexpr int kMaxMonitorChannels = 64;  // device channels a setting may use

enum class MonitorSolo { Project, InPlace, AfterFader, PreFader };

struct CueMix {
    bool on = false;
    int output = 2;          // its left channel on the device (the right is the next); added to speakers on the same
    double levelDb = 0;
    std::vector<int> tracks;  // audio tracks (index) heard in it, as they are mixed
    bool beeps = true;        // an ADR cycle's beeps
    bool program = false;     // the programme as the speakers hear it (before Dim and Mute), at programDb
    double programDb = -10;
    double talkbackDb = 0;
    bool operator==(const CueMix&) const = default;
};

struct MonitorSettings {
    bool on = false;  // off: playback as before (the stereo mix on the default output's first two channels)
    std::string speakers = "stereo";  // a speaker layout of core/Surround.h, or "headphones"
    std::vector<MonitorSpeaker> map;  // per speaker of the set; missing ones on channel = their index
    bool bassManagement = false;
    double crossoverHz = 80;
    std::string listenAs;  // "" the speakers' own layout (the programme's when it fits); a smaller layout; "stereo";
                           // "binaural" (headphones)
    bool dim = false;
    double dimDb = -20;
    bool mono = false;
    bool mute = false;
    int soloSpeaker = -1;  // one speaker of the set alone
    double levelDb = 0;    // the monitor level, as calibration sets it
    double referenceSpl = 85;  // dB SPL that calibration noise (-20 dBFS RMS) is set to: 79, 82 or 85
    MonitorSolo soloMode = MonitorSolo::Project;  // what the mixer's solo buttons do
    std::vector<int> soloTracks;                  // the monitor's solos (audio track indices), not the project's
    CueMix cue;
    bool operator==(const MonitorSettings&) const = default;

    QJsonObject toJson() const;
    static MonitorSettings fromJson(const QJsonObject& o);
};

// The speakers of a set ("headphones" is two, L and R) and the device channels a setting needs (the speakers' and
// the cue's).
const std::vector<Speaker>& monitorSpeakers(const std::string& set);
int monitorOutputs(const MonitorSettings& s);
MonitorSpeaker monitorSpeaker(const MonitorSettings& s, int speaker);  // its mapping (the default for a missing one)
// The first device channel after the speakers' (where a cue mix goes unless set).
int monitorFreeOutput(const MonitorSettings& s);
// What the programme is mixed as for these speakers: a speaker layout, "stereo" or "binaural".
std::string listenLayout(const MonitorSettings& s, const Sequence& seq);
// The layouts Listen As offers for a set (beside "" the set's own).
std::vector<std::string> listenChoices(const std::string& set);
// The sequence as the monitor mixes it: in listenLayout ("ambix3" for binaural) with the monitor's solos.
Sequence monitorSequence(const Sequence& seq, const MonitorSettings& s);

// Calibration noise: pink noise band-limited to 500 Hz - 2 kHz (SMPTE RP 200's band; fourth-order Linkwitz-Riley
// edges) at -20 dBFS RMS, ten seconds of it, looped seamlessly; the same every time. A subwoofer's is from 20 Hz to
// the crossover.
std::vector<float> calibrationNoise(int sampleRate, double lowHz = 500, double highHz = 2000);
// The noise for speaker `speaker` of a setting (a subwoofer's from 20 Hz to the crossover); empty for none.
std::vector<float> calibrationNoiseFor(const MonitorSettings& s, int sampleRate, int speaker);
// The monitor level that makes calibration noise read `referenceSpl` where it read `measuredSpl` at `currentDb`.
inline double calibratedLevel(double currentDb, double measuredSpl, double referenceSpl) { return currentDb + referenceSpl - measuredSpl; }

// After the mix: places the programme (in its listen layout) on the speakers, bass manages, trims, delays, dims and
// maps them to the device's channels, and makes the cue mix.
class MonitorSection {
public:
    void configure(const MonitorSettings& s, int sampleRate);  // filters and delays start afresh when the set changes
    const MonitorSettings& settings() const { return s_; }
    int outputs() const { return outputs_; }
    // `in` has layoutChannels(layout) channels ("binaural" and "mono" two); `beeps` (stereo, or null) is added to the
    // front pair. `out` gets outputs() channels, the cue's left empty.
    void process(const std::string& layout, const float* in, const float* beeps, int frames, float* out);
    // The cue mix into its two channels of `out`: `guide` (its tracks' stereo mix), `beeps` and `heard` (the programme's
    // stereo), each may be null, and the talkback microphone while talking.
    void addCue(const float* guide, const float* beeps, const float* heard, int frames, float* out);
    // The talkback microphone's sound (mono, at the section's rate), from any thread; heard while talking.
    void feedTalkback(const float* mono, int frames);
    void setTalkback(bool on);
    bool talkback() const { return talkback_; }
    // Calibration noise on speaker `speaker` instead of the programme (-1: off), through its trim, delay and the
    // monitor level (not bass management, Dim, Mono, Mute or a solo).
    void setCalibration(int speaker);
    // The same with the noise made beforehand (calibrationNoiseFor), so it is not made while the audio thread waits.
    // The same speaker again changes nothing (the noise plays on without a seam).
    void setCalibration(int speaker, std::vector<float> noise);
    int calibration() const { return calibrate_; }
    void reset();

private:
    MonitorSettings s_;
    int rate_ = 48000, outputs_ = 2;
    std::vector<Speaker> speakers_;
    struct Line {
        std::vector<float> buf;  // kMaxMonitorDelay + 1, a ring
        int pos = 0;
        fx::Biquad hp[2], lp[2];  // the crossover (two Butterworth sections each: fourth-order Linkwitz-Riley)
    };
    std::vector<Line> lines_;
    std::vector<float> noise_;  // for the speaker being calibrated
    size_t noisePos_ = 0;
    int calibrate_ = -1;
    std::mutex talkM_;
    std::vector<float> talk_;  // a ring of what the talkback microphone sent
    size_t talkRead_ = 0, talkWrite_ = 0;
    bool talkback_ = false;
    std::vector<float> placed_;
};

// The programme through the section: the mixes the monitor needs (in its listen layout, and the cue's tracks) and
// what the device gets. Prepared for a project and sequence (when either or the settings change), then rendered
// block by block from consecutive samples.
class Monitor {
public:
    Monitor();
    void setSettings(const MonitorSettings& s, int sampleRate);
    const MonitorSettings& settings() const { return section_.settings(); }
    MonitorSection& section() { return section_; }
    void setNonBlocking(bool on);
    void setOfflineProxies(bool on);
    void setAmbisonicBinaural(bool on);  // an ambisonic sequence heard as stereo: binaurally (playback's choice)
    void prepare(std::shared_ptr<const Project> project, Id sequence);
    void reset();
    int outputs() const { return section_.outputs(); }
    // `frames` from sample `start`: `out` gets outputs() channels; `heard` (stereo, or null) the programme as heard
    // (for the meters); `tracks` the track meters; `cycle` an ADR cycle whose beeps play. Without `program` (stopped)
    // only calibration noise and talkback are heard.
    void render(int64_t start, int frames, float* out, float* heard = nullptr, std::vector<MeterLevels>* tracks = nullptr,
                const AdrCycle* cycle = nullptr, bool program = true);

private:
    MonitorSection section_;
    AudioMixer program_, cue_;
    std::shared_ptr<const Project> project_;
    std::optional<Sequence> seq_, cueSeq_;
    std::string layout_;
    bool binaural_ = false;
    std::vector<float> full_, stereo_, guide_, beeps_;
};

}  // namespace montage
