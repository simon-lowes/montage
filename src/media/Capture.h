// Montage — recording a live feed (Resolve's Media page capture, Media Composer's Capture tool), through FFmpeg:
// a network stream (SRT, RIST, UDP or RTP MPEG-TS, RTMP, HLS or any URL FFmpeg opens), an Open Media Transport source
// (media/Omt.h: omt://host:port or its announced name, with its timecode and colour when Montage sent it) or a capture
// device FFmpeg reaches (V4L2 and ALSA or PulseAudio on Linux, AVFoundation on macOS, DirectShow on Windows; built with
// libavdevice), recorded to ProRes or DNxHR with its sound (up to 16 channels, 24-bit) and timecode (the
// feed's own SMPTE 12-M timecode, one given, or the time of day). QuickTime files are written in fragments half a
// second long, so the file can be imported and cut while it records (EditorState's growing media) and survives a crash;
// MXF is written whole at the end. Recording runs on a thread of its own at the feed's frame rate (frames late or
// missing repeated, frames early dropped; a jump in the feed's clock taken as a new start), its sound placed by its
// time against the pictures' (silence where it is missing, trimmed where it runs early), and stops when asked, after
// a set time, or when the feed stops (pictures and sound then end together).
// SDI and HDMI cards (Blackmagic DeckLink) and NDI need their makers' runtimes and are not here.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace montage {

struct CaptureSource {
    std::string url;     // a stream's URL, or the device's name (e.g. "/dev/video0", "0:0", "video=My Camera"); an OMT source's
                         // omt://host:port or "HOST (Name)"
    std::string device;  // FFmpeg's input device ("v4l2", "alsa", "pulse", "avfoundation", "dshow", "lavfi"), or "omt"; "" for a URL
    std::map<std::string, std::string> options;  // FFmpeg input options ("video_size", "framerate", "latency"...)
};

struct CaptureSettings {
    CaptureSource video;
    std::optional<CaptureSource> audio;  // a sound device of its own (else the video source's sound, if any)
    std::string path;                    // .mov or .mxf
    std::string codec = "prores_hq";     // prores_proxy, prores_lt, prores, prores_hq, dnxhr_lb, dnxhr_sq, dnxhr_hq, dnxhr_hqx
    std::string timecode;                // the first frame's ("hh:mm:ss:ff"); "" the feed's own, else the time of day
    double seconds = 0;                  // stop after this long (0: when stopped)
    double openTimeout = 10;             // seconds to wait for the feed to start (and to send again, at least 5)
};

// The codecs offered: id and name.
std::vector<std::pair<std::string, std::string>> captureCodecs();
// The capture devices this build can open: "omt" (Open Media Transport) and FFmpeg's (none without libavdevice).
std::vector<std::string> captureDeviceKinds();

struct CaptureStatus {
    bool recording = false, finished = false;
    int64_t frames = 0, dropped = 0, repeated = 0;
    double seconds = 0;
    int width = 0, height = 0, channels = 0;
    double fps = 0;
    std::string timecode;  // the first frame's
    std::string error;     // why it failed (nothing usable written)
    std::string warning;   // what went wrong in a recording kept (the feed or the sound device stopped...)
};

class Recorder {
public:
    Recorder();
    ~Recorder();  // stops

    // Starts recording on a thread of its own; false with the reason when the settings cannot be recorded (an unknown
    // codec, a file that is not .mov or .mxf, no feed named). The feed is opened on that thread: one that cannot be
    // opened, or sends no picture, ends the recording with the reason (onFinished, status().error) and writes nothing.
    // The file is written from the feed's first picture.
    bool start(const CaptureSettings& settings, std::string* error = nullptr);
    // Stops and finishes the file (waits for it).
    void stop();
    // Waits up to `seconds` for a recording with a set length to end by itself; false if it is still going.
    bool wait(double seconds);
    bool recording() const;
    CaptureStatus status() const;

    // On the recording thread: the file's first second is written (it can be opened and imported now), and the
    // recording ended: ok (the file finished; `message` any warning) or not (`message` why: the feed not found, a
    // disk full...).
    std::function<void()> onFirstFragment;
    std::function<void(bool ok, const std::string& message)> onFinished;

    struct State;

private:
    void run();
    std::unique_ptr<State> s_;
    std::thread thread_;
};

}  // namespace montage
