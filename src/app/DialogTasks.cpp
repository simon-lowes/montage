// Montage — the kinds of task the app's dialogs run as background jobs (DialogJobs.h): for each, its work off the
// interface thread, from the project as it was and the task's own input, and how its result is applied.
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <mutex>

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>
#include <QTimer>

#include "AutoMixDialog.h"
#include "LensCalibrationDialog.h"
#include "DialogJobs.h"
#include "DialogTasks.h"
#include "EditorState.h"
#include "InspectorWidget.h"
#include "core/Adr.h"
#include "core/EditOps.h"
#include "core/ProjectIO.h"
#include "audio/DialogueAlign.h"
#include "core/Captions.h"
#include "media/Translator.h"
#include "render/ImageSubtitles.h"
#include "render/SdhDraft.h"
#include "media/Analysis.h"
#include "media/AutoDuck.h"
#include "media/LensCalibration.h"
#include "render/RenderFarm.h"
#include "render/SegmentedExport.h"
#include "render/VfxPull.h"
#include "render/VfxShots.h"

namespace montage {

namespace {

// (each kind's texts under the name of the dialog they were written for, as lupdate files them)
struct DuckText {
    Q_DECLARE_TR_FUNCTIONS(montage::AutoDuckDialog)
};
struct WindowText {
    Q_DECLARE_TR_FUNCTIONS(montage::MainWindow)
};
struct AdrText {
    Q_DECLARE_TR_FUNCTIONS(montage::AdrPanel)
};
struct CaptionText {
    Q_DECLARE_TR_FUNCTIONS(montage::CaptionsPanel)
};

Id idOf(const QJsonValue& v) { return Id(v.toString().toULongLong()); }

const Sequence* sequenceOf(const TaskRun& t) { return t.project->findSequence(idOf(t.input.value("sequence"))); }

bool noSequence(TaskRun& t) {
    t.error = "The sequence is gone";
    return false;
}

std::vector<Id> idsOf(const QJsonValue& v) {
    std::vector<Id> out;
    for (const QJsonValue& x : v.toArray()) out.push_back(idOf(x));
    return out;
}

QJsonArray spansJson(const Spans& spans) {
    QJsonArray a;
    for (const auto& [from, to] : spans) a.append(QJsonArray{from, to});
    return a;
}

Spans spansFrom(const QJsonValue& v) {
    Spans out;
    for (const QJsonValue& x : v.toArray())
        if (const QJsonArray pair = x.toArray(); pair.size() == 2) out.emplace_back(pair.at(0).toDouble(), pair.at(1).toDouble());
    return out;
}

// --- Auto Duck ---

DuckOptions duckOptionsFrom(const QJsonObject& j) {
    DuckOptions o;
    o.amountDb = j.value("amount").toDouble(o.amountDb);
    o.fadeDown = j.value("fadeDown").toDouble(o.fadeDown);
    o.fadeUp = j.value("fadeUp").toDouble(o.fadeUp);
    o.minPause = j.value("minPause").toDouble(o.minPause);
    o.minSpeech = j.value("minSpeech").toDouble(o.minSpeech);
    o.thresholdDb = j.value("threshold").toDouble(o.thresholdDb);
    o.useTranscripts = j.value("transcripts").toBool(true);
    for (const QJsonValue& r : j.value("skipRoles").toArray()) o.skipRoles.push_back(r.toString().toStdString());
    return o;
}

std::vector<int> trackPlaces(const Sequence& s, const std::vector<Id>& ids) {
    std::vector<int> out;
    for (int i = 0; i < int(s.audioTracks.size()); ++i)
        if (std::find(ids.begin(), ids.end(), s.audioTracks[size_t(i)].id) != ids.end()) out.push_back(i);
    return out;
}

bool runDuck(TaskRun& t) {
    const Sequence* s = sequenceOf(t);
    if (!s) return noSequence(t);
    if (t.input.value("refresh").toBool()) {
        // The sound under every duck kept up to date is listened to (and kept: media/AutoDuck.h), so that they can
        // be made again.
        for (const Track& track : s->audioTracks)
            for (const Clip& c : track.clips) {
                if (!c.duck) continue;
                dialogueSpans(*t.project, *s, duckTracks(*s, *c.duck), duckOptions(*c.duck), &t.error, t.cancel);
                if (t.cancelled() || !t.error.empty()) return false;
            }
        return true;
    }
    const Spans spans = dialogueSpans(*t.project, *s, trackPlaces(*s, idsOf(t.input.value("tracks"))), duckOptionsFrom(t.input.value("options").toObject()),
                                      &t.error, t.cancel);
    if (t.cancelled() || !t.error.empty()) return false;
    t.result["spans"] = spansJson(spans);
    return true;
}

QJsonObject applyDuck(EditorState* state, const QJsonObject& input, const QJsonObject& result) {
    if (input.value("refresh").toBool()) {
        // Part of the edit that asked for it while that is still the latest; a step of its own otherwise.
        auto again = [](Project& p, Sequence& s) { return refreshKeptDucks(p, s, nullptr).clips > 0; };
        if (QString::number(state->history().revision()) == input.value("revision").toString()) state->amend(again);
        else state->edit(DuckText::tr("Auto Duck"), again);
        return {};
    }
    const Spans spans = spansFrom(result.value("spans"));
    const DuckOptions o = duckOptionsFrom(input.value("options").toObject());
    const std::vector<Id> music = idsOf(input.value("clips")), tracks = idsOf(input.value("tracks"));
    const bool keep = input.value("keep").toBool();
    int changed = 0, kept = 0;
    state->edit(DuckText::tr("Auto Duck"), [&](Project& p, Sequence& s) {
        for (Id id : music) {
            Clip* c = edit::clipById(s, id);
            if (!c) continue;
            if (keep) {
                // Made from the dialogue as it was heard, then again from where it is now: the same unless it moved.
                bool did = duckClipKept(*c, s, spans, o, tracks);
                if (const DuckRefresh r = refreshKeptDucks(p, s, nullptr); r.clips) did = true;
                kept += c->duck ? c->duck->kept : 0;
                changed += did ? 1 : 0;
            } else {
                const bool had = c->duck.has_value();
                c->duck.reset();
                changed += duckClip(*c, s, spans, o) || had ? 1 : 0;
            }
        }
        return changed > 0;
    });
    QString said = spans.empty() ? DuckText::tr("No dialogue found on those tracks")
                                 : DuckText::tr("Ducked %n clip(s) under %1 stretch(es) of dialogue", "", changed).arg(spans.size());
    if (kept) said += QStringLiteral(" ") + DuckText::tr("(%n keyframe(s) moved by hand kept)", "", kept);
    return {{"changed", changed}, {"kept", kept}, {"message", said}};
}

// --- Detect Scene Cuts ---

bool runSceneCuts(TaskRun& t) {
    const std::vector<double> cuts = detectSceneCuts(t.input.value("path").toString().toStdString(), 0.5, t.progress, t.cancel, &t.error);
    if (t.cancelled() || !t.error.empty()) return false;
    QJsonArray a;
    for (double c : cuts) a.append(c);
    t.result["cuts"] = a;
    return true;
}

QJsonObject applySceneCuts(EditorState* state, const QJsonObject& input, const QJsonObject& result) {
    const bool markOnly = input.value("markOnly").toBool();
    const std::vector<Id> clips = idsOf(input.value("clips"));
    const Id clipId = clips.empty() ? 0 : clips[0];
    std::vector<double> cuts;
    for (const QJsonValue& v : result.value("cuts").toArray()) cuts.push_back(v.toDouble());
    int made = 0;
    const bool ok = state->apply(markOnly ? WindowText::tr("Detect Shot Changes") : WindowText::tr("Detect Scene Cuts"), [&](Project& p, Sequence& sq) {
        Clip* clip = edit::clipById(sq, clipId);
        if (!clip) return edit::Result::fail("The clip is gone");
        // Map source times to timeline frames inside the clip.
        std::vector<FrameTime> frames;
        std::vector<FrameTime> sources;
        for (double sec : cuts) {
            double src = sec * sq.fpsValue();
            double local = clip->ramped() ? clip->localForSource(src) : (src - clip->sourceIn) / clip->speed;
            if (clip->reverse) local = double(clip->duration) - local;
            FrameTime f = clip->start + FrameTime(std::llround(local));
            if (f > clip->start && f < clip->end()) frames.push_back(f), sources.push_back(FrameTime(std::llround(src)));
        }
        // Each a shot-change marker on the clip's source, so it stays on that moment of the footage as the clip
        // is moved, slipped or trimmed (the razor's cuts are through edits, which captions do not count as shot
        // changes).
        for (FrameTime src : sources)
            if (std::none_of(clip->markers.begin(), clip->markers.end(), [src](const Marker& mk) { return mk.shotChange && mk.t == src; })) {
                Marker mk;
                mk.t = src;
                mk.name = WindowText::tr("Shot change").toStdString();
                mk.shotChange = true;
                clip->markers.push_back(mk);
                if (markOnly) ++made;
            }
        std::sort(clip->markers.begin(), clip->markers.end(), [](const Marker& a, const Marker& b) { return a.t < b.t; });
        if (markOnly) {
            if (!made) return edit::Result::fail("No new shot changes found in this clip");
            return edit::Result{};
        }
        std::vector<TrackRef> tracks;
        for (Id l : edit::linkedClips(sq, clipId))
            if (auto loc = edit::locate(sq, l)) tracks.push_back(loc->track);
        for (FrameTime f : frames) {
            std::map<Id, Id> regroup;
            for (TrackRef t : tracks) {
                const Clip* under = edit::clipAt(sq, t, f);
                Id group = under ? under->linkGroup : 0;
                auto r = edit::razor(p, sq, t, f);
                if (r.ok && group && !r.created.empty()) {
                    auto it = regroup.find(group);
                    if (it == regroup.end()) it = regroup.emplace(group, p.newId()).first;
                    if (Clip* rc = edit::clipById(sq, r.created[0])) rc->linkGroup = it->second;
                }
            }
            ++made;
        }
        if (!made) return edit::Result::fail("No scene cuts found in this clip");
        return edit::Result{};
    });
    QJsonObject out{{"made", ok ? made : 0}, {"failed", !ok}};
    if (ok && made)
        out["message"] = markOnly ? WindowText::tr("Marked %n shot change(s)", "", made) : WindowText::tr("Cut the clip at %n scene change(s)", "", made);
    return out;
}

// --- Auto Mix ---

QJsonArray pairsJson(const std::vector<std::pair<FrameTime, double>>& pairs) {
    QJsonArray a;
    for (const auto& [t, v] : pairs) a.append(QJsonArray{double(t), v});
    return a;
}

std::vector<std::pair<FrameTime, double>> pairsFrom(const QJsonValue& v) {
    std::vector<std::pair<FrameTime, double>> out;
    for (const QJsonValue& x : v.toArray())
        if (const QJsonArray pair = x.toArray(); pair.size() == 2) out.emplace_back(FrameTime(pair.at(0).toDouble()), pair.at(1).toDouble());
    return out;
}

bool runMix(TaskRun& t) {
    const Sequence* s = sequenceOf(t);
    if (!s) return noSequence(t);
    const std::vector<ClipMix> plan = planMix(*t.project, *s, MixOptions{}, t.progress, t.cancel, &t.error);
    if (t.cancelled()) return false;
    if (plan.empty() && !t.error.empty()) return false;
    t.result["plan"] = mixPlanToJson(plan);
    return true;
}

// The plan is looked over in the Auto Mix dialog, then written as one undo step.
QJsonObject applyMixPlan(EditorState* state, const QJsonObject&, const QJsonObject& result) {
    std::vector<ClipMix> plan = mixPlanFromJson(result.value("plan"));
    if (plan.empty()) return {{"changed", -1}, {"message", QCoreApplication::translate("montage::AutoMixDialog", "There is no audio to mix")}};
    AutoMixDialog dlg(state, std::move(plan), QApplication::activeWindow());
    if (dlg.exec() != QDialog::Accepted) return {{"changed", -1}, {"failed", true}};
    const int n = AutoMixDialog::apply(state, dlg.plan(), dlg.options());
    return {{"changed", n}, {"message", QCoreApplication::translate("montage::AutoMixDialog", "Mixed %n clip(s)", "", n)}};
}

// --- Conform Check ---

bool runConform(TaskRun& t) {
    const Sequence* s = sequenceOf(t);
    if (!s) return noSequence(t);
    ConformOptions o;
    o.referenceStart = FrameTime(t.input.value("referenceStart").toDouble());
    const std::vector<ConformShot> shots = conformCheck(*t.project, *s, idOf(t.input.value("reference")), o, t.progress, t.cancel, &t.error);
    if (t.cancelled()) return false;
    // (a reference that cannot be read is a result too: the dialog says why)
    t.result["shots"] = conformShotsToJson(shots);
    t.result["error"] = QString::fromStdString(t.error);
    t.error.clear();
    return true;
}

// --- Lens calibration from a clip of a checkerboard ---
//
// The boards found so far are kept as the frames are read: taken up after a crash, it goes on from the next frame.

QJsonArray pointsJson(const std::vector<Point2>& v) {
    QJsonArray a;
    for (const Point2& p : v) a.append(QJsonArray{p.x, p.y});
    return a;
}

std::vector<Point2> pointsFrom(const QJsonValue& v) {
    std::vector<Point2> out;
    for (const QJsonValue& x : v.toArray())
        if (const QJsonArray pair = x.toArray(); pair.size() == 2) out.push_back({pair.at(0).toDouble(), pair.at(1).toDouble()});
    return out;
}

QJsonObject boardsJson(const ClipBoards& b) {
    QJsonArray views, times;
    for (const auto& v : b.views) views.append(pointsJson(v));
    for (double t : b.times) times.append(t);
    return QJsonObject{{"width", b.width}, {"height", b.height}, {"frames", b.frames}, {"next", b.nextFrame},
                       {"read", b.framesRead}, {"found", b.boardsFound}, {"views", views}, {"times", times}};
}

ClipBoards boardsFrom(const QJsonObject& j) {
    ClipBoards b;
    b.width = j.value("width").toInt(), b.height = j.value("height").toInt(), b.frames = j.value("frames").toInt();
    b.nextFrame = j.value("next").toInt(), b.framesRead = j.value("read").toInt(), b.boardsFound = j.value("found").toInt();
    for (const QJsonValue& v : j.value("views").toArray()) b.views.push_back(pointsFrom(v));
    for (const QJsonValue& t : j.value("times").toArray()) b.times.push_back(t.toDouble());
    return b;
}

bool runLensBoard(TaskRun& t) {
    const QJsonObject& in = t.input;
    LensFitOptions o;
    o.model = lens::Distortion::Model(in.value("model").toInt(int(o.model)));
    o.fitSqueeze = in.value("fitSqueeze").toBool();
    o.fitCenter = in.value("fitCenter").toBool(true);
    ClipBoards boards;
    QJsonValue kept;
    if (t.part(QStringLiteral("boards"), &kept)) boards = boardsFrom(kept.toObject());
    const int passedOver = boards.nextFrame;
    ClipCalibration cc;
    const bool fitted = calibrateClip(in.value("path").toString().toStdString(), in.value("from").toDouble(), in.value("to").toDouble(),
                                      in.value("cols").toInt(), in.value("rows").toInt(), o, cc, &t.error, t.progress, t.cancel, 40, &boards,
                                      [&](const ClipBoards& b) { t.keepPart(QStringLiteral("boards"), boardsJson(b)); });
    if (t.cancelled()) return false;
    t.result = QJsonObject{{"width", cc.width}, {"height", cc.height}, {"framesRead", cc.framesRead}, {"boardsFound", cc.boardsFound},
                           {"passedOver", std::min(passedOver, boards.nextFrame)}};
    // (a fit that could not be made is a result too: the dialog says why)
    if (!fitted) {
        t.result["error"] = QString::fromStdString(t.error);
        t.error.clear();
        return true;
    }
    t.result["fit"] = lensFitToJson(cc.fit);
    return true;
}

// --- DCP and IMF masters ---
//
// Where the pictures can be coded apart from the package (JPEG 2000, as a render farm's segments are: Dcp.h's and
// Imf.h's `codestreams`), they are coded a stretch at a time into a folder beside the package, and each finished
// stretch is noted in the job's folder. Taken up again after a crash, the job passes over those stretches (their
// files are not touched) and goes on from the one being written; the package is then made from the coded pictures
// and the folder removed. It needs room for the pictures twice while it is made: without it, or where the pictures
// cannot be coded apart (an encrypted DCP, IMF applications other than #2E, supplemental packages), the package is
// made in one go, and starts again when taken up. A package made whose check was cut short is checked again, not
// made again. A job stopped by hand, or that failed, leaves no pictures behind.

const char* const kPicturesPrefix = ".montage-pictures-";

QJsonArray stringsJson(const std::vector<std::string>& v) {
    QJsonArray a;
    for (const std::string& x : v) a.append(QString::fromStdString(x));
    return a;
}

std::vector<std::string> stringsFrom(const QJsonValue& v) {
    std::vector<std::string> out;
    for (const QJsonValue& x : v.toArray()) out.push_back(x.toString().toStdString());
    return out;
}

// --- VFX pulls ---
//
// Each shot finished is kept as it is pulled: taken up after a crash, the pull passes over those (their files are not
// written again) and goes on from the next.

VfxPullOptions vfxOptionsFrom(const QJsonObject& j) {
    VfxPullOptions o;
    auto str = [&](const char* key, std::string& to) { to = j.value(key).toString(QString::fromStdString(to)).toStdString(); };
    str("folder", o.folder);
    str("format", o.format);
    str("colorSpace", o.colorSpace);
    str("compression", o.compression);
    str("namePattern", o.namePattern);
    str("returns", o.returns);
    str("returnsPattern", o.returnsPattern);
    o.handles = j.value("handles").toInt(o.handles);
    o.cutIn = j.value("cutIn").toInt(o.cutIn);
    o.amf = j.value("amf").toBool(o.amf);
    o.fdl = j.value("fdl").toBool(o.fdl);
    o.sourceLayers = j.value("sourceLayers").toBool(o.sourceLayers);
    o.mattes = j.value("mattes").toBool(o.mattes);
    o.stMaps = j.value("stMaps").toBool(o.stMaps);
    o.scripts = j.value("scripts").toBool(o.scripts);
    return o;
}

QJsonObject vfxShotJson(const VfxShot& v) {
    auto q = [](const std::string& x) { return QString::fromStdString(x); };
    return QJsonObject{{"name", q(v.name)}, {"media", q(v.media)}, {"folder", q(v.folder)}, {"firstFrame", v.firstFrame}, {"lastFrame", v.lastFrame},
                       {"cutIn", v.cutIn}, {"cutOut", v.cutOut}, {"sourceIn", v.sourceIn}, {"sourceOut", v.sourceOut},
                       {"headHandle", v.headHandle}, {"tailHandle", v.tailHandle}, {"amf", q(v.amf)}, {"fdl", q(v.fdl)},
                       {"layers", stringsJson(v.layers)}, {"undistortMap", q(v.undistortMap)}, {"redistortMap", q(v.redistortMap)},
                       {"overscanWidth", v.overscanWidth}, {"overscanHeight", v.overscanHeight}, {"deep", v.deep},
                       {"clip", QString::number(v.clip)}, {"plate", q(v.plate)}, {"colorSpace", q(v.colorSpace)}, {"width", v.width},
                       {"height", v.height}, {"fps", QJsonArray{v.fps.num, v.fps.den}}, {"cutSource", v.cutSource}, {"script", q(v.script)}};
}

VfxShot vfxShotFrom(const QJsonObject& j) {
    VfxShot v;
    auto s = [&](const char* key) { return j.value(key).toString().toStdString(); };
    v.name = s("name"), v.media = s("media"), v.folder = s("folder");
    v.firstFrame = j.value("firstFrame").toInt(), v.lastFrame = j.value("lastFrame").toInt();
    v.cutIn = j.value("cutIn").toInt(), v.cutOut = j.value("cutOut").toInt();
    v.sourceIn = j.value("sourceIn").toDouble(), v.sourceOut = j.value("sourceOut").toDouble();
    v.headHandle = j.value("headHandle").toInt(), v.tailHandle = j.value("tailHandle").toInt();
    v.amf = s("amf"), v.fdl = s("fdl");
    v.layers = stringsFrom(j.value("layers"));
    v.undistortMap = s("undistortMap"), v.redistortMap = s("redistortMap");
    v.overscanWidth = j.value("overscanWidth").toInt(), v.overscanHeight = j.value("overscanHeight").toInt();
    v.deep = j.value("deep").toBool();
    v.clip = idOf(j.value("clip"));
    v.plate = s("plate"), v.colorSpace = s("colorSpace");
    v.width = j.value("width").toInt(), v.height = j.value("height").toInt();
    if (const QJsonArray fps = j.value("fps").toArray(); fps.size() == 2) v.fps = Rational{fps.at(0).toInt(), fps.at(1).toInt()};
    v.cutSource = j.value("cutSource").toDouble();
    v.script = s("script");
    return v;
}

bool runVfxPull(TaskRun& t) {
    const Sequence* s = sequenceOf(t);
    if (!s) return noSequence(t);
    const VfxPullOptions o = vfxOptionsFrom(t.input.value("options").toObject());
    VfxPullResume resume;
    QJsonValue kept;
    QJsonArray pulled;
    if (t.part(QStringLiteral("shots"), &kept)) pulled = kept.toArray();
    for (const QJsonValue& v : pulled) resume.finished.push_back(vfxShotFrom(v.toObject()));
    resume.pulled = [&](const VfxShot& shot) {
        if (std::any_of(resume.finished.begin(), resume.finished.end(), [&](const VfxShot& d) { return d.clip == shot.clip && d.name == shot.name; })) return;
        pulled.append(vfxShotJson(shot));
        t.keepPart(QStringLiteral("shots"), pulled);
    };
    std::vector<VfxShot> shots;
    const bool ok = exportVfxPulls(*t.project, *s, idsOf(t.input.value("clips")), o, &shots,
                                   [&](double f, FrameTime) {
                                       if (t.progress) t.progress(f);
                                   },
                                   t.cancel, &t.error, &resume);
    if (t.cancelled() || !ok) return false;
    QJsonArray all;
    for (const VfxShot& shot : shots) all.append(vfxShotJson(shot));
    t.result["shots"] = all;
    t.result["passedOver"] = int(resume.finished.size());
    return true;
}

QJsonObject applyVfxPull(EditorState* state, const QJsonObject& input, const QJsonObject& result) {
    // The shots kept with the sequence, their versions looked for in the returns folder from now on.
    const VfxPullOptions o = vfxOptionsFrom(input.value("options").toObject());
    std::vector<VfxShot> shots;
    for (const QJsonValue& v : result.value("shots").toArray()) shots.push_back(vfxShotFrom(v.toObject()));
    QDir().mkpath(QString::fromStdString(o.returns));
    state->edit(WindowText::tr("Export VFX Pulls"), [&](Project&, Sequence& seq) {
        recordVfxShots(seq, shots);
        seq.vfxReturns = o.returns;
        return true;
    });
    return QJsonObject{{"message", WindowText::tr("Pulled %n shot(s) with a pull list into %1", "", int(shots.size()))
                                       .arg(QDir::toNativeSeparators(QString::fromStdString(o.folder)))}};
}

// --- DCP and IMF masters, again ---

// Codes frames [0, frames) in stretches of `perPart` with `code(folder, from, to, progress)`, passing over those an
// earlier run finished. `share`: how much of the job's progress this is. `made` lists the stretches: where each
// starts and ends, whether it was passed over, and when its last picture was written (as its file says).
bool codePictures(TaskRun& t, const QString& pictures, int64_t frames, int64_t perPart, double share, QJsonArray* made,
                  const std::function<bool(const std::string& folder, int64_t from, int64_t to, const std::function<bool(double)>& progress)>& code) {
    QDir().mkpath(pictures);
    const int64_t parts = (frames + perPart - 1) / perPart;
    for (int64_t i = 0; i < parts; ++i) {
        const int64_t from = i * perPart, to = std::min(frames, from + perPart);
        const QString name = QStringLiteral("pictures-%1-%2").arg(from).arg(to);
        const QFileInfo last(QString::fromStdString(codestreamFile(pictures.toStdString(), to - 1)));
        // (a stretch counts as finished only with its record and its last picture both there)
        const bool finished = t.part(name, nullptr) && last.exists();
        if (!finished) {
            const bool ok = code(pictures.toStdString(), from, to, [&](double f) {
                t.progress(share * (double(from) + f * double(to - from)) / double(frames));
                return !t.cancelled();
            });
            if (!ok || t.cancelled()) return false;
            t.keepPart(name, true);
        }
        t.progress(share * double(to) / double(frames));
        if (made)
            made->append(QJsonObject{{"from", double(from)},
                                     {"to", double(to)},
                                     {"passedOver", finished},
                                     {"written", double(QFileInfo(last.filePath()).lastModified().toMSecsSinceEpoch())}});
    }
    return true;
}

// Whether the folder the package goes in has room for its pictures a second time (`bytes`, a generous guess).
bool roomFor(const QString& parent, double bytes) {
    QDir().mkpath(parent);
    const QStorageInfo disk(parent);
    return disk.isValid() && double(disk.bytesAvailable()) > 2.2 * bytes;
}

QString picturesFolder(const QString& jobFolder, const QString& parent) {
    return jobFolder.isEmpty() || parent.isEmpty() ? QString() : QDir(parent).filePath(QLatin1String(kPicturesPrefix) + QFileInfo(jobFolder).fileName());
}

void removePictures(const QString& pictures) {
    if (!pictures.isEmpty() && QFileInfo(pictures).fileName().startsWith(QLatin1String(kPicturesPrefix))) QDir(pictures).removeRecursively();
}

void discardPictures(const QJsonObject& input, const QString& folder) { removePictures(picturesFolder(folder, input.value("parent").toString())); }

// A package an earlier run of this job made (its check cut short): where it is, if it is still there.
bool madeBefore(const TaskRun& t, QJsonObject* made) {
    QJsonValue kept;
    if (!t.part(QStringLiteral("package"), &kept) || !kept.isObject()) return false;
    *made = kept.toObject();
    return QFileInfo(made->value("folder").toString()).isDir();
}

bool runDcp(TaskRun& t) {
    const Sequence* s = sequenceOf(t);
    if (!s) return noSequence(t);
    DcpSettings settings;
    if (!dcpSettingsFromJson(t.input.value("settings").toObject(), settings, &t.error)) return false;
    const QString parent = t.input.value("parent").toString();
    const int64_t frames = dcpPackageFrames(*s, settings);
    const QString pictures = picturesFolder(t.folder, parent);
    DcpResult made;
    QJsonObject before;
    const bool already = madeBefore(t, &before);
    if (already) made = dcpResultFromJson(before);
    const bool apart = !already && !pictures.isEmpty() && !settings.encrypt && settings.codestreams.empty() && frames > 0 &&
                       (QFileInfo::exists(pictures) ||
                        roomFor(parent, double(frames) * 1.4e6 * (settings.resolution == "4k" ? 2 : 1) * (settings.stereo ? 2 : 1)));
    QJsonArray stretches = already ? before.value("pictures").toArray() : QJsonArray();
    const double share = apart ? 0.75 : 0;
    if (apart) {
        const int64_t perPart = std::max<int64_t>(1, t.input.value("partFrames").toInt(10 * std::max(1, dcpFrameRate(*s, settings.fps))));
        if (!codePictures(t, pictures, frames, perPart, share, &stretches, [&](const std::string& folder, int64_t from, int64_t to, const auto& progress) {
                DcpSettings part = settings;
                part.codestreams = folder;
                part.codeFrom = from, part.codeTo = to;
                return exportDcp(*t.project, *s, part, parent.toStdString(), nullptr, progress, &t.error);
            }))
            return false;
        settings.codestreams = pictures.toStdString();
    }
    if (!already) {
        if (!exportDcp(*t.project, *s, settings, parent.toStdString(), &made, [&](double f) {
                t.progress(share + (0.85 - share) * f);
                return !t.cancelled();
            }, &t.error))
            return false;
        QJsonObject kept = dcpResultToJson(made);
        kept["pictures"] = stretches;
        t.keepPart(QStringLiteral("package"), kept);
    }
    removePictures(pictures);
    // Then checked as a server would, reading every file back.
    std::vector<std::string> advice;
    const std::vector<std::string> issues = verifyDcp(made.folder, [&](double f) {
        t.progress(0.85 + 0.15 * f);
        return !t.cancelled();
    }, &advice);
    t.result = dcpResultToJson(made);
    t.result["issues"] = stringsJson(issues);
    t.result["advice"] = stringsJson(advice);
    t.result["pictures"] = stretches;
    if (t.cancelled()) {
        // (the package is made: only its check was stopped)
        t.result["stopped"] = true;
    }
    return true;
}

bool runImf(TaskRun& t) {
    const Sequence* s = sequenceOf(t);
    if (!s) return noSequence(t);
    ImfSettings settings;
    if (!imfSettingsFromJson(t.input.value("settings").toObject(), settings, &t.error)) return false;
    const QString parent = t.input.value("parent").toString();
    const QString pictures = picturesFolder(t.folder, parent);
    std::string why;
    const int64_t frames = settings.application == 2 && settings.supplementOf.empty() ? imfCompositionFrames(*s, settings, &why) : 0;
    ImfResult made;
    QJsonObject before;
    const bool already = madeBefore(t, &before);
    if (already) made = imfResultFromJson(before);
    // (a generous guess: half the uncompressed picture)
    const bool apart = !already && !pictures.isEmpty() && settings.codestreams.empty() && frames > 0 &&
                       (QFileInfo::exists(pictures) || roomFor(parent, double(frames) * double(s->width) * double(s->height) * 2.25));
    QJsonArray stretches = already ? before.value("pictures").toArray() : QJsonArray();
    const double share = apart ? 0.75 : 0;
    if (apart) {
        const int64_t perPart = std::max<int64_t>(1, t.input.value("partFrames").toInt(int(std::max(24.0, 10 * s->fpsValue()))));
        if (!codePictures(t, pictures, frames, perPart, share, &stretches, [&](const std::string& folder, int64_t from, int64_t to, const auto& progress) {
                ImfSettings part = settings;
                part.codestreams = folder;
                part.codeFrom = from, part.codeTo = to;
                return exportImf(*t.project, *s, part, parent.toStdString(), nullptr, progress, &t.error);
            }))
            return false;
        settings.codestreams = pictures.toStdString();
    }
    if (!already) {
        if (!exportImf(*t.project, *s, settings, parent.toStdString(), &made, [&](double f) {
                t.progress(share + (0.85 - share) * f);
                return !t.cancelled();
            }, &t.error))
            return false;
        QJsonObject kept = imfResultToJson(made);
        kept["pictures"] = stretches;
        t.keepPart(QStringLiteral("package"), kept);
    }
    removePictures(pictures);
    std::vector<std::string> advice;
    const std::vector<std::string> issues = verifyImf(made.folder, [&](double f) {
        t.progress(0.85 + 0.15 * f);
        return !t.cancelled();
    }, &advice);
    t.result = imfResultToJson(made);
    t.result["issues"] = stringsJson(issues);
    t.result["advice"] = stringsJson(advice);
    t.result["pictures"] = stretches;
    if (t.cancelled()) t.result["stopped"] = true;
    return true;
}

// --- ADR: Align to Guide ---
//
// The take's sound matched to the guide's on a copy of the sequence; the alignment it made is the result, with the
// take and the guide as they were (to see, when it is applied, that neither has changed since).

bool runAdrAlign(TaskRun& t) {
    const Sequence* s = sequenceOf(t);
    if (!s) return noSequence(t);
    const Id clip = idOf(t.input.value("clip")), guide = idOf(t.input.value("guide"));
    const Clip* was = edit::clipById(*s, clip);
    const Clip* guideWas = edit::clipById(*s, guide);
    if (!was || !guideWas) {
        t.error = "No such clip";
        return false;
    }
    DialogueAlignOptions o;
    o.precision = AlignPrecision(std::clamp(t.input.value("precision").toInt(int(AlignPrecision::Normal)), 0, 2));
    o.from = FrameTime(t.input.value("from").toDouble(-1));
    o.to = FrameTime(t.input.value("to").toDouble(-1));
    t.progress(0);
    Project p = *t.project;
    Sequence* sq = p.findSequence(s->id);
    DialogueAlignReport report;
    const edit::Result r = edit::alignDialogue(p, *sq, clip, guide, o, &report, t.cancel);
    if (t.cancelled()) return false;
    if (!r.ok) {
        t.error = r.error;
        return false;
    }
    const Clip* take = edit::clipById(*sq, clip);
    const auto made = take ? std::find_if(take->effects.rbegin(), take->effects.rend(), [](const Effect& e) { return e.type == "dialogue_align"; })
                           : std::vector<Effect>::const_reverse_iterator();
    if (!take || made == take->effects.rend()) {
        t.error = "The alignment was not made";
        return false;
    }
    t.result = QJsonObject{{"alignment", QString::fromStdString(effectToJsonString(*made))},
                           {"duration", double(take->duration)},
                           {"take", QString::fromStdString(clipToJsonString(*was))},
                           {"guide", QString::fromStdString(clipToJsonString(*guideWas))},
                           {"anchors", report.anchors},
                           {"minRate", report.minRate},
                           {"maxRate", report.maxRate}};
    t.progress(1);
    return true;
}

QJsonObject applyAdrAlign(EditorState* state, const QJsonObject& input, const QJsonObject& result) {
    Clip was, guideWas;
    Effect alignment;
    if (!clipFromJsonString(result.value("take").toString().toStdString(), was) || !clipFromJsonString(result.value("guide").toString().toStdString(), guideWas) ||
        !effectFromJsonString(result.value("alignment").toString().toStdString(), alignment))
        return {{"changed", false}, {"failed", true}};
    const Id clip = idOf(input.value("clip")), guide = idOf(input.value("guide")), cueId = idOf(input.value("cue"));
    const FrameTime from = FrameTime(input.value("from").toDouble(-1)), to = FrameTime(input.value("to").toDouble(-1));
    const FrameTime length = FrameTime(result.value("duration").toDouble());
    // What the alignment was made from: the take, the guide and the cue's range as they were.
    auto moved = [](const Clip& a, const Clip& b) {
        return a.start != b.start || a.duration != b.duration || a.sourceIn != b.sourceIn || a.mediaId != b.mediaId || a.channels != b.channels ||
               a.speed != b.speed || a.reverse != b.reverse || a.timing != b.timing || a.linkGroup != b.linkGroup;
    };
    const bool ok = state->apply(AdrText::tr("Align to Guide"), [&](Project& p, Sequence& sq) {
        Clip* c = edit::clipById(sq, clip);
        const Clip* g = edit::clipById(sq, guide);
        const AdrCue* cue = findAdrCue(sq, cueId);
        if (!c || !g) return edit::Result::fail("No such clip");
        const auto loc = edit::locate(sq, clip);
        if (moved(*c, was) || moved(*g, guideWas) || !cue || cue->start != from || cue->end != to || g->effects != guideWas.effects ||
            (loc && trackAt(sq, loc->track) && trackAt(sq, loc->track)->locked))
            return edit::Result::fail("The take, the guide or the cue changed while it was being aligned: align it again");
        // The alignment made on the copy, in place of the clip's own for that take, at the end as alignDialogue
        // leaves it.
        std::erase_if(c->effects, [&](const Effect& e) { return e.type == "dialogue_align" && dialogueAlignmentApplies(e, *c); });
        Effect e = alignment;
        e.id = p.newId();  // (made on the copy)
        c->effects.push_back(e);
        // (longer to the next clip, as it is now)
        FrameTime duration = length;
        for (Track& t : sq.audioTracks)
            if (std::any_of(t.clips.begin(), t.clips.end(), [&](const Clip& x) { return &x == c; }))
                for (const Clip& x : t.clips)
                    if (&x != c && x.start >= c->start && x.start < c->start + duration) duration = std::max(c->duration, x.start - c->start);
        c->duration = duration;
        return edit::Result{};
    });
    if (!ok) return {{"changed", false}, {"failed", true}};
    return {{"changed", true},
            {"message", AdrText::tr("Aligned to the guide: %1 anchors, stretched %2–%3 %")
                            .arg(result.value("anchors").toInt())
                            .arg(std::lround(100 * result.value("minRate").toDouble()))
                            .arg(std::lround(100 * result.value("maxRate").toDouble()))}};
}

// --- Captions: translate a track, draft SDH captions, read subtitles that are pictures ---
//
// Each makes a caption track, kept in the result as the project file keeps one, and added to the sequence as one
// undo step when it ends ("track": its id, for the panel to show).

QJsonObject trackResult(const CaptionTrack& t) { return {{"track", QString::fromStdString(captionTrackToJsonString(t))}}; }

bool trackFrom(const QJsonObject& result, CaptionTrack& t) { return captionTrackFromJsonString(result.value("track").toString().toStdString(), t); }

// Adds `t` (a new id) to the input's sequence; `hideOthers`: it becomes the one shown. Returns its id, 0 when the
// sequence is gone.
Id addCaptionTrack(EditorState* state, const QJsonObject& input, const QString& label, CaptionTrack t, bool hideOthers, int* hidden = nullptr) {
    Id id = 0;
    const Id seq = idOf(input.value("sequence"));
    state->edit(label, [&](Project& p, Sequence& sq) {
        if (sq.id != seq) return false;
        t.id = id = p.newId();
        if (hideOthers)
            for (CaptionTrack& o : sq.captionTracks)
                if (o.visible) o.visible = false, hidden && ++*hidden;
        sq.captionTracks.push_back(std::move(t));
        return true;
    });
    return id;
}

bool failed(TaskRun& t, const std::string& error) {
    if (!t.cancelled()) t.error = error.empty() ? "Cancelled" : error;
    return false;
}

bool runTranslateCaptions(TaskRun& t) {
    const Sequence* s = sequenceOf(t);
    if (!s) return noSequence(t);
    const Id id = idOf(t.input.value("track"));
    const auto source = std::find_if(s->captionTracks.begin(), s->captionTracks.end(), [&](const CaptionTrack& c) { return c.id == id; });
    if (source == s->captionTracks.end()) {
        t.error = "The caption track is gone";
        return false;
    }
    const std::string from = source->language.empty() ? "en" : source->language, to = t.input.value("to").toString().toStdString();
    std::vector<std::string> texts;
    std::string err;
    t.progress(0);
    if (!translateTexts(captionTexts(*source), from, to, texts, t.progress, t.cancel, &err)) return failed(t, err);
    CaptionTrack made = translatedTrack(*source, texts, 0, to, translationLanguageName(to));
    made.visible = false;  // the original stays the one shown until this is chosen
    t.result = trackResult(made);
    return true;
}

QJsonObject applyTranslateCaptions(EditorState* state, const QJsonObject& input, const QJsonObject& result) {
    CaptionTrack t;
    if (!trackFrom(result, t)) return {{"changed", false}, {"failed", true}};
    const Id id = addCaptionTrack(state, input, CaptionText::tr("Translate Captions"), t, false);
    if (!id) return {{"changed", false}, {"failed", true}};
    return {{"changed", true}, {"track", QString::number(id)}, {"message", CaptionText::tr("Translated into %1").arg(QString::fromStdString(translationLanguageName(t.language)))}};
}

SdhDraftOptions sdhOptionsFrom(const QJsonObject& j) {
    SdhDraftOptions o;
    const QJsonObject sdh = j.value("sdh").toObject(), rules = j.value("rules").toObject();
    o.sdh.style = SdhStyle(sdh.value("style").toInt(int(o.sdh.style)));
    o.sdh.minSeconds = sdh.value("minSeconds").toDouble(o.sdh.minSeconds);
    o.sdh.maxSeconds = sdh.value("maxSeconds").toDouble(o.sdh.maxSeconds);
    o.sdh.gapFrames = sdh.value("gapFrames").toInt(o.sdh.gapFrames);
    o.sdh.mergeSeconds = sdh.value("mergeSeconds").toDouble(o.sdh.mergeSeconds);
    o.sdh.speakerLabels = sdh.value("speakerLabels").toBool(o.sdh.speakerLabels);
    o.sdh.lyrics = sdh.value("lyrics").toBool(o.sdh.lyrics);
    o.sdh.lineChars = sdh.value("lineChars").toInt(o.sdh.lineChars);
    o.sdh.maxLines = sdh.value("maxLines").toInt(o.sdh.maxLines);
    o.rules.lineChars = rules.value("lineChars").toInt(o.rules.lineChars);
    o.rules.maxLines = rules.value("maxLines").toInt(o.rules.maxLines);
    o.rules.maxSeconds = rules.value("maxSeconds").toDouble(o.rules.maxSeconds);
    o.rules.minSeconds = rules.value("minSeconds").toDouble(o.rules.minSeconds);
    o.threshold = j.value("threshold").toDouble(o.threshold);
    o.loudDb = j.value("loudDb").toDouble(o.loudDb);
    o.minSoundSeconds = j.value("minSoundSeconds").toDouble(o.minSoundSeconds);
    o.sounds = j.value("sounds").toBool(o.sounds);
    o.faces = j.value("faces").toBool(o.faces);
    o.readText = j.value("readText").toBool(o.readText);
    return o;
}

bool runSdhDraft(TaskRun& t) {
    const Sequence* s = sequenceOf(t);
    if (!s) return noSequence(t);
    SdhDraft draft;
    std::string err;
    t.progress(0);
    if (!draftSdhCaptions(*t.project, *s, sdhOptionsFrom(t.input.value("options").toObject()), draft, t.progress, t.cancel, &err))
        return failed(t, err);
    CaptionTrack made;
    made.name = CaptionText::tr("SDH").toStdString();
    if (!draft.language.empty()) made.language = draft.language;
    made.style = draft.look;  // the captions were placed for it
    made.captions = draft.captions;
    t.result = trackResult(made);
    QJsonArray notes;
    for (const std::string& n : draft.notes) notes.append(QString::fromStdString(n));
    t.result["sounds"] = draft.summary.sounds, t.result["lyrics"] = draft.summary.lyrics;
    t.result["speakers"] = draft.summary.speakers, t.result["raised"] = draft.summary.raised;
    t.result["notes"] = notes;
    return true;
}

QJsonObject applySdhDraft(EditorState* state, const QJsonObject& input, const QJsonObject& result) {
    CaptionTrack t;
    if (!trackFrom(result, t)) return {{"changed", false}, {"failed", true}};
    // The new track is the one shown (the first visible), so the other tracks are hidden.
    int hidden = 0;
    const int n = int(t.captions.size());
    const Id id = addCaptionTrack(state, input, CaptionText::tr("Generate SDH Captions"), std::move(t), true, &hidden);
    if (!id) return {{"changed", false}, {"failed", true}};
    QString text = CaptionText::tr("%n SDH caption(s): ", "", n) + CaptionText::tr("%1 sound(s), %2 sung, %3 with the speaker shown, %4 moved off text")
                                                                     .arg(result.value("sounds").toInt())
                                                                     .arg(result.value("lyrics").toInt())
                                                                     .arg(result.value("speakers").toInt())
                                                                     .arg(result.value("raised").toInt());
    if (hidden) text += QStringLiteral(". ") + CaptionText::tr("%n other caption track(s) hidden", "", hidden);
    for (const QJsonValue& note : result.value("notes").toArray()) text += QStringLiteral(". ") + note.toString();
    return {{"changed", true}, {"track", QString::number(id)}, {"captions", n}, {"message", text}};
}

bool runReadSubtitles(TaskRun& t) {
    const Sequence* s = sequenceOf(t);
    if (!s) return noSequence(t);
    ImageSubtitleImport read;
    std::string err;
    t.progress(0);
    if (!readImageSubtitles(t.input.value("file").toString().toStdString(), s->fps, read, -1, {}, t.progress, t.cancel, &err)) return failed(t, err);
    t.result = trackResult(read.track);
    QJsonArray review;
    for (const OcrReview& r : read.review) {
        QJsonArray suggestions;
        for (const std::string& w : r.suggestions) suggestions.append(QString::fromStdString(w));
        review.append(QJsonObject{{"caption", double(r.caption)}, {"word", QString::fromStdString(r.word)}, {"suggestions", suggestions},
                                  {"confidence", double(r.confidence)}});
    }
    t.result["review"] = review;
    return true;
}

QJsonObject applyReadSubtitles(EditorState* state, const QJsonObject& input, const QJsonObject& result) {
    CaptionTrack t;
    if (!trackFrom(result, t)) return {{"changed", false}, {"failed", true}};
    const int n = int(t.captions.size()), unsure = int(result.value("review").toArray().size());
    const Id id = addCaptionTrack(state, input, CaptionText::tr("Import Captions"), std::move(t), false);
    if (!id) return {{"changed", false}, {"failed", true}};
    // The words read with little confidence are underlined by the spell check: say how many.
    return {{"changed", true},
            {"track", QString::number(id)},
            {"message", unsure ? CaptionText::tr("%n subtitle(s) read from their pictures; %1 word(s) to check (underlined)", "", n).arg(unsure)
                               : CaptionText::tr("%n subtitle(s) read from their pictures", "", n)}};
}

}  // namespace

QJsonObject dcpResultToJson(const DcpResult& r) {
    return QJsonObject{{"folder", QString::fromStdString(r.folder)}, {"name", QString::fromStdString(r.name)}, {"cpl", QString::fromStdString(r.cpl)},
                       {"frames", double(r.frames)},       {"fps", r.fps},                 {"width", r.width},
                       {"height", r.height},               {"channels", r.channels},       {"immersive", r.immersive},
                       {"iabBedChannels", r.iabBedChannels}, {"iabObjects", r.iabObjects}, {"cinemaProfile", r.cinemaProfile},
                       {"stereo", r.stereo},               {"encrypted", r.encrypted},     {"signedPackage", r.signedPackage},
                       {"subtitleCount", r.subtitleCount}, {"cplId", QString::fromStdString(r.cplId)},
                       {"dkdm", QString::fromStdString(r.dkdm)}, {"leqm", r.leqm},         {"leqmLimit", r.leqmLimit}};
}

DcpResult dcpResultFromJson(const QJsonObject& o) {
    DcpResult r;
    r.folder = o.value("folder").toString().toStdString();
    r.name = o.value("name").toString().toStdString();
    r.cpl = o.value("cpl").toString().toStdString();
    r.frames = int64_t(o.value("frames").toDouble());
    r.fps = o.value("fps").toInt(24);
    r.width = o.value("width").toInt();
    r.height = o.value("height").toInt();
    r.channels = o.value("channels").toInt(6);
    r.immersive = o.value("immersive").toBool();
    r.iabBedChannels = o.value("iabBedChannels").toInt();
    r.iabObjects = o.value("iabObjects").toInt();
    r.cinemaProfile = o.value("cinemaProfile").toBool(true);
    r.stereo = o.value("stereo").toBool();
    r.encrypted = o.value("encrypted").toBool();
    r.signedPackage = o.value("signedPackage").toBool();
    r.subtitleCount = o.value("subtitleCount").toInt();
    r.cplId = o.value("cplId").toString().toStdString();
    r.dkdm = o.value("dkdm").toString().toStdString();
    r.leqm = o.value("leqm").toDouble();
    r.leqmLimit = o.value("leqmLimit").toDouble();
    return r;
}

QJsonObject imfResultToJson(const ImfResult& r) {
    return QJsonObject{{"folder", QString::fromStdString(r.folder)}, {"cpl", QString::fromStdString(r.cpl)}, {"frames", double(r.frames)},
                       {"rateNum", double(r.rateNum)},     {"rateDen", double(r.rateDen)}, {"width", r.width},
                       {"height", r.height},               {"bits", r.bits},               {"channels", r.channels},
                       {"rsiz", int(r.rsiz)},              {"colour", QString::fromStdString(r.colour)},
                       {"sounds", r.sounds},               {"texts", r.texts},             {"edition", r.edition},
                       {"events", r.events},               {"newEvents", r.newEvents},     {"supplemental", r.supplemental},
                       {"carriedEventTracks", r.carriedEventTracks}, {"leftEventTracks", r.leftEventTracks},
                       {"codedFrames", double(r.codedFrames)}, {"newSamples", double(r.newSamples)},
                       {"trackFiles", stringsJson(r.trackFiles)}, {"application", r.application},
                       {"prores", QString::fromStdString(r.prores)}, {"fields", QString::fromStdString(r.fields)},
                       {"ycbcr444", r.ycbcr444},           {"fullRange", r.fullRange},
                       {"outputTransform", QString::fromStdString(r.outputTransform)}, {"targetFrames", r.targetFrames},
                       {"amf", QString::fromStdString(r.amf)}, {"immersive", r.immersive}, {"iabBedChannels", r.iabBedChannels},
                       {"iabObjects", r.iabObjects},       {"sadm", r.sadm},               {"adm", r.adm},
                       {"admBedChannels", r.admBedChannels}, {"admObjects", r.admObjects}};
}

ImfResult imfResultFromJson(const QJsonObject& o) {
    ImfResult r;
    r.folder = o.value("folder").toString().toStdString();
    r.cpl = o.value("cpl").toString().toStdString();
    r.frames = int64_t(o.value("frames").toDouble());
    r.rateNum = uint32_t(o.value("rateNum").toDouble(24));
    r.rateDen = uint32_t(o.value("rateDen").toDouble(1));
    r.width = o.value("width").toInt();
    r.height = o.value("height").toInt();
    r.bits = o.value("bits").toInt();
    r.channels = o.value("channels").toInt();
    r.rsiz = uint16_t(o.value("rsiz").toInt());
    r.colour = o.value("colour").toString().toStdString();
    r.sounds = o.value("sounds").toInt();
    r.texts = o.value("texts").toInt();
    r.edition = o.value("edition").toInt();
    r.events = o.value("events").toInt();
    r.newEvents = o.value("newEvents").toInt();
    r.carriedEventTracks = o.value("carriedEventTracks").toInt();
    r.leftEventTracks = o.value("leftEventTracks").toInt();
    r.supplemental = o.value("supplemental").toBool();
    r.codedFrames = int64_t(o.value("codedFrames").toDouble());
    r.newSamples = int64_t(o.value("newSamples").toDouble());
    r.trackFiles = stringsFrom(o.value("trackFiles"));
    r.application = o.value("application").toInt(2);
    r.prores = o.value("prores").toString().toStdString();
    r.fields = o.value("fields").toString().toStdString();
    r.ycbcr444 = o.value("ycbcr444").toBool();
    r.fullRange = o.value("fullRange").toBool();
    r.outputTransform = o.value("outputTransform").toString().toStdString();
    r.targetFrames = o.value("targetFrames").toInt();
    r.amf = o.value("amf").toString().toStdString();
    r.immersive = o.value("immersive").toBool();
    r.iabBedChannels = o.value("iabBedChannels").toInt();
    r.iabObjects = o.value("iabObjects").toInt();
    r.sadm = o.value("sadm").toBool();
    r.adm = o.value("adm").toBool();
    r.admBedChannels = o.value("admBedChannels").toInt();
    r.admObjects = o.value("admObjects").toInt();
    return r;
}

QJsonArray mixPlanToJson(const std::vector<ClipMix>& plan) {
    QJsonArray a;
    for (const ClipMix& m : plan)
        a.append(QJsonObject{{"clip", QString::number(m.clip)},
                             {"guess", QJsonObject{{"role", int(m.guess.role)}, {"speech", m.guess.speech},     {"music", m.guess.music},
                                                   {"loudness", m.guess.loudness}, {"pauses", m.guess.pauses}, {"syllabic", m.guess.syllabic},
                                                   {"beat", m.guess.beat}}},
                             {"role", int(m.role)},
                             {"gain", m.gainDb},
                             {"ride", pairsJson(m.ride)},
                             {"shortTerm", pairsJson(m.shortTerm)}});
    return a;
}

std::vector<ClipMix> mixPlanFromJson(const QJsonValue& v) {
    std::vector<ClipMix> out;
    for (const QJsonValue& x : v.toArray()) {
        const QJsonObject o = x.toObject(), g = o.value("guess").toObject();
        ClipMix m;
        m.clip = idOf(o.value("clip"));
        m.guess.role = AudioRole(g.value("role").toInt());
        m.guess.speech = g.value("speech").toDouble();
        m.guess.music = g.value("music").toDouble();
        m.guess.loudness = g.value("loudness").toDouble(-70);
        m.guess.pauses = g.value("pauses").toDouble();
        m.guess.syllabic = g.value("syllabic").toDouble();
        m.guess.beat = g.value("beat").toDouble();
        m.role = AudioRole(o.value("role").toInt());
        m.gainDb = o.value("gain").toDouble();
        m.ride = pairsFrom(o.value("ride"));
        m.shortTerm = pairsFrom(o.value("shortTerm"));
        out.push_back(std::move(m));
    }
    return out;
}

QJsonArray conformShotsToJson(const std::vector<ConformShot>& shots) {
    QJsonArray a;
    for (const ConformShot& s : shots) {
        QJsonArray keys;
        for (const ConformKey& k : s.keys) keys.append(QJsonArray{double(k.at), k.posX, k.posY, k.scale, k.rotation});
        a.append(QJsonObject{{"clip", QString::number(s.clip)}, {"start", double(s.start)},        {"length", double(s.length)},
                             {"verdict", int(s.verdict)},       {"score", s.score},                {"slip", double(s.slip)},
                             {"media", QString::number(s.media)}, {"sourceIn", s.sourceIn},        {"keys", keys},
                             {"details", QString::fromStdString(s.details)}});
    }
    return a;
}

std::vector<ConformShot> conformShotsFromJson(const QJsonValue& v) {
    std::vector<ConformShot> out;
    for (const QJsonValue& x : v.toArray()) {
        const QJsonObject o = x.toObject();
        ConformShot s;
        s.clip = idOf(o.value("clip"));
        s.start = FrameTime(o.value("start").toDouble());
        s.length = FrameTime(o.value("length").toDouble());
        s.verdict = ConformVerdict(o.value("verdict").toInt());
        s.score = o.value("score").toDouble();
        s.slip = FrameTime(o.value("slip").toDouble());
        s.media = idOf(o.value("media"));
        s.sourceIn = o.value("sourceIn").toDouble();
        for (const QJsonValue& kv : o.value("keys").toArray()) {
            const QJsonArray k = kv.toArray();
            if (k.size() != 5) continue;
            s.keys.push_back({FrameTime(k.at(0).toDouble()), k.at(1).toDouble(), k.at(2).toDouble(), k.at(3).toDouble(), k.at(4).toDouble()});
        }
        s.details = o.value("details").toString().toStdString();
        out.push_back(std::move(s));
    }
    return out;
}

void registerDialogTaskKinds() {
    static std::once_flag once;
    std::call_once(once, [] {
        DialogJobs::registerKind(QStringLiteral("auto-duck"), {runDuck, applyDuck, false, nullptr});
        DialogJobs::registerKind(QStringLiteral("scene-detect"), {runSceneCuts, applySceneCuts, false, nullptr});
        DialogJobs::registerKind(QStringLiteral("auto-mix"), {runMix, applyMixPlan, true, nullptr});
        DialogJobs::registerKind(QStringLiteral("conform-check"), {runConform, nullptr, false, nullptr});
        DialogJobs::registerKind(QStringLiteral("lens-board"), {runLensBoard, nullptr, false, nullptr});
        DialogJobs::registerKind(QStringLiteral("vfx-pull"), {runVfxPull, applyVfxPull, false, nullptr});
        registerInspectorTaskKinds();
        DialogJobs::registerKind(QStringLiteral("adr-align"), {runAdrAlign, applyAdrAlign, false, nullptr});
        DialogJobs::registerKind(QStringLiteral("captions-translate"), {runTranslateCaptions, applyTranslateCaptions, false, nullptr});
        DialogJobs::registerKind(QStringLiteral("captions-sdh"), {runSdhDraft, applySdhDraft, false, nullptr});
        DialogJobs::registerKind(QStringLiteral("captions-read"), {runReadSubtitles, applyReadSubtitles, false, nullptr});
        DialogJobs::registerKind(QStringLiteral("dcp"), {runDcp, nullptr, false, discardPictures});
        DialogJobs::registerKind(QStringLiteral("imf"), {runImf, nullptr, false, discardPictures});
    });
}

}  // namespace montage
