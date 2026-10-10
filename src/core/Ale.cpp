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

namespace montage {

namespace {

std::string lower(const std::string& s) { return QString::fromStdString(s).trimmed().toLower().toStdString(); }

std::string baseName(const std::string& path) { return QFileInfo(QString::fromStdString(path)).fileName().toStdString(); }
std::string stem(const std::string& name) { return QFileInfo(QString::fromStdString(name)).completeBaseName().toStdString(); }

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
            pick([&](const MediaItem& m) {
                return lower(stem(m.name)) == lower(stem(name)) || (!m.path.empty() && lower(stem(baseName(m.path))) == lower(stem(name)));
            });
        }
        FrameTime start = 0;
        if (!tape.empty() && parseTimecode(t.value(r, "Start"), fps, start))
            pick([&](const MediaItem& m) {
                const auto mt = m.metadata.find("tape");
                return mt != m.metadata.end() && lower(mt->second) == lower(tape) && m.timecode >= 0 &&
                       std::fabs(m.timecode * fps.toDouble() - double(start)) < 0.5;
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
    }
    if (cdlToClips && !graded.empty())
        for (Sequence& s : p.sequences)
            for (Track& tr : s.videoTracks)
                for (Clip& c : tr.clips) {
                    if (c.isGenerator() || std::find(graded.begin(), graded.end(), c.mediaId) == graded.end()) continue;
                    const MediaItem* m = p.findMedia(c.mediaId);
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
    const double rate = fps.toDouble();
    char fpsText[32];
    std::snprintf(fpsText, sizeof fpsText, "%.3f", rate);
    std::string fpsValue = fpsText;
    while (!fpsValue.empty() && fpsValue.back() == '0') fpsValue.pop_back();
    if (!fpsValue.empty() && fpsValue.back() == '.') fpsValue.pop_back();
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
        const FrameTime start = m->timecode > 0 ? FrameTime(std::llround(m->timecode * rate)) : 0;
        const FrameTime length = std::max<FrameTime>(1, FrameTime(std::llround(m->duration * rate)));
        const bool drop = isDropFrameRate(fps);
        std::vector<std::string> row{m->name,
                                     tracks,
                                     formatTimecode(start, fps, drop),
                                     formatTimecode(start + length, fps, drop),
                                     formatTimecode(length, fps, drop),
                                     meta("tape"),
                                     m->path.empty() ? std::string() : baseName(m->path),
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
                    if (!has && c.mediaId == id && clipCdl(c, c.start, cdl)) has = true;
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
