// Montage — audio devices for professional work (Monitoring › Audio device): JACK (through PipeWire's JACK library on
// Linux, or a system libjack, either loaded when Montage runs, never linked), and through PortAudio ALSA, Core Audio,
// WASAPI shared and exclusive (and ASIO, should a build ever include it). Qt Multimedia stays the default, and the
// fallback when a device cannot be opened.
//
// One duplex stream at a time, kept by the Engine: playback (each PlaybackController) adds outputs to it, recordings
// (Record Voiceover, ADR, talkback) add inputs. Each client works at its own sample rate (the sequence's); where the
// device runs at another, its sound is resampled both ways. Every input block comes with the timeline sample it
// belongs at: the sample the clock's playback was sending out in that block, less the round trip (the device's
// reported input and output latencies, or a loopback measured where the user ran one), so a recording lands where
// what was heard was played. Xruns (the device's own reports, or a callback too late) are counted.
#pragma once

#include <QJsonObject>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace montage::audiodev {

enum class Host { Qt, Jack, Alsa, CoreAudio, Wasapi, WasapiExclusive, Asio, Loopback };

const char* hostId(Host h);  // "qt", "jack", "alsa", "coreaudio", "wasapi", "wasapi_exclusive", "asio", "loopback"
std::optional<Host> hostFromId(const std::string& id);
// Whether this build has the host at all (JACK everywhere but where no library can be found; the others with
// PortAudio on their own platforms; ASIO in no build yet; the loopback, a test device, always).
bool hostBuilt(Host h);
// The hosts that can be opened here now, Qt first (the loopback left out: it is for tests).
std::vector<Host> availableHosts();

struct DeviceInfo {
    std::string id;    // what DeviceSettings::output and input name
    std::string name;  // as the system names it
    int inputs = 0, outputs = 0;
    double defaultRate = 0;
};
// A host's devices (JACK: the clients with physical ports, "system" first).
std::vector<DeviceInfo> devices(Host h, std::string* error = nullptr);

constexpr int kMinBuffer = 32, kMaxBuffer = 2048;
int clampBuffer(int frames);  // to a power of two from 32 to 2048

struct DeviceSettings {
    Host host = Host::Qt;
    std::string output, input;  // device ids ("" the host's default; JACK: the client whose physical ports are joined)
    int sampleRate = 0;         // 0: the sequence's (JACK: the server's, whatever is asked)
    int bufferFrames = 256;     // 32 to 2048 (JACK: asked of the server)
    std::string clock;          // "" the device's own
    int inputs = 2;             // input channels opened (0 to 64)
    std::vector<std::string> inputNames;  // the inputs' port names ("" or missing: "In N")
    bool autoConnect = true;    // JACK: ports joined to the physical ones in order
    // A loopback the user measured (output to input by a cable): the round trip in frames, and the rate and buffer it
    // was measured at (it holds only for them); 0 none.
    int loopbackFrames = 0, loopbackRate = 0, loopbackBuffer = 0;

    QJsonObject toJson() const;
    static DeviceSettings fromJson(const QJsonObject& o);
    bool operator==(const DeviceSettings& o) const;
    bool operator!=(const DeviceSettings& o) const { return !(*this == o); }
};

// An input's port name: its own, else "In N" (from 1).
std::string inputName(const DeviceSettings& s, int index);

struct Latency {
    int input = 0, output = 0;  // device frames, as the device reports them
};

// The frames a recording through the device arrives late by: the loopback measured at this rate and buffer, else the
// reported input and output latencies together. `measured` says which.
int roundTrip(const DeviceSettings& s, int rate, int buffer, Latency reported, bool* measured = nullptr);

// Where a click is in a recording (every `stride`th sample read): the first at or above half the loudest, -1 when
// nothing reaches `floor`.
int64_t findClick(const float* samples, int64_t n, int stride = 1, float floor = 0.05f);

// Places a recording's blocks on the timeline. Clocked (a playback to follow): blocks are passed over until one knows
// its timeline sample, and the take starts at `start` from there (silence first where that block is later, the frames
// before `start` passed over where it is earlier), and a later block more than a block (and at most two seconds) past
// where the take has reached (frames lost to an input overflow) is placed by its own sample again; a jump back, or
// further on, carries the take on from there. Unclocked: the first `fallback` frames
// (what arrived late from before the take began) are passed over and the rest kept as they come.
class TakeAligner {
public:
    TakeAligner(int64_t start = 0, bool clocked = false, int64_t fallback = 0) : start_(start), fallback_(fallback), clocked_(clocked) {}
    struct Step {
        int64_t silence = 0;  // frames of silence before this block's
        int skip = 0;         // this block's first frames passed over
    };
    Step next(int64_t sample, int frames);
    bool started() const { return started_; }

private:
    int64_t start_, fallback_, skip_ = 0, next_ = 0;  // (next_: the timeline sample the take has reached)
    bool clocked_, started_ = false, placed_ = false;
};

// ---------------------------------------------------------------------------
// One open device: a duplex callback, interleaved float.

struct StreamCallbacks {
    std::function<void(const float* in, int inputs, float* out, int outputs, int frames)> process;  // the audio thread
    std::function<void(int rate)> rateChanged;  // the device now runs at `rate` (any thread)
    std::function<void(const std::string& why)> lost;  // the device or server went away (any thread)
};

class Stream {
public:
    virtual ~Stream() = default;
    virtual int sampleRate() const = 0;
    virtual int bufferFrames() const = 0;
    virtual int inputs() const = 0;
    virtual int outputs() const = 0;
    virtual Latency latency() const = 0;
    virtual int64_t xruns() const = 0;
    virtual std::string device() const = 0;  // what it opened, as the system names it
    // JACK: the client's own ports, in full ("montage:L"); others none.
    virtual std::vector<std::string> ports() const { return {}; }
    // JACK: names the outputs after the speakers (in order); false where ports have no names.
    virtual bool renameOutputs(const std::vector<std::string>&) { return false; }
    // JACK: joins two ports by their full names.
    virtual bool connect(const std::string&, const std::string&) { return false; }
    // The clock sources the device offers (none: its own only), and the one in use.
    virtual std::vector<std::string> clockSources() const { return {}; }
    virtual std::string clock() const { return {}; }
    // How busy the callback keeps the device (0..1, -1 not known).
    virtual double load() const { return -1; }
};

// Opens `s.host` at `rate` (0: the device's own) with that many channels; the output ports named `outputNames`
// (JACK). Null with the reason on failure; Host::Qt opens nothing here.
std::unique_ptr<Stream> openStream(const DeviceSettings& s, int rate, int inputs, int outputs,
                                   const std::vector<std::string>& outputNames, StreamCallbacks callbacks, std::string* error);

// The loopback (Host::Loopback): an in-process device whose outputs come back on its inputs (output k on input k),
// late by the latencies it reports plus `unreported` frames, as an interface with a cable from output to input
// would. Paced in real time by a thread of its own; a callback that runs past its period is an xrun.
struct LoopbackOptions {
    int reportedInput = 0, reportedOutput = 0;  // frames
    int unreported = 0;                         // more frames of round trip it does not report
};
void setLoopbackOptions(const LoopbackOptions& o);
LoopbackOptions loopbackOptions();
// The loopback open now: runs at `rate` from its next period (as a server whose rate is changed), or holds its next
// callback back for `ms` (an overload).
void loopbackChangeRate(int rate);
void loopbackStall(int ms);

// JACK: the library that would be loaded (MONTAGE_JACK_LIBRARY, PipeWire's, then the system's), "" when none loads.
std::string jackLibrary();

// ---------------------------------------------------------------------------
// The engine: the device Montage uses, shared by playback and recording.

struct EngineStatus {
    bool open = false;
    std::string error;  // why it is not open (or what went wrong last)
    Host host = Host::Qt;
    std::string device;
    int rate = 0, buffer = 0, inputs = 0, outputs = 0;
    Latency latency;
    int roundTrip = 0;      // what recordings are moved by (frames at `rate`)
    bool measured = false;  // from a measured loopback
    int64_t xruns = 0;
    int rateChanges = 0;    // times the device's rate changed while open
    std::vector<std::string> ports;
    std::string clock;
    std::vector<std::string> clocks;
    double load = -1;
};

class Engine {
public:
    Engine();
    ~Engine();
    static Engine& shared();

    // Opens the device the settings name (closes it for Host::Qt). Clients stay, and go on through the new device.
    void setSettings(const DeviceSettings& s);
    DeviceSettings settings() const;
    bool active() const;  // a device other than Qt Multimedia's is chosen
    bool isOpen() const;
    // The rate to open at when the settings leave it to the sequence (the program's; reopens when it changes).
    void setPreferredRate(int rate);
    // At least this many output channels (the monitoring section's); reopens when more are needed.
    void setOutputChannels(int channels);
    // The outputs' port names (the monitoring section's speakers, in channel order).
    void setOutputNames(const std::vector<std::string>& names);

    // An output: `render` fills `frames` frames of `channels` channels (interleaved) at `rate`, and returns the timeline
    // sample its first frame is (-1 when nothing of the timeline plays). Summed with the others onto the first outputs.
    // `owner` names it for the inputs that follow it. -1 when no device is open (with the reason).
    using Render = std::function<int64_t(float* out, int channels, int frames)>;
    int addOutput(const void* owner, int rate, int channels, Render render, std::string* error = nullptr);
    void removeOutput(int id);
    // The seconds of an output heard so far: what it has sent less the output latency (the picture's clock); -1 when
    // it is not running.
    double heardSeconds(int id) const;

    // An input: every block of the device's inputs (all of them, interleaved, at `rate`), with the timeline sample its
    // first frame belongs at: the newest output of `clockOwner`'s less the round trip (-1 when that plays nothing of
    // the timeline, or none is named).
    using Tap = std::function<void(const float* in, int channels, int frames, int64_t sample)>;
    int addInput(const void* clockOwner, int rate, Tap tap, std::string* error = nullptr);
    void removeInput(int id);
    // The round trip in frames at `rate` (resampling included), as inputs are moved by it.
    int roundTripAt(int rate) const;

    EngineStatus status() const;
    void resetXruns();
    // JACK: joins two ports by their full names (the device's own and others').
    bool connect(const std::string& from, const std::string& to);
    // A click out of `output` and listened for on `input` (channels from 0), for up to `seconds`: the round trip in
    // device frames, kept in the settings for this rate and buffer. -1 with the reason when nothing came back.
    int measureLoopback(int output, int input, double seconds, std::string* error);

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

}  // namespace montage::audiodev
