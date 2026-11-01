// Montage — MIDI Learn: any MIDI controller's knobs, faders, encoders and buttons mapped onto the app's parameters.
// What a controller sends is decoded into controls (notes, 7-bit and 14-bit controllers, NRPN and RPN parameters,
// pitch bend, program changes); a control touched while learning is recognised, relative encoders in the three
// common encodings included; and each mapping turns a control's moves into a target's value with its range, curve,
// inversion, button behaviour and soft takeover, and the target's value back into what lights the control.
// No Qt here: the app (app/MidiLearn) owns the targets and the saved maps.
#pragma once

#include "Midi.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace montage::midimap {

enum class Kind : uint8_t { None, Note, Cc, Cc14, Nrpn, Rpn, PitchBend, Program };

// One control on a controller: what it sends, on which channel (0-15), and its number (the note, the controller —
// for 14-bit pairs the first of the pair, 0-31 —, the parameter for NRPN and RPN, 0-16383, the program for program
// changes; 0 for pitch bend, one control per channel).
struct Key {
    Kind kind = Kind::None;
    int channel = 0;
    int number = 0;
    bool operator==(const Key& o) const { return kind == o.kind && channel == o.channel && number == o.number; }
    bool operator!=(const Key& o) const { return !(*this == o); }
    bool operator<(const Key& o) const {
        return std::tie(kind, channel, number) < std::tie(o.kind, o.channel, o.number);
    }
};
// "cc:1:7" style names (channels counted from 1, as controllers label them) for saved maps and the MCP tool.
std::string keyName(const Key& key);
std::optional<Key> keyFromName(const std::string& name);
// What a person reads: "CC 7 (ch 1)", "14-bit CC 7/39 (ch 1)", "Note C3 (ch 10)", "NRPN 1:2 (ch 1)".
std::string keyLabel(const Key& key);

// A control moved: its value, out of `resolution` steps (128 for notes, 7-bit controllers and program changes;
// 16384 for 14-bit controllers, NRPN and RPN, and pitch bend). A note's value is its velocity, 0 when it ends.
struct Event {
    Key key;
    int value = 0;
    int resolution = 128;
    double position() const { return resolution > 1 ? double(value) / double(resolution - 1) : 0.0; }
};

// Whole messages (from MidiParser) into control events. Every controller is reported as a 7-bit one; a controller
// 0-31 followed at once (the next controller message on its channel) by its partner 32-63 is also reported as a 14-bit
// pair, when the fine half arrives (or, once the pair has been seen like that a few times, whenever the fine half comes
// alone, as some devices send it when only it changed). So a fader on CC 0 and a button on CC 32 both keep working,
// and a 14-bit mapping takes only the pair. Controllers 99/98 and 101/100 choose an NRPN or RPN parameter, whose value
// then comes in data entry (6, and 38 for the fine half, reported on 38 once the parameter has sent it).
class Decoder {
public:
    void feed(const MidiMessage& m, const std::function<void(const Event&)>& out);
    void reset();

private:
    struct Channel {
        int msb[32];
        int pairs[32];  // times n+32 came straight after n
        int last = -1;  // the last controller number seen
        int paramMsb = 127, paramLsb = 127;  // 127/127: none chosen (the "null" parameter)
        bool rpn = false;
        int dataMsb = -1;
        Channel();
    };
    Channel ch_[16];
    std::set<std::tuple<int, bool, int>> fine_;  // (channel, rpn, parameter) seen sending data entry's fine half
};

// How a control's values are read.
enum class Encoding : uint8_t {
    Absolute,       // a fader or knob: its position
    Button,         // pressed (note on, controller 64 and over) and released
    TwosComplement, // relative encoders: 1 = +1, 127 = -1
    SignMagnitude,  //   1 = +1, 65 = -1
    Offset64,       //   65 = +1, 63 = -1
};
std::string encodingName(Encoding e);
std::optional<Encoding> encodingFromName(const std::string& name);
// The steps a relative encoder's value turns by (0 for absolute controls and buttons).
int relativeSteps(Encoding e, int value);

// What Learn found.
struct Learnt {
    Key key;
    Encoding encoding = Encoding::Absolute;
};

// Watches the controls that move while learning and decides which one was touched, and how it encodes its values.
// Notes, program changes and pitch bend, 14-bit controllers and NRPN/RPN are known from their first event; a 7-bit
// controller after a few values (or when the app gives up waiting, finish()), so an encoder can be told from a fader:
// encoders repeat values or turn both ways within their encoding's bands, faders sweep through values once.
class Learner {
public:
    void add(const Event& e);
    bool ready() const;                 // enough seen to decide
    std::optional<Learnt> finish();     // decides (nothing seen: nothing) and starts again
    bool empty() const { return values_.empty(); }
    void clear();
    static constexpr size_t kValuesFor7Bit = 6;

private:
    Key key_;
    std::vector<int> values_;
};
// How a run of a 7-bit controller's values reads (the classification Learner uses).
Encoding classify7Bit(const std::vector<int>& values);

// One control mapped onto one target. `target` is the app's: "command:<action>", "widget:<path>",
// "strip:<n>:volume|pan|mute|solo" (a mixer strip counted from the bank's first track) and so on.
struct Mapping {
    Key key;
    Encoding encoding = Encoding::Absolute;
    std::string target;
    std::string layer;       // "" in every panel, else only while that panel has focus (and over a "" mapping)
    double low = 0, high = 1;  // the part of the target's range the control covers
    double curve = 1;        // >1 finer at the bottom of the travel, <1 at the top (target = low + range · position^curve)
    bool invert = false;
    bool toggle = false;     // buttons: each press flips the target; else it is on while held (momentary)
    bool pickup = true;      // absolute controls: soft takeover, ignored until they reach the target's value
    double step = 1.0 / 128; // relative encoders: the part of the range one step turns
};

// What a mapping asks of its target: a value (0-1 over the target's whole range) and, for buttons and controls
// crossing half way, the edge (commands run on a press).
struct Change {
    double value = 0;
    enum Edge : uint8_t { None, Press, Release } edge = None;
};

// What the app gives the mapper for the targets: the value now (0-1, NaN when the target is not there) and setting it.
struct Targets {
    std::function<double(const Mapping&)> get;
    std::function<void(const Mapping&, const Change&)> set;
};

class Mapper {
public:
    const std::vector<Mapping>& mappings() const { return mappings_; }
    void setMappingOptions(size_t index, const Mapping& m);  // the same control and layer, other settings
    void clear();
    // Replaces (or adds) the mapping for this control in this layer.
    void map(const Mapping& m);
    void unmap(const Key& key, const std::string& layer);
    // The focused panel; layered mappings follow it. Changing it (or the bank) takes over the controls afresh.
    void setLayer(const std::string& layer);
    const std::string& layer() const { return layer_; }
    void resetTakeover();

    // A control moved: the mappings it drives in the current layer change their targets.
    void handle(const Event& e, const Targets& targets);
    // Whether this mapping is the one its control drives in the current layer.
    bool active(size_t index) const;
    // A soft-takeover control that has not reached its target's value yet (shown so a person knows to move it).
    bool waiting(size_t index) const;

    // Bytes that show a mapping's target value on its control (lit buttons, LED rings, motor faders).
    static std::vector<MidiMessage> feedback(const Mapping& m, double targetValue);
    // What changed since last time, for the active mappings (a control's own moves are not echoed back to it).
    std::vector<MidiMessage> refresh(const std::function<double(const Mapping&)>& get);
    // Everything shown again (a controller plugged back in, the layer changed).
    void reshow();

    // A target value and a control position (0-1), through the mapping's range, curve and inversion.
    static double toTarget(const Mapping& m, double position);
    static double toPosition(const Mapping& m, double target);

private:
    struct Run {
        double last = -1;      // the control's last position (-1: none yet)
        bool caught = false;   // soft takeover: following the target
        double settled = -1;   // what the target read after the mapper last set it
        bool down = false;     // buttons: held
        int shown = -1;        // feedback: the quantised position last sent (or received)
    };
    Run& run(size_t index);
    std::vector<Mapping> mappings_;
    std::vector<Run> runs_;
    std::string layer_;
};

// The resolution a control's feedback and positions are quantised to.
int resolutionOf(Kind kind);

}  // namespace montage::midimap
