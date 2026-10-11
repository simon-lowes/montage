#include "Adr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <sstream>

#include "History.h"

namespace montage {

namespace {

std::string csvField(const std::string& v) {
    if (v.find_first_of(",\"\n\r") == std::string::npos) return v;
    std::string q = "\"";
    for (char c : v) q += c == '"' ? std::string("\"\"") : std::string(1, c);
    return q + "\"";
}

// Splits CSV text into rows of fields; quoted fields may hold separators and line breaks.
std::vector<std::vector<std::string>> csvRows(const std::string& text, char sep) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string cur;
    bool quoted = false, any = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"' && i + 1 < text.size() && text[i + 1] == '"') cur += '"', ++i;
            else if (c == '"') quoted = false;
            else cur += c;
        } else if (c == '"' && cur.empty()) {  // a quote opens a field only at its start ("6'2" tall" is text)
            quoted = any = true;
        } else if (c == sep) {
            row.push_back(cur);
            cur.clear();
            any = true;
        } else if (c == '\n') {
            row.push_back(cur);
            cur.clear();
            if (any || row.size() > 1 || !row.front().empty()) rows.push_back(row);
            row.clear();
            any = false;
        } else if (c != '\r') {
            cur += c;
            any = true;
        }
    }
    row.push_back(cur);
    if (any || row.size() > 1 || !row.front().empty()) rows.push_back(row);
    return rows;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// "Anna Maria Jones" → "AMJ"; letters only, at most three.
std::string initials(const std::string& name) {
    std::string out;
    bool start = true;
    for (char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalpha(u)) {
            if (start && out.size() < 3) out += char(std::toupper(u));
            start = false;
        } else if (u < 0x80) {
            start = true;
        }
    }
    return out;
}

// A speaker label at the start of a caption: "ANNA: line", "[Anna] line", "(ANNA) line", "- ANNA: line".
void splitSpeaker(const std::string& text, std::string& character, std::string& line) {
    std::string t = trim(text);
    if (t.rfind("- ", 0) == 0) t = trim(t.substr(2));
    character.clear();
    line = t;
    if (t.empty()) return;
    if (t[0] == '[' || t[0] == '(') {
        const char close = t[0] == '[' ? ']' : ')';
        const size_t e = t.find(close);
        if (e != std::string::npos && e > 1 && e <= 40) {
            character = trim(t.substr(1, e - 1));
            std::string rest = trim(t.substr(e + 1));
            if (!rest.empty() && rest[0] == ':') rest = trim(rest.substr(1));
            line = rest;
        }
        return;
    }
    const size_t colon = t.find(':');
    if (colon == std::string::npos || colon == 0 || colon > 30) return;
    const std::string who = trim(t.substr(0, colon));
    // A name: letters, spaces, dots, apostrophes and hyphens, at most three words, not a time ("10:30").
    int words = 1, letters = 0;
    for (char c : who) {
        const auto u = static_cast<unsigned char>(c);
        if (c == ' ') ++words;
        else if (std::isalpha(u) || u >= 0x80) ++letters;
        else if (c != '.' && c != '\'' && c != '-') return;
    }
    if (words > 3 || letters == 0) return;
    character = who;
    line = trim(t.substr(colon + 1));
}

void flatten(std::string& s) {
    for (char& c : s)
        if (c == '\n' || c == '\r') c = ' ';
}

void sortCues(Sequence& s) {
    std::stable_sort(s.adrCues.begin(), s.adrCues.end(), [](const AdrCue& a, const AdrCue& b) { return a.start < b.start; });
}

FrameTime framesFor(const Sequence& s, double seconds) { return FrameTime(std::llround(seconds * s.fpsValue())); }

}  // namespace

std::string adrStatusName(int status) {
    switch (status) {
        case kAdrRecorded: return "Recorded";
        case kAdrApproved: return "Approved";
        case kAdrOmitted: return "Omitted";
        default: return "To Record";
    }
}

int adrStatusFromName(const std::string& name) {
    const std::string n = lower(trim(name));
    if (n == "recorded") return kAdrRecorded;
    if (n == "approved" || n == "done" || n == "complete" || n == "completed") return kAdrApproved;
    if (n == "omitted" || n == "omit" || n == "cut") return kAdrOmitted;
    return kAdrToRecord;
}

AdrCue* findAdrCue(Sequence& s, Id id) {
    for (AdrCue& q : s.adrCues)
        if (q.id == id) return &q;
    return nullptr;
}

const AdrCue* findAdrCue(const Sequence& s, Id id) {
    for (const AdrCue& q : s.adrCues)
        if (q.id == id) return &q;
    return nullptr;
}

std::string nextAdrCueName(const Sequence& s, const std::string& character) {
    std::string prefix = initials(character);
    if (prefix.empty()) prefix = "ADR";
    int next = 101;
    for (const AdrCue& q : s.adrCues) {
        if (q.name.size() <= prefix.size() || q.name.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string digits = q.name.substr(prefix.size());
        if (digits.size() > 6 || !std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            continue;
        next = std::max(next, std::stoi(digits) + 1);
    }
    return prefix + std::to_string(next);
}

std::vector<AdrCue> adrCuesFromCaptions(const Sequence& s, int track, FrameTime from, FrameTime to) {
    std::vector<AdrCue> out;
    for (int t = 0; t < int(s.captionTracks.size()); ++t) {
        if (track >= 0 && t != track) continue;
        for (const Caption& c : s.captionTracks[size_t(t)].captions) {
            if (c.end <= c.start || c.end <= from || (to >= 0 && c.start > to)) continue;
            AdrCue q;
            q.start = c.start;
            q.end = c.end;
            std::string text = c.text;
            flatten(text);
            splitSpeaker(text, q.character, q.line);
            out.push_back(std::move(q));
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const AdrCue& a, const AdrCue& b) { return a.start < b.start; });
    return out;
}

std::vector<AdrCue> adrCuesFromMarkers(const Sequence& s, FrameTime from, FrameTime to, int color) {
    std::vector<AdrCue> out;
    for (const Marker& m : s.markers) {
        if (m.duration <= 0 || m.t + m.duration <= from || (to >= 0 && m.t > to)) continue;
        if (color >= 0 && m.color != color) continue;
        AdrCue q;
        q.start = m.t;
        q.end = m.t + m.duration;
        q.note = m.name;
        q.line = m.comment;
        flatten(q.line);
        out.push_back(std::move(q));
    }
    return out;
}

std::vector<Id> addAdrCues(Project& p, Sequence& s, std::vector<AdrCue> cues) {
    std::vector<Id> ids;
    // Named cues first (so a number given later in a sheet is not taken by a blank one numbered before it), then in
    // time order, so new numbers run with the picture.
    for (AdrCue& q : cues) q.name = trim(q.name);
    std::stable_sort(cues.begin(), cues.end(), [](const AdrCue& a, const AdrCue& b) {
        if (a.name.empty() != b.name.empty()) return !a.name.empty();
        return a.start < b.start;
    });
    for (AdrCue& q : cues) {
        if (q.end <= q.start) continue;
        q.name = trim(q.name);
        AdrCue* same = nullptr;
        if (!q.name.empty())
            for (AdrCue& o : s.adrCues)
                if (o.name == q.name) same = &o;
        if (same) {
            same->start = q.start;
            same->end = q.end;
            if (!q.character.empty()) same->character = q.character;
            if (!q.line.empty()) same->line = q.line;
            if (!q.note.empty()) same->note = q.note;
            if (q.status != kAdrToRecord) same->status = q.status;
            ids.push_back(same->id);
            continue;
        }
        if (q.name.empty()) q.name = nextAdrCueName(s, q.character);
        q.id = p.newId();
        q.status = std::clamp(q.status, 0, int(kAdrOmitted));
        if (q.clip && !edit::clipById(s, q.clip)) q.clip = 0;
        ids.push_back(q.id);
        s.adrCues.push_back(std::move(q));
    }
    sortCues(s);
    return ids;
}

bool removeAdrCue(Sequence& s, Id id) {
    const auto it = std::find_if(s.adrCues.begin(), s.adrCues.end(), [&](const AdrCue& q) { return q.id == id; });
    if (it == s.adrCues.end()) return false;
    s.adrCues.erase(it);
    return true;
}

void rippleAdrCues(Sequence& s, FrameTime a, FrameTime b) {
    if (b <= a) return;
    const FrameTime len = b - a;
    std::vector<AdrCue> kept;
    for (AdrCue q : s.adrCues) {
        if (q.start >= b) {
            q.start -= len;
            q.end -= len;
        } else if (q.end > a) {  // overlaps what was taken out: what is outside it stays
            const FrameTime before = std::max<FrameTime>(0, a - q.start), after = std::max<FrameTime>(0, q.end - b);
            if (before + after <= 0) continue;
            q.start = std::min(q.start, a);
            q.end = q.start + before + after;
        }
        kept.push_back(std::move(q));
    }
    s.adrCues = std::move(kept);
    sortCues(s);
}

int adrTakeCount(const Sequence& s, const AdrCue& cue) {
    const Clip* c = cue.clip ? edit::clipById(s, cue.clip) : nullptr;
    if (!c) return 0;
    return c->takes.empty() ? 1 : int(c->takes.size());
}

std::string adrCueSheetCsv(const Sequence& s) {
    auto tc = [&](FrameTime f) { return formatTimecode(f, s.fps); };
    std::string out = "Cue,Character,Start,End,Duration,Line,Note,Status,Takes\n";
    for (const AdrCue& q : s.adrCues)
        out += csvField(q.name) + "," + csvField(q.character) + "," + tc(q.start) + "," + tc(q.end) + "," + tc(q.end - q.start) + "," +
               csvField(q.line) + "," + csvField(q.note) + "," + adrStatusName(q.status) + "," + std::to_string(adrTakeCount(s, q)) + "\n";
    return out;
}

bool parseAdrCueSheet(const std::string& input, const Sequence& s, std::vector<AdrCue>& out, std::string* error) {
    out.clear();
    std::string text = input;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) text = text.substr(3);  // UTF-8 BOM
    const std::string first = text.substr(0, text.find('\n'));
    const char sep = first.find('\t') != std::string::npos && first.find(',') == std::string::npos ? '\t' : ',';
    const auto rows = csvRows(text, sep);
    if (rows.empty()) {
        if (error) *error = "The file is empty";
        return false;
    }
    const std::vector<std::string>& header = rows[0];
    auto column = [&](std::initializer_list<const char*> names) {
        for (const char* n : names)
            for (size_t i = 0; i < header.size(); ++i)
                if (lower(trim(header[i])) == n) return int(i);
        return -1;
    };
    const int cCue = column({"cue", "cue number", "cue #", "cue no", "cue no.", "number", "#"});
    const int cChar = column({"character", "role", "char", "speaker"});
    const int cStart = column({"start", "in", "tc in", "timecode in", "record in", "timecode", "start tc"});
    const int cEnd = column({"end", "out", "tc out", "timecode out", "record out", "end tc"});
    const int cDur = column({"duration", "length"});
    const int cLine = column({"line", "dialogue", "dialog", "text", "line of dialogue", "script"});
    const int cNote = column({"note", "notes", "reason", "comment", "comments"});
    const int cStatus = column({"status", "state"});
    if (cStart < 0) {
        if (error) *error = "The cue sheet has no Start (or In) column";
        return false;
    }
    auto time = [&](const std::string& v, FrameTime& f) { return !trim(v).empty() && parseTimecode(trim(v), s.fps, f); };
    for (size_t i = 1; i < rows.size(); ++i) {
        const auto& f = rows[i];
        auto at = [&](int c) { return c >= 0 && c < int(f.size()) ? trim(f[size_t(c)]) : std::string(); };
        AdrCue q;
        if (!time(at(cStart), q.start)) continue;
        FrameTime v = 0;
        if (cEnd >= 0 && time(at(cEnd), v) && v > q.start) q.end = v;
        else if (cDur >= 0 && time(at(cDur), v) && v > 0) q.end = q.start + v;
        else q.end = q.start + std::max<FrameTime>(1, framesFor(s, 2));
        q.name = at(cCue);
        q.character = at(cChar);
        q.line = at(cLine);
        flatten(q.line);
        q.note = at(cNote);
        q.status = adrStatusFromName(at(cStatus));
        out.push_back(std::move(q));
    }
    if (out.empty()) {
        if (error) *error = "No cues with timecodes were found";
        return false;
    }
    // Sheets made on timelines that start at 01:00:00:00.
    FrameTime hour = framesFor(s, 3600);
    parseTimecode("01:00:00:00", s.fps, hour);  // as the sheet's timecodes count (86400 frames at 23.976, not 86314)
    if (s.duration() < hour && std::all_of(out.begin(), out.end(), [&](const AdrCue& q) { return q.start >= hour; }))
        for (AdrCue& q : out) q.start -= hour, q.end -= hour;
    std::stable_sort(out.begin(), out.end(), [](const AdrCue& a, const AdrCue& b) { return a.start < b.start; });
    return true;
}

AdrCycle adrCycle(const Sequence& s, const AdrCue& cue, const AdrSettings& st) {
    AdrCycle c;
    if (cue.end <= cue.start) return c;
    const double fps = s.fpsValue();
    const int beeps = std::clamp(st.beeps, 0, 3);
    // Enough pre-roll for the beeps and half a second before the first.
    const double pre = std::max({0.0, st.preRoll, beeps > 0 ? beeps + 0.5 : 0.0, st.streamer > 0 ? st.streamer : 0.0});
    c.lineFrom = cue.start;
    c.lineTo = cue.end;
    c.playFrom = std::max<FrameTime>(0, cue.start - framesFor(s, pre));
    c.playTo = cue.end + std::max<FrameTime>(0, framesFor(s, std::max(0.0, st.postRoll)));
    for (int k = beeps; k >= 1; --k) {
        const FrameTime b = cue.start - FrameTime(std::llround(k * fps));
        if (b >= c.playFrom) c.beeps.push_back(b);
    }
    if (st.streamer > 0) {
        c.streamerTo = cue.start;
        c.streamerFrom = std::max(c.playFrom, cue.start - std::max<FrameTime>(1, framesFor(s, st.streamer)));
        if (c.streamerFrom >= c.streamerTo) c.streamerFrom = c.streamerTo = -1;
    }
    return c;
}

double adrStreamerPosition(const AdrCycle& c, FrameTime t) {
    if (c.streamerTo <= c.streamerFrom || t < c.streamerFrom || t > c.streamerTo) return -1;
    return double(t - c.streamerFrom) / double(c.streamerTo - c.streamerFrom);
}

bool adrPunch(const AdrCycle& c, FrameTime t) { return c.valid() && t >= c.lineFrom && t < c.lineFrom + 2 && t < c.lineTo; }

void addAdrBeeps(const AdrCycle& c, double fps, int sampleRate, int64_t firstSample, float* interleaved, int frames, int channels,
                 float gain) {
    if (c.beeps.empty() || fps <= 0 || sampleRate <= 0 || frames <= 0 || channels <= 0) return;
    const int64_t length = std::max<int64_t>(std::llround(sampleRate / fps), std::llround(sampleRate * 0.04));
    const int64_t ramp = std::max<int64_t>(1, sampleRate / 500);  // 2 ms in and out, no clicks
    const int64_t last = firstSample + frames;
    for (FrameTime b : c.beeps) {
        const int64_t from = int64_t(std::llround(double(b) * sampleRate / fps));
        const int64_t a = std::max(from, firstSample), e = std::min(from + length, last);
        for (int64_t i = a; i < e; ++i) {
            const int64_t k = i - from;
            const double env = std::min({1.0, double(k + 1) / double(ramp), double(length - k) / double(ramp)});
            const float v = float(gain * env * std::sin(2 * M_PI * 1000.0 * double(k) / sampleRate));
            float* frame = interleaved + (i - firstSample) * channels;
            for (int ch = 0; ch < channels; ++ch) frame[ch] += v;
        }
    }
}

namespace edit {

int adrTrack(Project& p, Sequence& s) {
    for (size_t i = 0; i < s.audioTracks.size(); ++i)
        if (s.audioTracks[i].name == "ADR") return int(i);
    const TrackRef r = addTrack(p, s, TrackKind::Audio);
    s.audioTracks[size_t(r.index)].name = "ADR";
    return r.index;
}

Result addAdrTake(Project& p, Sequence& s, Id cueId, Id mediaId, FrameTime recordedFrom, int track) {
    AdrCue* cue = findAdrCue(s, cueId);
    if (!cue) return Result::fail("No such ADR cue");
    const MediaItem* m = p.findMedia(mediaId);
    if (!m || !m->hasAudio) return Result::fail("A take needs sound");
    const double mediaFrames = m->duration * s.fpsValue();
    // Where the take has sound of the line: from the line's start (or the take's, if later) to its end (or the take's).
    const FrameTime takeEnd = mediaFrames > 0 ? recordedFrom + FrameTime(std::floor(mediaFrames)) : cue->end;
    Clip* c = cue->clip ? clipById(s, cue->clip) : nullptr;
    Result done;
    if (c) {
        // Another take of the same audition, in sync where the clip now starts.
        const double in = std::max(0.0, double(c->start - recordedFrom));
        if (mediaFrames > 0 && mediaFrames <= in) return Result::fail("The take ended before the line");
        const Id clipId = c->id;
        if (Result r = addTakes(p, s, clipId, {{mediaId, in}}); !r.ok) return r;
        const Clip* now = clipById(s, clipId);
        if (Result r = pickTake(p, s, clipId, int(now->takes.size()) - 1); !r.ok) return r;
        // A clip cut short by an earlier take stopped mid-line grows to what this one covers, where there is room.
        if (Clip* grown = clipById(s, clipId); grown && grown->speed == 1.0) {
            const FrameTime want = std::min(cue->end, takeEnd);
            const auto loc = locate(s, clipId);
            const Track* t = loc ? trackAt(s, loc->track) : nullptr;
            if (t && want > grown->end() && trackEmpty(*t, grown->end(), want, {clipId})) grown->duration = want - grown->start;
        }
    } else {
        // The line, cut from the take where it was recorded; never before the take began or after it ended.
        const FrameTime at = std::max(cue->start, recordedFrom);
        const FrameTime to = std::min(cue->end, takeEnd);
        if (to <= at) return Result::fail("The take ended before the line");
        // On a track with room over the line: the one asked for, else the first ADR track free there ("ADR",
        // "ADR 2"...), so overlapping lines never cut into each other's takes.
        auto freeHere = [&](int i) {
            const Track& t = s.audioTracks[size_t(i)];
            return !t.locked && trackEmpty(t, at, to);
        };
        if (track >= 0 && track < int(s.audioTracks.size()) && !freeHere(track)) track = -1;
        if (track < 0) {
            int adr = 0;
            for (int i = 0; i < int(s.audioTracks.size()) && track < 0; ++i) {
                const std::string& n = s.audioTracks[size_t(i)].name;
                if (n == "ADR" || n.rfind("ADR ", 0) == 0) {
                    ++adr;
                    if (freeHere(i)) track = i;
                }
            }
            if (track < 0) {
                track = addTrack(p, s, TrackKind::Audio).index;
                s.audioTracks[size_t(track)].name = adr == 0 ? "ADR" : "ADR " + std::to_string(adr + 1);
            }
        }
        while (track >= int(s.audioTracks.size())) addTrack(p, s, TrackKind::Audio);
        if (s.audioTracks[size_t(track)].locked) return Result::fail("The ADR track is locked");
        const double srcIn = double(at - recordedFrom), srcOut = srcIn + double(to - at);
        // Sound only: a take with picture leaves the video tracks alone.
        Result r = placeMedia(p, s, mediaId, at, srcIn, srcOut, {TrackKind::Video, -1}, {TrackKind::Audio, track}, false);
        if (!r.ok) return r;
        cue = findAdrCue(s, cueId);
        for (Id id : r.created)
            if (const auto loc = locate(s, id); loc && loc->track.kind == TrackKind::Audio) {
                cue->clip = id;
                break;
            }
        if (!cue->clip) return Result::fail("The take could not be placed");
        if (Clip* placed = clipById(s, cue->clip)) placed->role = "Dialogue";
        done.created = {cue->clip};
    }
    cue = findAdrCue(s, cueId);
    if (cue->status == kAdrToRecord) cue->status = kAdrRecorded;
    return done;
}

}  // namespace edit

}  // namespace montage
