// Montage — MIDI ports, for control surfaces (control/MackieControl.h): the system's MIDI inputs and outputs, and a
// connection to one of each with incoming bytes split into whole messages. CoreMIDI on macOS, WinMM on Windows, and
// ALSA's raw MIDI devices (/dev/snd/midiC*D*, as USB surfaces appear) on Linux, so no MIDI library is needed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace montage {

struct MidiPortInfo {
    std::string id;    // how to open it again (platform-specific, stable while it stays plugged in)
    std::string name;  // what the device calls it
};
std::vector<MidiPortInfo> midiInputs();
std::vector<MidiPortInfo> midiOutputs();

using MidiMessage = std::vector<uint8_t>;

// Splits a MIDI byte stream into messages: running status filled in, system exclusive kept whole (F0 ... F7),
// real-time bytes (clock, start, stop) passed on as they come, even in the middle of another message.
class MidiParser {
public:
    void feed(const uint8_t* data, size_t n, const std::function<void(const MidiMessage&)>& out);
    void reset();

private:
    MidiMessage msg_;
    uint8_t running_ = 0;
    size_t need_ = 0;
    bool sysex_ = false;
};
// The data bytes a message with this status byte carries (0 for system exclusive and unknown statuses).
int midiDataBytes(uint8_t status);

// An open input and output. Messages arrive on a MIDI thread.
class MidiConnection {
public:
    virtual ~MidiConnection() = default;
    virtual bool send(const MidiMessage& bytes) = 0;
    virtual std::string inputName() const = 0;
    virtual std::string outputName() const = 0;
};
// Opens the input and output with these ids (from midiInputs and midiOutputs). `onMessage` is called for every whole
// message received, and `onLost` (at most once) when the device goes away (unplugged, switched off), both from another
// thread, until the connection is destroyed. Sending never blocks the caller for long: a port that cannot keep up
// drops what does not fit rather than stalling.
std::unique_ptr<MidiConnection> openMidi(const std::string& inputId, const std::string& outputId,
                                         std::function<void(const MidiMessage&)> onMessage, std::string* error = nullptr,
                                         std::function<void()> onLost = {});

}  // namespace montage
