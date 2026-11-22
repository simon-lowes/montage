// Montage — the kinds of task the app's dialogs run as background jobs (DialogJobs.h): for each, its work off the
// interface thread, from the project as it was and the task's own input, and how its result is applied.
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <mutex>

#include "DialogJobs.h"
#include "EditorState.h"
#include "core/EditOps.h"
#include "media/Analysis.h"
#include "media/AutoDuck.h"

namespace montage {

namespace {

// (each kind's texts under the name of the dialog they were written for, as lupdate files them)
struct DuckText {
    Q_DECLARE_TR_FUNCTIONS(montage::AutoDuckDialog)
};
struct WindowText {
    Q_DECLARE_TR_FUNCTIONS(montage::MainWindow)
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

}  // namespace

void registerDialogTaskKinds() {
    static std::once_flag once;
    std::call_once(once, [] {
        DialogJobs::registerKind(QStringLiteral("auto-duck"), {runDuck, applyDuck});
        DialogJobs::registerKind(QStringLiteral("scene-detect"), {runSceneCuts, applySceneCuts});
    });
}

}  // namespace montage
