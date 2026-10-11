#include "GraphicsTemplate.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>

#include "Effects.h"
#include "Interpretation.h"
#include "Production.h"
#include "ProjectIO.h"

namespace montage {

namespace {

Effect* effectOf(Clip& c, Id effect) {
    if (effect == 0) return c.generator.empty() ? nullptr : &c.generator;
    for (Effect* e : {&c.generator, &c.motion, &c.audio, &c.timing})
        if (e->id == effect && !e->empty()) return e;
    for (Effect& e : c.effects)
        if (e.id == effect) return &e;
    return nullptr;
}

bool paramInfo(const Effect& e, const std::string& name, ParamInfo& out) {
    for (const ParamInfo& pi : effectParams(e))
        if (pi.name == name) {
            out = pi;
            return true;
        }
    return false;
}

const StringParamInfo* textInfo(const Effect& e, const std::string& name) {
    const EffectInfo* info = findEffectInfo(e.type);
    if (!info) return nullptr;
    for (const StringParamInfo& si : info->strings)
        if (si.name == name && (si.kind == StringKind::Text || si.kind == StringKind::MultilineText || si.kind == StringKind::Font)) return &si;
    return nullptr;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string number(double v) { return QString::number(v, 'g', 10).toStdString(); }

bool parseNumber(const std::string& s, double& v) {
    bool ok = false;
    v = QString::fromStdString(trim(s)).toDouble(&ok);
    return ok && std::isfinite(v);
}

std::string hexColor(double r, double g, double b) {
    char buf[8];
    auto byte = [](double v) { return int(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", byte(r), byte(g), byte(b));
    return buf;
}

bool parseColor(const std::string& in, double rgb[3]) {
    const std::string s = trim(in);
    auto hexDigit = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    if (!s.empty() && s[0] == '#') {
        const std::string h = s.substr(1);
        if (h.size() != 3 && h.size() != 6) return false;
        for (char c : h)
            if (hexDigit(c) < 0) return false;
        for (int i = 0; i < 3; ++i)
            rgb[i] = h.size() == 3 ? hexDigit(h[size_t(i)]) * 17 / 255.0 : (hexDigit(h[size_t(i) * 2]) * 16 + hexDigit(h[size_t(i) * 2 + 1])) / 255.0;
        return true;
    }
    // "r,g,b" in 0-255.
    std::vector<std::string> parts;
    size_t at = 0;
    for (size_t k; (k = s.find(',', at)) != std::string::npos; at = k + 1) parts.push_back(s.substr(at, k - at));
    parts.push_back(s.substr(at));
    if (parts.size() != 3) return false;
    for (int i = 0; i < 3; ++i) {
        double v;
        if (!parseNumber(parts[size_t(i)], v) || v < 0 || v > 255) return false;
        rgb[i] = v / 255.0;
    }
    return true;
}

// An id from a label: lower case letters, digits and underscores.
std::string slug(const std::string& label) {
    std::string out;
    for (char c : label) {
        if (std::isalnum(static_cast<unsigned char>(c))) out += char(std::tolower(static_cast<unsigned char>(c)));
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? std::string("field") : out;
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
        } else if (c == '"' && cur.empty()) {
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

// Puts a normalised value into the template copy.
void apply(Sequence& s, const TemplateField& f, const std::string& v) {
    Clip* c = edit::clipById(s, f.clip);
    if (!c) return;
    if (f.kind == "media") {
        c->mediaId = Id(std::strtoull(v.c_str(), nullptr, 10));
        return;
    }
    Effect* e = effectOf(*c, f.effect);
    if (!e) return;
    if (f.kind == "text") {
        e->strings[f.param] = v;
    } else if (f.kind == "color") {
        double rgb[3];
        if (!parseColor(v, rgb)) return;
        for (int i = 0; i < 3; ++i) e->params[f.param + "." + "rgb"[i]] = Param(rgb[i]);
    } else {
        double x;
        if (parseNumber(v, x)) e->params[f.param] = Param(x);
    }
}

}  // namespace

std::optional<TemplateField> fieldFor(const Sequence& s, Id clip, Id effect, const std::string& param, const std::string& label) {
    Clip* c = edit::clipById(const_cast<Sequence&>(s), clip);
    Effect* e = c ? effectOf(*c, effect) : nullptr;
    if (!e) return std::nullopt;
    TemplateField f;
    f.clip = clip;
    f.effect = e == &c->generator ? 0 : e->id;
    f.param = param;
    std::string name;
    if (const StringParamInfo* si = textInfo(*e, param)) {
        f.kind = "text";
        name = si->label;
    } else {
        ParamInfo pi;
        if (!paramInfo(*e, param, pi)) return std::nullopt;
        f.kind = pi.kind == ParamKind::Color ? "color" : pi.kind == ParamKind::Bool ? "toggle" : pi.kind == ParamKind::Choice ? "choice" : "number";
        name = pi.label;
    }
    f.label = label.empty() ? name : label;
    return f;
}

std::optional<TemplateField> mediaFieldFor(const Project& p, const Sequence& s, Id clip, const std::string& label) {
    const Clip* c = edit::clipById(s, clip);
    const MediaItem* m = c && c->mediaId ? p.findMedia(c->mediaId) : nullptr;
    if (!m || (m->kind != MediaKind::Image && m->kind != MediaKind::Video)) return std::nullopt;
    TemplateField f;
    f.clip = clip;
    f.kind = "media";
    f.label = label.empty() ? std::string(m->kind == MediaKind::Image ? "Image" : "Video") : label;
    return f;
}

edit::Result publishField(Sequence& s, TemplateField f, std::string* id) {
    if (!edit::clipById(s, f.clip)) return edit::Result::fail("No such clip in the sequence");
    for (const TemplateField& o : s.fields)
        if (o.clip == f.clip && o.effect == f.effect && o.param == f.param && o.kind == f.kind)
            return edit::Result::fail("\"" + o.label + "\" already publishes that");
    if (f.label.empty()) f.label = f.kind == "media" ? "Media" : f.param;
    std::string base = f.id.empty() ? slug(f.label) : f.id;
    auto taken = [&](const std::string& x) { return std::any_of(s.fields.begin(), s.fields.end(), [&](const TemplateField& o) { return o.id == x; }); };
    f.id = base;
    for (int n = 2; taken(f.id); ++n) f.id = base + "_" + std::to_string(n);
    if (id) *id = f.id;
    s.fields.push_back(std::move(f));
    return {};
}

edit::Result unpublishField(Sequence& s, const std::string& id) {
    const auto before = s.fields.size();
    std::erase_if(s.fields, [&](const TemplateField& f) { return f.id == id; });
    return before == s.fields.size() ? edit::Result::fail("No field \"" + id + "\"") : edit::Result{};
}

std::vector<TemplateField> liveFields(const Sequence& s) {
    std::vector<TemplateField> out;
    for (const TemplateField& f : s.fields) {
        Clip* c = edit::clipById(const_cast<Sequence&>(s), f.clip);
        if (c && (f.kind == "media" || effectOf(*c, f.effect))) out.push_back(f);
    }
    return out;
}

const Sequence* graphicOf(const Project& p, const Clip& c) {
    const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
    const Sequence* s = m && m->kind == MediaKind::Sequence ? p.findSequence(m->sequenceId) : nullptr;
    return s && !s->fields.empty() ? s : nullptr;
}

std::string fieldValue(const Sequence& tpl, const TemplateField& f) {
    Clip* c = edit::clipById(const_cast<Sequence&>(tpl), f.clip);
    if (!c) return {};
    if (f.kind == "media") return std::to_string(c->mediaId);
    const Effect* e = effectOf(*c, f.effect);
    if (!e) return {};
    if (f.kind == "text") {
        const StringParamInfo* si = textInfo(*e, f.param);
        return e->s(f.param, si ? si->def : std::string());
    }
    ParamInfo pi;
    const bool known = paramInfo(*e, f.param, pi);
    if (f.kind == "color")
        return hexColor(e->p(f.param + ".r", 0, known ? pi.def : 0), e->p(f.param + ".g", 0, known ? pi.defG : 0),
                        e->p(f.param + ".b", 0, known ? pi.defB : 0));
    const double v = e->p(f.param, 0, known ? pi.def : 0);
    if (f.kind == "toggle") return v > 0.5 ? "1" : "0";
    if (f.kind == "choice") return std::to_string(int(std::lround(v)));
    return number(v);
}

std::string fieldValue(const Sequence& tpl, const Clip& graphic, const TemplateField& f) {
    const auto it = graphic.fields.find(f.id);
    return it != graphic.fields.end() ? it->second : fieldValue(tpl, f);
}

bool normalizeFieldValue(const Project& p, const Sequence& tpl, const TemplateField& f, const std::string& in, std::string& out,
                         std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = f.label + ": " + why;
        return false;
    };
    Clip* c = edit::clipById(const_cast<Sequence&>(tpl), f.clip);
    if (!c) return fail("its clip is no longer in the graphic");
    if (f.kind == "media") {
        const MediaItem* m = p.findMedia(Id(std::strtoull(trim(in).c_str(), nullptr, 10)));
        if (!m || (m->kind != MediaKind::Image && m->kind != MediaKind::Video)) return fail("not an image or video in the project");
        out = std::to_string(m->id);
        return true;
    }
    const Effect* e = effectOf(*c, f.effect);
    if (!e) return fail("its effect is no longer in the graphic");
    if (f.kind == "text") {
        out = in;
        return true;
    }
    if (f.kind == "color") {
        double rgb[3];
        if (!parseColor(in, rgb)) return fail("\"" + in + "\" is not a colour (#rrggbb)");
        out = hexColor(rgb[0], rgb[1], rgb[2]);
        return true;
    }
    ParamInfo pi;
    if (!paramInfo(*e, f.param, pi)) return fail("no such setting");
    if (f.kind == "toggle") {
        const std::string v = lower(trim(in));
        if (v == "1" || v == "true" || v == "yes" || v == "on") out = "1";
        else if (v == "0" || v == "false" || v == "no" || v == "off" || v.empty()) out = "0";
        else return fail("\"" + in + "\" is not on or off");
        return true;
    }
    if (f.kind == "choice") {
        double x;
        if (parseNumber(in, x) && x == std::floor(x) && x >= 0 && x < double(pi.choices.size())) {
            out = std::to_string(int(x));
            return true;
        }
        for (size_t i = 0; i < pi.choices.size(); ++i)
            if (lower(pi.choices[i]) == lower(trim(in))) {
                out = std::to_string(i);
                return true;
            }
        return fail("\"" + in + "\" is not one of its choices");
    }
    double x;
    if (!parseNumber(in, x)) return fail("\"" + in + "\" is not a number");
    out = number(std::clamp(x, pi.min, pi.max));
    return true;
}

bool fieldRange(const Sequence& tpl, const TemplateField& f, double& min, double& max, std::vector<std::string>* choices) {
    Clip* c = edit::clipById(const_cast<Sequence&>(tpl), f.clip);
    const Effect* e = c ? effectOf(*c, f.effect) : nullptr;
    ParamInfo pi;
    if (!e || !paramInfo(*e, f.param, pi)) return false;
    min = pi.min;
    max = pi.max;
    if (choices) *choices = pi.choices;
    return true;
}

Sequence withFieldValues(const Project& p, const Sequence& tpl, const std::map<std::string, std::string>& values) {
    Sequence s = tpl;
    for (const TemplateField& f : tpl.fields) {
        const auto it = values.find(f.id);
        std::string v;
        if (it != values.end() && normalizeFieldValue(p, tpl, f, it->second, v)) apply(s, f, v);
    }
    return s;
}

edit::Result fillGraphicsFromTable(Project& p, Sequence& s, Id clip, const std::string& csv, std::vector<Id>* made,
                                   std::vector<std::string>* unmatched) {
    const Clip* first = edit::clipById(s, clip);
    const Sequence* tpl = first ? graphicOf(p, *first) : nullptr;
    const auto loc = first ? edit::locate(s, clip) : std::nullopt;
    if (!tpl || !loc || loc->track.kind != TrackKind::Video) return edit::Result::fail("That is not a graphic with fields to fill");
    std::string text = csv;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) text = text.substr(3);  // UTF-8 BOM
    const std::string line = text.substr(0, text.find('\n'));
    const char sep = line.find('\t') != std::string::npos && line.find(',') == std::string::npos ? '\t' : ',';
    const auto rows = csvRows(text, sep);
    if (rows.size() < 2) return edit::Result::fail("The table needs a header row and at least one row of values");
    // Columns to fields, by id or label in any case.
    const std::vector<TemplateField> fields = liveFields(*tpl);
    std::vector<const TemplateField*> columns;
    for (const std::string& h : rows[0]) {
        // A field's id first, then the first field with that label.
        const TemplateField* match = nullptr;
        for (const TemplateField& f : fields)
            if (!match && lower(trim(h)) == lower(f.id)) match = &f;
        for (const TemplateField& f : fields)
            if (!match && lower(trim(h)) == lower(trim(f.label))) match = &f;
        if (!match && unmatched && !trim(h).empty()) unmatched->push_back(trim(h));
        columns.push_back(match);
    }
    if (std::none_of(columns.begin(), columns.end(), [](const TemplateField* f) { return f; }))
        return edit::Result::fail("No column of the table names a field of the graphic");
    // Every value checked before anything changes.
    std::vector<std::map<std::string, std::string>> values;
    for (size_t r = 1; r < rows.size(); ++r) {
        std::map<std::string, std::string> v = first->fields;
        for (size_t k = 0; k < columns.size() && k < rows[r].size(); ++k) {
            if (!columns[k]) continue;
            std::string out, err;
            if (!normalizeFieldValue(p, *tpl, *columns[k], rows[r][k], out, &err))
                return edit::Result::fail("Row " + std::to_string(r + 1) + ": " + err);
            v[columns[k]->id] = out;
        }
        values.push_back(std::move(v));
    }
    const Clip base = *first;
    edit::Result all;
    all.ok = true;
    edit::clipById(s, clip)->fields = values[0];
    all.created.push_back(clip);
    FrameTime at = base.end();
    for (size_t r = 1; r < values.size(); ++r) {
        // On the graphic's track if the stretch is free, else the first track above where it is.
        auto freeOn = [&](int t) {
            if (t >= int(s.videoTracks.size())) return true;
            const Track& tr = s.videoTracks[size_t(t)];
            return !tr.locked && std::none_of(tr.clips.begin(), tr.clips.end(),
                                              [&](const Clip& o) { return o.start < at + base.duration && at < o.end(); });
        };
        int t = loc->track.index;
        while (!freeOn(t)) ++t;
        while (t >= int(s.videoTracks.size())) edit::addTrack(p, s, TrackKind::Video);
        Clip c = base;
        c.id = p.newId();
        c.start = at;
        c.linkGroup = 0;
        c.fields = values[r];
        const edit::Result put = edit::overwrite(p, s, {TrackKind::Video, t}, c);
        if (!put.ok) return put;
        all.created.insert(all.created.end(), put.created.begin(), put.created.end());
        at += base.duration;
    }
    if (made) *made = all.created;
    return all;
}

// ---- The library -----------------------------------------------------------------------------------------------------

std::string graphicsTemplatesDirectory() {
    if (const char* env = std::getenv("MONTAGE_TEMPLATES_DIR"); env && *env) return env;
    return (QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/Graphics Templates")).toStdString();
}

namespace {

std::string safeName(const std::string& name) {
    std::string out;
    for (char c : trim(name)) out += std::string("/\\:*?\"<>|").find(c) == std::string::npos ? c : '_';
    return out.empty() ? std::string("Graphic") : out;
}

}  // namespace

bool saveGraphicsTemplate(const Project& p, Id sequence, const std::string& name, const std::string& folder, std::string* path,
                          std::string* error) {
    const Sequence* src = p.findSequence(sequence);
    if (!src) {
        if (error) *error = "No such sequence";
        return false;
    }
    const std::string base = safeName(name);
    const QString dir = QString::fromStdString(folder) + '/' + QString::fromStdString(base);
    QDir(dir).removeRecursively();
    if (!QDir().mkpath(dir)) {
        if (error) *error = "Cannot make " + dir.toStdString();
        return false;
    }
    Project t;
    t.name = base;
    const std::vector<Id> ids = importFromProject(t, p, {sequence});
    if (ids.empty()) {
        if (error) *error = "The sequence could not be copied";
        return false;
    }
    Sequence* main = t.findSequence(ids[0]);
    main->name = base;
    for (MediaItem& m : t.media)
        if (m.kind == MediaKind::Sequence && m.sequenceId == main->id) m.name = base;
    t.activeSequence = main->id;
    // The files it plays, beside it (a file the template has twice is copied once).
    std::map<std::string, std::string> copied;
    std::set<QString> names;
    for (MediaItem& m : t.media) {
        if (m.kind == MediaKind::Sequence || m.path.empty()) continue;
        const std::string file = uninterpretedPath(m.path);
        if (!QFileInfo(QString::fromStdString(file)).isFile()) continue;  // (an image sequence or missing file stays where it is)
        std::string& to = copied[file];
        if (to.empty()) {
            const QFileInfo fi(QString::fromStdString(file));
            QString target = dir + '/' + fi.fileName();
            for (int n = 2; names.count(target) || QFileInfo::exists(target); ++n)
                target = dir + '/' + fi.completeBaseName() + ' ' + QString::number(n) + '.' + fi.suffix();
            names.insert(target);
            if (!QFile::copy(fi.absoluteFilePath(), target)) {
                if (error) *error = "Cannot copy " + file;
                return false;
            }
            to = target.toStdString();
        }
        m.path = to + m.path.substr(file.size());  // keeping how it is read (Interpret Footage)
        m.proxyPath.clear();
    }
    const std::string file = (dir + '/' + QString::fromStdString(base) + QStringLiteral(".montage")).toStdString();
    if (!saveProject(t, file, error)) return false;
    if (path) *path = file;
    return true;
}

std::vector<GraphicsTemplateInfo> listGraphicsTemplates(const std::string& folder) {
    std::vector<GraphicsTemplateInfo> out;
    const QDir root(QString::fromStdString(folder));
    for (const QString& sub : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase)) {
        const QString file = root.filePath(sub + '/' + sub + QStringLiteral(".montage"));
        Project t;
        if (!QFileInfo::exists(file) || !loadProject(file.toStdString(), t)) continue;
        const Sequence* s = t.active();
        if (!s) continue;
        GraphicsTemplateInfo info;
        info.name = sub.toStdString();
        info.path = file.toStdString();
        for (const TemplateField& f : liveFields(*s)) info.fields.push_back(f.label);
        info.seconds = double(s->duration()) / s->fpsValue();
        info.width = s->width;
        info.height = s->height;
        out.push_back(std::move(info));
    }
    return out;
}

edit::Result insertGraphicsTemplate(Project& p, Sequence& s, const std::string& path, FrameTime at, int track) {
    Project t;
    std::string err;
    if (!loadProject(path, t, &err)) return edit::Result::fail("Cannot read the template: " + err);
    const Sequence* main = t.active();
    if (!main) return edit::Result::fail("The template has no graphic");
    if (track >= int(s.videoTracks.size())) return edit::Result::fail("No such video track");
    const Id outer = s.id;
    const std::vector<Id> ids = importFromProject(p, t, {main->id});
    Sequence* sq = p.findSequence(outer);
    if (ids.empty() || !sq) return edit::Result::fail("The template could not be brought in");
    Id media = 0;
    for (const MediaItem& m : p.media)
        if (m.kind == MediaKind::Sequence && m.sequenceId == ids[0]) media = m.id;
    if (!media) return edit::Result::fail("The template could not be brought in");
    if (MediaItem* m = p.findMedia(media)) {
        const Sequence* g = p.findSequence(ids[0]);
        m->duration = double(g->duration()) / g->fpsValue();
        if (m->bin.empty()) m->bin = "Graphics";
        if (std::find(p.bins.begin(), p.bins.end(), m->bin) == p.bins.end()) p.bins.push_back(m->bin);
    }
    const int v = track < 0 ? edit::addTrack(p, *sq, TrackKind::Video).index : track;
    TrackRef audio{TrackKind::Audio, 0};
    return edit::placeMedia(p, *sq, media, at, 0, -1, {TrackKind::Video, v}, audio, false);
}

}  // namespace montage
