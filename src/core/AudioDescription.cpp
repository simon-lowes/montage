#include "AudioDescription.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace montage {

std::vector<DescriptionGap> descriptionGaps(const std::vector<std::pair<double, double>>& speech, double fps, FrameTime from,
                                            FrameTime to, double minSeconds, double margin) {
    std::vector<DescriptionGap> out;
    if (fps <= 0 || to <= from) return out;
    const double lo = double(from) / fps, hi = double(to) / fps;
    auto add = [&](double a, double b) {
        // Clear of the speech on either side (not of the range's own ends).
        const double s = a > lo ? a + margin : a, e = b < hi ? b - margin : b;
        if (e - s + 1e-9 < minSeconds) return;
        const FrameTime fs = FrameTime(std::ceil(s * fps - 1e-6)), fe = FrameTime(std::floor(e * fps + 1e-6));
        if (fe > fs) out.push_back({fs, fe});
    };
    double at = lo;
    for (const auto& [a, b] : speech) {
        if (b <= at) continue;
        if (a >= hi) break;
        if (a > at) add(at, std::min(a, hi));
        at = std::max(at, b);
    }
    if (at < hi) add(at, hi);
    return out;
}

int descriptionWords(const std::string& text) {
    int words = 0;
    bool in = false;
    for (char c : text) {
        const bool letter = !std::isspace(static_cast<unsigned char>(c));
        if (letter && !in) ++words;
        in = letter;
    }
    return words;
}

double descriptionSeconds(const std::string& text, double wordsPerMinute) {
    return wordsPerMinute > 0 ? descriptionWords(text) * 60.0 / wordsPerMinute : 0;
}

DescriptionFit descriptionFit(const std::string& text, double room, double wordsPerMinute, double maxSpeed) {
    DescriptionFit f;
    f.needed = descriptionSeconds(text, wordsPerMinute);
    f.room = std::max(0.0, room);
    f.speed = f.room > 0 ? std::max(1.0, f.needed / f.room) : (f.needed > 0 ? 1e9 : 1.0);
    f.fits = f.speed <= maxSpeed + 1e-9;
    if (f.needed > f.room && wordsPerMinute > 0) {
        const int words = descriptionWords(text), room_words = int(std::floor(f.room * wordsPerMinute / 60.0 + 1e-9));
        f.overWords = std::max(0, words - room_words);
    }
    return f;
}

int findDescriptionTrack(const Sequence& s) {
    for (size_t i = 0; i < s.captionTracks.size(); ++i)
        if (s.captionTracks[i].name == kDescriptionTrackName) return int(i);
    return -1;
}

int descriptionTrack(Project& p, Sequence& s, const std::string& language) {
    if (const int i = findDescriptionTrack(s); i >= 0) return i;
    CaptionTrack t;
    t.id = p.newId();
    t.name = kDescriptionTrackName;
    t.language = language.empty() ? "en" : language;
    t.visible = false;  // lines to speak, not subtitles
    s.captionTracks.push_back(std::move(t));
    return int(s.captionTracks.size()) - 1;
}

bool setDescription(Project& p, Sequence& s, FrameTime start, FrameTime end, const std::string& text) {
    const bool remove = text.find_first_not_of(" \t\r\n") == std::string::npos;
    if (remove) {
        const int i = findDescriptionTrack(s);
        if (i < 0) return false;
        auto& caps = s.captionTracks[size_t(i)].captions;
        const auto before = caps.size();
        std::erase_if(caps, [&](const Caption& c) { return c.start <= start && c.end > start; });
        return caps.size() != before;
    }
    if (end <= start) return false;
    auto& caps = s.captionTracks[size_t(descriptionTrack(p, s))].captions;
    std::erase_if(caps, [&](const Caption& c) { return c.start < end && c.end > start; });
    Caption c;
    c.start = start;
    c.end = end;
    c.text = text;
    caps.push_back(std::move(c));
    std::sort(caps.begin(), caps.end(), [](const Caption& a, const Caption& b) { return a.start < b.start; });
    return true;
}

bool hasDescriptionClips(const Sequence& s) {
    for (const Track& t : s.audioTracks)
        for (const Clip& c : t.clips)
            if (c.role == kDescriptionRole) return true;
    return false;
}

}  // namespace montage
