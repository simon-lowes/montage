// Montage — work with a deck under 9-pin control (control/NinePin.h): batch capture from a log, each clip recorded
// from the deck's picture (media/Capture.h) starting on the frame its timecode says, with handles, keeping the tape's
// timecode and reel; and edit to tape, the deck recording the Program (played out through Reference Output) from In to
// Out, assembled or inserted on the channels chosen, after its pre-roll.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "control/NinePin.h"
#include "core/Model.h"
#include "media/Capture.h"

namespace montage {

// A clip logged on a tape: its frames [in, out) as frame numbers on the tape's timecode (audio/Ltc counts them, drop
// frame or not as the log's rate says; batch capture finds the frames with those labels on a deck that counts the
// other way).
struct LogEntry {
    std::string name, reel;
    int64_t in = 0, out = 0;
    std::string comment;
};

// "hh:mm:ss:ff" as a frame number at `base` frames a second; -1 if not one. Drop frame (at 30) when ";" comes before
// the frames, or with `assumeDrop` (a deck or Program known to run in drop frame, its timecode written with colons).
int64_t parseTimecode(const std::string& text, int base, bool* dropFrame = nullptr, bool assumeDrop = false);
std::string formatTimecode(int64_t frame, int base, bool dropFrame);

// A capture log: an Avid Log Exchange file (its Name, Start, End and Tape columns; End is the first frame after, as
// ALE has it) or a CSV with a header naming name, reel (or tape), in (or start) and out (or end) columns, out being
// the first frame after. `base` and `dropFrame` read the timecodes (an ALE's own FPS heading wins); entries without
// a reel take `defaultReel`.
std::vector<LogEntry> readCaptureLog(const std::string& path, int base, bool dropFrame, std::string* error = nullptr,
                                     const std::string& defaultReel = {});

struct BatchCaptureSettings {
    CaptureSettings capture;   // the deck's picture and how it is recorded (path, range and timecode set per clip)
    Rational rate{25, 1};      // the tape's frame rate
    std::string folder;        // where the clips go, each "<reel>_<name>.mov" (or .mxf)
    std::string extension = ".mov";
    double handles = 2;        // seconds recorded either side of each clip
    double preroll = 5;        // seconds the deck runs up before each clip's first frame (handles included)
    int latencyFrames = 0;     // how many frames the picture arrives after the deck shows them (a feed without its own timecode)
    ninepin::TimeSource source = ninepin::TimeSource::Ltc;
    double cueTimeout = 120;   // seconds a cue may take (a long shuttle down the tape)
};

struct BatchClip {
    LogEntry entry;
    std::string path, timecode;  // the file, and its first frame's timecode (the clip's In less its handle)
    int64_t frames = 0;
    bool ok = false;
    std::string error;
};

// Captures each entry in turn: the deck cued to its pre-roll, the recording started, the deck played, and the
// pictures from the entry's In less its handle to its Out plus its handle kept, each placed by its timecode: the
// picture's own when the feed carries it (SDI, Open Media Transport), else the deck's, read as it plays. The entries'
// timecodes are counted as the deck was set (Deck::setBase); a deck that counts the other way at 29.97 is sent the
// same labels, and the clips keep its own.
// `progress` says which clip and what is happening; `cancel` stops after the clip being captured.
std::vector<BatchClip> batchCapture(ninepin::Deck& deck, const std::vector<LogEntry>& log, const BatchCaptureSettings& s,
                                    const std::function<void(int index, const std::string& what)>& progress = {},
                                    const std::atomic<bool>* cancel = nullptr);

// An edit onto tape: In on the tape's timecode (counted as the deck was set), out - in frames long, the channels, and
// the deck's pre-roll.
struct TapeEdit {
    int64_t in = 0, out = 0;
    ninepin::EditChannels channels;
    double preroll = 5;  // seconds (less when In is nearer 00:00:00:00)
};

// Sets the edit on the deck (channels, In, Out, pre-roll) and runs it (AUTO EDIT): the deck pre-rolls and records In
// to Out. `roll` is told as soon as the deck is seen running up towards In (moving forward at play speed within its
// pre-roll, its servo locked when it says so), how many frames before In it was and when (captureClock()), so the
// Program can be started to reach the edit's first frame as the deck reaches In; told again should the deck then jump
// (one that was playing when told, searching back to its pre-roll). Waits for the edit to finish; false with the reason
// if the deck refused it, stopped answering or never got there, the deck stopped once it had been told to record.
bool editToTape(ninepin::Deck& deck, const TapeEdit& e, const std::function<void(int64_t framesToIn, double clock)>& roll,
                const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);

}  // namespace montage
