#include "FilmLists.h"

#include "core/EditOps.h"
#include "core/Effects.h"
#include "core/Interchange.h"
#include "core/Multicam.h"

#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QPainter>
#include <QPdfWriter>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <tuple>

namespace montage {

namespace {

// The media frames [in, out) a clip plays over sequence frames [from, to).
std::pair<int64_t, int64_t> mediaSpan(const Clip& c, FrameTime from, FrameTime to, const Sequence& s, const MediaItem& m) {
    const int64_t a = clipMediaFrame(c, from, s, m), b = clipMediaFrame(c, std::max(from, to - 1), s, m);
    return {std::min(a, b), std::max(a, b) + 1};
}

// A media item's key (or ink) number at its frame, as text in its own gauge (`g` where it has none).
std::string number(const MediaItem* m, bool ink, int64_t frame, const FilmGauge& g) {
    return m ? mediaFilmNumberText(*m, ink, frame, g) : std::string();
}

// How many frames the media has (0: not known).
int64_t mediaFrames(const MediaItem& m) { return m.fps.valid() ? int64_t(std::llround(m.duration * m.fps.toDouble())) : 0; }

std::string metadata(const MediaItem& m, const char* key) {
    const auto it = m.metadata.find(key);
    return it == m.metadata.end() ? std::string() : it->second;
}

std::string percent(double v) {
    std::ostringstream o;
    o.precision(4);
    o << v << "%";
    return o.str();
}

// What a clip's motion does, where it is moved, scaled, rotated, cropped or flipped ("" where it is not).
std::string reposition(const Clip& c) {
    const Effect& e = c.motion;
    if (e.empty() || !e.enabled) return {};
    auto moved = [&](const char* name, double def) {
        const auto it = e.params.find(name);
        return it != e.params.end() && (!it->second.keys.empty() || std::fabs(it->second.value - def) > 1e-6);
    };
    auto value = [&](const char* name) {
        std::ostringstream o;
        const auto it = e.params.find(name);
        o.precision(5);
        if (it == e.params.end()) o << 0;
        else if (!it->second.keys.empty()) o << "keyframed";
        else o << it->second.value;
        return o.str();
    };
    std::vector<std::string> parts;
    if (moved("pos_x", 0) || moved("pos_y", 0)) parts.push_back("position " + value("pos_x") + ", " + value("pos_y"));
    if (moved("scale", 100)) parts.push_back("scale " + value("scale") + "%");
    if (moved("scale_x", 100) || moved("scale_y", 100)) parts.push_back("width " + value("scale_x") + "%, height " + value("scale_y") + "%");
    if (moved("rotation", 0)) parts.push_back("rotation " + value("rotation"));
    if (moved("crop_left", 0) || moved("crop_right", 0) || moved("crop_top", 0) || moved("crop_bottom", 0))
        parts.push_back("crop " + value("crop_left") + "/" + value("crop_right") + "/" + value("crop_top") + "/" + value("crop_bottom"));
    if (moved("flip_h", 0)) parts.push_back("flipped");
    if (moved("flip_v", 0)) parts.push_back("flopped");
    std::string out;
    for (const std::string& part : parts) out += (out.empty() ? "" : ", ") + part;
    return out;
}

std::string pad(const std::string& s, size_t width, bool right = false) {
    if (s.size() >= width) return s;
    return right ? std::string(width - s.size(), ' ') + s : s + std::string(width - s.size(), ' ');
}

}  // namespace

FilmLists filmLists(const Project& p, const Sequence& sequence, const FilmListOptions& options) {
    const Sequence s = interchangeSequence(flattenedMulticam(p, sequence));
    FilmLists l;
    l.sequence = s.name;
    l.gauge = filmGauge(options.gauge.empty() ? s.filmGauge : options.gauge);
    l.fps = s.fps;
    l.length = s.duration();
    const FilmGauge& g = l.gauge;

    struct Placed {
        const Clip* clip;
        int track;
    };
    std::vector<Placed> placed;
    for (int t = 0; t < int(s.videoTracks.size()); ++t) {
        if (options.videoTrack >= 0 && t != options.videoTrack) continue;
        const Track& tr = s.videoTracks[size_t(t)];
        if (tr.muted) continue;
        for (const Clip& c : tr.clips)
            if (c.enabled && c.duration > 0) placed.push_back({&c, t});
    }
    std::stable_sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) {
        return a.clip->start != b.clip->start ? a.clip->start < b.clip->start : a.track < b.track;
    });

    std::map<Id, int> eventOf;  // clip id → event number
    for (const Placed& pl : placed) {
        const Clip& c = *pl.clip;
        FilmEvent e;
        e.number = int(l.events.size()) + 1;
        e.track = pl.track;
        e.recIn = c.start;
        e.recOut = c.end();
        e.name = c.name;
        const MediaItem* m = c.isGenerator() ? nullptr : p.findMedia(c.mediaId);
        if (m) {
            e.mediaId = m->id;
            if (e.name.empty()) e.name = m->name;
            std::tie(e.srcIn, e.srcOut) = mediaSpan(c, c.start, c.end(), s, *m);
            e.negIn = e.srcIn;
            e.negOut = e.srcOut;
            const int64_t first = clipMediaFrame(c, c.start, s, *m), last = clipMediaFrame(c, c.end() - 1, s, *m);
            e.keyIn = number(m, false, first, g);
            e.keyOut = number(m, false, last, g);
            e.inkIn = number(m, true, first, g);
            e.inkOut = number(m, true, last, g);
            // The roll: the key number's prefix (else the ink number's, the camera roll, the media), and where along
            // it the shot's first frame is.
            const FilmGauge& own = mediaFilmGauge(*m, g);
            e.rollPosition = e.srcIn * own.perfsPerFrame;
            for (const bool ink : {false, true})
                if (const auto n = mediaFilmNumber(*m, ink, std::min(first, last), own)) {
                    e.roll = n->prefix;
                    e.rollPosition = n->count * filmNumberSpacing(*n, own) + n->perfs;
                    break;
                }
            if (e.roll.empty()) e.roll = metadata(*m, "tape");
            if (e.roll.empty()) e.roll = m->name;
        }
        eventOf[c.id] = e.number;
        l.events.push_back(e);
    }

    auto clipById = [&](int track, Id id) -> const Clip* {
        for (const Clip& c : s.videoTracks[size_t(track)].clips)
            if (c.id == id) return &c;
        return nullptr;
    };

    // Transitions: dissolves, wipes and fades, and the negative each side needs.
    for (int t = 0; t < int(s.videoTracks.size()); ++t) {
        const Track& tr = s.videoTracks[size_t(t)];
        for (const Transition& x : tr.transitions) {
            FrameTime from = 0, to = 0;
            if (!edit::transitionRange(tr, x, from, to)) continue;
            const Clip* a = x.clipA ? clipById(t, x.clipA) : nullptr;
            const Clip* b = x.clipB ? clipById(t, x.clipB) : nullptr;
            const int ea = a && eventOf.count(a->id) ? eventOf[a->id] : 0, eb = b && eventOf.count(b->id) ? eventOf[b->id] : 0;
            if (!ea && !eb) continue;
            FilmOptical o;
            const EffectInfo* info = findEffectInfo(x.type);
            const std::string group = info ? info->group : std::string();
            const bool plain = x.type == "cross_dissolve";
            o.kind = !(a && b) ? (b ? "Fade in" : "Fade out")
                     : x.type.rfind("dip_", 0) == 0 ? "Dip"
                     : group == "Dissolve"           ? "Dissolve"
                     : group == "Wipe"               ? "Wipe"
                                                     : "Transition";
            o.event = eb ? eb : ea;
            o.outgoing = a && b ? ea : 0;
            o.recIn = from;
            o.recOut = to;
            o.detail = std::to_string(to - from) + " frames" + (info && !plain ? ", " + info->displayName : std::string());
            // Each side's negative over the whole transition (what the media has of it), and the events' negative
            // grown to it.
            auto side = [&](const Clip* c, int ev, std::string& in, std::string& out) {
                if (!c || !ev) return;
                const MediaItem* m = p.findMedia(c->mediaId);
                if (!m || c->isGenerator()) return;
                auto [lo, hi] = mediaSpan(*c, from, to, s, *m);
                const int64_t frames = mediaFrames(*m);
                const int64_t shortBy = std::max<int64_t>(0, -lo) + (frames > 0 ? std::max<int64_t>(0, hi - frames) : 0);
                lo = std::max<int64_t>(0, lo);
                if (frames > 0) hi = std::max(lo + 1, std::min(hi, frames));
                if (shortBy > 0) o.detail += ", " + (c->name.empty() ? m->name : c->name) + " " + std::to_string(shortBy) + " frames short";
                in = number(m, false, lo, g);
                out = number(m, false, hi - 1, g);
                FilmEvent& e = l.events[size_t(ev - 1)];
                e.negIn = std::min(e.negIn, lo);
                e.negOut = std::max(e.negOut, hi);
            };
            if (b) side(b, eb, o.keyIn, o.keyOut);
            if (a) side(a, ea, b ? o.outKeyIn : o.keyIn, b ? o.outKeyOut : o.keyOut);
            l.opticals.push_back(o);
        }
    }

    // Motion: speed changes, reverse, ramps, repositions; superimpositions and titles.
    for (const Placed& pl : placed) {
        const Clip& c = *pl.clip;
        const FilmEvent& e = l.events[size_t(eventOf[c.id] - 1)];
        auto add = [&](const char* kind, const std::string& detail) {
            FilmOptical o;
            o.kind = kind;
            o.event = e.number;
            o.recIn = e.recIn;
            o.recOut = e.recOut;
            o.detail = detail;
            o.keyIn = e.keyIn;
            o.keyOut = e.keyOut;
            l.opticals.push_back(o);
        };
        if (c.isGenerator()) {
            const EffectInfo* info = findEffectInfo(c.generator.type);
            add("Title", info ? info->displayName : c.generator.type);
            continue;
        }
        const auto timed = c.timing.params.find("speed");
        if (c.ramped() && timed != c.timing.params.end() && timed->second.animated())
            add("Speed ramp", "from " + percent(c.speedAt(0) * 100) + " to " + percent(c.speedAt(double(c.duration - 1)) * 100));
        else if (c.reverse) add("Reverse", percent(c.speed * 100));
        else if (std::fabs(c.speedAt(0) - 1) > 1e-6) add("Speed", percent(c.speedAt(0) * 100));
        if (const std::string r = reposition(c); !r.empty()) add("Reposition", r);
        if (pl.track > 0) {
            bool over = false;
            for (int t = 0; t < pl.track && !over; ++t)
                for (const Clip& below : s.videoTracks[size_t(t)].clips)
                    if (below.enabled && below.start < c.end() && c.start < below.end()) over = true;
            if (over) add("Superimposition", "V" + std::to_string(pl.track + 1));
        }
    }
    std::stable_sort(l.opticals.begin(), l.opticals.end(), [](const FilmOptical& a, const FilmOptical& b) { return a.recIn < b.recIn; });

    // Dupes: a later event playing negative an earlier one plays (handles included).
    for (size_t j = 0; j < l.events.size(); ++j)
        for (size_t i = 0; i < j; ++i) {
            const FilmEvent &a = l.events[i], &b = l.events[j];
            if (!a.mediaId || a.mediaId != b.mediaId) continue;
            int64_t lo = std::max(a.negIn, b.negIn), hi = std::min(a.negOut, b.negOut);
            if (lo >= hi) continue;
            FilmDupe d;
            d.first = a.number;
            d.second = b.number;
            d.roll = b.roll;
            const MediaItem* m = p.findMedia(b.mediaId);
            const int64_t frames = m ? mediaFrames(*m) : 0;
            lo = std::max<int64_t>(0, lo);
            if (frames > 0) hi = std::max(lo + 1, std::min(hi, frames));
            d.srcIn = lo;
            d.srcOut = hi;
            d.keyIn = number(m, false, lo, g);
            d.keyOut = number(m, false, hi - 1, g);
            l.dupes.push_back(d);
        }

    // The pull list: rolls in the order first used, each one's events in negative order.
    for (const FilmEvent& e : l.events) {
        if (!e.mediaId) continue;
        auto it = std::find_if(l.pulls.begin(), l.pulls.end(), [&](const auto& r) { return r.first == e.roll; });
        if (it == l.pulls.end()) it = l.pulls.insert(l.pulls.end(), {e.roll, {}});
        it->second.push_back(e.number);
    }
    for (auto& [roll, events] : l.pulls)
        std::stable_sort(events.begin(), events.end(), [&](int a, int b) {
            const FilmEvent &x = l.events[size_t(a - 1)], &y = l.events[size_t(b - 1)];
            return std::tie(x.rollPosition, x.mediaId, x.srcIn) < std::tie(y.rollPosition, y.mediaId, y.srcIn);
        });
    return l;
}

namespace {

// Each list as rows of cells: its heading row first.
struct Table {
    std::string list;
    std::vector<std::vector<std::string>> rows;
};

std::vector<Table> tables(const FilmLists& l, const CutChanges* changes) {
    const FilmGauge& g = l.gauge;
    auto ff = [&](FrameTime f) { return formatFeetFrames(f, g); };
    std::vector<Table> out;

    Table cut{"Cut list", {{"Event", "Footage in", "Footage out", "Length", "Clip", "Roll", "Key in", "Key out", "Ink in", "Ink out"}}};
    for (const FilmEvent& e : l.events)
        cut.rows.push_back({std::to_string(e.number), ff(e.recIn), ff(e.recOut - 1), ff(e.recOut - e.recIn), e.name, e.roll,
                            e.keyIn, e.keyOut, e.inkIn, e.inkOut});
    out.push_back(cut);

    Table optical{"Optical list", {{"Event", "Optical", "Footage in", "Footage out", "Length", "Detail", "Key in", "Key out",
                                    "Outgoing", "Outgoing key in", "Outgoing key out"}}};
    for (const FilmOptical& o : l.opticals)
        optical.rows.push_back({std::to_string(o.event), o.kind, ff(o.recIn), ff(o.recOut - 1), ff(o.recOut - o.recIn), o.detail,
                                o.keyIn, o.keyOut, o.outgoing ? std::to_string(o.outgoing) : std::string(),
                                o.outKeyIn, o.outKeyOut});
    out.push_back(optical);

    Table dupe{"Dupe list", {{"Event", "Dupe of", "Roll", "Key in", "Key out", "Length"}}};
    for (const FilmDupe& d : l.dupes)
        dupe.rows.push_back({std::to_string(d.second), std::to_string(d.first), d.roll, d.keyIn, d.keyOut,
                             ff(d.frames())});
    out.push_back(dupe);

    Table pull{"Pull list", {{"Roll", "Event", "Clip", "Key in", "Key out", "Length"}}};
    for (const auto& [roll, events] : l.pulls)
        for (int n : events) {
            const FilmEvent& e = l.events[size_t(n - 1)];
            pull.rows.push_back({roll, std::to_string(n), e.name, e.keyIn, e.keyOut, ff(e.recOut - e.recIn)});
        }
    out.push_back(pull);

    if (changes) {
        Table change{"Change list", {{"Change", "Kind", "Shot", "Old in", "Old out", "New in", "New out", "Length", "Shift"}}};
        int n = 0;
        for (const CutEvent& c : changes->events) {
            if (c.kind == CutEventKind::Same) continue;
            const bool hasOld = c.oldOut > c.oldIn, hasNew = c.newOut > c.newIn;
            const bool shifts = c.kind == CutEventKind::Moved;
            change.rows.push_back({std::to_string(++n), cutEventName(c.kind), c.black ? std::string("(black)") : c.shot,
                                   hasOld ? ff(c.oldIn) : std::string(), hasOld ? ff(c.oldOut - 1) : std::string(),
                                   hasNew ? ff(c.newIn) : ff(c.newIn) + " (at)", hasNew ? ff(c.newOut - 1) : std::string(),
                                   ff(c.length()), shifts ? (c.shift() < 0 ? "-" : "+") + ff(std::llabs(c.shift())) : std::string()});
        }
        out.push_back(change);
    }
    return out;
}

std::string heading(const FilmLists& l, const Table& t, const CutChanges* changes) {
    std::string h = t.list + "  " + l.sequence + "  " + l.gauge.name + "  " + formatFeetFrames(l.length, l.gauge) + " in all";
    if (t.list == "Change list" && changes)
        h = t.list + "  " + changes->oldName + " to " + changes->newName + "  " + l.gauge.name + "  " +
            formatFeetFrames(changes->oldLength, l.gauge) + " to " + formatFeetFrames(changes->newLength, l.gauge);
    return h;
}

std::vector<std::string> textLines(const FilmLists& l, const CutChanges* changes) {
    std::vector<std::string> lines;
    for (const Table& t : tables(l, changes)) {
        if (!lines.empty()) lines.push_back("");
        lines.push_back(heading(l, t, changes));
        lines.push_back("");
        std::vector<size_t> width(t.rows[0].size(), 0);
        for (const auto& row : t.rows)
            for (size_t i = 0; i < row.size(); ++i) width[i] = std::max(width[i], row[i].size());
        for (size_t r = 0; r < t.rows.size(); ++r) {
            std::string line;
            for (size_t i = 0; i < t.rows[r].size(); ++i) {
                if (i) line += "  ";
                line += pad(t.rows[r][i], width[i]);
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            lines.push_back(line);
        }
        if (t.rows.size() == 1) lines.push_back("(none)");
    }
    return lines;
}

std::string csvCell(const std::string& v) {
    if (v.find_first_of(",\"\n") == std::string::npos) return v;
    std::string o = "\"";
    for (char c : v) o += c == '"' ? std::string("\"\"") : std::string(1, c);
    return o + "\"";
}

}  // namespace

std::string filmListsText(const FilmLists& l, const CutChanges* changes) {
    std::string out;
    for (const std::string& line : textLines(l, changes)) out += line + "\n";
    return out;
}

std::string filmListsCsv(const FilmLists& l, const CutChanges* changes) {
    // One table: the list, then each list's own columns under its own heading row.
    std::string out;
    for (const Table& t : tables(l, changes))
        for (size_t r = 0; r < t.rows.size(); ++r) {
            out += csvCell(r == 0 ? "List" : t.list);
            for (const std::string& v : t.rows[r]) out += "," + csvCell(v);
            out += "\n";
        }
    return out;
}

bool writeFilmListsPdf(const FilmLists& l, const CutChanges* changes, const QString& path, QString* error) {
    const std::vector<std::string> lines = textLines(l, changes);
    size_t longest = 60;
    for (const std::string& line : lines) longest = std::max(longest, line.size());
    QPdfWriter writer(path);
    writer.setPageLayout(QPageLayout(QPageSize(QPageSize::Letter), QPageLayout::Landscape, QMarginsF(0, 0, 0, 0)));
    writer.setTitle(QString::fromStdString("Film lists: " + l.sequence));
    writer.setCreator(QStringLiteral("Montage"));
    QPainter painter;
    if (!painter.begin(&writer)) {
        if (error) *error = QStringLiteral("Cannot write %1").arg(path);
        return false;
    }
    // Points on a landscape Letter page; the type sized so the longest line fits.
    constexpr double kW = 792, kH = 612, kMargin = 36;
    painter.setWindow(0, 0, int(kW), int(kH));
    QFont font(QStringLiteral("Courier"));
    font.setStyleHint(QFont::Monospace);
    const double size = std::clamp((kW - 2 * kMargin) / (0.6 * double(longest)), 4.0, 9.0);
    font.setPointSizeF(size * 72.0 / writer.resolution());  // (points of the window, not of the writer's resolution)
    painter.setFont(font);
    const double row = size * 1.3;
    const int perPage = std::max(1, int((kH - 2 * kMargin - row * 2) / row));
    const int pages = std::max(1, int((lines.size() + size_t(perPage) - 1) / size_t(perPage)));
    for (int page = 0; page < pages; ++page) {
        if (page > 0) writer.newPage();
        for (int i = 0; i < perPage; ++i) {
            const size_t at = size_t(page) * size_t(perPage) + size_t(i);
            if (at >= lines.size()) break;
            painter.drawText(QPointF(kMargin, kMargin + row * (i + 1)), QString::fromStdString(lines[at]));
        }
        painter.drawText(QRectF(kMargin, kH - kMargin, kW - 2 * kMargin, row), Qt::AlignRight,
                         QStringLiteral("%1  page %2 of %3").arg(QString::fromStdString(l.sequence)).arg(page + 1).arg(pages));
    }
    painter.end();
    return true;
}

bool writeFilmLists(const FilmLists& l, const CutChanges* changes, const QString& path, QString* error) {
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QLatin1String("pdf")) return writeFilmListsPdf(l, changes, path, error);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("Cannot write %1").arg(path);
        return false;
    }
    const std::string text = ext == QLatin1String("csv") ? filmListsCsv(l, changes) : filmListsText(l, changes);
    if (f.write(text.data(), qint64(text.size())) != qint64(text.size())) {
        if (error) *error = QStringLiteral("Cannot write %1").arg(path);
        return false;
    }
    return true;
}

}  // namespace montage
