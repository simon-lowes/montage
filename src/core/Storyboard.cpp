#include "Storyboard.h"

#include <algorithm>
#include <cmath>

namespace montage {

std::vector<Id> storyboardOrder(const Project& p, const std::vector<Id>& media) {
    struct Placed {
        Id id;
        double x, y;
    };
    std::vector<Placed> placed;
    std::vector<Id> loose;
    for (Id id : media) {
        const MediaItem* m = p.findMedia(id);
        if (m && m->board.placed) placed.push_back({id, m->board.x, m->board.y});
        else if (m) loose.push_back(id);
    }
    std::stable_sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) { return a.y < b.y; });
    std::vector<Id> out;
    for (size_t i = 0; i < placed.size();) {
        // A row: everything whose top is within half a cell of its first item's.
        size_t j = i + 1;
        while (j < placed.size() && placed[j].y - placed[i].y < 0.5) ++j;
        std::stable_sort(placed.begin() + long(i), placed.begin() + long(j), [](const Placed& a, const Placed& b) { return a.x < b.x; });
        for (size_t k = i; k < j; ++k) out.push_back(placed[k].id);
        i = j;
    }
    out.insert(out.end(), loose.begin(), loose.end());
    return out;
}

std::map<Id, BoardSpot> boardLayout(const Project& p, const std::vector<Id>& media, int columns) {
    columns = std::max(1, columns);
    std::map<Id, BoardSpot> out;
    double bottom = -1;  // the lowest row anything placed reaches
    for (Id id : media)
        if (const MediaItem* m = p.findMedia(id); m && m->board.placed) {
            out[id] = m->board;
            bottom = std::max(bottom, m->board.y);
        }
    // The rest in rows below (a whole row clear of the lowest placed item).
    const double first = bottom < 0 ? 0 : std::ceil(bottom + 1 - 1e-9);  // (an item at row 2.3 reaches 3.3: from row 4)
    int n = 0;
    for (Id id : media)
        if (const MediaItem* m = p.findMedia(id); m && !m->board.placed) {
            out[id] = {double(n % columns), first + double(n / columns), false};
            ++n;
        }
    return out;
}

void arrangeStoryboard(Project& p, const std::vector<Id>& media, int columns) {
    columns = std::max(1, columns);
    int n = 0;
    for (Id id : media)
        if (MediaItem* m = p.findMedia(id)) {
            m->board = {double(n % columns), double(n / columns), true};
            ++n;
        }
}

edit::Result assembleStoryboard(Project& p, Sequence& s, const std::vector<Id>& media, FrameTime at, TrackRef video,
                                TrackRef audio, bool insert) {
    edit::Result all;
    all.ok = true;
    const double fps = s.fpsValue();
    for (Id id : media) {
        const MediaItem* m = p.findMedia(id);
        if (!m) continue;
        Id use = id;
        double in = 0, out = -1;
        if (m->subclipOf && m->subclipOut > m->subclipIn) {
            use = m->subclipOf;
            in = std::floor(m->subclipIn * fps + 1e-6);
            out = std::ceil(m->subclipOut * fps - 1e-6);
        }
        const edit::Result r = edit::placeMedia(p, s, use, at, in, out, video, audio, insert);
        if (!r.ok) return edit::Result::fail((m->name.empty() ? std::string("A clip") : m->name) + ": " + r.error);
        FrameTime end = at;
        for (Id c : r.created)
            if (const Clip* clip = edit::clipById(s, c)) end = std::max(end, clip->end());
        all.created.insert(all.created.end(), r.created.begin(), r.created.end());
        at = end;
    }
    if (all.created.empty()) return edit::Result::fail("Nothing to assemble");
    return all;
}

}  // namespace montage
