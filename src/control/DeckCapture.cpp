#include "DeckCapture.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "audio/Ltc.h"
#include "core/Ale.h"

namespace montage {

int64_t parseTimecode(const std::string& text, int base, bool* dropFrame, bool assumeDrop) {
    LtcFrame f;
    char a = 0, b = 0, c = 0;
    if (std::sscanf(text.c_str(), " %d%c%d%c%d%c%d", &f.hours, &a, &f.minutes, &b, &f.seconds, &c, &f.frames) != 7) return -1;
    if (f.hours < 0 || f.hours > 23 || f.minutes < 0 || f.minutes > 59 || f.seconds < 0 || f.seconds > 59 || f.frames < 0 || f.frames >= base)
        return -1;
    f.dropFrame = (assumeDrop || c == ';' || c == '.' || c == ',') && base == 30;
    if (dropFrame) *dropFrame = f.dropFrame;
    return ltcFrameNumber(f, base);
}

std::string formatTimecode(int64_t frame, int base, bool dropFrame) { return ltcLabel(ltcFrameAt(frame, base, dropFrame && base == 30)); }

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> split(const std::string& line, char sep) {
    std::vector<std::string> out;
    std::string cell;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (sep == ',' && c == '"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"') cell += '"', ++i;
            else quoted = !quoted;
        } else if (c == sep && !quoted) {
            out.push_back(trim(cell));
            cell.clear();
        } else if (c != '\r') {
            cell += c;
        }
    }
    out.push_back(trim(cell));
    return out;
}

// Which column holds what, by the header's names.
struct Columns {
    int name = -1, reel = -1, in = -1, out = -1, comment = -1;
    explicit Columns(const std::vector<std::string>& header) {
        for (int i = 0; i < int(header.size()); ++i) {
            const std::string h = lower(header[size_t(i)]);
            if (name < 0 && (h == "name" || h == "clip" || h == "clip name")) name = i;
            else if (reel < 0 && (h == "tape" || h == "reel" || h == "tape name" || h == "reel name" || h == "source")) reel = i;
            else if (in < 0 && (h == "start" || h == "in" || h == "mark in" || h == "source in")) in = i;
            else if (out < 0 && (h == "end" || h == "out" || h == "mark out" || h == "source out")) out = i;
            else if (comment < 0 && (h == "comments" || h == "comment" || h == "description")) comment = i;
        }
    }
    bool ok() const { return in >= 0 && out >= 0; }
};

}  // namespace

std::vector<LogEntry> readCaptureLog(const std::string& path, int base, bool dropFrame, std::string* error, const std::string& defaultReel) {
    std::vector<LogEntry> out;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "Cannot read " + path;
        return out;
    }
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
    }
    if (!lines.empty() && lines[0].rfind("\xEF\xBB\xBF", 0) == 0) lines[0].erase(0, 3);
    auto add = [&](const Columns& c, const std::vector<std::string>& cells, int row) {
        auto cell = [&](int i) { return i >= 0 && i < int(cells.size()) ? cells[size_t(i)] : std::string(); };
        LogEntry e;
        bool drop = dropFrame;
        e.in = parseTimecode(cell(c.in), base, &drop, dropFrame);
        e.out = parseTimecode(cell(c.out), base, nullptr, drop);
        if (e.in < 0 || e.out < 0) {
            if (error && error->empty()) *error = "Line " + std::to_string(row + 1) + " has no usable In and Out";
            return;
        }
        if (e.out <= e.in) e.out += ltcFramesPerDay(base, drop);  // (over midnight)
        e.name = cell(c.name).empty() ? "Clip " + std::to_string(out.size() + 1) : cell(c.name);
        e.reel = cell(c.reel).empty() ? defaultReel : cell(c.reel);
        e.comment = cell(c.comment);
        out.push_back(e);
    };
    if (error) error->clear();
    const bool ale = std::any_of(lines.begin(), lines.end(), [](const std::string& l) { return trim(l) == "Column"; });
    if (ale) {
        // Avid Log Exchange (core/Ale.h): its FPS, and the Name, Start, End and Tape columns.
        std::string text;
        for (const std::string& l : lines) text += l + "\n";
        AleTable t;
        if (!parseAle(text, t, error)) return {};
        if (t.headingValue("FPS").size()) {
            const Rational r = t.fps();
            base = int(std::lround(r.toDouble()));
            dropFrame = dropFrame || (r.den == 1001 && base == 30);
        }
        const Columns cols(t.columns);
        if (!cols.ok()) {
            if (error) *error = "The ALE file has no Start and End columns";
            return {};
        }
        for (size_t r = 0; r < t.rows.size(); ++r) add(cols, t.rows[r], int(r));
        return out;
    }
    // A CSV with a header.
    int headerRow = -1;
    for (int i = 0; i < int(lines.size()) && headerRow < 0; ++i)
        if (!trim(lines[size_t(i)]).empty()) headerRow = i;
    if (headerRow < 0) {
        if (error) *error = "The log is empty";
        return out;
    }
    const char sep = lines[size_t(headerRow)].find('\t') != std::string::npos ? '\t' : lines[size_t(headerRow)].find(';') != std::string::npos ? ';' : ',';
    const Columns cols(split(lines[size_t(headerRow)], sep));
    if (!cols.ok()) {
        if (error) *error = "The log's header names no In (Start) and Out (End) columns";
        return out;
    }
    for (int i = headerRow + 1; i < int(lines.size()); ++i)
        if (!trim(lines[size_t(i)]).empty()) add(cols, split(lines[size_t(i)], sep), i);
    return out;
}

namespace {

std::string safeName(std::string s) {
    for (char& c : s)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    return s.empty() ? std::string("Clip") : s;
}

// The deck's timecode against the clock: which frame was showing at a moment. A reading of the deck gives a frame
// seen at some time between asking and the answer, so frame f began no later than the answer and frame f + 1 no
// earlier than the asking; the last two seconds of readings, taken together, place the frames' edges.
class TapeClock {
public:
    explicit TapeClock(double fps) : fps_(fps) {}
    void reading(int64_t frame, double before, double after) {
        std::lock_guard lock(m_);
        // (a jump: the tape stopped or was cued, so what was learnt no longer holds)
        if (!readings_.empty() && std::fabs((after - anchor()) * fps_ - double(frame)) > 3) readings_.clear();
        readings_.push_back({frame, before, after});
        while (readings_.size() > 1 && readings_.back().after - readings_.front().after > 2) readings_.pop_front();
    }
    int64_t frameAt(double t) const {
        std::lock_guard lock(m_);
        if (readings_.empty()) return -1;
        return int64_t(std::floor((t - anchor()) * fps_ + 1e-6));
    }

private:
    struct Reading {
        int64_t frame;
        double before, after;
    };
    // When frame 0 would have begun: between the latest lower and earliest upper bound the readings give.
    double anchor() const {
        double lo = -1e300, hi = 1e300;
        for (const Reading& r : readings_) {
            hi = std::min(hi, r.after - double(r.frame) / fps_);
            lo = std::max(lo, r.before - double(r.frame + 1) / fps_);
        }
        return lo <= hi ? (lo + hi) / 2 : hi;
    }
    mutable std::mutex m_;
    double fps_;
    std::deque<Reading> readings_;
};

}  // namespace

std::vector<BatchClip> batchCapture(ninepin::Deck& deck, const std::vector<LogEntry>& log, const BatchCaptureSettings& s,
                                    const std::function<void(int, const std::string&)>& progress, const std::atomic<bool>* cancel) {
    std::vector<BatchClip> out;
    const double fps = s.rate.valid() ? s.rate.toDouble() : 25;
    const int base = int(std::lround(fps));
    const int64_t handle = int64_t(std::llround(s.handles * fps)), preroll = int64_t(std::llround(s.preroll * fps));
    auto say = [&](int i, const std::string& what) {
        if (progress) progress(i, what);
    };
    for (int i = 0; i < int(log.size()); ++i) {
        BatchClip clip;
        clip.entry = log[size_t(i)];
        const int64_t from = std::max<int64_t>(0, clip.entry.in - handle), to = clip.entry.out + handle;
        std::string name = safeName(clip.entry.reel.empty() ? clip.entry.name : clip.entry.reel + "_" + clip.entry.name);
        clip.path = s.folder + "/" + name + s.extension;
        for (int k = 2; std::ifstream(clip.path).good(); ++k) clip.path = s.folder + "/" + name + " " + std::to_string(k) + s.extension;
        if (cancel && *cancel) {
            clip.error = "Stopped";
            out.push_back(clip);
            continue;
        }
        if (to > ltcFramesPerDay(base, deck.dropFrame())) {
            clip.error = "The clip runs over midnight on the tape: log it as two clips";
            out.push_back(clip);
            continue;
        }
        // Cued to its pre-roll.
        const int64_t cueAt = std::max<int64_t>(0, from - preroll);
        say(i, "Cueing to " + formatTimecode(cueAt, base, deck.dropFrame()));
        if (!deck.cue(cueAt) || !deck.waitForCue(cueAt, s.cueTimeout, cancel)) {
            clip.error = deck.lastError().empty() ? "The deck did not reach the cue" : deck.lastError();
            out.push_back(clip);
            continue;
        }
        // The recording waits for the range's first frame; the deck's timecode labels pictures that carry none.
        TapeClock tape(fps);
        CaptureSettings cs = s.capture;
        cs.path = clip.path;
        cs.reel = clip.entry.reel;
        cs.timecode.clear();
        cs.seconds = 0;
        cs.fromFrame = from;
        cs.frames = to - from;
        cs.labelDropFrame = deck.dropFrame();
        const int latency = s.latencyFrames;
        cs.label = [&tape, latency](double arrived) {
            const int64_t f = tape.frameAt(arrived);
            return f < 0 ? f : f - latency;
        };
        cs.openTimeout = std::max(cs.openTimeout, s.preroll + 10);
        Recorder rec;
        std::string err;
        if (!rec.start(cs, &err)) {
            clip.error = err;
            out.push_back(clip);
            continue;
        }
        say(i, "Rolling");
        if (!deck.play()) {
            rec.stop();
            clip.error = deck.lastError();
            out.push_back(clip);
            continue;
        }
        // The deck read as it plays, until the range is recorded (or the deck runs well past it).
        const double limit = captureClock() + (double(to - from + preroll) / fps) + 30;
        bool recorded = false, stopped = false;
        while (!(recorded = rec.wait(0.005))) {
            if (cancel && *cancel) {
                stopped = true;
                break;
            }
            const double before = captureClock();
            const int64_t f = deck.timecode(s.source);
            const double after = captureClock();
            if (f >= 0) tape.reading(f, before, after);
            if (after > limit || (f >= 0 && f > to + int64_t(fps) * 3)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        if (!recorded) rec.stop();
        deck.stop();
        const CaptureStatus st = rec.status();
        clip.frames = st.frames;
        clip.timecode = st.timecode;
        const int64_t first = parseTimecode(st.timecode, base, nullptr, deck.dropFrame());
        if (stopped) clip.error = "Stopped";
        else if (!st.error.empty()) clip.error = st.error;
        else if (st.frames == 0) clip.error = "No pictures came from the feed while the deck played";
        else if (first != from)
            clip.error = "The recording began at " + st.timecode + ", not " + formatTimecode(from, base, deck.dropFrame()) +
                         " (the deck rolled late, or the feed's timecode does not match the deck's)";
        else if (st.frames < to - from) clip.error = "The deck's picture stopped short of the clip's end";
        clip.ok = clip.error.empty();
        say(i, clip.ok ? "Captured" : clip.error);
        out.push_back(clip);
    }
    return out;
}

bool editToTape(ninepin::Deck& deck, const TapeEdit& e, const std::function<void(int64_t, double)>& roll, const std::atomic<bool>* cancel,
                std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (e.out <= e.in) return fail("The edit's Out is not after its In");
    const int base = deck.base();
    const double fps = base == 30 ? 30000.0 / 1001 : double(base);  // (for how long to wait)
    const int64_t preroll = int64_t(std::llround(e.preroll * base));
    if (!deck.setEditChannels(e.channels) || !deck.setIn(e.in) || !deck.setOut(e.out) || !deck.setPrerollTime(preroll))
        return fail("The deck refused the edit: " + deck.lastError());
    if (!deck.autoEdit()) return fail("The deck refused the edit: " + deck.lastError());
    // Watched as it runs: rolling up to In (the Program started then), recording, and stopping after Out.
    const double limit = captureClock() + double(preroll + (e.out - e.in)) / fps + 60;
    bool rolled = false, recorded = false;
    int64_t last = -1;
    while (captureClock() < limit) {
        if (cancel && *cancel) {
            deck.stop();
            return fail("Stopped");
        }
        const auto st = deck.status();
        const double before = captureClock();
        const int64_t f = deck.timecode(ninepin::TimeSource::Ltc);
        if (!st || f < 0) return fail("The deck stopped answering: " + deck.lastError());
        if (!rolled && st->play && f < e.in && (last < 0 || f > last)) {
            rolled = true;
            if (roll) roll(f, before);
        }
        recorded = recorded || st->record;
        if (rolled && !st->autoEdit && !st->edit && !st->record && (st->stop || st->still || f >= e.out)) break;
        last = f;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!rolled) return fail("The deck never rolled to the edit");
    if (!recorded) return fail("The deck did not record");
    return true;
}

}  // namespace montage
