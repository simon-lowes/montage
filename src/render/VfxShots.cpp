#include "VfxShots.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QUrl>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "ColorSpace.h"
#include "media/Decoder.h"
#include "media/Exr.h"
#include "media/ImageSequence.h"

namespace montage {

namespace {

const char* const kDefaultPattern = "{shot}_comp_v###";

std::string patternOr(const std::string& pattern) { return pattern.empty() ? std::string(kDefaultPattern) : pattern; }

std::string extensionOf(const std::string& format) { return format == "tiff" ? "tif" : format.empty() ? "exr" : format; }

std::string csvField(const std::string& v) {
    if (v.find_first_of(",\"\n\r") == std::string::npos) return v;
    std::string out = "\"";
    for (char c : v) out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

// Text for a Nuke knob: quoted, with what Tcl would read inside quotes (\ " [ $) escaped.
std::string tclQuoted(const std::string& v) {
    std::string out = "\"";
    for (char c : v) {
        if (c == '\\' || c == '"' || c == '[' || c == ']' || c == '$') out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out += c;
    }
    return out + "\"";
}

std::string fpsText(const Rational& r) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", r.toDouble());
    return buf;
}

// The version's take on the clip (by its media), -1 when it has none.
int takeOf(const Clip& c, Id media) {
    for (size_t k = 0; k < c.takes.size(); ++k)
        if (c.takes[k].mediaId == media) return int(k);
    return -1;
}

// Where a version (its media's time 0 the frame numbered `zero`) goes in as a take on `c`: its in-point (sequence
// frames) so the frame numbered as the cut's first is where the cut's first frame was, wherever the clip's plate take
// now starts; false when the clip no longer has the footage it was pulled from.
bool versionIn(const Sequence& s, const VfxShotRecord& r, const Clip& c, int zero, double& in) {
    const int plate = takeOf(c, r.plateMedia);
    if (plate < 0 && c.mediaId != r.plateMedia) return false;
    const double seqFps = s.fpsValue(), mf = r.fps.valid() ? r.fps.toDouble() : seqFps;
    const double platePos = c.sourceIn + (plate >= 0 ? c.takes[size_t(plate)].offset : 0.0);  // the clip's in-point in the plate
    in = double(int64_t(r.cutIn) - zero) * seqFps / mf + (platePos - r.cutSource);
    return true;
}

}  // namespace

const char* vfxStatusName(int status) {
    static const char* names[] = {"In Progress", "For Review", "Approved", "Final"};
    return names[std::clamp(status, 0, 3)];
}

VfxShotRecord vfxShotRecord(const VfxShot& v) {
    VfxShotRecord r;
    r.name = v.name;
    r.clip = v.clip;
    r.cutSource = v.cutSource;
    r.folder = v.folder;
    r.plate = v.plate;
    r.firstFrame = v.firstFrame, r.lastFrame = v.lastFrame, r.cutIn = v.cutIn, r.cutOut = v.cutOut;
    r.width = v.width, r.height = v.height;
    r.fps = v.fps;
    r.colorSpace = v.colorSpace;
    r.amf = v.amf;
    r.script = v.script;
    return r;
}

void recordVfxShots(Sequence& s, const std::vector<VfxShot>& shots) {
    for (const VfxShot& v : shots) {
        VfxShotRecord r = vfxShotRecord(v);
        const Clip* c = edit::clipById(s, v.clip);
        // A clip pulled again (by the clip, whatever it is called now) keeps its versions, status and note.
        VfxShotRecord* old = vfxShotOfClip(s, v.clip);
        // (the footage it was pulled from: its plate take when it shows a version, else what it shows)
        if (c)
            r.plateMedia = old && old->plateMedia && (c->mediaId == old->plateMedia || takeOf(*c, old->plateMedia) >= 0) ? old->plateMedia
                                                                                                                       : c->mediaId;
        if (old) {
            r.status = old->status, r.note = old->note, r.versions = old->versions, r.version = old->version;
            if (r.script.empty()) r.script = old->script;
            *old = std::move(r);
        } else {
            s.vfxShots.push_back(std::move(r));
        }
    }
}

int vfxShownVersion(const Sequence& s, const VfxShotRecord& r) {
    const Clip* c = edit::clipById(s, r.clip);
    if (!c) return r.version;
    if (c->mediaId == r.plateMedia) return 0;
    for (const VfxVersion& v : r.versions)
        if (v.media == c->mediaId) return v.number;
    return r.version;
}

std::string vfxScriptFile(const VfxShotRecord& r, const std::string& folder) {
    std::string name;
    for (char ch : r.name) name += std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' ? ch : '_';
    if (name.empty() || name.find_first_not_of('_') == std::string::npos) name = "shot";
    return QDir(QString::fromStdString(folder)).filePath(QString::fromStdString(name) + QStringLiteral(".nk")).toStdString();
}

VfxShotRecord* findVfxShot(Sequence& s, const std::string& name) {
    for (VfxShotRecord& r : s.vfxShots)
        if (r.name == name) return &r;
    return nullptr;
}

const VfxShotRecord* findVfxShot(const Sequence& s, const std::string& name) { return findVfxShot(const_cast<Sequence&>(s), name); }

VfxShotRecord* vfxShotOfClip(Sequence& s, Id clip) {
    for (VfxShotRecord& r : s.vfxShots)
        if (clip && r.clip == clip) return &r;
    return nullptr;
}

const VfxShotRecord* vfxShotOfClip(const Sequence& s, Id clip) { return vfxShotOfClip(const_cast<Sequence&>(s), clip); }

std::string vfxVersionName(const std::string& pattern, const std::string& shot, int version) {
    QString out = QString::fromStdString(patternOr(pattern));
    out.replace(QStringLiteral("{shot}"), QString::fromStdString(shot));
    const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("#+")).match(out);
    const QString number = QStringLiteral("%1").arg(version, m.hasMatch() ? int(m.capturedLength()) : 3, 10, QChar('0'));
    if (m.hasMatch()) out.replace(m.capturedStart(), m.capturedLength(), number);
    else out += number;
    return out.toStdString();
}

std::string vfxVersionPath(const std::string& returns, const std::string& pattern, const VfxShotRecord& r, int version, const std::string& format) {
    const QString v = QString::fromStdString(vfxVersionName(pattern, r.name, version));
    return QDir(QString::fromStdString(returns)).filePath(v + "/" + v + ".####." + QString::fromStdString(extensionOf(format))).toStdString();
}

std::vector<std::string> vfxVersionProblems(const VfxShotRecord& r, const std::vector<int>& numbers, int width, int height,
                                            const std::string& firstFile, int movieFrames) {
    std::vector<std::string> out;
    auto range = [](int a, int b) { return std::to_string(a) + "-" + std::to_string(b); };
    if (!numbers.empty()) {
        const int first = numbers.front(), last = numbers.back();
        // (the gaps counted, the first ten named: a stray number far off must not list millions)
        int64_t missing = 0;
        std::string list;
        int named = 0;
        for (size_t k = 1; k < numbers.size(); ++k) {
            missing += std::max<int64_t>(0, int64_t(numbers[k]) - numbers[k - 1] - 1);
            for (int64_t n = int64_t(numbers[k - 1]) + 1; n < numbers[k] && named < 10; ++n, ++named) list += (named ? ", " : "") + std::to_string(n);
        }
        if (missing > 0) {
            if (missing > named) list += " and " + std::to_string(missing - named) + " more";
            out.push_back(std::to_string(missing) + (missing == 1 ? " frame missing: " : " frames missing: ") + list);
        }
        if (first > r.firstFrame || last < r.lastFrame)
            out.push_back("Frames " + range(first, last) + ", short of the pull's " + range(r.firstFrame, r.lastFrame));
    } else if (const int64_t pulled = int64_t(r.lastFrame) - r.firstFrame + 1; movieFrames >= 0 && movieFrames < pulled) {
        out.push_back(std::to_string(movieFrames) + " frames, short of the pull's " + std::to_string(pulled));
    }
    if (width > 0 && height > 0 && r.width > 0 && r.height > 0 && (width != r.width || height != r.height))
        out.push_back(std::to_string(width) + " x " + std::to_string(height) + ", not the plate's " + std::to_string(r.width) + " x " +
                      std::to_string(r.height));
    // OpenEXR's chromaticities against the plate's space (none written: nothing to say).
    double xy[8];
    const ColorSpace* space = findColorSpace(r.colorSpace);
    if (space && QString::fromStdString(firstFile).endsWith(QStringLiteral(".exr"), Qt::CaseInsensitive) && readExrChromaticities(firstFile, xy)) {
        double theirs[9], ours[9];
        chromaticitiesToXyz(xy, theirs);
        primariesToXyz(space->primaries, ours);
        double worst = 0;
        for (int k = 0; k < 9; ++k) worst = std::max(worst, std::fabs(theirs[k] - ours[k]));
        if (worst > 2e-3) out.push_back("Its chromaticities are not those of the plate's " + space->label);
    }
    return out;
}

bool scanVfxReturns(Project& p, Sequence& s, const std::string& folder, const std::string& pattern, VfxScan* out, std::string* error) {
    if (out) *out = {};
    const QDir dir(QString::fromStdString(folder));
    if (folder.empty() || !dir.exists()) {
        if (error) *error = "No folder at " + folder;
        return false;
    }
    if (s.vfxShots.empty()) return true;
    // The names to look for: the pattern with the shots' names as a choice and the version's digits caught.
    std::vector<std::string> names;
    for (const VfxShotRecord& r : s.vfxShots) names.push_back(r.name);
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    QString shots;
    for (const std::string& n : names) shots += (shots.isEmpty() ? "" : "|") + QRegularExpression::escape(QString::fromStdString(n));
    const QString pat = QString::fromStdString(patternOr(pattern.empty() ? s.vfxPattern : pattern));
    QString re;
    bool number = false;
    for (int i = 0; i < pat.size();) {
        if (pat.mid(i, 6) == QLatin1String("{shot}")) {
            re += "(?<shot>" + shots + ")";
            i += 6;
        } else if (pat[i] == '#') {
            while (i < pat.size() && pat[i] == '#') ++i;
            re += QStringLiteral("(?<version>\\d+)");
            number = true;
        } else {
            re += QRegularExpression::escape(QString(pat[i]));
            ++i;
        }
    }
    if (!number) re += QStringLiteral("(?<version>\\d+)");
    // Then frames ("name.1001.exr", "name_1001.exr") or a movie.
    const QRegularExpression frames(QStringLiteral("^") + re + QStringLiteral("[._](?<frame>\\d+)\\.(?<ext>exr|dpx|tif|tiff|png)$"),
                                    QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression movie(QStringLiteral("^") + re + QStringLiteral("\\.(?<ext>mov|mp4|mxf|mkv)$"), QRegularExpression::CaseInsensitiveOption);
    struct Found {
        std::string shot;
        int version = 0;
        std::map<int, QString> files;  // frames by number
        QString movie;
        int pad = 0;
        QString prefix, suffix;  // a frame's file: prefix + number + suffix
    };
    std::map<std::pair<std::string, int>, Found> found;
    int files = 0;
    QDirIterator it(dir.absolutePath(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && ++files < 500000) {
        const QString path = it.next();
        const QString name = it.fileName();
        if (QRegularExpressionMatch m = frames.match(name); m.hasMatch()) {
            Found& f = found[{m.captured("shot").toStdString(), m.captured("version").toInt()}];
            const QString frame = m.captured("frame");
            const QString prefix = QFileInfo(path).absolutePath() + "/" + name.left(m.capturedStart("frame"));
            const QString suffix = name.mid(m.capturedEnd("frame"));
            if (f.files.empty() || f.prefix == prefix) {  // (one run of frames a version)
                f.shot = m.captured("shot").toStdString();
                f.version = m.captured("version").toInt();
                f.prefix = prefix, f.suffix = suffix, f.pad = int(frame.size());
                f.files[frame.toInt()] = path;
            }
        } else if (QRegularExpressionMatch mv = movie.match(name); mv.hasMatch()) {
            Found& f = found[{mv.captured("shot").toStdString(), mv.captured("version").toInt()}];
            if (f.files.empty()) {
                f.shot = mv.captured("shot").toStdString();
                f.version = mv.captured("version").toInt();
                f.movie = path;
            }
        }
    }
    // (a shot's name as found, in any case, to its record)
    auto shotNamed = [&](const std::string& name) -> VfxShotRecord* {
        if (VfxShotRecord* r = findVfxShot(s, name)) return r;
        for (VfxShotRecord& r : s.vfxShots)
            if (QString::fromStdString(r.name).compare(QString::fromStdString(name), Qt::CaseInsensitive) == 0) return &r;
        return nullptr;
    };
    auto escapePercent = [](QString t) { return t.replace(QLatin1Char('%'), QStringLiteral("%%")); };
    for (auto& [key, f] : found) {
        VfxShotRecord* r = shotNamed(f.shot);
        if (!r || f.version <= 0) continue;
        const std::string label = r->name + " v" + std::to_string(f.version);
        // The media: frames as an image sequence at the footage's rate, its time 0 the first number found; a movie as it is.
        std::string path;
        std::vector<int> numbers;
        if (!f.files.empty()) {
            for (const auto& [n, file] : f.files) numbers.push_back(n);
            ImageSequence seq;
            seq.pattern = (escapePercent(f.prefix) + (f.pad > 1 ? QStringLiteral("%0") + QString::number(f.pad) + QStringLiteral("d") : QStringLiteral("%d")) +
                           escapePercent(f.suffix))
                              .toStdString();
            seq.first = numbers.front(), seq.last = numbers.back();
            seq.fps = r->fps;
            path = imageSequencePath(seq);
        } else {
            path = f.movie.toStdString();
        }
        const int zero = numbers.empty() ? r->firstFrame : numbers.front();  // the number at the media's time 0
        auto ver = std::find_if(r->versions.begin(), r->versions.end(), [&](const VfxVersion& v) { return v.number == f.version; });
        const bool same = ver != r->versions.end() && ver->path == path && p.findMedia(ver->media);
        MediaItem m;
        if (same) {
            m = *p.findMedia(ver->media);
        } else {
            m.id = p.newId();
            std::string why;
            if (!probeMedia(path, m, &why)) {
                if (out) out->problems.push_back(label + ": " + why);
                continue;
            }
            // Read as the plate was written, unless the file says its own space (renders come back as their plates went
            // out): a plate of a display's light (displayLightOf) always, so the version shows as the plate did.
            const ColorSpace* plateSpace = findColorSpace(r->colorSpace);
            if (plateSpace && ((m.colorSpace.empty() && m.colorOverride.empty()) || r->colorSpace.ends_with("+display")))
                m.colorOverride = r->colorSpace;
            m.name = vfxVersionName(pattern.empty() ? s.vfxPattern : pattern, r->name, f.version);
        }
        const int movieFrames = f.files.empty() && m.fps.valid() ? int(std::lround(m.duration * m.fps.toDouble())) : -1;
        std::vector<std::string> problems =
            vfxVersionProblems(*r, numbers, m.width, m.height, f.files.empty() ? std::string() : f.files.begin()->second.toStdString(), movieFrames);
        Clip* c = edit::clipById(s, r->clip);
        if (ver != r->versions.end() && ver->media && p.findMedia(ver->media)) {
            if (!same) {
                // The same version with other frames (more written since, or moved): the media item follows them, and its
                // take moves by as much as its first number did, so the frames stay where they were.
                MediaItem* old = p.findMedia(ver->media);
                ImageSequence before;
                const int oldZero = parseImageSequencePath(ver->path, before) ? before.first : r->firstFrame;
                const Id keep = old->id;
                *old = m;
                old->id = keep;
                ver->path = path;
                const double seqFps = s.fpsValue(), mf = r->fps.valid() ? r->fps.toDouble() : seqFps;
                if (const double delta = double(oldZero - zero) * seqFps / mf; c && delta != 0) {
                    if (c->mediaId == keep) {
                        c->sourceIn += delta;
                        for (Take& t : c->takes)
                            if (t.mediaId != keep) t.offset -= delta;
                    } else if (const int t = takeOf(*c, keep); t >= 0) {
                        c->takes[size_t(t)].offset += delta;
                    }
                }
            }
            ver->problems = problems;
            // A take let go (an audition finalized) comes back.
            double in = 0;
            if (c && c->mediaId != ver->media && takeOf(*c, ver->media) < 0 && versionIn(s, *r, *c, zero, in))
                edit::addTakes(p, s, r->clip, {{ver->media, in}});
            continue;
        }
        // A take on the shot's clip, placed so the frame numbered as the cut's first is where the cut's first frame was.
        double in = 0;
        if (!c) {
            if (out) out->problems.push_back(r->name + ": its clip is no longer in the cut");
            continue;
        }
        if (!versionIn(s, *r, *c, zero, in)) {
            if (out) out->problems.push_back(r->name + ": its clip no longer shows the footage it was pulled from");
            continue;
        }
        p.media.push_back(m);
        if (!edit::addTakes(p, s, r->clip, {{m.id, in}}).ok) {
            p.media.pop_back();
            continue;
        }
        VfxVersion v;
        v.number = f.version;
        v.path = path;
        v.media = m.id;
        v.problems = problems;
        r->versions.erase(std::remove_if(r->versions.begin(), r->versions.end(), [&](const VfxVersion& x) { return x.number == f.version; }),
                          r->versions.end());
        r->versions.push_back(std::move(v));
        std::sort(r->versions.begin(), r->versions.end(), [](const VfxVersion& a, const VfxVersion& b) { return a.number < b.number; });
        if (out) out->added.push_back(label);
    }
    if (out)
        for (const VfxShotRecord& r : s.vfxShots)
            for (const VfxVersion& v : r.versions)
                for (const std::string& pr : v.problems) out->problems.push_back(r.name + " v" + std::to_string(v.number) + ": " + pr);
    return true;
}

namespace edit {

Result setVfxVersion(Project& p, Sequence& s, Id clip, int version) {
    VfxShotRecord* r = vfxShotOfClip(s, clip);
    const Clip* c = clipById(s, clip);
    if (!r || !c) return Result::fail("That clip is not a VFX shot");
    if (version < 0) version = r->versions.empty() ? 0 : r->versions.back().number;
    Id media = r->plateMedia;
    if (version > 0) {
        const auto v = std::find_if(r->versions.begin(), r->versions.end(), [&](const VfxVersion& x) { return x.number == version; });
        if (v == r->versions.end()) return Result::fail(r->name + " has no version " + std::to_string(version));
        media = v->media;
    }
    if (c->takes.empty() && c->mediaId == media) {
        r->version = version;
        return {};
    }
    const int take = takeOf(*c, media);
    if (take < 0) return Result::fail("That version is not a take of the clip any more");
    if (Result res = pickTake(p, s, clip, take, false); !res.ok) return res;  // (the production sound stays)
    if ((r = vfxShotOfClip(s, clip))) r->version = version;
    return {};
}

Result stepVfxVersion(Project& p, Sequence& s, Id clip, int step) {
    const VfxShotRecord* r = vfxShotOfClip(s, clip);
    if (!r) return Result::fail("That clip is not a VFX shot");
    std::vector<int> numbers{0};
    for (const VfxVersion& v : r->versions) numbers.push_back(v.number);
    auto at = std::find(numbers.begin(), numbers.end(), vfxShownVersion(s, *r));  // (what the clip shows, however it came to)
    const long i = at == numbers.end() ? 0 : long(at - numbers.begin());
    const long to = std::clamp(i + (step > 0 ? 1 : -1), 0L, long(numbers.size()) - 1);
    if (to == i) return Result::fail(step > 0 ? r->name + " shows its latest version" : r->name + " shows its plate");
    return setVfxVersion(p, s, clip, numbers[size_t(to)]);
}

Result setVfxStatus(Sequence& s, const std::string& shot, int status, const std::string* note) {
    VfxShotRecord* r = findVfxShot(s, shot);
    if (!r) return Result::fail("No VFX shot named " + shot);
    if (status < 0 || status > 3) return Result::fail("A status is In Progress, For Review, Approved or Final");
    r->status = status;
    if (note) r->note = *note;
    return {};
}

}  // namespace edit

std::string vfxShotListCsv(const Sequence& s) {
    std::string csv = "Shot,Record In,Status,Version,Versions,Note,First Frame,Cut In,Cut Out,Last Frame,Plate,Colour Space,Problems\n";
    for (const VfxShotRecord& r : s.vfxShots) {
        const Clip* c = edit::clipById(s, r.clip);
        std::string versions, problems;
        for (const VfxVersion& v : r.versions) {
            versions += (versions.empty() ? "" : " ") + std::string("v") + std::to_string(v.number);
            if (v.number == vfxShownVersion(s, r))
                for (const std::string& pr : v.problems) problems += (problems.empty() ? "" : "; ") + pr;
        }
        const ColorSpace* space = findColorSpace(r.colorSpace);
        csv += csvField(r.name) + "," + (c ? std::to_string(c->start) : std::string()) + "," + vfxStatusName(r.status) + "," +
               (vfxShownVersion(s, r) ? "v" + std::to_string(vfxShownVersion(s, r)) : std::string("plate")) + "," + csvField(versions) + "," + csvField(r.note) + "," +
               std::to_string(r.firstFrame) + "," + std::to_string(r.cutIn) + "," + std::to_string(r.cutOut) + "," + std::to_string(r.lastFrame) +
               "," + csvField(r.plate) + "," + csvField(space ? space->label : r.colorSpace) + "," + csvField(problems) + "\n";
    }
    return csv;
}

std::string vfxShotListOtio(const Sequence& s) {
    const double rate = s.fpsValue();
    auto rt = [](double value, double r) { return QJsonObject{{"OTIO_SCHEMA", "RationalTime.1"}, {"rate", r}, {"value", value}}; };
    auto range = [&](double start, double duration, double r) {
        return QJsonObject{{"OTIO_SCHEMA", "TimeRange.1"}, {"start_time", rt(start, r)}, {"duration", rt(duration, r)}};
    };
    // The shots in the cut's order, each at its clip's place (gaps between), its plate's frames as its media.
    std::vector<std::pair<FrameTime, const VfxShotRecord*>> placed;
    for (const VfxShotRecord& r : s.vfxShots)
        if (const Clip* c = edit::clipById(s, r.clip)) placed.emplace_back(c->start, &r);
    std::sort(placed.begin(), placed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    QJsonArray children;
    double cursor = 0;
    for (const auto& [start, rp] : placed) {
        const VfxShotRecord& r = *rp;
        const Clip* c = edit::clipById(s, r.clip);
        if (double(start) < cursor) continue;  // (shots stacked on other tracks: the first)
        if (double(start) > cursor) {
            children.append(QJsonObject{{"OTIO_SCHEMA", "Gap.1"}, {"name", ""}, {"source_range", range(0, double(start) - cursor, rate)},
                                        {"effects", QJsonArray()}, {"markers", QJsonArray()}, {"metadata", QJsonObject()}, {"enabled", true}});
        }
        const double fr = r.fps.valid() ? r.fps.toDouble() : rate;
        const QFileInfo plate(QString::fromStdString(r.plate));
        const QString file = plate.fileName();
        const int hashes = int(file.count('#'));
        const qsizetype at = file.indexOf('#');
        QJsonObject ref{{"OTIO_SCHEMA", "ImageSequenceReference.1"},
                        {"name", QString::fromStdString(r.name)},
                        {"target_url_base", QUrl::fromLocalFile(plate.absolutePath() + "/").toString()},
                        {"name_prefix", at >= 0 ? file.left(at) : file},
                        {"name_suffix", at >= 0 ? file.mid(at + hashes) : QString()},
                        {"start_frame", r.firstFrame},
                        {"frame_step", 1},
                        {"rate", fr},
                        {"frame_zero_padding", hashes},
                        {"missing_frame_policy", "error"},
                        {"available_range", range(r.firstFrame, r.lastFrame - r.firstFrame + 1, fr)},
                        {"metadata", QJsonObject()}};
        QJsonArray versions;
        for (const VfxVersion& v : r.versions) {
            QJsonArray problems;
            for (const std::string& pr : v.problems) problems.append(QString::fromStdString(pr));
            versions.append(QJsonObject{{"number", v.number}, {"path", QString::fromStdString(v.path)}, {"problems", problems}});
        }
        const QJsonObject vfx{{"shot", QString::fromStdString(r.name)},
                              {"status", vfxStatusName(r.status)},
                              {"version", vfxShownVersion(s, r)},
                              {"note", QString::fromStdString(r.note)},
                              {"first_frame", r.firstFrame},
                              {"cut_in", r.cutIn},
                              {"cut_out", r.cutOut},
                              {"last_frame", r.lastFrame},
                              {"color_space", QString::fromStdString(r.colorSpace)},
                              {"amf", QString::fromStdString(r.amf)},
                              {"script", QString::fromStdString(r.script)},
                              {"versions", versions}};
        const double length = c ? double(c->duration) : double(r.cutOut - r.cutIn + 1) * rate / fr;
        children.append(QJsonObject{{"OTIO_SCHEMA", "Clip.2"},
                                    {"name", QString::fromStdString(r.name)},
                                    {"source_range", range(r.cutIn, double(r.cutOut - r.cutIn + 1), fr)},
                                    {"media_references", QJsonObject{{"DEFAULT_MEDIA", ref}}},
                                    {"active_media_reference_key", "DEFAULT_MEDIA"},
                                    {"effects", QJsonArray()},
                                    {"markers", QJsonArray()},
                                    {"metadata", QJsonObject{{"montage", QJsonObject{{"vfx", vfx}}}}},
                                    {"enabled", true}});
        cursor = double(start) + length;
    }
    const QJsonObject track{{"OTIO_SCHEMA", "Track.1"}, {"name", "VFX"}, {"kind", "Video"}, {"source_range", QJsonValue()},
                            {"children", children},     {"effects", QJsonArray()}, {"markers", QJsonArray()},
                            {"metadata", QJsonObject()}, {"enabled", true}};
    const QJsonObject stack{{"OTIO_SCHEMA", "Stack.1"}, {"name", "tracks"}, {"source_range", QJsonValue()}, {"children", QJsonArray{track}},
                            {"effects", QJsonArray()},  {"markers", QJsonArray()}, {"metadata", QJsonObject()}, {"enabled", true}};
    const QJsonObject root{{"OTIO_SCHEMA", "Timeline.1"},
                           {"name", QString::fromStdString(s.name + " VFX shots")},
                           {"global_start_time", rt(0, rate)},
                           {"tracks", stack},
                           {"metadata", QJsonObject()}};
    return QJsonDocument(root).toJson(QJsonDocument::Indented).toStdString();
}

std::string nukeScript(const VfxShotRecord& r, const std::string& writePath) {
    const ColorSpace* space = findColorSpace(r.colorSpace);
    const std::string spaceName = space ? space->label : r.colorSpace.empty() ? std::string("the footage's own") : r.colorSpace;
    const QString plate = QString::fromStdString(r.plate);
    const QString ext = QFileInfo(plate).suffix().toLower();
    const std::string type = ext == "tif" || ext == "tiff" ? "tiff" : ext == "dpx" ? "dpx" : "exr";
    const std::string format = std::to_string(r.width) + " " + std::to_string(r.height) + " 0 0 " + std::to_string(r.width) + " " +
                               std::to_string(r.height) + " 1 " + r.name;
    const std::string first = std::to_string(r.firstFrame), last = std::to_string(r.lastFrame);
    std::string nk;
    nk += "#! nuke\n";
    std::string plain = r.name;  // (a comment ends at the line's end: nothing in the name may start another)
    std::replace_if(plain.begin(), plain.end(), [](char ch) { return ch == '\n' || ch == '\r'; }, ' ');
    nk += "# Written by Montage for VFX shot " + plain + ": its plate read untouched and written to its next version.\n";
    nk += "Root {\n inputs 0\n first_frame " + first + "\n last_frame " + last + "\n lock_range true\n fps " + fpsText(r.fps) +
          "\n format " + tclQuoted(format) + "\n}\n";
    nk += "Read {\n inputs 0\n file_type " + type + "\n file " + tclQuoted(r.plate) + "\n format " + tclQuoted(format) + "\n first " + first +
          "\n last " + last + "\n origfirst " + first + "\n origlast " + last + "\n origset true\n raw true\n name Plate\n label " +
          tclQuoted(spaceName) + "\n xpos 0\n ypos 0\n}\n";
    nk += "Write {\n file " + tclQuoted(writePath) + "\n file_type " + type + "\n raw true\n create_directories true\n first " + first +
          "\n last " + last + "\n use_limit true\n name Return\n xpos 0\n ypos 120\n}\n";
    std::string about = "Shot " + r.name + "\nFrames " + first + "-" + last + ", the cut " + std::to_string(r.cutIn) + "-" + std::to_string(r.cutOut) +
                        "\nColour: " + spaceName + " (read and written raw)";
    if (!r.amf.empty()) about += "\nACES Metadata File: " + r.amf;
    nk += "StickyNote {\n inputs 0\n name Pull\n label " + tclQuoted(about) + "\n xpos 160\n ypos 0\n}\n";
    return nk;
}

}  // namespace montage
