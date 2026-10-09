#include "Reconform.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <sstream>

#include "Captions.h"
#include "EditOps.h"
#include "History.h"
#include "MediaLog.h"

namespace montage {

const char* cutEventName(CutEventKind k) {
    switch (k) {
        case CutEventKind::Same: return "Same";
        case CutEventKind::Moved: return "Moved";
        case CutEventKind::Inserted: return "Inserted";
        case CutEventKind::Extended: return "Extended";
        case CutEventKind::Deleted: return "Deleted";
        case CutEventKind::Trimmed: return "Trimmed";
    }
    return "";
}

int CutChanges::changed() const {
    return int(std::count_if(events.begin(), events.end(), [](const CutEvent& e) { return e.kind != CutEventKind::Same; }));
}

namespace {

bool sameRate(Rational a, Rational b) { return int64_t(a.num) * b.den == int64_t(b.num) * a.den; }

// A stretch of a cut showing one clip (or black).
struct Segment {
    FrameTime t0 = 0, t1 = 0;
    const Clip* clip = nullptr;
    std::string key;     // what it shows: the file (and multicam angle), a nested sequence, a generator and its settings
    bool still = false;  // the same picture throughout: a still image, or black
    std::string name() const { return clip ? clip->name : "Black"; }
};

const Clip* clipAt(const Track& t, FrameTime f) {
    auto it = std::upper_bound(t.clips.begin(), t.clips.end(), f, [](FrameTime v, const Clip& c) { return v < c.start; });
    if (it == t.clips.begin()) return nullptr;
    --it;
    return it->contains(f) ? &*it : nullptr;
}

std::string pictureKey(const Project& p, const Clip& c, bool& still) {
    still = false;
    if (c.isGenerator()) {
        std::ostringstream k;
        k << "gen:" << c.generator.type;
        for (const auto& [n, v] : c.generator.strings) k << '|' << n << '=' << v;
        for (const auto& [n, v] : c.generator.params) k << '|' << n << '=' << v.value << (v.keys.empty() ? "" : "~");
        return k.str();
    }
    const MediaItem* m = p.findMedia(c.mediaId);
    if (!m) return "missing:" + std::to_string(c.mediaId);
    std::string key;
    if (m->kind == MediaKind::Sequence) {
        const Sequence* n = p.findSequence(m->sequenceId);
        key = "seq:" + (n ? n->name : m->name);
    } else {
        key = "file:" + (m->path.empty() ? m->name : m->path);
    }
    if (c.angle) key += "#" + std::to_string(c.angle);
    still = m->kind == MediaKind::Image && m->duration <= 0;
    return key;
}

// What each frame shows, as runs of the same clip: the topmost enabled clip with media on a track whose output is on;
// a title, matte or other generator only where no media is under it; adjustment layers never.
std::vector<Segment> segments(const Project& p, const Sequence& s, FrameTime length) {
    std::vector<const Track*> tracks;
    for (size_t i = s.videoTracks.size(); i-- > 0;)
        if (!s.videoTracks[i].muted) tracks.push_back(&s.videoTracks[i]);
    std::vector<Segment> out;
    for (FrameTime f = 0; f < length; ++f) {
        const Clip *media = nullptr, *graphic = nullptr;
        for (const Track* t : tracks) {
            const Clip* c = clipAt(*t, f);
            if (!c || !c->enabled) continue;
            if (c->isGenerator()) {
                if (c->generator.type != "adjustment" && !graphic) graphic = c;
                continue;
            }
            media = c;
            break;
        }
        const Clip* shown = media ? media : graphic;
        if (!out.empty() && out.back().clip == shown && out.back().t1 == f) {
            ++out.back().t1;
            continue;
        }
        Segment g;
        g.t0 = f;
        g.t1 = f + 1;
        g.clip = shown;
        if (shown) {
            g.key = pictureKey(p, *shown, g.still);
        } else {
            g.key = "black";
            g.still = true;
        }
        out.push_back(std::move(g));
    }
    return out;
}

// New frames [t0, t1) showing what old frames t + d showed.
struct Run {
    FrameTime t0 = 0, t1 = 0, d = 0;
    FrameTime length() const { return t1 - t0; }
};

void matchSegments(const Segment& a, const Segment& b, std::vector<Run>& runs) {
    if (a.still || b.still) {
        // The same picture all through: lined up from the start, as much as both have.
        const FrameTime n = std::min(a.t1 - a.t0, b.t1 - b.t0);
        runs.push_back({b.t0, b.t0 + n, a.t0 - b.t0});
        return;
    }
    const Clip& ca = *a.clip;
    const Clip& cb = *b.clip;
    auto rate = [](const Clip& c) { return c.reverse ? -c.speed : c.speed; };
    if (!ca.ramped() && !cb.ramped()) {
        // Constant speeds: the same footage at the same speed lines up at one offset, if within half a source frame.
        const double r = rate(ca);
        if (std::fabs(r - rate(cb)) > 1e-9 || r == 0) return;
        const double exact = (cb.sourceFrameAt(b.t0) - ca.sourceFrameAt(a.t0)) / r + double(a.t0 - b.t0);
        const FrameTime d = FrameTime(std::llround(exact));
        if (std::fabs(double(d) - exact) * std::fabs(r) >= 0.5) return;
        const FrameTime t0 = std::max(b.t0, a.t0 - d), t1 = std::min(b.t1, a.t1 - d);
        if (t1 > t0) runs.push_back({t0, t1, d});
        return;
    }
    // Speed ramps: lined up on the first frame (and on the last), the frames showing the same source frame.
    FrameTime lastD = std::numeric_limits<FrameTime>::min();
    for (FrameTime from : {b.t0, b.t1 - 1}) {
        const double local = ca.localForSource(cb.sourceFrameAt(from));
        const FrameTime d = ca.start + FrameTime(std::llround(local)) - from;
        if (d == lastD) continue;
        lastD = d;
        const FrameTime t0 = std::max(b.t0, a.t0 - d), t1 = std::min(b.t1, a.t1 - d);
        FrameTime start = -1;
        for (FrameTime t = t0; t <= t1; ++t) {
            const bool same = t < t1 && std::llround(ca.sourceFrameAt(t + d)) == std::llround(cb.sourceFrameAt(t));
            if (same && start < 0) start = t;
            if (!same && start >= 0) {
                if (t - start >= 2 || (start == b.t0 && t == b.t1)) runs.push_back({start, t, d});
                start = -1;
            }
        }
    }
}

std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

}  // namespace

CutChanges cutChanges(const Project& p, const Sequence& oldCut, const Sequence& newCut, std::string* error) {
    return cutChanges(p, oldCut, p, newCut, error);
}

CutChanges cutChanges(const Project& pOld, const Sequence& oldCut, const Project& pNew, const Sequence& newCut, std::string* error) {
    CutChanges out;
    out.fps = newCut.fps;
    out.oldName = oldCut.name;
    out.newName = newCut.name;
    out.oldLength = oldCut.duration();
    out.newLength = newCut.duration();
    if (!sameRate(oldCut.fps, newCut.fps)) {
        if (error) *error = "The two cuts have different frame rates";
        return out;
    }
    const std::vector<Segment> A = segments(pOld, oldCut, out.oldLength), B = segments(pNew, newCut, out.newLength);
    std::map<std::string, std::vector<size_t>> byKey;
    for (size_t i = 0; i < A.size(); ++i) byKey[A[i].key].push_back(i);

    // Every stretch the new cut shares with the old, then the longest first (the nearest to where it was among equals),
    // each new frame taken once. An old stretch may be taken twice (a shot used again).
    std::vector<Run> runs;
    for (const Segment& b : B) {
        auto it = byKey.find(b.key);
        if (it == byKey.end()) continue;
        for (size_t ai : it->second) matchSegments(A[ai], b, runs);
    }
    std::stable_sort(runs.begin(), runs.end(), [](const Run& x, const Run& y) {
        if (x.length() != y.length()) return x.length() > y.length();
        if (std::llabs(x.d) != std::llabs(y.d)) return std::llabs(x.d) < std::llabs(y.d);
        return x.t0 < y.t0;
    });
    std::vector<char> covered(size_t(std::max<FrameTime>(0, out.newLength)), 0);
    std::vector<Run> pieces;
    for (const Run& r : runs) {
        FrameTime start = -1;
        for (FrameTime t = r.t0; t <= r.t1; ++t) {
            const bool open = t < r.t1 && !covered[size_t(t)];
            if (open && start < 0) start = t;
            if (!open && start >= 0) {
                pieces.push_back({start, t, r.d});
                start = -1;
            }
        }
        for (FrameTime t = r.t0; t < r.t1; ++t) covered[size_t(t)] = 1;
    }
    std::sort(pieces.begin(), pieces.end(), [](const Run& x, const Run& y) { return x.t0 < y.t0; });
    std::vector<Run> merged;
    for (const Run& r : pieces) {
        if (!merged.empty() && merged.back().t1 == r.t0 && merged.back().d == r.d) merged.back().t1 = r.t1;
        else merged.push_back(r);
    }
    pieces = std::move(merged);

    // The stretches still in their old order (the most frames, each after the last in the old cut) are the same; the
    // rest moved.
    const size_t k = pieces.size();
    std::vector<FrameTime> best(k, 0);
    std::vector<int> prev(k, -1);
    for (size_t i = 0; i < k; ++i) {
        best[i] = pieces[i].length();
        const FrameTime start = pieces[i].t0 + pieces[i].d;
        for (size_t j = 0; j < i; ++j)
            if (pieces[j].t1 + pieces[j].d <= start && best[j] + pieces[i].length() > best[i]) {
                best[i] = best[j] + pieces[i].length();
                prev[i] = int(j);
            }
    }
    std::vector<char> inOrder(k, 0);
    if (k) {
        int end = int(std::max_element(best.begin(), best.end()) - best.begin());
        for (int i = end; i >= 0; i = prev[size_t(i)]) inOrder[size_t(i)] = 1;
    }

    auto segmentAt = [](const std::vector<Segment>& segs, FrameTime t) {
        auto it = std::upper_bound(segs.begin(), segs.end(), t, [](FrameTime v, const Segment& s) { return v < s.t0; });
        return size_t(it - segs.begin()) - 1;
    };
    for (size_t i = 0; i < k; ++i) {
        const Run& r = pieces[i];
        CutEvent e;
        e.kind = inOrder[i] ? CutEventKind::Same : CutEventKind::Moved;
        e.newIn = r.t0;
        e.newOut = r.t1;
        e.oldIn = r.t0 + r.d;
        e.oldOut = r.t1 + r.d;
        const size_t first = segmentAt(B, r.t0), last = segmentAt(B, r.t1 - 1);
        e.shot = B[first].name();
        e.shots = int(last - first + 1);
        out.events.push_back(e);
    }
    // New material: more of a shot the new cut otherwise keeps, or a shot (or black) the old cut did not have.
    for (const Segment& b : B) {
        FrameTime start = -1;
        for (FrameTime t = b.t0; t <= b.t1; ++t) {
            const bool open = t < b.t1 && !covered[size_t(t)];
            if (open && start < 0) start = t;
            if (!open && start >= 0) {
                CutEvent e;
                const bool extends = (start > b.t0 && covered[size_t(start - 1)]) || (t < b.t1 && covered[size_t(t)]);
                e.kind = extends ? CutEventKind::Extended : CutEventKind::Inserted;
                e.newIn = start;
                e.newOut = t;
                e.oldIn = e.oldOut = -1;
                e.shot = b.name();
                out.events.push_back(e);
                start = -1;
            }
        }
    }
    // What the new cut no longer plays: part of a shot it keeps some of, or all of one; placed where it would have been.
    std::vector<char> used(size_t(std::max<FrameTime>(0, out.oldLength)), 0);
    for (const Run& r : pieces)
        for (FrameTime t = r.t0 + r.d; t < r.t1 + r.d; ++t)
            if (t >= 0 && t < out.oldLength) used[size_t(t)] = 1;
    auto placeOf = [&](FrameTime from, FrameTime to) {
        for (const Run& r : pieces)
            if (r.t1 + r.d == from) return r.t1;
        for (const Run& r : pieces)
            if (r.t0 + r.d == to) return r.t0;
        FrameTime at = 0, bestEnd = std::numeric_limits<FrameTime>::min();
        for (const Run& r : pieces)
            if (r.t1 + r.d <= from && r.t1 + r.d > bestEnd) bestEnd = r.t1 + r.d, at = r.t1;
        return at;
    };
    for (const Segment& a : A) {
        FrameTime start = -1;
        for (FrameTime t = a.t0; t <= a.t1; ++t) {
            const bool open = t < a.t1 && !used[size_t(t)];
            if (open && start < 0) start = t;
            if (!open && start >= 0) {
                CutEvent e;
                const bool trims = (start > a.t0 && used[size_t(start - 1)]) || (t < a.t1 && used[size_t(t)]);
                e.kind = trims ? CutEventKind::Trimmed : CutEventKind::Deleted;
                e.oldIn = start;
                e.oldOut = t;
                e.newIn = e.newOut = placeOf(start, t);
                e.shot = a.name();
                out.events.push_back(e);
                start = -1;
            }
        }
    }
    auto removal = [](const CutEvent& e) { return e.kind == CutEventKind::Deleted || e.kind == CutEventKind::Trimmed; };
    std::stable_sort(out.events.begin(), out.events.end(), [&](const CutEvent& x, const CutEvent& y) {
        if (x.newIn != y.newIn) return x.newIn < y.newIn;
        if (removal(x) != removal(y)) return removal(x);
        return x.oldIn < y.oldIn;
    });
    return out;
}

std::string changeListCsv(const CutChanges& c) {
    std::ostringstream out;
    out << "Event,Change,Shot,Shots,Old In,Old Out,New In,New Out,Length,Shift\n";
    auto tc = [&](FrameTime f) { return formatTimecode(f, c.fps); };
    int n = 0;
    for (const CutEvent& e : c.events) {
        const bool hasOld = e.kind != CutEventKind::Inserted && e.kind != CutEventKind::Extended;
        const bool kept = e.kind == CutEventKind::Same || e.kind == CutEventKind::Moved;
        out << ++n << ',' << cutEventName(e.kind) << ',' << csvField(e.shot) << ',' << e.shots << ',' << (hasOld ? tc(e.oldIn) : "")
            << ',' << (hasOld ? tc(e.oldOut) : "") << ',' << tc(e.newIn) << ',' << tc(e.newOut) << ',' << e.length() << ',';
        if (kept) out << (e.shift() > 0 ? "+" : "") << e.shift();
        out << '\n';
    }
    return out.str();
}

std::string changeEdl(const CutChanges& c, const std::string& oldReel, const std::string& newReel) {
    std::ostringstream out;
    out << "TITLE: " << c.newName << " (changes from " << c.oldName << ")\n";
    out << "FCM: " << (isDropFrameRate(c.fps) ? "DROP FRAME" : "NON-DROP FRAME") << "\n\n";
    auto tc = [&](FrameTime f) { return formatTimecode(f, c.fps); };
    std::vector<std::string> pending;  // what was taken out, noted on the next event
    int n = 0;
    for (const CutEvent& e : c.events) {
        if (e.kind == CutEventKind::Deleted || e.kind == CutEventKind::Trimmed) {
            pending.push_back(std::string("* ") + (e.kind == CutEventKind::Deleted ? "DELETED" : "TRIMMED") + ": " + e.shot + " " +
                              tc(e.oldIn) + " " + tc(e.oldOut));
            continue;
        }
        const bool old = e.kind == CutEventKind::Same || e.kind == CutEventKind::Moved;
        const bool black = !old && e.shot == "Black";
        std::string reel = old ? oldReel : black ? std::string("BL") : newReel;
        if (reel.size() > 8) reel.resize(8);
        const FrameTime srcIn = old ? e.oldIn : black ? 0 : e.newIn;
        const FrameTime srcOut = srcIn + (e.newOut - e.newIn);
        char line[160];
        std::snprintf(line, sizeof line, "%03d  %-8s V     C        %s %s %s %s\n", ++n, reel.c_str(), tc(srcIn).c_str(), tc(srcOut).c_str(),
                      tc(e.newIn).c_str(), tc(e.newOut).c_str());
        out << line;
        out << "* FROM CLIP NAME: " << e.shot << (e.shots > 1 ? " (+" + std::to_string(e.shots - 1) + " more)" : "") << "\n";
        std::string change = cutEventName(e.kind);
        std::transform(change.begin(), change.end(), change.begin(), [](unsigned char ch) { return char(std::toupper(ch)); });
        out << "* CHANGE: " << change;
        if (old && e.shift()) out << " (" << (e.shift() > 0 ? "+" : "") << e.shift() << " FRAMES)";
        out << "\n";
        for (const std::string& p : pending) out << p << "\n";
        pending.clear();
        out << "\n";
    }
    for (const std::string& p : pending) out << p << "\n";
    return out.str();
}

namespace {

// A stretch [i0, i1) of the source placed at j0.
struct Piece {
    FrameTime i0 = 0, i1 = 0, j0 = 0;
    FrameTime shift() const { return j0 - i0; }
};

// Timeline keyframes (track automation, track, bus and master effects) carried with the pieces: those inside each,
// with keys at its ends holding the curve's values there.
Param remapParam(const Param& in, const std::vector<Piece>& pieces) {
    if (in.keys.empty()) return in;
    Param out = in;
    out.keys.clear();
    out.repeat = Repeat::Hold;
    for (const Piece& pc : pieces) {
        const FrameTime last = pc.i1 - 1;
        std::vector<Keyframe> inside;
        Interp before = Interp::Linear;
        for (const Keyframe& k : in.keys) {
            if (k.t < pc.i0) before = k.interp;
            if (k.t >= pc.i0 && k.t <= last) inside.push_back(k);
        }
        if (inside.empty() || inside.front().t != pc.i0) {
            Keyframe k;
            k.t = pc.i0;
            k.v = in.at(pc.i0);
            k.interp = before == Interp::Bezier ? Interp::Linear : before;
            inside.insert(inside.begin(), k);
        }
        if (inside.back().t != last) {
            Keyframe k;
            k.t = last;
            k.v = in.at(last);
            inside.push_back(k);
        }
        for (Keyframe k : inside) {
            k.t += pc.shift();
            out.keys.push_back(k);
        }
    }
    std::stable_sort(out.keys.begin(), out.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
    std::vector<Keyframe> unique;
    for (const Keyframe& k : out.keys) {
        if (!unique.empty() && unique.back().t == k.t) unique.back() = k;
        else unique.push_back(k);
    }
    out.keys = std::move(unique);
    return out;
}

void remapEffects(std::vector<Effect>& effects, const std::vector<Piece>& pieces) {
    for (Effect& e : effects)
        for (auto& [name, param] : e.params) param = remapParam(param, pieces);
}

}  // namespace

ReconformResult reconformSequence(Project& p, Id sourceId, const CutChanges& changes, Id newCutId, const ReconformOptions& options,
                                  std::string* error) {
    ReconformResult result;
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return result;
    };
    const Sequence* found = p.findSequence(sourceId);
    if (!found) return fail("No such sequence");
    if (!sameRate(found->fps, changes.fps)) return fail("The sequence's frame rate is not the cuts'");
    if (changes.events.empty()) return fail("There are no changes to conform to");
    const Sequence source = *found;  // copies: the project's sequences grow below
    const Sequence* foundNew = p.findSequence(newCutId);
    const bool haveNew = foundNew != nullptr;
    const Sequence newCut = haveNew ? *foundNew : Sequence{};

    std::vector<Piece> pieces;
    std::vector<const CutEvent*> inserts, removals;
    for (const CutEvent& e : changes.events) {
        if (e.kind == CutEventKind::Same || e.kind == CutEventKind::Moved) pieces.push_back({e.oldIn, e.oldOut, e.newIn});
        else if (e.kind == CutEventKind::Inserted || e.kind == CutEventKind::Extended) inserts.push_back(&e);
        else removals.push_back(&e);
    }
    const std::string name = options.name.empty() ? source.name + " (Conformed)" : options.name;
    const Id id = edit::duplicateSequence(p, sourceId, name);
    if (!id) return fail("No such sequence");
    Sequence& out = *p.findSequence(id);

    auto renew = [&](Effect& e) {
        if (e.id) e.id = p.newId();
    };
    std::map<std::pair<size_t, Id>, Id> groups;  // (stretch, link group) -> the copies' link group
    auto copyClip = [&](const Clip& c, FrameTime from, FrameTime to, FrameTime shift, size_t stretch) {
        Clip piece = edit::subClip(c, from, to);
        piece.start += shift;
        piece.id = p.newId();
        if (c.linkGroup) {
            auto [it, added] = groups.try_emplace({stretch, c.linkGroup}, 0);
            if (added) it->second = p.newId();
            piece.linkGroup = it->second;
        }
        for (Effect* e : {&piece.generator, &piece.motion, &piece.audio, &piece.timing}) renew(*e);
        for (Effect& e : piece.effects) renew(e);
        return piece;
    };
    auto rebuild = [&](const Track& from, Track& to) {
        to.clips.clear();
        to.transitions.clear();
        for (size_t k = 0; k < pieces.size(); ++k) {
            const Piece& pc = pieces[k];
            std::map<Id, Id> ids;                         // a source clip -> its copy in this stretch
            std::map<Id, std::pair<bool, bool>> wholeEnds;  // whether the copy keeps the clip's start, its end
            for (const Clip& c : from.clips) {
                if (c.end() <= pc.i0 || c.start >= pc.i1) continue;
                Clip piece = copyClip(c, std::max(c.start, pc.i0), std::min(c.end(), pc.i1), pc.shift(), k);
                ids[c.id] = piece.id;
                wholeEnds[c.id] = {c.start >= pc.i0, c.end() <= pc.i1};
                to.clips.push_back(std::move(piece));
            }
            // A dissolve where both its clips came over; a fade in (out) where its clip's start (end) did.
            for (const Transition& tr : from.transitions) {
                const bool a = tr.clipA && ids.count(tr.clipA), b = tr.clipB && ids.count(tr.clipB);
                const bool keep = tr.clipA && tr.clipB ? a && b : tr.clipA ? a && wholeEnds[tr.clipA].second : b && wholeEnds[tr.clipB].first;
                if (!keep) continue;
                Transition t = tr;
                t.id = p.newId();
                renew(t.params);
                t.clipA = tr.clipA ? ids[tr.clipA] : 0;
                t.clipB = tr.clipB ? ids[tr.clipB] : 0;
                to.transitions.push_back(t);
            }
        }
        to.volumeAuto = remapParam(from.volumeAuto, pieces);
        to.panAuto = remapParam(from.panAuto, pieces);
        remapEffects(to.effects, pieces);
    };
    for (size_t i = 0; i < out.videoTracks.size() && i < source.videoTracks.size(); ++i) rebuild(source.videoTracks[i], out.videoTracks[i]);
    for (size_t i = 0; i < out.audioTracks.size() && i < source.audioTracks.size(); ++i) rebuild(source.audioTracks[i], out.audioTracks[i]);
    remapEffects(out.masterEffects, pieces);
    for (Bus& b : out.buses) remapEffects(b.effects, pieces);

    // Markers and captions inside the stretches go with them.
    out.markers.clear();
    for (const Piece& pc : pieces)
        for (const Marker& m : source.markers)
            if (m.t >= pc.i0 && m.t < pc.i1) {
                Marker c = m;
                c.t += pc.shift();
                c.duration = std::min(m.duration, pc.i1 - m.t);
                out.markers.push_back(c);
            }
    for (size_t ct = 0; ct < out.captionTracks.size() && ct < source.captionTracks.size(); ++ct) {
        std::vector<Caption> caps;
        for (const Piece& pc : pieces)
            for (const Caption& cap : source.captionTracks[ct].captions) {
                if (cap.end <= pc.i0 || cap.start >= pc.i1) continue;
                Caption c = cap;
                const FrameTime s = std::max(cap.start, pc.i0), e = std::min(cap.end, pc.i1);
                if (s != cap.start || e != cap.end) c.wordTimes.clear();  // timed across the whole caption
                c.start = s + pc.shift();
                c.end = e + pc.shift();
                caps.push_back(std::move(c));
            }
        std::stable_sort(caps.begin(), caps.end(), [](const Caption& a, const Caption& b) { return a.start < b.start; });
        out.captionTracks[ct].captions = std::move(caps);
    }

    // New material from the new cut, on the same tracks (more where it has more), labelled to stand out.
    const int newLabel = std::max(0, labelFromName("Forest"));
    if (options.fillFromNewCut && haveNew) {
        for (size_t k = 0; k < inserts.size(); ++k) {
            const CutEvent& e = *inserts[k];
            for (TrackRef r : allTracks(newCut)) {
                const Track* from = trackAt(newCut, r);
                bool any = false;
                for (const Clip& c : from->clips) any = any || (c.end() > e.newIn && c.start < e.newOut);
                if (!any) continue;
                auto& list = r.kind == TrackKind::Video ? out.videoTracks : out.audioTracks;
                while (int(list.size()) <= r.index) edit::addTrack(p, out, r.kind);
                Track& to = list[size_t(r.index)];
                for (const Clip& c : from->clips) {
                    if (c.end() <= e.newIn || c.start >= e.newOut) continue;
                    Clip piece = copyClip(c, std::max(c.start, e.newIn), std::min(c.end(), e.newOut), 0, pieces.size() + k);
                    piece.colorLabel = newLabel;
                    to.clips.push_back(std::move(piece));
                }
            }
        }
    }
    for (auto* tracks : {&out.videoTracks, &out.audioTracks})
        for (Track& t : *tracks) edit::normalize(t);

    if (options.markers) {
        auto tc = [&](FrameTime f) { return formatTimecode(f, changes.fps); };
        for (const CutEvent* e : inserts)
            out.markers.push_back(Marker{e->newIn, e->newOut - e->newIn, std::string(cutEventName(e->kind)) + ": " + e->shot,
                                         "New material from " + changes.newName,
                                         std::max(0, labelFromName(e->kind == CutEventKind::Inserted ? "Forest" : "Yellow"))});
        for (const CutEvent* e : removals)
            out.markers.push_back(Marker{e->newIn, 0, std::string(cutEventName(e->kind)) + ": " + e->shot,
                                         "Was " + tc(e->oldIn) + " to " + tc(e->oldOut) + " in " + changes.oldName, std::max(0, labelFromName("Red"))});
    }
    std::stable_sort(out.markers.begin(), out.markers.end(), [](const Marker& a, const Marker& b) { return a.t < b.t; });
    out.inPoint = out.outPoint = -1;
    out.playhead = 0;

    result.sequence = id;
    result.pieces = int(pieces.size());
    result.inserts = int(inserts.size());
    for (const auto* tracks : {&out.videoTracks, &out.audioTracks})
        for (const Track& t : *tracks) result.clips += int(t.clips.size());
    return result;
}

}  // namespace montage
