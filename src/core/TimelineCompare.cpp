#include "TimelineCompare.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace montage {

const char* changeKindName(ChangeKind k) {
    switch (k) {
        case ChangeKind::Added: return "Added";
        case ChangeKind::Removed: return "Removed";
        case ChangeKind::Trimmed: return "Trimmed";
        case ChangeKind::Moved: return "Moved";
        case ChangeKind::Changed: return "Changed";
    }
    return "";
}

namespace {

struct Entry {
    const Clip* clip = nullptr;
    TrackRef track;
    std::string key;    // what it plays: the file, a nested sequence, or a generator and its text
    double in = 0, out = 0;  // the source stretch, in seconds
    bool matched = false;
};

std::string trackName(TrackRef t) { return (t.kind == TrackKind::Video ? "V" : "A") + std::to_string(t.index + 1); }

std::vector<Entry> entries(const Project& p, const Sequence& s) {
    std::vector<Entry> out;
    const double fps = s.fpsValue();
    for (TrackRef r : allTracks(s))
        for (const Clip& c : trackAt(s, r)->clips) {
            Entry e;
            e.clip = &c;
            e.track = r;
            if (c.isGenerator()) {
                e.key = "gen:" + c.generator.type;
            } else if (const MediaItem* m = p.findMedia(c.mediaId)) {
                if (m->kind == MediaKind::Sequence) {
                    const Sequence* n = p.findSequence(m->sequenceId);
                    e.key = "seq:" + (n ? n->name : m->name);
                } else {
                    e.key = "file:" + (m->path.empty() ? m->name : m->path);
                }
            } else {
                e.key = "missing:" + std::to_string(c.mediaId);
            }
            e.in = c.sourceIn / fps;
            e.out = e.in + c.sourceExtent() / fps;
            if (e.out < e.in) std::swap(e.in, e.out);
            out.push_back(e);
        }
    return out;
}

std::string signed_(double v) { return (v > 0 ? "+" : "") + std::to_string(long(std::lround(v))); }

// What else differs about the same clip: speed, direction, on/off, effects, picture, sound, a generator's settings.
std::vector<std::string> otherChanges(const Clip& a, const Clip& b) {
    std::vector<std::string> out;
    if (a.speed != b.speed || a.reverse != b.reverse) {
        auto pct = [](const Clip& c) { return (c.reverse ? "-" : "") + std::to_string(long(std::lround(c.speed * 100))) + " %"; };
        out.push_back("speed " + pct(a) + " → " + pct(b));
    }
    if (a.enabled != b.enabled) out.push_back(b.enabled ? "turned on" : "turned off");
    if (a.timing != b.timing) out.push_back("time remapping");
    std::vector<std::string> fa, fb;
    for (const Effect& e : a.effects) fa.push_back(e.type);
    for (const Effect& e : b.effects) fb.push_back(e.type);
    if (fa != fb) {
        out.push_back("effects");
    } else {
        for (size_t i = 0; i < a.effects.size(); ++i)
            if (a.effects[i].params != b.effects[i].params || a.effects[i].enabled != b.effects[i].enabled) {
                out.push_back("effect settings");
                break;
            }
    }
    if (a.motion != b.motion || a.blendMode != b.blendMode) out.push_back("position, size or opacity");
    if (a.audio != b.audio) out.push_back("volume or pan");
    if (a.generator != b.generator) out.push_back(a.generator.type == "title" ? "title" : "settings");
    if (a.colorLabel != b.colorLabel || a.role != b.role) out.push_back("label");
    return out;
}

std::string join(const std::vector<std::string>& parts) {
    std::string s;
    for (const std::string& p : parts) s += (s.empty() ? "" : ", ") + p;
    return s;
}

}  // namespace

std::vector<TimelineChange> compareSequences(const Project& p, const Sequence& before, const Sequence& after) {
    return compareSequences(p, before, p, after);
}

std::vector<TimelineChange> compareSequences(const Project& pBefore, const Sequence& before, const Project& pAfter,
                                             const Sequence& after) {
    std::vector<Entry> A = entries(pBefore, before), B = entries(pAfter, after);
    const double fps = after.fpsValue();
    // Each clip of the new version against the unmatched clips of the old one playing the same thing on the same
    // kind of track: the most source in common, then the same track, then the nearest start.
    std::vector<int> match(B.size(), -1);
    std::vector<size_t> order(B.size());
    for (size_t i = 0; i < B.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return B[x].clip->start < B[y].clip->start; });
    for (size_t bi : order) {
        const Entry& b = B[bi];
        int best = -1;
        double bestScore = -1;
        for (size_t ai = 0; ai < A.size(); ++ai) {
            const Entry& a = A[ai];
            if (a.matched || a.key != b.key || a.track.kind != b.track.kind) continue;
            const bool generator = b.clip->isGenerator();
            const double overlap = std::min(a.out, b.out) - std::max(a.in, b.in);
            if (!generator && overlap <= 0) continue;
            // Generators (titles, mattes) have no footage in common: the same text first, then the same track,
            // then the nearest start.
            const double apart = std::fabs(double(a.clip->start - b.clip->start));
            const double score = (generator ? 1 : overlap) * 1000 +
                                 (generator && a.clip->generator.s("text") == b.clip->generator.s("text") ? 20 : 0) +
                                 (a.track == b.track ? 10 : 0) - 9 * apart / (apart + 100);
            if (score > bestScore) bestScore = score, best = int(ai);
        }
        if (best >= 0) {
            match[bi] = best;
            A[size_t(best)].matched = true;
        }
    }
    // Order changes: on each track of the new version, the matched clips whose old order does not follow the rest
    // (outside the longest run keeping their old order) moved.
    std::vector<bool> reordered(B.size(), false);
    std::map<std::pair<int, int>, std::vector<size_t>> perTrack;
    for (size_t bi : order)
        if (match[bi] >= 0) perTrack[{int(B[bi].track.kind), B[bi].track.index}].push_back(bi);
    for (auto& [track, list] : perTrack) {
        const size_t n = list.size();
        std::vector<int> len(n, 1), prev(n, -1);
        auto key = [&](size_t i) {
            const Entry& a = A[size_t(match[list[i]])];
            return std::make_tuple(int(a.track.kind), a.track.index, a.clip->start);
        };
        for (size_t i = 0; i < n; ++i)
            for (size_t j = 0; j < i; ++j)
                if (key(j) < key(i) && len[j] + 1 > len[i]) len[i] = len[j] + 1, prev[i] = int(j);
        int end = 0;
        for (size_t i = 0; i < n; ++i)
            if (len[i] > len[size_t(end)]) end = int(i);
        std::vector<bool> inRun(n, false);
        for (int i = n ? end : -1; i >= 0; i = prev[size_t(i)]) inRun[size_t(i)] = true;
        for (size_t i = 0; i < n; ++i) reordered[list[i]] = !inRun[i];
    }

    std::vector<TimelineChange> out;
    for (size_t bi = 0; bi < B.size(); ++bi) {
        const Entry& b = B[bi];
        TimelineChange ch;
        ch.after = b.clip->id;
        ch.track = b.track;
        ch.at = b.clip->start;
        ch.length = b.clip->duration;
        ch.name = b.clip->name;
        if (match[bi] < 0) {
            ch.kind = ChangeKind::Added;
            out.push_back(ch);
            continue;
        }
        const Entry& a = A[size_t(match[bi])];
        ch.before = a.clip->id;
        std::vector<std::string> what;
        bool moved = false, trimmed = false;
        if (a.track != b.track) {
            what.push_back(trackName(a.track) + " → " + trackName(b.track));
            moved = true;
        } else if (reordered[bi]) {
            what.push_back("changed places");
            moved = true;
        }
        // Trims, in frames of the source: where it now starts and ends in the footage (a generator: its length).
        if (b.clip->isGenerator()) {
            const double dLen = double(b.clip->duration - a.clip->duration);
            if (std::fabs(dLen) >= 0.5) what.push_back("length " + signed_(dLen)), trimmed = true;
        } else {
            const double dIn = (b.in - a.in) * fps, dOut = (b.out - a.out) * fps;
            if (std::fabs(dIn) >= 0.5) what.push_back("in " + signed_(dIn)), trimmed = true;
            if (std::fabs(dOut) >= 0.5) what.push_back("out " + signed_(dOut)), trimmed = true;
        }
        const std::vector<std::string> other = otherChanges(*a.clip, *b.clip);
        what.insert(what.end(), other.begin(), other.end());
        if (what.empty()) continue;
        ch.kind = moved ? ChangeKind::Moved : trimmed ? ChangeKind::Trimmed : ChangeKind::Changed;
        ch.details = join(what);
        out.push_back(ch);
    }
    for (const Entry& a : A) {
        if (a.matched) continue;
        TimelineChange ch;
        ch.kind = ChangeKind::Removed;
        ch.before = a.clip->id;
        ch.track = a.track;
        ch.at = a.clip->start;
        ch.length = a.clip->duration;
        ch.name = a.clip->name;
        out.push_back(ch);
    }
    // A clip's linked sound changing the same way as its picture is the same change.
    auto partnerChanged = [&](const TimelineChange& c, const Sequence& s, Id id) {
        const Clip* clip = nullptr;
        for (TrackRef r : allTracks(s))
            for (const Clip& x : trackAt(s, r)->clips)
                if (x.id == id) clip = &x;
        if (!clip || !clip->linkGroup) return false;
        const bool removed = c.kind == ChangeKind::Removed;
        for (const TimelineChange& o : out)
            if (&o != &c && o.kind == c.kind && o.track.kind == TrackKind::Video) {
                const Id oid = removed ? o.before : o.after;
                for (const Clip& x : trackAt(s, TrackRef{TrackKind::Video, o.track.index})->clips)
                    if (x.id == oid && x.linkGroup == clip->linkGroup) return true;
            }
        return false;
    };
    std::vector<TimelineChange> kept;
    for (const TimelineChange& c : out)
        if (c.track.kind != TrackKind::Audio ||
            !partnerChanged(c, c.kind == ChangeKind::Removed ? before : after, c.kind == ChangeKind::Removed ? c.before : c.after))
            kept.push_back(c);
    std::stable_sort(kept.begin(), kept.end(), [](const TimelineChange& x, const TimelineChange& y) {
        return std::make_tuple(x.at, int(x.track.kind), x.track.index) < std::make_tuple(y.at, int(y.track.kind), y.track.index);
    });
    return kept;
}

}  // namespace montage
