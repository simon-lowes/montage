#include "MidiMap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace montage::midimap {

namespace {

const char* kindName(Kind k) {
    switch (k) {
        case Kind::Note: return "note";
        case Kind::Cc: return "cc";
        case Kind::Cc14: return "cc14";
        case Kind::Nrpn: return "nrpn";
        case Kind::Rpn: return "rpn";
        case Kind::PitchBend: return "bend";
        case Kind::Program: return "program";
        case Kind::None: break;
    }
    return "";
}

int maxNumber(Kind k) {
    switch (k) {
        case Kind::Note:
        case Kind::Cc:
        case Kind::Program: return 127;
        case Kind::Cc14: return 31;
        case Kind::Nrpn:
        case Kind::Rpn: return 16383;
        case Kind::PitchBend: return 0;
        case Kind::None: break;
    }
    return -1;
}

bool isNaN(double v) { return std::isnan(v); }

}  // namespace

int resolutionOf(Kind kind) {
    switch (kind) {
        case Kind::Cc14:
        case Kind::Nrpn:
        case Kind::Rpn:
        case Kind::PitchBend: return 16384;
        default: return 128;
    }
}

std::string keyName(const Key& key) {
    if (key.kind == Kind::None) return {};
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s:%d:%d", kindName(key.kind), key.channel + 1, key.number);
    return buf;
}

std::optional<Key> keyFromName(const std::string& name) {
    const size_t a = name.find(':');
    const size_t b = a == std::string::npos ? a : name.find(':', a + 1);
    if (b == std::string::npos) return std::nullopt;
    const std::string kind = name.substr(0, a);
    Key key;
    for (Kind k : {Kind::Note, Kind::Cc, Kind::Cc14, Kind::Nrpn, Kind::Rpn, Kind::PitchBend, Kind::Program})
        if (kind == kindName(k)) key.kind = k;
    if (key.kind == Kind::None) return std::nullopt;
    auto number = [](const std::string& s, int& out) {
        if (s.empty() || s.size() > 6) return false;
        for (char c : s)
            if (c < '0' || c > '9') return false;
        out = std::stoi(s);
        return true;
    };
    int channel = 0, n = 0;
    if (!number(name.substr(a + 1, b - a - 1), channel) || !number(name.substr(b + 1), n)) return std::nullopt;
    if (channel < 1 || channel > 16 || n > maxNumber(key.kind)) return std::nullopt;
    key.channel = channel - 1;
    key.number = n;
    return key;
}

std::string keyLabel(const Key& key) {
    char buf[64];
    const int ch = key.channel + 1;
    switch (key.kind) {
        case Kind::Note: {
            static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
            std::snprintf(buf, sizeof buf, "Note %s%d (ch %d)", names[key.number % 12], key.number / 12 - 1, ch);
            break;
        }
        case Kind::Cc: std::snprintf(buf, sizeof buf, "CC %d (ch %d)", key.number, ch); break;
        case Kind::Cc14: std::snprintf(buf, sizeof buf, "14-bit CC %d/%d (ch %d)", key.number, key.number + 32, ch); break;
        case Kind::Nrpn: std::snprintf(buf, sizeof buf, "NRPN %d:%d (ch %d)", key.number >> 7, key.number & 127, ch); break;
        case Kind::Rpn: std::snprintf(buf, sizeof buf, "RPN %d:%d (ch %d)", key.number >> 7, key.number & 127, ch); break;
        case Kind::PitchBend: std::snprintf(buf, sizeof buf, "Pitch bend (ch %d)", ch); break;
        case Kind::Program: std::snprintf(buf, sizeof buf, "Program %d (ch %d)", key.number, ch); break;
        case Kind::None: return {};
    }
    return buf;
}

// ---- Decoding.

Decoder::Channel::Channel() {
    std::fill(std::begin(msb), std::end(msb), -1);
    std::fill(std::begin(pairs), std::end(pairs), 0);
}

void Decoder::reset() {
    for (Channel& c : ch_) c = Channel();
    fine_.clear();
    coarse_.clear();
}

void Decoder::feed(const MidiMessage& m, const std::function<void(const Event&)>& out) {
    if (m.size() < 2 || m[0] < 0x80 || m[0] >= 0xF0) return;
    const int status = m[0] & 0xF0, ch = m[0] & 0x0F;
    const int d1 = m[1] & 0x7F, d2 = m.size() > 2 ? m[2] & 0x7F : 0;
    switch (status) {
        case 0x80:
            if (m.size() > 2) out({{Kind::Note, ch, d1}, 0, 128});
            return;
        case 0x90:
            if (m.size() > 2) out({{Kind::Note, ch, d1}, d2, 128});
            return;
        case 0xC0: out({{Kind::Program, ch, d1}, 127, 128}); return;
        case 0xE0:
            if (m.size() > 2) out({{Kind::PitchBend, ch, 0}, d1 | (d2 << 7), 16384});
            return;
        case 0xB0: break;
        default: return;  // aftertouch
    }
    if (m.size() < 3) return;
    Channel& c = ch_[ch];
    const int n = d1, v = d2;
    switch (n) {  // choosing a parameter
        case 99: c.paramMsb = v; c.rpn = false; c.dataMsb = -1; return;
        case 98: c.paramLsb = v; c.rpn = false; c.dataMsb = -1; return;
        case 101: c.paramMsb = v; c.rpn = true; c.dataMsb = -1; return;
        case 100: c.paramLsb = v; c.rpn = true; c.dataMsb = -1; return;
        default: break;
    }
    const bool chosen = !(c.paramMsb == 127 && c.paramLsb == 127);
    if (chosen && (n == 6 || n == 38 || n == 96 || n == 97)) {
        const int param = (c.paramMsb << 7) | c.paramLsb;
        const Key key{c.rpn ? Kind::Rpn : Kind::Nrpn, ch, param};
        const auto id = std::make_tuple(ch, c.rpn, param);
        if (n == 6) {
            c.dataMsb = v;
            coarse_[id] = v;
            if (!fine_.count(id)) out({key, v, 128});  // a parameter that sends only the coarse half
        } else if (n == 38) {
            fine_.insert(id);
            // (a fine half alone, after the parameter was chosen again: with the coarse half it last had)
            int msb = c.dataMsb;
            if (msb < 0)
                if (const auto it = coarse_.find(id); it != coarse_.end()) msb = it->second;
            out({key, (std::max(msb, 0) << 7) | v, 16384});
        }
        return;  // (increment and decrement are not read: no controller in common use sends them for knobs)
    }
    const int last = c.last;
    c.last = n;
    if (n < 32) c.msb[n] = v;
    out({{Kind::Cc, ch, n}, v, 128});
    if (n >= 32 && n < 64 && c.msb[n - 32] >= 0) {
        int& pairs = c.pairs[n - 32];
        if (last == n - 32) pairs = std::min(pairs + 1, 1000);
        if (last == n - 32 || pairs >= 4) out({{Kind::Cc14, ch, n - 32}, (c.msb[n - 32] << 7) | v, 16384});
    }
}

// ---- Encodings and learning.

std::string encodingName(Encoding e) {
    switch (e) {
        case Encoding::Absolute: return "absolute";
        case Encoding::Button: return "button";
        case Encoding::TwosComplement: return "twos-complement";
        case Encoding::SignMagnitude: return "sign-magnitude";
        case Encoding::Offset64: return "offset-64";
    }
    return {};
}

std::optional<Encoding> encodingFromName(const std::string& name) {
    for (Encoding e : {Encoding::Absolute, Encoding::Button, Encoding::TwosComplement, Encoding::SignMagnitude,
                       Encoding::Offset64})
        if (name == encodingName(e)) return e;
    return std::nullopt;
}

int relativeSteps(Encoding e, int value) {
    value &= 0x7F;
    switch (e) {
        // (65-79 read as sign and magnitude: a sign-and-magnitude encoder turned only up while learning looks like
        // two's complement, and a two's complement encoder never sends them short of 49 steps at once)
        case Encoding::TwosComplement: return value < 64 ? value : value <= 79 ? -(value & 0x3F) : value - 128;
        case Encoding::SignMagnitude: return (value & 0x40) ? -(value & 0x3F) : value;
        case Encoding::Offset64: return value - 64;
        default: return 0;
    }
}

Encoding classify7Bit(const std::vector<int>& values) {
    if (values.empty()) return Encoding::Absolute;
    // Buttons: 127 and 0 (or one "on" value and 0).
    std::set<int> distinct(values.begin(), values.end());
    const int top = *distinct.rbegin();
    // (127 alone, once: a button that sends no release; again and again: an encoder turned down slowly)
    if (top >= 64 && (distinct == std::set<int>{top} ? top == 127 && values.size() == 1 : distinct == std::set<int>{0, top}))
        return Encoding::Button;
    // Relative encoders keep to a few values either side of their "no change" (0 or 64).
    auto in = [](int v, int lo, int hi) { return v >= lo && v <= hi; };
    bool up = false, twosDown = false, offDown = false, high = false;  // 1-15, 113-127, 49-63, 65-79
    for (int v : values) {
        if (in(v, 1, 15)) up = true;
        else if (in(v, 113, 127)) twosDown = true;
        else if (in(v, 49, 63)) offDown = true;
        else if (in(v, 65, 79)) high = true;
        else return Encoding::Absolute;
    }
    Encoding family;
    if (twosDown) {
        if (offDown || high) return Encoding::Absolute;
        family = Encoding::TwosComplement;
    } else if (offDown) {
        if (up) return Encoding::Absolute;
        family = Encoding::Offset64;
    } else if (up && high) {
        family = Encoding::SignMagnitude;
    } else {
        // One way only: 1-15 reads the same in two's complement and sign-and-magnitude; 65-79 is taken as offset 64
        // turned up (a person learning turns clockwise first) rather than sign-and-magnitude turned down.
        family = up ? Encoding::TwosComplement : Encoding::Offset64;
    }
    // A fader sweeps: each value once, one way. An encoder repeats its step or turns back.
    if (values.size() == 1) return family;
    bool repeat = false, rising = true, falling = true;
    for (size_t i = 1; i < values.size(); ++i) {
        if (values[i] == values[i - 1]) repeat = true;
        if (values[i] < values[i - 1]) rising = false;
        if (values[i] > values[i - 1]) falling = false;
    }
    return repeat || !(rising || falling) ? family : Encoding::Absolute;
}

void Learner::clear() {
    key_ = {};
    values_.clear();
}

void Learner::add(const Event& e) {
    if (e.key.kind == Kind::None) return;
    if (key_.kind == Kind::None) {
        if (e.key.kind == Kind::Note && e.value == 0) return;  // a note let go that was pressed before learning
        key_ = e.key;
    } else if (key_.kind == Kind::Cc && e.key.kind == Kind::Cc14 && e.key.channel == key_.channel &&
               (e.key.number == key_.number || e.key.number + 32 == key_.number)) {
        key_ = e.key;  // the controller turned out to be a 14-bit pair
        values_.clear();
    } else if (e.key != key_) {
        return;  // the first control touched is the one learnt
    }
    values_.push_back(e.value);
}

bool Learner::ready() const {
    if (key_.kind == Kind::None) return false;
    return key_.kind != Kind::Cc || values_.size() >= kValuesFor7Bit;
}

std::optional<Learnt> Learner::finish() {
    if (key_.kind == Kind::None) return std::nullopt;
    Learnt l{key_, Encoding::Absolute};
    if (key_.kind == Kind::Note || key_.kind == Kind::Program) l.encoding = Encoding::Button;
    else if (key_.kind == Kind::Cc) l.encoding = classify7Bit(values_);
    clear();
    return l;
}

// ---- Mapping.

double Mapper::toTarget(const Mapping& m, double position) {
    double p = std::clamp(position, 0.0, 1.0);
    if (m.invert) p = 1 - p;
    if (m.curve > 0 && m.curve != 1) p = std::pow(p, m.curve);
    return m.low + (m.high - m.low) * p;
}

double Mapper::toPosition(const Mapping& m, double target) {
    if (m.high == m.low) return 0;
    double p = std::clamp((target - m.low) / (m.high - m.low), 0.0, 1.0);
    if (m.curve > 0 && m.curve != 1) p = std::pow(p, 1 / m.curve);
    return m.invert ? 1 - p : p;
}

Mapper::Run& Mapper::run(size_t index) {
    if (runs_.size() != mappings_.size()) runs_.resize(mappings_.size());
    return runs_[index];
}

void Mapper::clear() {
    mappings_.clear();
    runs_.clear();
}

void Mapper::setMappingOptions(size_t index, const Mapping& m) {
    if (index >= mappings_.size() || mappings_[index].key != m.key || mappings_[index].layer != m.layer) return;
    mappings_[index] = m;
    run(index) = Run();
}

void Mapper::map(const Mapping& m) {
    if (runs_.size() != mappings_.size()) runs_.resize(mappings_.size());
    for (size_t i = 0; i < mappings_.size(); ++i)
        if (mappings_[i].key == m.key && mappings_[i].layer == m.layer) {
            mappings_[i] = m;
            runs_[i] = Run();
            return;
        }
    mappings_.push_back(m);
    runs_.push_back(Run());
}

void Mapper::unmap(const Key& key, const std::string& layer) {
    if (runs_.size() != mappings_.size()) runs_.resize(mappings_.size());
    for (size_t i = 0; i < mappings_.size(); ++i)
        if (mappings_[i].key == key && mappings_[i].layer == layer) {
            mappings_.erase(mappings_.begin() + std::ptrdiff_t(i));
            runs_.erase(runs_.begin() + std::ptrdiff_t(i));
            return;
        }
}

void Mapper::setLayer(const std::string& layer) {
    if (layer == layer_) return;
    layer_ = layer;
    resetTakeover();
    reshow();
}

void Mapper::resetTakeover() {
    for (size_t i = 0; i < mappings_.size(); ++i) {
        Run& r = run(i);
        r.caught = false;
        r.last = -1;
        r.settled = -1;
        r.down = false;
    }
}

void Mapper::reshow() {
    for (size_t i = 0; i < mappings_.size(); ++i) run(i).shown = -1;
}

bool Mapper::active(size_t index) const {
    if (index >= mappings_.size()) return false;
    const Mapping& m = mappings_[index];
    if (!m.layer.empty()) return m.layer == layer_;
    if (layer_.empty()) return true;
    for (const Mapping& o : mappings_)
        if (o.key == m.key && o.layer == layer_) return false;
    return true;
}

bool Mapper::waiting(size_t index) const {
    if (index >= mappings_.size() || index >= runs_.size()) return false;
    const Mapping& m = mappings_[index];
    const Run& r = runs_[index];
    return m.encoding == Encoding::Absolute && m.pickup && !r.caught && r.last >= 0;
}

void Mapper::handle(const Event& e, const Targets& targets) {
    for (size_t i = 0; i < mappings_.size(); ++i) {
        const Mapping m = mappings_[i];  // (a copy: setting a target may change the map)
        if (m.key != e.key || !active(i)) continue;
        Run& r = run(i);
        const double cur = targets.get ? targets.get(m) : std::nan("");
        const double off = m.invert ? m.high : m.low, on = m.invert ? m.low : m.high;
        Change change;
        bool set = false, echo = false;
        const double p = e.position();
        if (m.encoding == Encoding::Button) {
            const bool down = e.key.kind == Kind::Note ? e.value > 0 : e.key.kind == Kind::Program || e.value >= 64;
            // A controller button pressed again with no release between: one that sends none.
            if (down && r.down && e.key.kind == Kind::Cc) r.down = false;
            if (down && !r.down) {
                const bool lit = !isNaN(cur) && std::fabs(cur - on) < std::fabs(cur - off);
                change = {m.toggle ? (lit ? off : on) : on, Change::Press};
                set = true;
            } else if (!down && r.down && !m.toggle) {
                change = {off, Change::Release};
                set = true;
            }
            r.down = e.key.kind == Kind::Program ? false : down;
        } else if (m.encoding != Encoding::Absolute) {
            int steps = relativeSteps(m.encoding, e.value);
            if (m.invert) steps = -steps;
            if (steps == 0) continue;
            const double lo = std::min(m.low, m.high), hi = std::max(m.low, m.high);
            const double base = isNaN(cur) ? m.low : cur;
            change.value = std::clamp(base + steps * m.step * (hi - lo), lo, hi);
            set = true;
        } else {
            if (m.pickup && !isNaN(cur)) {
                // Moved elsewhere since (the mouse, automation, undo): the control has to find it again.
                if (r.caught && r.settled >= 0 && std::fabs(cur - r.settled) > 1e-9) r.caught = false;
                if (!r.caught) {
                    const double at = toPosition(m, cur);
                    const double tolerance = 1.0 / double(e.resolution - 1) + 1e-9;
                    const bool crossed = r.last >= 0 && ((r.last <= at && p >= at) || (r.last >= at && p <= at));
                    r.caught = crossed || std::fabs(p - at) <= tolerance;
                }
            } else {
                r.caught = true;
            }
            const double before = r.last;
            r.last = p;
            if (!r.caught) continue;
            change.value = toTarget(m, p);
            if (before >= 0 && before < 0.5 && p >= 0.5) change.edge = Change::Press;
            else if (before >= 0.5 && p < 0.5) change.edge = Change::Release;
            else if (before < 0 && p >= 0.5) change.edge = Change::Press;
            set = true;
            echo = true;
        }
        if (!set) continue;
        if (targets.set) targets.set(m, change);
        if (i >= mappings_.size() || !(mappings_[i].key == m.key && mappings_[i].layer == m.layer)) continue;
        Run& after = run(i);
        after.settled = targets.get ? targets.get(m) : change.value;
        if (echo && !isNaN(after.settled))  // what the control itself shows is not sent back to it
            after.shown = int(std::lround(toPosition(m, after.settled) * (resolutionOf(m.key.kind) - 1)));
    }
}

std::vector<MidiMessage> Mapper::feedback(const Mapping& m, double targetValue) {
    const double p = toPosition(m, targetValue);
    const uint8_t ch = uint8_t(m.key.channel & 0x0F);
    const int q14 = int(std::lround(p * 16383));
    const bool lit = p >= 0.5;
    switch (m.key.kind) {
        case Kind::Note: return {{uint8_t(0x90 | ch), uint8_t(m.key.number), uint8_t(lit ? 127 : 0)}};
        case Kind::Cc: {
            const int v = m.encoding == Encoding::Button ? (lit ? 127 : 0) : int(std::lround(p * 127));
            return {{uint8_t(0xB0 | ch), uint8_t(m.key.number), uint8_t(v)}};
        }
        case Kind::Cc14:
            return {{uint8_t(0xB0 | ch), uint8_t(m.key.number), uint8_t(q14 >> 7)},
                    {uint8_t(0xB0 | ch), uint8_t(m.key.number + 32), uint8_t(q14 & 127)}};
        case Kind::Nrpn:
        case Kind::Rpn: {
            const bool rpn = m.key.kind == Kind::Rpn;
            return {{uint8_t(0xB0 | ch), uint8_t(rpn ? 101 : 99), uint8_t(m.key.number >> 7)},
                    {uint8_t(0xB0 | ch), uint8_t(rpn ? 100 : 98), uint8_t(m.key.number & 127)},
                    {uint8_t(0xB0 | ch), 6, uint8_t(q14 >> 7)},
                    {uint8_t(0xB0 | ch), 38, uint8_t(q14 & 127)}};
        }
        case Kind::PitchBend: return {{uint8_t(0xE0 | ch), uint8_t(q14 & 127), uint8_t(q14 >> 7)}};
        case Kind::Program:
        case Kind::None: break;
    }
    return {};
}

std::vector<MidiMessage> Mapper::refresh(const std::function<double(const Mapping&)>& get) {
    std::vector<MidiMessage> out;
    for (size_t i = 0; i < mappings_.size(); ++i) {
        const Mapping& m = mappings_[i];
        if (m.key.kind == Kind::Program || !active(i)) continue;
        const double v = get ? get(m) : std::nan("");
        if (isNaN(v)) continue;
        const double p = toPosition(m, v);
        const int q = m.encoding == Encoding::Button ? (p >= 0.5 ? 1 : 0)
                                                     : int(std::lround(p * (resolutionOf(m.key.kind) - 1)));
        Run& r = run(i);
        if (q == r.shown) continue;
        r.shown = q;
        for (MidiMessage& b : feedback(m, v)) out.push_back(std::move(b));
    }
    return out;
}

}  // namespace montage::midimap
