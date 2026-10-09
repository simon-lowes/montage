#include "FaceTracks.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

#include "Decoder.h"
#include "core/Effects.h"
#include "core/FaceIndex.h"
#include "core/Model.h"
#include "core/Numbers.h"

namespace montage {

namespace {

float cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || a.size() != b.size()) return 0;
    double d = 0;
    for (size_t i = 0; i < a.size(); ++i) d += double(a[i]) * b[i];
    return float(d);
}

void normalise(std::vector<float>& v) {
    double len = 0;
    for (float x : v) len += double(x) * x;
    if (len <= 0) {
        v.clear();
        return;
    }
    for (float& x : v) x = float(x / std::sqrt(len));
}

void accumulate(std::vector<float>& sum, const std::vector<float>& v) {
    if (v.empty()) return;
    if (sum.empty()) sum.assign(v.size(), 0.0f);
    if (sum.size() != v.size()) return;
    for (size_t i = 0; i < v.size(); ++i) sum[i] += v[i];
}

std::string identityToHex(const std::vector<float>& v) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(v.size() * 2);
    for (float x : v) {
        const auto q = uint8_t(int8_t(std::clamp(std::lround(x * 127.0f), -127L, 127L)));
        s += digits[q >> 4];
        s += digits[q & 15];
    }
    return s;
}

std::vector<float> identityFromHex(const std::string& s) {
    auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    std::vector<float> v;
    if (s.size() % 2) return v;
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        const int hi = nibble(s[i]), lo = nibble(s[i + 1]);
        if (hi < 0 || lo < 0) return {};
        v.push_back(float(int8_t(uint8_t(hi * 16 + lo))) / 127.0f);
    }
    normalise(v);
    return v;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

}  // namespace

const FaceTrack* FaceTracks::track(int id) const {
    for (const FaceTrack& t : tracks)
        if (t.id == id) return &t;
    return nullptr;
}

// "faces1 fps start end step" then one line per track: "id score identity|t x y w h;t x y w h;...".
std::string faceTracksToString(const FaceTracks& t) {
    std::string s = "faces1 " + formatNumber(t.fps, 8) + " " + formatNumber(t.start, 10) + " " + formatNumber(t.end, 10) + " " +
                    formatNumber(t.step, 8) + "\n";
    for (const FaceTrack& tr : t.tracks) {
        s += std::to_string(tr.id) + " " + formatNumber(tr.score, 3) + " " + (tr.identity.empty() ? "-" : identityToHex(tr.identity)) + "|";
        for (size_t i = 0; i < tr.boxes.size(); ++i) {
            const FaceTrack::Box& b = tr.boxes[i];
            if (i) s += ';';
            s += formatNumber(b.time, 10) + " " + formatNumber(b.x, 4) + " " + formatNumber(b.y, 4) + " " + formatNumber(b.w, 4) + " " +
                 formatNumber(b.h, 4);
        }
        s += "\n";
    }
    return s;
}

bool faceTracksFromString(const std::string& text, FaceTracks& out) {
    std::vector<std::string> lines = split(text, '\n');
    if (lines.empty()) return false;
    std::vector<std::string> head = split(lines[0], ' ');
    if (head.size() != 5 || head[0] != "faces1") return false;
    FaceTracks t;
    t.fps = parseNumber(head[1], 25);
    t.start = parseNumber(head[2]);
    t.end = parseNumber(head[3]);
    t.step = parseNumber(head[4]);
    if (!(t.fps > 0) || t.end < t.start || t.step < 0) return false;
    for (size_t n = 1; n < lines.size(); ++n) {
        if (lines[n].empty()) continue;
        const size_t bar = lines[n].find('|');
        if (bar == std::string::npos) return false;
        std::vector<std::string> h = split(lines[n].substr(0, bar), ' ');
        if (h.size() != 3) return false;
        FaceTrack tr;
        tr.id = int(parseNumber(h[0], -1));
        if (tr.id <= 0) return false;
        tr.score = float(parseNumber(h[1]));
        if (h[2] != "-") {
            tr.identity = identityFromHex(h[2]);
            if (tr.identity.empty()) return false;
        }
        for (const std::string& box : split(lines[n].substr(bar + 1), ';')) {
            if (box.empty()) continue;
            std::vector<std::string> v = split(box, ' ');
            if (v.size() != 5) return false;
            FaceTrack::Box b;
            b.time = parseNumber(v[0]);
            b.x = float(parseNumber(v[1])), b.y = float(parseNumber(v[2])), b.w = float(parseNumber(v[3])), b.h = float(parseNumber(v[4]));
            if (!(b.w > 0) || !(b.h > 0)) return false;
            tr.boxes.push_back(b);
        }
        if (tr.boxes.empty()) return false;
        std::sort(tr.boxes.begin(), tr.boxes.end(), [](const FaceTrack::Box& a, const FaceTrack::Box& b) { return a.time < b.time; });
        t.tracks.push_back(std::move(tr));
    }
    out = std::move(t);
    return true;
}

std::vector<FaceTrack> linkFaceTracks(const std::vector<std::vector<FaceSighting>>& frames, double maxGap, bool keepSingles) {
    struct Building {
        FaceTrack track;
        std::vector<float> sum;    // of identities
        std::vector<float> recent; // the last identity taken
        double last = 0;
        int seen = 0;
    };
    std::vector<Building> all;
    std::vector<size_t> active;
    int nextId = 1;
    for (const std::vector<FaceSighting>& frame : frames) {
        if (frame.empty()) continue;
        const double now = frame.front().time;
        active.erase(std::remove_if(active.begin(), active.end(), [&](size_t i) { return now - all[i].last > maxGap + 1e-9; }), active.end());
        // Every pairing that could be the same face, cheapest first.
        struct Pair {
            double cost;
            size_t track, face;
        };
        std::vector<Pair> pairs;
        for (size_t ti = 0; ti < active.size(); ++ti) {
            const Building& b = all[active[ti]];
            const FaceTrack::Box& l = b.track.boxes.back();
            const double gap = now - b.last;
            for (size_t fi = 0; fi < frame.size(); ++fi) {
                const FaceSighting& f = frame[fi];
                const double size = std::max({l.w, l.h, f.w, f.h});
                const double ratio = std::max(f.w * f.h, l.w * l.h) / std::max(1e-9, double(std::min(f.w * f.h, l.w * l.h)));
                if (ratio > 4) continue;  // twice the width: another face
                const double d = std::hypot((f.x + f.w / 2) - (l.x + l.w / 2), (f.y + f.h / 2) - (l.y + l.h / 2));
                const double reach = size * (0.6 + 1.5 * gap);
                if (d > reach) continue;
                double cost = d / reach + 0.01 * gap;  // (alike, the track seen last)
                const std::vector<float>& who = !b.recent.empty() ? b.recent : b.sum;
                if (!f.identity.empty() && !who.empty()) {
                    std::vector<float> mean = who;
                    normalise(mean);
                    const float c = cosine(mean, f.identity);
                    if (c < 0.2f) continue;  // someone else, though in the same place
                    cost -= 0.25 * c;
                }
                pairs.push_back({cost, ti, fi});
            }
        }
        std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.cost < b.cost; });
        std::vector<char> trackUsed(active.size(), 0), faceUsed(frame.size(), 0);
        for (const Pair& p : pairs) {
            if (trackUsed[p.track] || faceUsed[p.face]) continue;
            trackUsed[p.track] = faceUsed[p.face] = 1;
            Building& b = all[active[p.track]];
            const FaceSighting& f = frame[p.face];
            b.track.boxes.push_back({f.time, f.x, f.y, f.w, f.h});
            b.track.score = std::max(b.track.score, f.score);
            accumulate(b.sum, f.identity);
            if (!f.identity.empty()) b.recent = f.identity;
            b.last = f.time;
            ++b.seen;
        }
        for (size_t fi = 0; fi < frame.size(); ++fi) {
            if (faceUsed[fi]) continue;
            const FaceSighting& f = frame[fi];
            Building b;
            b.track.id = nextId++;
            b.track.boxes.push_back({f.time, f.x, f.y, f.w, f.h});
            b.track.score = f.score;
            accumulate(b.sum, f.identity);
            b.recent = f.identity;
            b.last = f.time;
            b.seen = 1;
            all.push_back(std::move(b));
            active.push_back(all.size() - 1);
        }
    }
    std::vector<FaceTrack> out;
    int id = 1;
    for (Building& b : all) {
        if (b.seen < 2 && !keepSingles && b.track.score < 0.9f) continue;  // a passing false find
        b.track.identity = b.sum;
        normalise(b.track.identity);
        b.track.id = id++;
        out.push_back(std::move(b.track));
    }
    return out;
}

bool trackFaces(const std::string& path, double start, double end, FaceTracks& out, const std::function<bool(double)>& progress,
                std::string* error) {
    std::shared_ptr<FaceModel> model = FaceModel::load(error);
    if (!model) return false;
    VideoDecoder dec;
    if (!dec.open(path, error)) return false;
    const bool still = dec.isStill() || dec.duration() <= 0;
    const double fps = dec.fps() > 0 ? dec.fps() : 25;
    // Decoded with the long side at most 1280, as for People search.
    const int dw = std::max(1, dec.displayWidth()), dh = std::max(1, dec.displayHeight());
    const double k = std::min(1.0, 1280.0 / std::max(dw, dh));
    const int w = std::max(1, int(std::lround(dw * k))), h = std::max(1, int(std::lround(dh * k)));
    const int stride = std::max(1, int(std::lround(fps / 30)));
    FaceTracks result;
    result.fps = fps;
    if (still) {
        result.start = result.end = 0;
        result.step = 0;
    } else {
        const double last = std::max(0.0, dec.duration() - 0.5 / fps);
        result.start = std::clamp(std::min(start, end), 0.0, last);
        result.end = std::clamp(std::max(start, end), result.start, last);
        result.step = stride / fps;
    }
    std::vector<std::vector<FaceSighting>> frames;
    const long first = still ? 0 : long(std::floor(result.start * fps)), lastFrame = still ? 0 : long(std::ceil(result.end * fps));
    const long total = std::max(1L, (lastFrame - first) / stride + 1);
    double previousPts = -1;
    long n = 0;
    for (long frame = first; frame <= lastFrame; frame += stride, ++n) {
        if (progress && !progress(double(n) / double(total))) {
            if (error) *error = "Cancelled";
            return false;
        }
        Frame16Ptr f = dec.frameAt(still ? 0.0 : (double(frame) + 0.25) / fps, w, h, true);
        if (!f) continue;
        const double time = still ? 0.0 : f->pts;
        if (!still && previousPts >= 0 && std::fabs(time - previousPts) < 0.25 / fps) continue;  // the same frame again
        previousPts = time;
        std::vector<FaceSighting> sightings;
        for (const DetectedFace& d : model->detect(*f, 0.7f)) {
            FaceSighting s;
            s.time = time;
            s.x = d.x / float(f->width), s.y = d.y / float(f->height), s.w = d.w / float(f->width), s.h = d.h / float(f->height);
            s.score = d.score;
            // Every frame's identities, so someone stepping in where another person just was starts a track of
            // their own at once (and is not left showing with them).
            if (d.h >= 24) s.identity = model->embed(*f, d);
            sightings.push_back(std::move(s));
        }
        frames.push_back(std::move(sightings));
    }
    if (progress) progress(1.0);
    result.tracks = linkFaceTracks(frames, 1.0, still);
    out = std::move(result);
    return true;
}

std::vector<TrackedFace> trackedFacesAt(const FaceTracks& t, double time, int holdFrames) {
    std::vector<TrackedFace> out;
    const bool still = t.step <= 0;
    const double hold = std::max(0, holdFrames) / t.fps + 0.5 * std::max(t.step, 1 / t.fps);
    const double bridge = std::max(2.0 * std::max(0, holdFrames) / t.fps, 1.5 * t.step);
    auto box = [](const FaceTrack::Box& b) {
        FaceBox f;
        f.x = b.x, f.y = b.y, f.w = b.w, f.h = b.h, f.score = 1;
        return f;
    };
    for (const FaceTrack& tr : t.tracks) {
        if (tr.boxes.empty()) continue;
        if (still) {
            out.push_back({tr.id, box(tr.boxes.front())});
            continue;
        }
        auto after = std::lower_bound(tr.boxes.begin(), tr.boxes.end(), time, [](const FaceTrack::Box& b, double v) { return b.time < v; });
        const FaceTrack::Box* b = after != tr.boxes.end() ? &*after : nullptr;
        const FaceTrack::Box* a = after != tr.boxes.begin() ? &*(after - 1) : nullptr;
        if (b && std::fabs(b->time - time) < 1e-9) {
            out.push_back({tr.id, box(*b)});
        } else if (a && b && b->time - a->time <= bridge + 1e-9) {
            const float u = float((time - a->time) / (b->time - a->time));
            FaceTrack::Box m;
            m.x = a->x + (b->x - a->x) * u, m.y = a->y + (b->y - a->y) * u, m.w = a->w + (b->w - a->w) * u, m.h = a->h + (b->h - a->h) * u;
            out.push_back({tr.id, box(m)});
        } else if (a && time - a->time <= hold) {
            out.push_back({tr.id, box(*a)});
        } else if (b && b->time - time <= hold) {
            out.push_back({tr.id, box(*b)});
        }
    }
    return out;
}

std::vector<FaceGroup> groupFaceTracks(const FaceTracks& t, float threshold) {
    std::vector<const FaceTrack*> order;
    for (const FaceTrack& tr : t.tracks) order.push_back(&tr);
    const double step = t.step > 0 ? t.step : 1 / t.fps;
    std::stable_sort(order.begin(), order.end(), [&](const FaceTrack* a, const FaceTrack* b) { return a->seconds(step) > b->seconds(step); });
    std::vector<FaceGroup> groups;
    std::vector<std::vector<float>> sums;
    std::vector<float> bestSize;
    for (const FaceTrack* tr : order) {
        size_t g = groups.size();
        float best = threshold;
        if (!tr->identity.empty())
            for (size_t i = 0; i < groups.size(); ++i) {
                const float c = cosine(groups[i].identity, tr->identity);
                if (c >= best) best = c, g = i;
            }
        if (g == groups.size()) {
            groups.emplace_back();
            sums.emplace_back();
            bestSize.push_back(-1);
        }
        FaceGroup& G = groups[g];
        G.tracks.push_back(tr->id);
        G.seconds += tr->seconds(t.step > 0 ? t.step : 0);
        accumulate(sums[g], tr->identity);
        G.identity = sums[g];
        normalise(G.identity);
        for (const FaceTrack::Box& b : tr->boxes)
            if (b.w * b.h > bestSize[g]) bestSize[g] = b.w * b.h, G.best = b, G.bestTime = b.time;
    }
    std::stable_sort(groups.begin(), groups.end(), [](const FaceGroup& a, const FaceGroup& b) { return a.seconds > b.seconds; });
    return groups;
}

std::vector<float> personIdentity(const Project& p, int person) {
    std::vector<float> sum;
    for (const MediaItem& m : p.media) {
        if (!m.faces) continue;
        for (size_t i = 0; i < m.faces->faces.size(); ++i)
            if (m.faces->faces[i].person == person) accumulate(sum, m.faces->embedding(i));
    }
    normalise(sum);
    return sum;
}

void matchProjectPeople(const Project& p, std::vector<FaceGroup>& groups, float threshold) {
    std::vector<std::pair<int, std::vector<float>>> people;
    for (const PersonSummary& s : peopleIn(p)) {
        std::vector<float> id = personIdentity(p, s.id);
        if (!id.empty()) people.emplace_back(s.id, std::move(id));
    }
    for (FaceGroup& g : groups) {
        g.person = 0;
        float best = threshold;
        for (const auto& [id, identity] : people) {
            const float c = cosine(g.identity, identity);
            if (c >= best) best = c, g.person = id;
        }
    }
}

void clipMediaSpan(const Sequence& s, const Clip& c, bool still, double& start, double& end) {
    start = end = 0;
    if (still || c.duration <= 0) return;
    const double fps = s.fpsValue();
    start = 1e300, end = -1e300;
    for (FrameTime t = c.start; t < c.end(); ++t) {
        const double sec = c.sourceFrameAt(t) / fps;
        start = std::min(start, sec), end = std::max(end, sec);
    }
    start = std::max(0.0, start);
    end = std::max(start, end);
}

Effect* redactFacesEffectOf(Project& p, Clip& c, bool create) {
    for (Effect& e : c.effects)
        if (e.type == "redact_faces") return &e;
    if (!create) return nullptr;
    c.effects.insert(c.effects.begin(), makeEffect(p, "redact_faces"));
    return &c.effects.front();
}

std::string trackIdsToString(const std::set<int>& ids) {
    std::string s;
    for (int id : ids) s += (s.empty() ? "" : ",") + std::to_string(id);
    return s;
}

std::set<int> trackIdsFromString(const std::string& text) {
    std::set<int> ids;
    for (const std::string& part : split(text, ',')) {
        const double v = parseNumber(part, -1);
        if (v >= 1) ids.insert(int(v));
    }
    return ids;
}

}  // namespace montage
