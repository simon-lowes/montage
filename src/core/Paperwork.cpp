#include "Paperwork.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

#include "EditOps.h"
#include "FilmCount.h"
#include "History.h"
#include "TranscriptEdit.h"

namespace montage {

std::string cueMetadataKey(const std::string& field) { return "cue." + field; }

namespace {

std::string metaOf(const MediaItem& m, const std::string& key) {
    auto it = m.metadata.find(key);
    return it == m.metadata.end() ? std::string() : it->second;
}

std::string stemOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
    return name;
}

bool tracksHeard(const Sequence& seq, size_t t) {
    bool anySolo = false;
    for (const Track& tr : seq.audioTracks) anySolo |= tr.solo;
    const Track& tr = seq.audioTracks[t];
    return anySolo ? tr.solo : !tr.muted;
}

std::string tc(FrameTime f, const Sequence& seq) { return formatTimecode(f, seq.fps); }

// A stretch of the picture as seen: the top clip with media (else the top generator) at each frame, a clip at a time
// (a clip cut in two where it plays on unbroken counts as one).
struct Seen {
    FrameTime in = 0, out = 0;
    const Clip* clip = nullptr;
};

std::vector<Seen> seenShots(const Sequence& seq) {
    std::set<FrameTime> marks;
    for (const Track& t : seq.videoTracks) {
        if (t.muted) continue;
        for (const Clip& c : t.clips)
            if (c.enabled) marks.insert(c.start), marks.insert(c.end());
    }
    std::vector<Seen> out;
    if (marks.size() < 2) return out;
    auto topAt = [&](FrameTime f) -> const Clip* {
        const Clip* generator = nullptr;
        for (size_t ti = seq.videoTracks.size(); ti-- > 0;) {
            const Track& t = seq.videoTracks[ti];
            if (t.muted) continue;
            for (const Clip& c : t.clips) {
                if (!c.enabled || !c.contains(f)) continue;
                if (c.mediaId) return &c;
                if (!generator) generator = &c;
            }
        }
        return generator;
    };
    for (auto it = marks.begin(); std::next(it) != marks.end(); ++it) {
        const FrameTime a = *it, b = *std::next(it);
        const Clip* c = topAt(a);
        if (!c) continue;
        if (!out.empty() && out.back().out == a) {
            const Clip* prev = out.back().clip;
            // The same clip on, or the next piece of the same media played on from where it left off.
            const bool same = prev == c || (prev->mediaId && prev->mediaId == c->mediaId && prev->speed == c->speed &&
                                            prev->reverse == c->reverse && !c->reverse &&
                                            std::fabs(prev->sourceFrameAt(prev->end() - 1) + prev->speed - c->sourceFrameAt(a)) < 0.5);
            if (same) {
                out.back().out = b;
                continue;
            }
        }
        out.push_back({a, b, c});
    }
    return out;
}

double mediaSeconds(const Clip& c, FrameTime f, const Sequence& seq) { return c.sourceFrameAt(f) / seq.fpsValue(); }

const ShotDescription* describedAt(const MediaItem& m, double t) {
    for (const ShotDescription& d : m.shots)
        if (t >= d.start && t < d.end) return &d;
    return nullptr;
}

bool sentenceEnds(const std::string& w) {
    if (w.empty()) return false;
    const char c = w.back();
    return c == '.' || c == '?' || c == '!';
}

}  // namespace

// ---- Music cue sheet -------------------------------------------------------------------------------------------------

std::vector<MusicCue> musicCues(const Project& p, const Sequence& seq, const MusicTagReader& tags) {
    std::map<Id, std::map<std::string, std::string>> tagCache;
    auto tagsOf = [&](const MediaItem& m) -> const std::map<std::string, std::string>& {
        auto it = tagCache.find(m.id);
        if (it == tagCache.end()) it = tagCache.emplace(m.id, tags ? tags(m) : std::map<std::string, std::string>{}).first;
        return it->second;
    };
    auto field = [&](const MediaItem& m, const std::string& f) {
        const std::string own = metaOf(m, cueMetadataKey(f));
        if (!own.empty()) return own;
        auto& t = tagsOf(m);
        auto it = t.find(f);
        return it == t.end() ? std::string() : it->second;
    };
    auto isMusic = [&](const Clip& c, const MediaItem& m) {
        const std::string said = metaOf(m, kCueMusic);
        if (said == "yes") return true;
        if (said == "no") return false;
        if (c.role == "Music") return true;
        if (!c.role.empty()) return false;
        for (const char* f : {"composer", "publisher", "isrc", "iswc"})
            if (!field(m, f).empty()) return true;
        return false;
    };
    struct Use {
        FrameTime in, out;
        Id group;
    };
    std::map<Id, std::vector<Use>> uses;
    for (size_t t = 0; t < seq.audioTracks.size(); ++t) {
        if (!tracksHeard(seq, t)) continue;
        for (const Clip& c : seq.audioTracks[t].clips) {
            if (!c.enabled || !c.mediaId || edit::roleMuted(seq, c.role)) continue;
            const MediaItem* m = p.findMedia(c.mediaId);
            if (!m || !isMusic(c, *m)) continue;
            uses[c.mediaId].push_back({c.start, c.end(), c.linkGroup ? c.linkGroup : c.id});
        }
    }
    const FrameTime bridge = std::max<FrameTime>(1, FrameTime(std::llround(seq.fpsValue())));
    std::vector<MusicCue> cues;
    for (auto& [media, list] : uses) {
        std::sort(list.begin(), list.end(), [](const Use& a, const Use& b) { return a.in < b.in; });
        const MediaItem& m = *p.findMedia(media);
        std::set<Id> groups;
        for (size_t i = 0; i < list.size(); ++i) {
            if (cues.empty() || cues.back().mediaId != media || list[i].in >= cues.back().out + bridge) {
                MusicCue cue;
                cue.mediaId = media;
                cue.file = m.name.empty() ? stemOf(m.path) : m.name;
                cue.in = list[i].in;
                cue.out = list[i].out;
                for (const char* f : kCueFields) cue.fields[f] = field(m, f);
                if (cue.fields["title"].empty()) cue.fields["title"] = stemOf(m.path);
                const std::string use = metaOf(m, kCueUse);
                cue.use = use.empty() ? "background" : use;
                cue.vocal = metaOf(m, kCueVocal);
                cue.theme = metaOf(m, kCueTheme);
                cues.push_back(std::move(cue));
                groups.clear();
            }
            MusicCue& cue = cues.back();
            cue.out = std::max(cue.out, list[i].out);
            if (groups.insert(list[i].group).second) ++cue.clips;
        }
    }
    std::sort(cues.begin(), cues.end(), [](const MusicCue& a, const MusicCue& b) { return a.in != b.in ? a.in < b.in : a.mediaId < b.mediaId; });
    for (size_t i = 0; i < cues.size(); ++i) cues[i].number = int(i) + 1;
    return cues;
}

// ---- Dialogue list ---------------------------------------------------------------------------------------------------

std::vector<DialogueEntry> dialogueList(const Project& p, const Sequence& seq, const DialogueOptions& o,
                                        const std::vector<OnScreenText>& onScreen) {
    const double fps = seq.fpsValue();
    std::vector<DialogueEntry> out;
    // The scenes, from the logged scene of the picture seen.
    std::string scene;
    for (const Seen& s : seenShots(seq)) {
        const MediaItem* m = s.clip->mediaId ? p.findMedia(s.clip->mediaId) : nullptr;
        const std::string here = m ? metaOf(*m, "scene") : std::string();
        if (here.empty() || here == scene) continue;
        scene = here;
        DialogueEntry e;
        e.kind = DialogueEntry::Kind::Scene;
        e.in = s.in;
        e.out = s.out;
        e.text = "SCENE " + here;
        if (const ShotDescription* d = describedAt(*m, mediaSeconds(*s.clip, s.in, seq))) e.description = d->text;
        out.push_back(e);
    }
    // Close each scene where the next begins (or the cut ends).
    for (size_t i = 0; i < out.size(); ++i) out[i].out = i + 1 < out.size() ? out[i + 1].in : std::max(out[i].out, seq.duration());

    // The lines.
    const std::vector<TranscriptWord> words = sequenceTranscriptWords(p, seq);
    size_t i = 0;
    while (i < words.size()) {
        size_t j = i + 1;
        while (j < words.size() && words[j].speaker == words[i].speaker && words[j].start - words[j - 1].end <= o.pause &&
               !(words[j - 1].end - words[i].start > o.longest && sentenceEnds(words[j - 1].text)))
            ++j;
        DialogueEntry e;
        e.kind = DialogueEntry::Kind::Line;
        e.speaker = words[i].speaker;
        for (size_t k = i; k < j; ++k) e.text += (k > i ? " " : "") + words[k].text;
        e.in = FrameTime(std::floor(words[i].start * fps + 1e-6));
        e.out = std::max(e.in + 1, FrameTime(std::ceil(words[j - 1].end * fps - 1e-6)));
        out.push_back(e);
        i = j;
    }
    for (const OnScreenText& t : onScreen) {
        DialogueEntry e;
        e.kind = DialogueEntry::Kind::OnScreen;
        e.in = t.in;
        e.out = t.out;
        e.text = t.text;
        out.push_back(e);
    }
    std::stable_sort(out.begin(), out.end(), [](const DialogueEntry& a, const DialogueEntry& b) {
        if (a.in != b.in) return a.in < b.in;
        return int(a.kind) < int(b.kind);  // a scene's heading first
    });
    return out;
}

// ---- CCSL -------------------------------------------------------------------------------------------------------------

std::string shotSizeFor(float h) { return h >= 0.3f ? "CU" : h >= 0.12f ? "MS" : "WS"; }

std::vector<CcslShot> ccsl(const Project& p, const Sequence& seq, const std::string& gauge) {
    const std::string gaugeId = !gauge.empty() ? gauge : !seq.filmGauge.empty() ? seq.filmGauge : std::string("35mm-4perf");
    const FilmGauge& g = filmGauge(gaugeId);
    const std::vector<DialogueEntry> lines = dialogueList(p, seq);
    const CaptionTrack* subs = seq.captionTracks.empty() ? nullptr : &seq.captionTracks.front();
    std::vector<CcslShot> shots;
    for (const Seen& s : seenShots(seq)) {
        CcslShot shot;
        shot.number = int(shots.size()) + 1;
        shot.in = s.in;
        shot.out = s.out;
        shot.clipId = s.clip->id;
        shot.mediaId = s.clip->mediaId;
        shot.name = s.clip->name;
        shot.footageIn = formatFeetFrames(s.in, g);
        shot.footageOut = formatFeetFrames(s.out - 1, g);  // its last frame, as the film lists count
        shot.length = formatFeetFrames(s.out - s.in, g);
        if (const MediaItem* m = s.clip->mediaId ? p.findMedia(s.clip->mediaId) : nullptr) {
            const double a = mediaSeconds(*s.clip, s.in, seq), b = mediaSeconds(*s.clip, s.out - 1, seq);
            const double lo = std::min(a, b), hi = std::max(a, b) + 1 / seq.fpsValue();
            if (m->faces) {
                // The faces sampled in the shot (or, in a shot shorter than the sampling, the nearest sample).
                const double tol = std::min(0.5, m->faces->step / 2);
                float best = 0;
                for (const FaceIndex::Face& f : m->faces->faces)
                    if (f.time >= lo - tol && f.time < hi + tol) best = std::max(best, f.h);
                shot.faceHeight = best;
                shot.shotSize = shotSizeFor(best);
            }
            if (const ShotDescription* d = describedAt(*m, a)) shot.description = d->text;
        }
        for (const DialogueEntry& e : lines)
            if (e.kind == DialogueEntry::Kind::Line && e.in >= s.in && e.in < s.out) shot.dialogue.push_back(e);
        if (subs)
            for (const Caption& c : subs->captions)
                if (c.start >= s.in && c.start < s.out) shot.subtitles.push_back(c);
        shots.push_back(std::move(shot));
    }
    return shots;
}

// ---- Tables -----------------------------------------------------------------------------------------------------------

XlsxSheet cueSheetTable(const std::vector<MusicCue>& cues, const Sequence& seq) {
    XlsxSheet t;
    t.name = "Music Cue Sheet";
    t.rows.push_back({"Cue", "Title", "Composer", "Publisher", "Society", "Library", "Catalogue", "ISRC", "ISWC", "Use", "Vocal",
                      "Theme", "In", "Out", "Duration", "Seconds", "File"});
    t.widths = {6, 28, 24, 24, 10, 16, 14, 14, 16, 12, 13, 11, 13, 13, 13, 9, 24};
    for (const MusicCue& c : cues) {
        auto f = [&](const char* k) {
            auto it = c.fields.find(k);
            return XlsxCell(it == c.fields.end() ? std::string() : it->second);
        };
        t.rows.push_back({XlsxCell::num(c.number), f("title"), f("composer"), f("publisher"), f("society"), f("library"), f("catalogue"),
                          f("isrc"), f("iswc"), XlsxCell(c.use), XlsxCell(c.vocal), XlsxCell(c.theme), XlsxCell(tc(c.in, seq)),
                          XlsxCell(tc(c.out, seq)), XlsxCell(tc(c.duration(), seq)),
                          XlsxCell::num(std::round(double(c.duration()) / seq.fpsValue() * 100) / 100), XlsxCell(c.file)});
    }
    return t;
}

namespace {

const char* kindName(DialogueEntry::Kind k) {
    return k == DialogueEntry::Kind::Scene ? "Scene" : k == DialogueEntry::Kind::Line ? "Dialogue" : "On screen";
}

}  // namespace

XlsxSheet dialogueTable(const std::vector<DialogueEntry>& list, const Sequence& seq) {
    XlsxSheet t;
    t.name = "Dialogue List";
    t.rows.push_back({"#", "Kind", "In", "Out", "Speaker", "Text", "Description"});
    t.widths = {6, 10, 13, 13, 16, 70, 40};
    int n = 0;
    for (const DialogueEntry& e : list)
        t.rows.push_back({XlsxCell::num(++n), XlsxCell(kindName(e.kind)), XlsxCell(tc(e.in, seq)), XlsxCell(tc(e.out, seq)), XlsxCell(e.speaker),
                          XlsxCell(e.text), XlsxCell(e.description)});
    return t;
}

XlsxSheet ccslTable(const std::vector<CcslShot>& shots, const Sequence& seq) {
    XlsxSheet t;
    t.name = "CCSL";
    t.rows.push_back({"Shot", "In", "Out", "Footage In", "Footage Out", "Length", "Size", "Description", "Dialogue", "Subtitles"});
    t.widths = {6, 13, 13, 11, 11, 9, 6, 40, 60, 60};
    for (const CcslShot& s : shots) {
        std::string dialogue, subtitles;
        for (const DialogueEntry& e : s.dialogue)
            dialogue += (dialogue.empty() ? "" : "\n") + (e.speaker.empty() ? e.text : e.speaker + ": " + e.text);
        for (const Caption& c : s.subtitles) {
            std::string text = c.text;
            std::replace(text.begin(), text.end(), '\n', ' ');
            subtitles += (subtitles.empty() ? "" : "\n") + tc(c.start, seq) + " " + tc(c.end, seq) + " " + text;
        }
        t.rows.push_back({XlsxCell::num(s.number), XlsxCell(tc(s.in, seq)), XlsxCell(tc(s.out, seq)), XlsxCell(s.footageIn),
                          XlsxCell(s.footageOut), XlsxCell(s.length), XlsxCell(s.shotSize),
                          XlsxCell(s.description.empty() ? s.name : s.description), XlsxCell(dialogue), XlsxCell(subtitles)});
    }
    return t;
}

std::string tableCsv(const XlsxSheet& t) {
    std::string out;
    for (const auto& row : t.rows) {
        for (size_t i = 0; i < row.size(); ++i) {
            const std::string& v = row[i].text;
            if (i) out += ',';
            if (v.find_first_of(",\"\r\n") != std::string::npos || (!v.empty() && (v.front() == ' ' || v.back() == ' '))) {
                out += '"';
                for (char c : v) out += c == '"' ? std::string("\"\"") : std::string(1, c);
                out += '"';
            } else {
                out += v;
            }
        }
        out += "\r\n";
    }
    return out;
}

std::string asBroadcastScript(const std::vector<DialogueEntry>& list, const Sequence& seq) {
    std::string out = seq.name.empty() ? std::string() : seq.name + "\nAs broadcast\n\n";
    for (const DialogueEntry& e : list) {
        switch (e.kind) {
            case DialogueEntry::Kind::Scene:
                out += "\n" + tc(e.in, seq) + "  " + e.text + (e.description.empty() ? "" : " - " + e.description) + "\n\n";
                break;
            case DialogueEntry::Kind::Line: {
                std::string who = e.speaker.empty() ? std::string("(UNKNOWN)") : e.speaker;
                for (char& c : who) c = char(std::toupper(static_cast<unsigned char>(c)));
                out += tc(e.in, seq) + "  " + who + "\n              " + e.text + "\n\n";
                break;
            }
            case DialogueEntry::Kind::OnScreen: out += tc(e.in, seq) + "  ON SCREEN: " + e.text + "\n\n"; break;
        }
    }
    return out;
}

}  // namespace montage
