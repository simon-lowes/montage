#include "Hui.h"

#include <algorithm>

namespace montage::hui {

namespace {

const uint8_t kHeader[] = {0xF0, 0x00, 0x00, 0x66, 0x05, 0x00};

MidiMessage sysex(uint8_t command) {
    MidiMessage m(std::begin(kHeader), std::end(kHeader));
    m.push_back(command);
    return m;
}

// A relative step: above 0x40 clockwise by the excess, below it anticlockwise by the value.
int delta(int v) { return v > 0x40 ? v - 0x40 : v > 0 && v < 0x40 ? -v : 0; }

// Mackie Control notes and the HUI's zones and ports for the same buttons (and lights).
struct Pair {
    int note, zone, port;
};
constexpr Pair kPairs[] = {
    {mcu::Control, 0x08, 0},       {mcu::Shift, 0x08, 1},         {mcu::Undo, 0x08, 3},         {mcu::Alt, 0x08, 4},
    {mcu::Option, 0x08, 5},        {mcu::Save, 0x08, 7},          {mcu::ChannelLeft, 0x0A, 0},  {mcu::BankLeft, 0x0A, 1},
    {mcu::ChannelRight, 0x0A, 2},  {mcu::BankRight, 0x0A, 3},     {mcu::Down, 0x0D, 0},         {mcu::Left, 0x0D, 1},
    {mcu::Right, 0x0D, 3},         {mcu::Up, 0x0D, 4},            {mcu::Scrub, 0x0D, 5},        {mcu::Rewind, 0x0E, 1},
    {mcu::FastForward, 0x0E, 2},   {mcu::Stop, 0x0E, 3},          {mcu::Play, 0x0E, 4},         {mcu::Record, 0x0E, 5},
    {ReturnToZero, 0x0F, 0},       {GoToEnd, 0x0F, 1},            {mcu::Cycle, 0x0F, 3},        {mcu::Marker, 0x14, 0},  // the keypad's Enter
    {mcu::SmpteLed, 0x16, 0},      {mcu::BeatsLed, 0x16, 2},      {mcu::Trim, 0x18, 0},         {mcu::Latch, 0x18, 1},
    {mcu::Read, 0x18, 2},          {AutomationOff, 0x18, 3},      {mcu::Write, 0x18, 4},        {mcu::Touch, 0x18, 5},
    {mcu::F1, 0x1B, 0},            {mcu::F1 + 1, 0x1B, 1},        {mcu::F1 + 2, 0x1B, 2},       {mcu::F1 + 3, 0x1B, 3},
    {mcu::F1 + 4, 0x1B, 4},        {mcu::F1 + 5, 0x1B, 5},        {mcu::F1 + 6, 0x1B, 6},       {mcu::F1 + 7, 0x1B, 7},
};

// A strip's buttons: port 1 Select, 2 Mute, 3 Solo, 5 V-Select (the V-Pot's push), 7 Rec (port 0 is the fader touch).
constexpr std::pair<int, int> kStripPorts[] = {{1, mcu::Select}, {2, mcu::Mute}, {3, mcu::Solo}, {5, mcu::VPotPush}, {7, mcu::RecArm}};

}  // namespace

Port portOf(int note) {
    for (const auto& [port, base] : kStripPorts)
        if (note >= base && note < base + kStrips) return {note - base, port};
    for (const Pair& p : kPairs)
        if (p.note == note) return {p.zone, p.port};
    return {};
}

int noteOf(int zone, int port) {
    if (zone >= 0 && zone < kStrips) {
        for (const auto& [p, base] : kStripPorts)
            if (p == port) return base + zone;
        return -1;
    }
    for (const Pair& p : kPairs)
        if (p.zone == zone && p.port == port) return p.note;
    return -1;
}

mcu::Event Decoder::decode(const MidiMessage& m) {
    mcu::Event e;
    if (m.size() < 3 || m[0] != 0xB0) return e;
    const int cc = m[1], v = m[2];
    if (cc == 0x0F) {
        zone_ = v;  // the next port is in this zone
    } else if (cc == 0x2F) {
        if (zone_ < 0) return e;
        const int port = v & 0x0F;
        const bool down = (v & 0x40) != 0;
        if (zone_ < kStrips && port == 0) {
            e.kind = mcu::Event::Touch;
            e.index = zone_;
            e.down = down;
        } else if (const int note = noteOf(zone_, port); note >= 0) {
            e.kind = mcu::Event::Button;
            e.index = note;
            e.down = down;
        }
    } else if (cc < kStrips) {
        high_[size_t(cc)] = v;  // a fader's high seven bits: its low ones follow
    } else if (cc >= 0x20 && cc < 0x20 + kStrips) {
        e.kind = mcu::Event::Fader;
        e.index = cc - 0x20;
        e.value = (high_[size_t(e.index)] << 7) | v;
    } else if (cc >= 0x40 && cc < 0x40 + kStrips) {
        e.kind = mcu::Event::VPot;
        e.index = cc - 0x40;
        e.value = delta(v);
    } else if (cc == 0x0D || cc == 0x4C) {  // the jog wheel, and the scroll V-Pot
        e.kind = mcu::Event::Jog;
        e.value = delta(v);
    }
    if ((e.kind == mcu::Event::VPot || e.kind == mcu::Event::Jog) && e.value == 0) e.kind = mcu::Event::None;
    return e;
}

void Decoder::reset() {
    zone_ = -1;
    high_ = {};
}

bool isPingReply(const MidiMessage& m) { return m.size() >= 3 && m[0] == 0x90 && m[1] == 0x00 && m[2] == 0x7F; }

bool isSystemReset(const MidiMessage& m) { return m.size() == 1 && m[0] == 0xFF; }

MidiMessage ping() { return {0x90, 0x00, 0x00}; }

std::vector<MidiMessage> fader(int strip, int value) {
    strip = std::clamp(strip, 0, kStrips - 1);
    value = std::clamp(value, 0, 16383);
    return {{0xB0, uint8_t(strip), uint8_t(value >> 7)}, {0xB0, uint8_t(0x20 + strip), uint8_t(value & 0x7F)}};
}

std::vector<MidiMessage> led(int zone, int port, bool on) {
    return {{0xB0, 0x0C, uint8_t(zone & 0x7F)}, {0xB0, 0x2C, uint8_t((on ? 0x40 : 0x00) | (port & 0x07))}};
}

std::vector<MidiMessage> zoneOff(int zone) {
    std::vector<MidiMessage> out{{0xB0, 0x0C, uint8_t(zone & 0x7F)}};
    for (int port = 0; port < 8; ++port) out.push_back({0xB0, 0x2C, uint8_t(port)});
    return out;
}

MidiMessage ring(int strip, int value) { return {0xB0, uint8_t(0x10 + std::clamp(strip, 0, kStrips - 1)), uint8_t(value & 0x7F)}; }

MidiMessage meter(int strip, int side, int level) {
    return {0xA0, uint8_t(std::clamp(strip, 0, kStrips - 1)), uint8_t(((side ? 1 : 0) << 4) | std::clamp(level, 0, 0x0C))};
}

std::string fit(const std::string& text, int width, bool mainDisplay) {
    std::string out;
    for (unsigned char c : text) {
        if (int(out.size()) == width) break;
        if (c >= 0x80) {
            if ((c & 0xC0) == 0x80) continue;  // the rest of a UTF-8 character
            out += '?';
        } else if (c >= 0x20 && c < 0x7F) {
            // The main display has its own characters for backslash and tilde (a yen sign, an arrow).
            out += mainDisplay && c == '\\' ? '/' : mainDisplay && c == '~' ? '-' : char(c);
        }
    }
    out.resize(size_t(std::max(0, width)), ' ');
    return out;
}

MidiMessage stripText(int strip, const std::string& text) {
    MidiMessage m = sysex(0x10);
    m.push_back(uint8_t(std::clamp(strip, 0, kStrips)));
    for (char c : fit(text, kStripChars)) m.push_back(uint8_t(c));
    m.push_back(0xF7);
    return m;
}

MidiMessage display(int zone, const std::string& text) {
    MidiMessage m = sysex(0x12);
    m.push_back(uint8_t(std::clamp(zone, 0, 7)));
    for (char c : fit(text, kZoneChars, true)) m.push_back(uint8_t(c));
    m.push_back(0xF7);
    return m;
}

std::vector<int> timecodeDigits(const std::string& text) {
    // The digits, with a point after the last digit of each group but the last (hours, minutes and seconds).
    std::vector<int> digits;
    for (char c : text) {
        if (c >= '0' && c <= '9') digits.push_back(c - '0');
        else if ((c == ':' || c == ';' || c == '.') && !digits.empty()) digits.back() |= 0x10;
    }
    if (digits.empty()) return {};
    if (digits.size() > 8) digits.erase(digits.begin(), digits.end() - 8);
    digits.back() &= 0x0F;  // (the rightmost digit has no point)
    std::reverse(digits.begin(), digits.end());
    return digits;
}

MidiMessage timecode(const std::vector<int>& digits, size_t count) {
    MidiMessage m = sysex(0x11);
    for (size_t i = 0; i < std::min(count, digits.size()); ++i) m.push_back(uint8_t(digits[i]));
    m.push_back(0xF7);
    return m;
}

std::vector<MidiMessage> timecode(const std::string& text) {
    const std::vector<int> digits = timecodeDigits(text);
    if (digits.empty()) return {};
    return {timecode(digits, digits.size())};
}

}  // namespace montage::hui
