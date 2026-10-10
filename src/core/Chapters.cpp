#include "Chapters.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace montage {

std::vector<Chapter> chaptersOf(const Sequence& s, FrameTime in, FrameTime out, const std::string& intro) {
    if (out < 0) out = s.duration();
    std::vector<const Marker*> marks;
    for (const Marker& m : s.markers)
        if (m.chapter && m.t >= in && m.t < out) marks.push_back(&m);
    std::sort(marks.begin(), marks.end(), [](const Marker* a, const Marker* b) { return a->t < b->t; });
    std::vector<Chapter> out_;
    if (marks.empty()) return out_;
    if (marks.front()->t > in) out_.push_back({0, 0, intro});
    for (const Marker* m : marks) out_.push_back({m->t - in, 0, m->name.empty() ? "Chapter " + std::to_string(out_.size() + 1) : m->name});
    for (size_t i = 0; i < out_.size(); ++i) out_[i].end = i + 1 < out_.size() ? out_[i + 1].start : out - in;
    return out_;
}

std::string youtubeChapters(const Sequence& s, FrameTime in, FrameTime out, std::string* warning) {
    const std::vector<Chapter> ch = chaptersOf(s, in, out);
    const double fps = s.fpsValue();
    std::string text;
    for (const Chapter& c : ch) {
        const long secs = long(std::floor(double(c.start) / fps + 1e-6));
        char buf[32];
        if (secs >= 3600) std::snprintf(buf, sizeof buf, "%ld:%02ld:%02ld", secs / 3600, (secs / 60) % 60, secs % 60);
        else std::snprintf(buf, sizeof buf, "%ld:%02ld", secs / 60, secs % 60);
        text += std::string(buf) + " " + c.title + "\n";
    }
    if (warning) {
        warning->clear();
        if (ch.empty()) *warning = "There are no chapter markers";
        else if (ch.size() < 3) *warning = "YouTube shows chapters only when there are at least three";
        else
            for (const Chapter& c : ch)
                if (double(c.end - c.start) / fps < 10) {
                    *warning = "YouTube shows chapters only when each is at least ten seconds long (\"" + c.title + "\" is shorter)";
                    break;
                }
    }
    return text;
}

}  // namespace montage
