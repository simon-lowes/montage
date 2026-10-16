// Montage — reference output: the program sent to a broadcast monitor through a video interface (SDI or HDMI on a
// Blackmagic DeckLink card, or NDI over the network), as Resolve's video monitoring, Premiere's Mercury Transmit and
// Media Composer's do: 10-bit 4:2:2, HDR signalled (PQ, HLG and their static metadata), embedded audio, timecode, and
// offsets for a monitor's latency. Each interface is a Backend, its library loaded when it is used (none is linked);
// a Scheduler feeds one at the device's own pace with the frames playback renders.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"
#include "render/VideoPacking.h"

namespace montage {
struct Lut3D;  // (render/Processing.h)
}

namespace montage::output {

// A video mode: its frame size and frame rate (an interlaced mode's frames are its field pairs).
struct Mode {
    std::string name;  // "1080p25", "2160p59.94", "1080i50"
    int width = 1920, height = 1080;
    Rational fps{25, 1};
    bool interlaced = false;
    int64_t id = 0;  // the interface's own code for it
};
std::string modeName(int width, int height, Rational fps, bool interlaced);
// SDI's and HDMI's usual modes (HD, 2K, UHD and 4K DCI at the usual rates).
std::vector<Mode> standardModes();
// The mode a sequence plays in: its size and rate (progressive before interlaced), else its rate at the nearest
// larger size, else its rate at the largest; null when no mode has its rate.
const Mode* matchMode(const std::vector<Mode>& modes, int width, int height, Rational fps);
const Mode* findMode(const std::vector<Mode>& modes, const std::string& name);

enum class PixelFormat { Uyvy8, V210, P216 };
size_t rowBytes(PixelFormat f, int width);
size_t frameBytes(PixelFormat f, int width, int height);

enum class Eotf { Sdr, Pq, Hlg };

// What goes down the wire.
struct Signal {
    PixelFormat format = PixelFormat::V210;
    std::string colorSpace = "rec709";  // frames are converted to it: rec709, rec2020, rec2100pq or rec2100hlg
    video::YcbcrMatrix matrix = video::YcbcrMatrix::Bt709;
    bool bt2020 = false;  // BT.2020 primaries
    Eotf eotf = Eotf::Sdr;
    bool hdrMetadata = false;  // HDR10 static metadata sent (PQ): the mastering display and light levels below
    bool masteringP3 = true;   // the mastering display's primaries: P3-D65, else BT.2020
    double masteringPeak = 1000, masteringMin = 0.0001;  // nits
    int maxCll = 0, maxFall = 0;
};

struct Timecode {
    int hours = 0, minutes = 0, seconds = 0, frames = 0;
    bool dropFrame = false;
};
Timecode timecodeOf(FrameTime frame, Rational fps);

// One frame for the wire.
struct VideoFrame {
    int width = 0, height = 0;
    size_t stride = 0;
    PixelFormat format = PixelFormat::V210;
    const uint8_t* data = nullptr;
    FrameTime frame = 0;  // the sequence's frame (its timecode below)
    Timecode timecode;
    FrameTime timecodeFrame = 0;  // the frames `timecode` counts, at `fps`
    Rational fps{25, 1};
};
// The audio that goes with it: `frames` samples of `channels` interleaved (none when no channels).
struct Audio {
    const float* samples = nullptr;
    int frames = 0, channels = 0, rate = 48000;
};

struct Device {
    std::string id, name;
    std::vector<Mode> modes;  // empty: any size and rate (NDI)
    std::vector<PixelFormat> formats{PixelFormat::V210, PixelFormat::Uyvy8};
    bool hdr = false;                  // takes HDR metadata
    std::vector<int> audioChannels{2};  // embedded channel counts it takes
    std::vector<int> audioRates{48000};  // empty: any
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::string name() const = 0;  // "NDI", "DeckLink", "Virtual"
    // Its library present; else why not (what to install).
    virtual bool available(std::string* why = nullptr) = 0;
    virtual std::vector<Device> devices() = 0;
    virtual bool open(const Device& device, const Mode& mode, const Signal& signal, int audioChannels, int audioRate,
                      std::string* error) = 0;
    // Sends a frame and its audio, returning when the device wants the next one (its clock paces the output).
    // False when the output failed.
    virtual bool send(const VideoFrame& frame, const Audio& audio) = 0;
    virtual void close() = 0;
    // Makes a `send` waiting for the device return (stopping).
    virtual void wake() {}
    // Receivers connected (NDI); -1 when it does not know.
    virtual int receivers() { return -1; }
};

// What the user chose.
struct Config {
    std::string backend, device;  // by name and id ("" = the first)
    std::string mode;             // a mode's name; "" = the sequence's
    int depth = 10;               // 8 or 10 bits (P216's 16 on NDI for 10)
    bool rec709 = false;          // the picture as the viewer shows it (HDR tone mapped to SDR); else the sequence's own
    bool hdrMetadata = true;
    enum class Scaling { Fit, Fill, None } scaling = Scaling::Fit;
    int audioChannels = 2;        // 0: none
    int videoOffset = 0;          // frames the picture runs ahead (+) of the speakers, for a monitor's latency
    int audioOffsetMs = 0;        // embedded audio's delay against its picture
};

// The signal a sequence of colour space `space` goes out as, in `format`: its own (BT.709, BT.2020, PQ or HLG; P3 PQ in a
// BT.2020 PQ signal mastered on P3-D65; sRGB and P3 SDR as BT.709) or, with `rec709`, the viewer's BT.709 picture.
// False and why when the format cannot carry it (HDR needs 10 bits).
bool signalFor(const Sequence& s, const Config& c, PixelFormat format, Signal& out, std::string* error = nullptr);
// The rectangle a w x h picture takes in a W x H frame.
struct Rect {
    int x = 0, y = 0, width = 0, height = 0;
};
Rect fitRect(int w, int h, int W, int H, Config::Scaling scaling);
// `img` in a W x H frame over black (bilinear when it is not already the rectangle's size).
Image fitInto(const Image& img, int W, int H, Config::Scaling scaling);
// NDI's colour metadata for a signal (its <ndi_color_info/>).
std::string ndiColorInfo(const Signal& s);

// Feeds a backend from playback: the frames rendered (any thread) are fitted, converted and packed on a thread of its
// own, and sent on another at the device's pace, the frame at the playhead (plus the video offset) each time; a frame
// not rendered yet repeats the last one sent. Embedded audio plays at normal speed only, from `setAudioSource`.
class Scheduler {
public:
    explicit Scheduler(std::shared_ptr<Backend> backend);
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // Opens the device for sequence `s` (its size, rate and colour); false and why when it cannot.
    bool start(const Device& device, const Mode& mode, const Sequence& s, const Config& config, std::string* error);
    void stop();
    bool running() const { return running_; }
    const Mode& mode() const { return mode_; }
    const Signal& signal() const { return signal_; }
    // The render scale that fits the sequence in the mode (what `submit` is best given).
    double renderScale() const { return renderScale_; }

    // A rendered frame of the sequence in its own colour space (any size; fitted into the mode).
    void submit(FrameTime t, std::shared_ptr<const Image> img);
    // The playhead and the playing speed (0 parked).
    void setPosition(FrameTime t, double speed);
    // Frame `t` packed, ready to send.
    bool ready(FrameTime t) const;
    // Whether frame `t` is still to be submitted: not packed or waiting, and not behind the frame due to be sent.
    bool wants(FrameTime t) const;
    // Frames submitted before are let go (the project changed).
    void clearFrames();
    // The monitor's calibration LUT (render/MonitorCalibration.h), applied to the signal's code values last, after the
    // conversion and framing; null for none. Frames packed before are packed again.
    void setCalibration(std::shared_ptr<const Lut3D> lut);
    std::shared_ptr<const Lut3D> calibration() const;
    // A calibration patch sent in place of the program while set (null: the program again): a mode-sized frame of the
    // signal's code values, sent as they are (no conversion or calibration).
    void setPatch(std::shared_ptr<const Image> patch);
    // Mixes `frames` samples from sample `start` into `out` (`channels` interleaved, zeroed).
    using AudioSource = std::function<void(int64_t start, int frames, int channels, float* out)>;
    void setAudioSource(AudioSource source);

    struct Stats {
        int64_t sent = 0;      // frames sent
        int64_t late = 0;      // frames repeated while playing because the next was not rendered in time
        FrameTime last = -1;   // the sequence frame last sent
        int receivers = -1;
        std::string error;     // why it stopped
    };
    Stats stats() const;

private:
    struct Packed {
        std::vector<uint8_t> data;
    };
    void packLoop();
    void sendLoop();
    std::shared_ptr<const Packed> pack(const Image& img, const std::shared_ptr<const Lut3D>& lut) const;
    std::shared_ptr<const Packed> packRaw(const Image& img) const;

    std::shared_ptr<Backend> backend_;
    Device device_;
    Mode mode_;
    Signal signal_;
    Config config_;
    std::string seqSpace_;
    double peakNits_ = 1000;
    Rational seqFps_{25, 1};
    int audioRate_ = 48000, audioChannels_ = 0;
    double renderScale_ = 1;

    std::atomic<bool> running_{false};
    std::thread packer_, sender_;
    mutable std::mutex m_;
    std::condition_variable wake_;
    std::map<FrameTime, std::shared_ptr<const Image>> pending_;
    std::map<FrameTime, std::shared_ptr<const Packed>> packed_;
    std::shared_ptr<const Packed> black_;
    std::shared_ptr<const Packed> patch_;
    std::shared_ptr<const Lut3D> calibration_;
    uint64_t calibrationGeneration_ = 0;
    FrameTime position_ = 0;
    double speed_ = 0;
    AudioSource audio_;
    Stats stats_;
};

std::shared_ptr<Backend> makeNdiBackend();
std::shared_ptr<Backend> makeDeckLinkBackend();

// The interfaces Montage knows (NDI and DeckLink; and Virtual, a test device, with MONTAGE_VIRTUAL_OUTPUT set).
std::vector<std::shared_ptr<Backend>> backends();
std::shared_ptr<Backend> findBackend(const std::string& name);

// A device that keeps what it is sent, for tests (and MONTAGE_VIRTUAL_OUTPUT). Paced by the frame rate, or by `step`
// when manual.
class VirtualBackend : public Backend {
public:
    std::string name() const override { return "Virtual"; }
    bool available(std::string* why = nullptr) override { return true; }
    std::vector<Device> devices() override;
    bool open(const Device& device, const Mode& mode, const Signal& signal, int audioChannels, int audioRate,
              std::string* error) override;
    bool send(const VideoFrame& frame, const Audio& audio) override;
    void close() override;
    void wake() override;

    struct Sent {
        FrameTime frame = 0;
        Timecode timecode;
        std::vector<uint8_t> data;  // the last few frames' only
        int audioFrames = 0;
    };
    void setManual(bool on) { manual_ = on; }
    // Lets `frames` more frames through (manual).
    void step(int frames = 1);
    // Waits until `count` frames have been sent (or `ms` pass).
    bool waitSent(size_t count, int ms = 5000);
    std::vector<Sent> sent() const;
    std::vector<float> audio() const;  // everything sent, interleaved
    Mode openMode() const;
    Signal openSignal() const;
    int openChannels() const { return channels_; }

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool manual_ = false, open_ = false, woken_ = false;
    int permits_ = 0;
    Mode mode_;
    Signal signal_;
    int channels_ = 0;
    std::vector<Sent> sent_;
    std::vector<float> audio_;
    std::chrono::steady_clock::time_point next_;
};

}  // namespace montage::output
