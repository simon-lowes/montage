// Montage — Sony's 9-pin deck control protocol (RS-422 at 38400 baud, odd parity), as Sony's VTR protocol manuals set
// it out and every edit controller, DAW machine control and broadcast deck since the 1980s speaks it. Both sides:
//  - Montage controlling a deck (`Deck`): play, stop, shuttle, jog and variable speed, cue to a timecode, pre-roll,
//    status and timecode sense (LTC, VITC, timer), and an edit: In, Out, pre-roll time, the channels and auto edit.
//  - Montage answering as a deck (`DeckEmulator`), so an edit controller or a DAW's machine control locates, plays,
//    shuttles and jogs the Program, with timecode reported back.
// A message is CMD-1 (its group in the high four bits, the number of data bytes in the low four), CMD-2, the data and
// a checksum (the low eight bits of the sum of the rest). A deck answers each command within 9 ms: ACK (10 01), NAK
// (11 12 with the reasons) or the data asked for.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "control/SerialPort.h"

namespace montage::ninepin {

struct Message {
    uint8_t group = 0;  // CMD-1's high four bits: 0 system, 1 its answers, 2 transport, 4 preset/select, 6 sense, 7 their answers
    uint8_t cmd = 0;    // CMD-2
    std::vector<uint8_t> data;  // up to 15 bytes
    bool is(uint8_t g, uint8_t c) const { return group == g && cmd == c; }
};

// The bytes sent: CMD-1, CMD-2, data and checksum.
std::vector<uint8_t> encode(const Message& m);
inline std::vector<uint8_t> encode(uint8_t group, uint8_t cmd, std::vector<uint8_t> data = {}) { return encode(Message{group, cmd, std::move(data)}); }

// Messages out of a stream of bytes. A message whose checksum is wrong is reported as such (a deck then answers NAK
// with its checksum bit); bytes that cannot start a message are passed over.
class Parser {
public:
    void feed(const uint8_t* bytes, size_t n);
    enum Result { None, Ok, BadChecksum };
    Result next(Message& out);
    void clear() { buf_.clear(); }

private:
    std::vector<uint8_t> buf_;
};

// Common commands (group, CMD-2).
namespace cmd {
constexpr uint8_t LocalDisable = 0x0C, DeviceTypeRequest = 0x11, LocalEnable = 0x1D;       // group 0
constexpr uint8_t Ack = 0x01, Nak = 0x12, DeviceType = 0x11;                                  // group 1
constexpr uint8_t Stop = 0x00, Play = 0x01, Record = 0x02, StandbyOff = 0x04, StandbyOn = 0x05, Eject = 0x0F,
                  FastForward = 0x10, JogForward = 0x11, VarForward = 0x12, ShuttleForward = 0x13, StepForward = 0x14,
                  Rewind = 0x20, JogReverse = 0x21, VarReverse = 0x22, ShuttleReverse = 0x23, StepReverse = 0x24,
                  Preroll = 0x30, CueUpWithData = 0x31, SyncPlay = 0x34, Preview = 0x40, Review = 0x41, AutoEdit = 0x42,
                  EditOff = 0x64, EditOn = 0x65;                                              // group 2
constexpr uint8_t TimecodePreset = 0x04, InEntry = 0x10, OutEntry = 0x11, InDataPreset = 0x14, OutDataPreset = 0x15,
                  InReset = 0x20, OutReset = 0x21, EditPreset = 0x30, PrerollTimePreset = 0x31;  // group 4
constexpr uint8_t CurrentTimeSense = 0x0C, InDataSense = 0x10, OutDataSense = 0x11, StatusSense = 0x20,
                  SpeedSense = 0x2E, EditPresetSense = 0x30, PrerollTimeSense = 0x31;          // group 6
// CURRENT TIME SENSE's request byte, and the answers' CMD-2 (group 7).
constexpr uint8_t SenseLtc = 0x01, SenseVitc = 0x02, SenseTimer1 = 0x04, SenseTimer2 = 0x08, SenseLtcUserBits = 0x10,
                  SenseVitcUserBits = 0x20;
constexpr uint8_t Timer1Data = 0x00, Timer2Data = 0x01, LtcTimeData = 0x04, LtcUserBitsData = 0x05, VitcTimeData = 0x06,
                  VitcUserBitsData = 0x07, InData = 0x10, OutData = 0x11, StatusData = 0x20, SpeedData = 0x2E,
                  EditPresetData = 0x30, PrerollTimeData = 0x31;
// NAK's reasons.
constexpr uint8_t NakUnknownCommand = 0x01, NakChecksum = 0x04, NakParity = 0x10, NakOverrun = 0x20, NakFraming = 0x40,
                  NakTimeout = 0x80;
}  // namespace cmd

// A timecode as the protocol carries it: four BCD bytes, frames first (the frames byte's bit 6 marking drop frame), for
// a frame number counted as `audio/Ltc` counts (frames since 00:00:00:00 at `base` 24, 25 or 30 a second).
std::array<uint8_t, 4> timecodeBytes(int64_t frame, int base, bool dropFrame);
// The frame number of four timecode bytes, or -1 when a digit is not BCD; `dropFrame` set from the frames byte.
int64_t timecodeFromBytes(const uint8_t* b, int base, bool* dropFrame = nullptr);

// Shuttle, jog and variable speed's data: one byte N (or N and a finer second byte) standing for 10^(N/32 - 2) times
// play speed, so 64 is play, 32 a tenth of it and 79 about three times it (Sony's table).
double speedFromData(const std::vector<uint8_t>& data);
std::vector<uint8_t> speedData(double timesPlay, bool fine = false);

// STATUS DATA's first ten bytes, as the bits Sony's manuals set out.
struct Status {
    bool local = false, tapeOut = false, servoRefMissing = false;                                   // byte 0
    bool play = false, record = false, fastForward = false, rewind = false, eject = false, stop = false,
         standby = false;                                                                           // byte 1
    bool cueComplete = false, still = false, reverse = false, var = false, jog = false, shuttle = false,
         servoLock = false;                                                                         // byte 2
    bool inSet = false, outSet = false, audioInSet = false, audioOutSet = false, autoMode = false;   // byte 3
    bool preroll = false, preview = false, autoEdit = false, review = false, edit = false, fullEe = false,
         selectEe = false;                                                                          // byte 4
    std::array<uint8_t, 10> bytes() const;
    static Status from(const uint8_t* b, size_t n);
};

// An edit's channels (EDIT PRESET's data): assemble (everything, with timecode and control track), or insert on the
// channels chosen.
struct EditChannels {
    bool assemble = false;
    bool video = true, timecode = false;
    std::array<bool, 8> audio{true, true, false, false, false, false, false, false};
    std::vector<uint8_t> data() const;  // one byte for A1-A2, two with A3-A4, three past them
    static EditChannels from(const std::vector<uint8_t>& d);
};

// Which timecode a deck reads back.
enum class TimeSource { Ltc, Vitc, Timer1, Auto };

// Montage controlling a deck down a serial line. Each command waits for its answer (with the protocol's 9 ms and the
// line's time, about 30 ms in all); one at a time, from any thread.
class Deck {
public:
    Deck() = default;
    ~Deck();
    bool open(const std::string& port, std::string* error = nullptr);
    void close();
    bool isOpen() const;
    std::string lastError() const;

    // The deck's frame rate, for its timecode (24, 25 or 30; drop frame read from what it sends, kept for what is sent).
    void setBase(int base, bool dropFrame = false) { base_ = base, dropFrame_ = dropFrame && base == 30; }
    int base() const { return base_; }
    bool dropFrame() const { return dropFrame_; }

    // One command and its answer (ACK or the data asked for); nothing on no answer, a NAK or a garbled answer
    // (lastError() says which).
    std::optional<Message> command(const Message& m, int timeoutMs = 100);
    bool ack(uint8_t group, uint8_t c, std::vector<uint8_t> data = {});

    std::optional<std::array<uint8_t, 2>> deviceType();
    bool play() { return ack(2, cmd::Play); }
    bool stop() { return ack(2, cmd::Stop); }
    bool record() { return ack(2, cmd::Record); }
    bool fastForward() { return ack(2, cmd::FastForward); }
    bool rewind() { return ack(2, cmd::Rewind); }
    bool standby(bool on) { return ack(2, on ? cmd::StandbyOn : cmd::StandbyOff); }
    bool eject() { return ack(2, cmd::Eject); }
    // Speed in times play, its sign the direction.
    bool shuttle(double speed);
    bool jog(double speed);
    bool variable(double speed);
    bool step(bool forward) { return ack(2, forward ? cmd::StepForward : cmd::StepReverse); }
    bool cue(int64_t frame);
    bool preroll() { return ack(2, cmd::Preroll); }
    bool setIn(int64_t frame);
    bool setOut(int64_t frame);
    bool setPrerollTime(int64_t frames);
    bool setEditChannels(const EditChannels& c);
    bool autoEdit() { return ack(2, cmd::AutoEdit); }
    bool preview() { return ack(2, cmd::Preview); }
    bool review() { return ack(2, cmd::Review); }

    std::optional<Status> status();
    // The tape's position as a frame number on its timecode; -1 when it could not be read.
    int64_t timecode(TimeSource source = TimeSource::Ltc);
    // Waits until a cue to `target` has finished (STATUS's CUE UP, and the timecode there, so a CUE UP left from the
    // last cue does not count), up to `seconds`; false if it has not.
    bool waitForCue(int64_t target, double seconds, const std::atomic<bool>* cancel = nullptr);

private:
    mutable std::mutex m_;
    SerialPort port_;
    Parser parser_;
    std::string error_;
    std::atomic<int> base_{25};
    std::atomic<bool> dropFrame_{false};
};

// What a deck Montage plays is doing, as the Program's transport publishes it: its frame on the Program's timecode,
// moving at `speed` times play since `at`. The emulator answers from it at once (within the protocol's 9 ms), without
// waiting on whatever drives the Program.
struct TransportState {
    int64_t frame = 0;
    double speed = 0;
    std::chrono::steady_clock::time_point at = std::chrono::steady_clock::now();
    bool cueing = false;  // moving to a cue point (STATUS's CUE UP is set once it gets there)
    double fps = 25;
    // Where it is now.
    double frameNow(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;
};

// A request from the controller, for the Program's transport to carry out (on whatever thread the transport lives).
struct TransportRequest {
    enum Kind { Locate, Speed, Stop, Step } kind = Stop;
    int64_t frame = 0;  // Locate's
    double speed = 0;   // Speed's, times play (negative: backwards); Step's +1 or -1
};

// Montage answering as a deck on a serial port: a controller's commands become transport requests, and status and
// timecode are answered from the published state. It runs on a thread of its own.
class DeckEmulator {
public:
    DeckEmulator();
    ~DeckEmulator();
    bool start(const std::string& port, std::string* error = nullptr);
    void stop();
    bool running() const;

    void setBase(int base, bool dropFrame) { base_ = base, dropFrame_ = dropFrame; }
    // Publishes where the Program is (call as it moves, and when a request has been carried out).
    void publish(const TransportState& s);
    TransportState state() const;
    // Requests, on the emulator's thread (hand them on to the transport's).
    std::function<void(const TransportRequest&)> onRequest;
    // The commands answered so far (for the panel's activity light).
    int64_t commandsAnswered() const { return answered_; }

    // The answer to one message (for the tests and the thread): ACK, NAK or data.
    Message answer(const Message& m);

private:
    void run();
    SerialPort port_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int64_t> answered_{0};
    mutable std::mutex m_;
    TransportState state_;
    int base_ = 25;
    bool dropFrame_ = false;
    int64_t in_ = -1, out_ = -1, preroll_ = 125;
    EditChannels channels_;
    int64_t userBits_ = 0;
};

}  // namespace montage::ninepin
