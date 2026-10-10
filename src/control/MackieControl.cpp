#include "MackieControl.h"

#include <algorithm>
#include <cmath>

namespace montage::mcu {

namespace {

// A relative step: bit 6 set turns anticlockwise, the low six bits say how far.
int relative(int v) {
    const int steps = std::max(1, v & 0x3F);  // (a zero count still means a step)
    return (v & 0x40) ? -steps : steps;
}

constexpr int kFaderTop = 0x3F70;

// The fader law's corners: (position 0-1, dB).
constexpr double kLaw[][2] = {{0.0, -60}, {0.25, -30}, {0.5, -12}, {0.75, 0}, {1.0, 12}};
constexpr int kLawPoints = int(sizeof kLaw / sizeof kLaw[0]);

}  // namespace

Event decode(const MidiMessage& m) {
    Event e;
    if (m.empty()) return e;
    const int status = m[0] & 0xF0, channel = m[0] & 0x0F;
    if (status == 0xE0 && m.size() >= 3 && channel <= kMasterChannel) {
        e.kind = Event::Fader;
        e.index = channel;
        e.value = m[1] | (m[2] << 7);
    } else if ((status == 0x90 || status == 0x80) && m.size() >= 3) {
        const int note = m[1];
        e.down = status == 0x90 && m[2] > 0;
        if (note >= FaderTouch && note <= MasterTouch) {
            e.kind = Event::Touch;
            e.index = note - FaderTouch;
            e.down = status == 0x90 && m[2] > 0x40;
        } else {
            e.kind = Event::Button;
            e.index = note;
        }
    } else if (status == 0xB0 && m.size() >= 3) {
        if (m[1] >= 0x10 && m[1] < 0x10 + kStrips) {
            e.kind = Event::VPot;
            e.index = m[1] - 0x10;
            e.value = relative(m[2]);
        } else if (m[1] == 0x3C) {
            e.kind = Event::Jog;
            e.value = relative(m[2]);
        }
    }
    return e;
}

MidiMessage deviceQuery(int device) { return {0xF0, 0x00, 0x00, 0x66, uint8_t(device & 0x7F), 0x00, 0xF7}; }

MidiMessage meterMode(int strip, int mode, int device) {
    return {0xF0, 0x00, 0x00, 0x66, uint8_t(device & 0x7F), 0x20, uint8_t(std::clamp(strip, 0, kStrips - 1)), uint8_t(mode & 0x07), 0xF7};
}

bool surfaceSysex(const MidiMessage& m, int& device, int& command) {
    if (m.size() < 7 || m[0] != 0xF0 || m[1] != 0x00 || m[2] != 0x00 || m[3] != 0x66 || m.back() != 0xF7) return false;
    device = m[4];
    command = m[5];
    return true;
}

MidiMessage fader(int channel, int value) {
    value = std::clamp(value, 0, 16383);
    return {uint8_t(0xE0 | (std::clamp(channel, 0, 15))), uint8_t(value & 0x7F), uint8_t((value >> 7) & 0x7F)};
}

MidiMessage led(int note, int state) {
    return {0x90, uint8_t(note & 0x7F), uint8_t(state >= 2 ? 0x7F : state == 1 ? 0x01 : 0x00)};
}

MidiMessage ring(int strip, int value) { return {0xB0, uint8_t(0x30 + std::clamp(strip, 0, kStrips - 1)), uint8_t(value & 0x7F)}; }

MidiMessage lcd(int offset, const std::string& text, int device) {
    MidiMessage m{0xF0, 0x00, 0x00, 0x66, uint8_t(device & 0x7F), 0x12, uint8_t(std::clamp(offset, 0, 2 * kLcdWidth - 1))};
    for (size_t i = 0; i < text.size() && offset + int(i) < 2 * kLcdWidth; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        m.push_back(c >= 0x20 && c < 0x7F ? c : '?');
    }
    m.push_back(0xF7);
    return m;
}

MidiMessage meter(int strip, int level) {
    return {0xD0, uint8_t((std::clamp(strip, 0, kStrips - 1) << 4) | std::clamp(level, 0, 0xF))};
}

std::vector<MidiMessage> timecode(const std::string& text) {
    // Groups (hours, minutes, seconds, frames) laid out 3 + 2 + 2 + 3 across the ten digits, a dot after each of the
    // first three; digit 0x40 is the rightmost.
    std::vector<std::string> groups;
    std::string cur;
    for (char c : text) {
        if (c == ':' || c == ';' || c == '.') {
            groups.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    groups.push_back(cur);
    while (groups.size() < 4) groups.insert(groups.begin(), std::string());
    const int widths[4] = {3, 2, 2, 3};
    std::string digits;
    std::vector<bool> dots;
    for (size_t g = groups.size() - 4, k = 0; g < groups.size(); ++g, ++k) {
        std::string s = groups[g];
        if (int(s.size()) > widths[k]) s = s.substr(s.size() - size_t(widths[k]));
        s.insert(0, size_t(widths[k]) - s.size(), ' ');
        for (size_t i = 0; i < s.size(); ++i) {
            digits += s[i];
            dots.push_back(k < 3 && i + 1 == s.size());
        }
    }
    std::vector<MidiMessage> out;
    for (int i = 0; i < 10; ++i) {
        const char c = digits[size_t(9 - i)];
        int v = c >= 0x40 && c <= 0x5F ? c - 0x40 : c >= 0x60 && c <= 0x7F ? c - 0x60 : (c >= 0x20 && c <= 0x3F ? c : 0x20);
        if (dots[size_t(9 - i)]) v |= 0x40;
        out.push_back({0xB0, uint8_t(0x40 + i), uint8_t(v)});
    }
    return out;
}

std::vector<MidiMessage> assignment(const std::string& text) {
    std::string t = text.substr(0, 2);
    t.insert(0, 2 - t.size(), ' ');
    auto code = [](char c) { return c >= 0x40 && c <= 0x5F ? c - 0x40 : c >= 0x60 && c <= 0x7F ? c - 0x60 : c >= 0x20 && c <= 0x3F ? c : 0x20; };
    return {{0xB0, 0x4B, uint8_t(code(t[0]))}, {0xB0, 0x4A, uint8_t(code(t[1]))}};  // 0x4B the left character
}

int faderValue(double db) {
    if (!(db > kLaw[0][1])) return 0;
    for (int i = 1; i < kLawPoints; ++i)
        if (db <= kLaw[i][1]) {
            const double t = (db - kLaw[i - 1][1]) / (kLaw[i][1] - kLaw[i - 1][1]);
            return int(std::lround((kLaw[i - 1][0] + t * (kLaw[i][0] - kLaw[i - 1][0])) * 16383));
        }
    return 16383;
}

double faderDb(int value) {
    const double p = value >= kFaderTop ? 1.0 : std::clamp(value / 16383.0, 0.0, 1.0);
    for (int i = 1; i < kLawPoints; ++i)
        if (p <= kLaw[i][0]) {
            const double t = (p - kLaw[i - 1][0]) / (kLaw[i][0] - kLaw[i - 1][0]);
            return kLaw[i - 1][1] + t * (kLaw[i][1] - kLaw[i - 1][1]);
        }
    return kLaw[kLawPoints - 1][1];
}

int panRing(double pan) {
    // Single-dot mode (bits 4-5 zero), positions 1-11 with 6 in the middle.
    return 6 + int(std::lround(std::clamp(pan, -1.0, 1.0) * 5));
}

int meterLevel(double peak) {
    if (!(peak > 0)) return 0;
    // 1 from -60 dB, then -50, -40, -30, -20, -14, -10, -8, -6, -4, -2, 12 at 0 dB, 13 over it.
    const double db = 20 * std::log10(peak);
    if (db > 0.01) return 13;
    const double steps[] = {-60, -50, -40, -30, -20, -14, -10, -8, -6, -4, -2, -0.01};
    int level = 0;
    for (double s : steps)
        if (db >= s) ++level;
    return level;
}

std::string stripText(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        if (out.size() == 6) break;
        if (c >= 0x80) {
            if ((c & 0xC0) == 0x80) continue;  // the rest of a UTF-8 character
            out += '?';
        } else if (c >= 0x20 && c < 0x7F) {
            out += char(c);
        }
    }
    out.resize(7, ' ');
    return out;
}

}  // namespace montage::mcu
