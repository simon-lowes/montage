#include "History.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <utility>
#include <vector>

namespace montage {

void History::push(const std::string& label, const Project& before) {
    undo_.push_back({label, before});
    while (undo_.size() > limit_) undo_.pop_front();
    redo_.clear();
    ++revision_;
}

bool History::undo(Project& current) {
    if (undo_.empty()) return false;
    Entry e = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back({e.label, current});
    current = std::move(e.state);
    ++revision_;
    return true;
}

bool History::redo(Project& current) {
    if (redo_.empty()) return false;
    Entry e = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back({e.label, current});
    current = std::move(e.state);
    ++revision_;
    return true;
}

void History::clear() {
    undo_.clear();
    redo_.clear();
    ++revision_;
}

// ---------------------------------------------------------------------------
// Timecode

bool isDropFrameRate(Rational fps) {
    return fps.den == 1001 && (fps.num == 30000 || fps.num == 60000);
}

std::string formatTimecode(FrameTime frame, Rational fps, bool dropFrame) {
    bool neg = frame < 0;
    if (neg) frame = -frame;
    int nominal = int(std::lround(fps.toDouble()));
    if (nominal <= 0) nominal = 30;
    bool df = dropFrame && isDropFrameRate(fps);
    if (df) {
        // SMPTE drop-frame: skip 2 (or 4) frame numbers each minute except every tenth.
        int dropFrames = nominal == 60 ? 4 : 2;
        FrameTime framesPer10Min = FrameTime(std::llround(fps.toDouble() * 600));
        FrameTime framesPerMin = FrameTime(nominal) * 60 - dropFrames;
        FrameTime d = frame / framesPer10Min;
        FrameTime m = frame % framesPer10Min;
        if (m > dropFrames)
            frame += dropFrames * 9 * d + dropFrames * ((m - dropFrames) / framesPerMin);
        else
            frame += dropFrames * 9 * d;
    }
    FrameTime ff = frame % nominal;
    FrameTime totalSec = frame / nominal;
    FrameTime ss = totalSec % 60;
    FrameTime mm = (totalSec / 60) % 60;
    FrameTime hh = totalSec / 3600;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s%02lld:%02lld:%02lld%c%02lld", neg ? "-" : "", (long long)hh, (long long)mm,
                  (long long)ss, df ? ';' : ':', (long long)ff);
    return buf;
}

bool parseTimecode(const std::string& textIn, Rational fps, FrameTime& out) {
    std::string text;
    for (char c : textIn)
        if (!std::isspace(static_cast<unsigned char>(c))) text += c;
    if (text.empty()) return false;
    int nominal = int(std::lround(fps.toDouble()));
    if (nominal <= 0) nominal = 30;

    // Seconds: "12.5s"
    if (text.back() == 's') {
        try {
            double sec = std::stod(text.substr(0, text.size() - 1));
            out = FrameTime(std::llround(sec * fps.toDouble()));
            return true;
        } catch (...) {
            return false;
        }
    }
    // Relative or plain frame count.
    bool allDigits = true;
    for (size_t i = 0; i < text.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(text[i])) && !(i == 0 && (text[i] == '+' || text[i] == '-')))
            allDigits = false;
    if (allDigits) {
        try {
            out = std::stoll(text);
            return true;
        } catch (...) {
            return false;
        }
    }
    // HH:MM:SS:FF (any of ':', ';', '.' as separators); fewer fields are allowed.
    std::vector<long long> parts;
    bool df = false;
    std::string cur;
    for (char c : text) {
        if (c == ':' || c == ';' || c == '.') {
            if (c == ';') df = true;
            if (cur.empty()) return false;
            parts.push_back(std::stoll(cur));
            cur.clear();
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            cur += c;
        } else {
            return false;
        }
    }
    if (cur.empty()) return false;
    parts.push_back(std::stoll(cur));
    while (parts.size() < 4) parts.insert(parts.begin(), 0);
    if (parts.size() > 4) return false;
    long long hh = parts[0], mm = parts[1], ss = parts[2], ff = parts[3];
    FrameTime frames = ((hh * 60 + mm) * 60 + ss) * nominal + ff;
    if ((df || isDropFrameRate(fps)) && isDropFrameRate(fps)) {
        int dropFrames = nominal == 60 ? 4 : 2;
        long long totalMinutes = hh * 60 + mm;
        frames -= dropFrames * (totalMinutes - totalMinutes / 10);
    }
    out = frames;
    return true;
}

}  // namespace montage
