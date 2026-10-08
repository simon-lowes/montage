#include "Checkerboard.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "Transcript.h"

namespace montage {

namespace {

bool freeOver(const Track& t, FrameTime a, FrameTime b) {
    for (const Clip& c : t.clips)
        if (c.start < b && c.end() > a) return false;
    return true;
}

}  // namespace

edit::Result checkerboardBySpeaker(Project& p, Sequence& s, Id clipId, int* speakerCount) {
    auto loc = edit::locate(s, clipId);
    if (!loc || loc->track.kind != TrackKind::Audio) return edit::Result::fail("Choose an audio clip");
    const Clip c = s.audioTracks[size_t(loc->track.index)].clips[loc->index];
    if (s.audioTracks[size_t(loc->track.index)].locked) return edit::Result::fail("The track is locked");
    if (std::fabs(c.speed - 1) > 1e-9 || c.reverse || c.ramped()) return edit::Result::fail("The clip changes speed");
    const MediaItem* m = p.findMedia(c.mediaId);
    if (!m || !m->transcript) return edit::Result::fail("Transcribe the clip's media first (with speaker labels)");
    const double fps = s.fpsValue();
    const double from = c.sourceIn / fps, to = (c.sourceIn + double(c.duration)) / fps;
    // Who speaks when inside the clip, in order, the same speaker's segments joined.
    struct Turn {
        double start, end;
        int speaker;
    };
    std::vector<Turn> turns;
    for (const TranscriptSegment& seg : m->transcript->segments) {
        if (seg.speaker < 0 || seg.end <= from || seg.start >= to) continue;
        if (!turns.empty() && turns.back().speaker == seg.speaker) turns.back().end = std::max(turns.back().end, seg.end);
        else turns.push_back({seg.start, seg.end, seg.speaker});
    }
    std::vector<int> order;  // speakers by first appearance
    for (const Turn& t : turns)
        if (std::find(order.begin(), order.end(), t.speaker) == order.end()) order.push_back(t.speaker);
    if (speakerCount) *speakerCount = int(order.size());
    if (turns.empty()) return edit::Result::fail("The transcript has no speaker labels: transcribe with speakers");
    if (order.size() < 2) return edit::Result::fail("Only one person speaks in this clip");
    // Cut in the middle of the gap between two people (or of their overlap).
    std::vector<std::pair<FrameTime, int>> cuts;  // frame, speaker from there
    for (size_t i = 0; i + 1 < turns.size(); ++i) {
        const double at = (turns[i].end + turns[i + 1].start) / 2;
        const FrameTime f = c.start + FrameTime(std::llround(at * fps - c.sourceIn));
        if (f <= c.start || f >= c.end() || (!cuts.empty() && f <= cuts.back().first)) continue;
        cuts.push_back({f, turns[i + 1].speaker});
    }
    if (cuts.empty()) return edit::Result::fail("The speakers change too quickly to split");
    // Split, left to right; each piece knows its speaker.
    std::vector<std::pair<Id, int>> pieces = {{c.id, turns.front().speaker}};
    for (const auto& [f, speaker] : cuts) {
        const edit::Result r = edit::razor(p, s, loc->track, f);
        if (!r.ok || r.created.empty()) return edit::Result::fail(r.error.empty() ? "Could not split the clip" : r.error);
        pieces.push_back({r.created.front(), speaker});
    }
    // Each other speaker's pieces to a track of their own.
    edit::Result res;
    const int home = loc->track.index;
    std::map<int, int> trackOf;  // speaker -> audio track index
    trackOf[order.front()] = home;
    for (size_t k = 1; k < order.size(); ++k) {
        const int speaker = order[k];
        std::vector<std::pair<FrameTime, FrameTime>> spans;
        for (const auto& [id, spk] : pieces)
            if (spk == speaker)
                if (const Clip* pc = edit::clipById(s, id)) spans.push_back({pc->start, pc->end()});
        int chosen = -1;
        for (int ti = home + 1; ti < int(s.audioTracks.size()) && chosen < 0; ++ti) {
            bool taken = false;
            for (const auto& [sp, tr] : trackOf) taken |= tr == ti;
            const Track& t = s.audioTracks[size_t(ti)];
            if (taken || t.locked) continue;
            if (std::all_of(spans.begin(), spans.end(), [&](const auto& sp) { return freeOver(t, sp.first, sp.second); })) chosen = ti;
        }
        if (chosen < 0) {
            chosen = edit::addTrack(p, s, TrackKind::Audio).index;
            s.audioTracks[size_t(chosen)].name = speakerName(*m->transcript, speaker);
        }
        trackOf[speaker] = chosen;
    }
    Track& src = s.audioTracks[size_t(home)];
    for (const auto& [id, speaker] : pieces) {
        res.created.push_back(id);
        const int to = trackOf[speaker];
        if (to == home) continue;
        auto it = std::find_if(src.clips.begin(), src.clips.end(), [&](const Clip& x) { return x.id == id; });
        if (it == src.clips.end()) continue;
        Clip moved = *it;
        moved.linkGroup = 0;  // its own now: moving it does not move the picture
        src.clips.erase(it);
        // Crossfades and fades with it stay behind with the clips they join.
        src.transitions.erase(std::remove_if(src.transitions.begin(), src.transitions.end(),
                                             [&](const Transition& tr) { return tr.clipA == id || tr.clipB == id; }),
                              src.transitions.end());
        Track& dst = s.audioTracks[size_t(to)];
        dst.clips.push_back(moved);
        edit::normalize(dst);
    }
    edit::normalize(src);
    return res;
}

}  // namespace montage
