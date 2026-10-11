#include "Ale.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <map>

#include "Cdl.h"
#include "History.h"
#include "Interpretation.h"

namespace montage {

namespace {

std::string lower(const std::string& s) { return QString::fromStdString(s).trimmed().toLower().toStdString(); }

// A file's name from its path, whichever separator the path was written with (and without Interpret Footage's suffix).
std::string baseName(const std::string& path) {
    QString p = QString::fromStdString(uninterpretedPath(path));
    p.replace('\\', '/');
    return p.mid(p.lastIndexOf('/') + 1).toStdString();
}

// An ALE timecode in frames: drop-frame only when written with ';' at a drop-frame rate; false if it is not one.
bool aleTimecode(const std::string& text, Rational fps, FrameTime& out) {
    const QStringList parts = QString::fromStdString(text).trimmed().split(QRegularExpression("[:;.]"));
    if (parts.size() != 4) return false;
    long long v[4];
    for (int i = 0; i < 4; ++i) {
        bool ok = false;
        v[i] = parts[i].toLongLong(&ok);
        if (!ok || parts[i].size() > 3 || v[i] < 0) return false;
    }
    const int nominal = std::max(1, int(std::lround(fps.toDouble())));
    out = ((v[0] * 60 + v[1]) * 60 + v[2]) * nominal + v[3];
    if (text.find(';') != std::string::npos && isDropFrameRate(fps)) {
        const int drop = nominal == 60 ? 4 : 2;
        const long long minutes = v[0] * 60 + v[1];
        out -= drop * (minutes - minutes / 10);
    }
    return true;
}

// ALE columns and the log fields they fill (the first column present wins for a field).
struct ColumnField {
    const char* column;
    const char* field;
};
const ColumnField kColumns[] = {
    {"Scene", "scene"},   {"Shot", "shot"},         {"Take", "take"},          {"Tape", "tape"},
    {"Camroll", "tape"},  {"Cam Roll", "tape"},     {"Camera", "camera"},      {"Description", "description"},
    {"Comments", "comment"}, {"Comment", "comment"},
};

}  // namespace

std::string AleTable::value(size_t row, const std::string& column) const {
    if (row >= rows.size()) return {};
    const std::string want = lower(column);
    for (size_t i = 0; i < columns.size(); ++i)
        if (lower(columns[i]) == want) return i < rows[row].size() ? rows[row][i] : std::string();
    return {};
}

std::string AleTable::headingValue(const std::string& key) const {
    const std::string want = lower(key);
    for (const auto& [k, v] : heading)
        if (lower(k) == want) return v;
    return {};
}

Rational AleTable::fps() const {
    const double r = QString::fromStdString(headingValue("FPS")).toDouble();
    if (!(r > 0) || r > 1000) return {24, 1};
    for (int n : {24, 30, 48, 60, 120})
        if (std::fabs(r - n * 1000.0 / 1001.0) < 0.01) return {n * 1000, 1001};
    return {int(std::lround(r)), 1};
}

bool parseAle(const std::string& text, AleTable& out, std::string* error) {
    AleTable t;
    const QStringList lines = QString::fromStdString(text).split(QRegularExpression("\\r\\n|\\r|\\n"));
    enum { None, Heading, Column, Data } section = None;
    bool sawHeading = false, sawColumn = false;
    for (const QString& raw : lines) {
        const QString trimmed = raw.trimmed();
        if (trimmed.compare("Heading", Qt::CaseInsensitive) == 0) {
            section = Heading;
            sawHeading = true;
            continue;
        }
        if (trimmed.compare("Column", Qt::CaseInsensitive) == 0) {
            section = Column;
            continue;
        }
        if (trimmed.compare("Data", Qt::CaseInsensitive) == 0) {
            section = Data;
            continue;
        }
        if (trimmed.isEmpty()) continue;
        QStringList cells = raw.split('\t');
        if (section == Heading) {
            if (cells.size() >= 2) t.heading.push_back({cells[0].trimmed().toStdString(), cells[1].trimmed().toStdString()});
        } else if (section == Column && !sawColumn) {
            while (!cells.isEmpty() && cells.back().trimmed().isEmpty()) cells.removeLast();
            for (const QString& c : cells) t.columns.push_back(c.trimmed().toStdString());
            sawColumn = true;
        } else if (section == Data) {
            std::vector<std::string> row;
            for (const QString& c : cells) row.push_back(c.trimmed().toStdString());
            row.resize(t.columns.size());  // (trailing tabs, or cells left off the end)
            t.rows.push_back(std::move(row));
        }
    }
    if (!sawHeading || t.columns.empty()) {
        if (error) *error = "Not an Avid Log Exchange file (no Heading or Column section)";
        return false;
    }
    out = std::move(t);
    return true;
}

std::string writeAle(const AleTable& t) {
    std::string s = "Heading\n";
    for (const auto& [k, v] : t.heading) s += k + "\t" + v + "\n";
    s += "\nColumn\n";
    for (size_t i = 0; i < t.columns.size(); ++i) s += (i ? "\t" : "") + t.columns[i];
    s += "\n\nData\n";
    auto clean = [](std::string v) {
        std::replace(v.begin(), v.end(), '\t', ' ');
        std::replace(v.begin(), v.end(), '\n', ' ');
        std::replace(v.begin(), v.end(), '\r', ' ');
        return v;
    };
    for (const auto& row : t.rows) {
        for (size_t i = 0; i < t.columns.size(); ++i) s += (i ? "\t" : "") + clean(i < row.size() ? row[i] : std::string());
        s += "\n";
    }
    return s;
}

AleImport applyAle(Project& p, const AleTable& t, bool cdlToClips) {
    AleImport res;
    res.rows = int(t.rows.size());
    const Rational fps = t.fps();
    std::vector<Id> graded;
    for (size_t r = 0; r < t.rows.size(); ++r) {
        const std::string name = t.value(r, "Name"), file = t.value(r, "Source File"), tape = t.value(r, "Tape");
        // The media this row describes.
        MediaItem* found = nullptr;
        auto pick = [&](auto&& test) {
            for (MediaItem& m : p.media)
                if (!found && m.kind != MediaKind::Sequence && !m.subclipOf && test(m)) found = &m;
        };
        if (!file.empty()) pick([&](const MediaItem& m) { return !m.path.empty() && lower(baseName(m.path)) == lower(baseName(file)); });
        if (!name.empty()) {
            pick([&](const MediaItem& m) { return lower(m.name) == lower(name); });
            // Without a media file extension ("A001C003" for "A001C003.mov"; "Sc12.1" stays itself).
            const std::string plain = lower(withoutMediaExtension(name));
            pick([&](const MediaItem& m) {
                return lower(withoutMediaExtension(m.name)) == plain || (!m.path.empty() && lower(withoutMediaExtension(baseName(m.path))) == plain);
            });
        }
        FrameTime start = 0;
        if (!tape.empty() && aleTimecode(t.value(r, "Start"), fps, start))
            pick([&](const MediaItem& m) {
                const auto mt = m.metadata.find("tape");
                return mt != m.metadata.end() && lower(mt->second) == lower(tape) && m.timecode >= 0 &&
                       std::fabs(m.timecode - double(start) / fps.toDouble()) < 0.5 / fps.toDouble();
            });
        if (!found) {
            res.unmatched.push_back(!name.empty() ? name : !file.empty() ? file : "row " + std::to_string(r + 1));
            continue;
        }
        std::map<std::string, bool> set;
        for (const ColumnField& cf : kColumns) {
            const std::string v = t.value(r, cf.column);
            if (v.empty() || set[cf.field]) continue;
            found->metadata[cf.field] = v;
            set[cf.field] = true;
        }
        Cdl cdl;
        const std::string sop = t.value(r, "ASC_SOP"), sat = t.value(r, "ASC_SAT");
        bool hasCdl = !sop.empty() && parseCdlSop(sop, cdl);
        if (!sat.empty()) {
            bool ok = false;
            const double v = QString::fromStdString(sat).toDouble(&ok);
            if (ok && std::isfinite(v)) {
                cdl.saturation = std::max(0.0, v);
                hasCdl = true;
            }
        }
        if (hasCdl) {
            setMediaCdl(*found, cdl);
            ++res.cdls;
            graded.push_back(found->id);
        }
        if (std::find(res.matched.begin(), res.matched.end(), found->id) == res.matched.end()) res.matched.push_back(found->id);
        // Its subclips carry its log too (made from it, they copied it then).
        for (MediaItem& sub : p.media)
            if (sub.subclipOf == found->id) {
                for (const ColumnField& cf : kColumns)
                    if (const auto v = found->metadata.find(cf.field); v != found->metadata.end()) sub.metadata[cf.field] = v->second;
                if (hasCdl) setMediaCdl(sub, cdl);
            }
    }
    if (cdlToClips && !graded.empty())
        for (Sequence& s : p.sequences)
            for (Track& tr : s.videoTracks)
                for (Clip& c : tr.clips) {
                    if (c.isGenerator()) continue;
                    const MediaItem* m = p.findMedia(c.mediaId);
                    const Id source = m && m->subclipOf ? m->subclipOf : c.mediaId;  // (a subclip's clips are its parent's picture)
                    if (std::find(graded.begin(), graded.end(), source) == graded.end()) continue;
                    Cdl cdl;
                    if (!m || !mediaCdl(*m, cdl)) continue;
                    setClipCdl(p, c, cdl);
                    ++res.clips;
                }
    return res;
}

AleTable aleFromMedia(const Project& p, const std::vector<Id>& ids, Rational fps) {
    AleTable t;
    if (!fps.valid()) fps = {24, 1};
    int height = 0;
    for (Id id : ids)
        if (const MediaItem* m = p.findMedia(id); m && m->hasVideo) height = std::max(height, m->height);
    // The heading's rate: the media's own (the most common among them), else the one given.
    std::map<std::pair<int, int>, int> rates;
    for (Id id : ids)
        if (const MediaItem* m = p.findMedia(id); m && m->fps.valid() && m->kind != MediaKind::Sequence && m->kind != MediaKind::Image)
            ++rates[{m->fps.num, m->fps.den}];
    if (!rates.empty()) {
        const auto most = std::max_element(rates.begin(), rates.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        fps = {most->first.first, most->first.second};
    }
    const double rate = fps.toDouble();
    QString fpsText = QString::number(rate, 'f', 3);  // (locale-independent)
    while (fpsText.endsWith('0')) fpsText.chop(1);
    if (fpsText.endsWith('.')) fpsText.chop(1);
    const std::string fpsValue = fpsText.toStdString();
    t.heading = {{"FIELD_DELIM", "TABS"},
                 {"VIDEO_FORMAT", height >= 1080 ? "1080" : height >= 720 ? "720" : height > 0 ? "NTSC" : "CUSTOM"},
                 {"AUDIO_FORMAT", "48khz"},
                 {"FPS", fpsValue}};
    t.columns = {"Name", "Tracks", "Start", "End", "Duration", "Tape", "Source File", "Scene", "Shot", "Take", "Camera", "Description", "Comments"};
    bool anyCdl = false;
    std::vector<std::pair<std::vector<std::string>, std::pair<bool, Cdl>>> rows;
    for (Id id : ids) {
        const MediaItem* m = p.findMedia(id);
        if (!m || m->kind == MediaKind::Sequence || m->kind == MediaKind::Image) continue;
        auto meta = [&](const char* key) {
            const auto it = m->metadata.find(key);
            return it == m->metadata.end() ? std::string() : it->second;
        };
        std::string tracks = m->hasVideo ? "V" : "";
        for (int c = 1; m->hasAudio && c <= std::clamp(m->channels, 1, 8); ++c) tracks += "A" + std::to_string(c);
        // Timecodes at the media's own rate, drop-frame only when its own timecode is.
        const Rational own = m->fps.valid() ? m->fps : fps;
        const double ownRate = own.toDouble();
        const FrameTime start = m->timecode > 0 ? FrameTime(std::llround(m->timecode * ownRate)) : 0;
        const FrameTime length = std::max<FrameTime>(1, FrameTime(std::llround(m->duration * ownRate)));
        const auto df = m->metadata.find("timecode_drop");
        const bool drop = isDropFrameRate(own) && df != m->metadata.end() && df->second == "1";
        std::vector<std::string> row{m->name,
                                     tracks,
                                     formatTimecode(start, own, drop),
                                     formatTimecode(start + length, own, drop),
                                     formatTimecode(length, own, drop),
                                     meta("tape"),
                                     m->path.empty() ? std::string() : baseName(m->path),  // (without Interpret Footage's settings)
                                     meta("scene"),
                                     meta("shot"),
                                     meta("take"),
                                     meta("camera"),
                                     meta("description"),
                                     meta("comment")};
        // The CDL: the media's own, else that of the first clip of it graded with one.
        Cdl cdl;
        bool has = mediaCdl(*m, cdl);
        for (const Sequence& s : p.sequences)
            for (const Track& tr : s.videoTracks)
                for (const Clip& c : tr.clips)
                    if (!has && c.mediaId == id && clipCdl(c, 0, cdl)) has = true;
        anyCdl |= has;
        rows.push_back({row, {has, cdl}});
    }
    if (anyCdl) {
        t.columns.push_back("ASC_SOP");
        t.columns.push_back("ASC_SAT");
    }
    for (auto& [row, cdl] : rows) {
        if (anyCdl) {
            row.push_back(cdl.first ? cdlSopText(cdl.second) : std::string());
            row.push_back(cdl.first ? cdlNumber(cdl.second.saturation) : std::string());
        }
        t.rows.push_back(row);
    }
    return t;
}

}  // namespace montage
