// Montage — reading timelines from other editors: OpenTimelineIO and CMX
// 3600 EDL (FCP XML lives in FcpXml.cpp). Shared: a builder that adds a
// sequence, its tracks and clips, and the media they reference.
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

#include "EditOps.h"
#include "Effects.h"
#include "History.h"
#include "ImportBuilder.h"
#include "Interchange.h"

namespace montage {

// ---------------------------------------------------------------------------
// TimelineBuilder

TimelineBuilder::TimelineBuilder(Project& p, const std::string& name, Rational fps, const MediaProber& probe)
    : p_(p), probe_(probe) {
    seq_ = makeSequence(p, name.empty() ? "Imported" : name, 1920, 1080, fps.valid() ? fps : Rational{30, 1}, 0, 0);
    for (const MediaItem& m : p.media)
        if (!m.path.empty()) byPath_[m.path] = m.id;
}

Track& TimelineBuilder::track(TrackKind kind, int index) {
    auto& list = kind == TrackKind::Video ? seq_.videoTracks : seq_.audioTracks;
    while (int(list.size()) <= index)
        list.push_back(makeTrack(p_, kind, (kind == TrackKind::Video ? "V" : "A") + std::to_string(list.size() + 1)));
    return list[size_t(index)];
}

Id TimelineBuilder::media(const std::string& path, const std::string& name, bool video, bool audio, double seconds) {
    const std::string key = path.empty() ? "offline:" + name : path;
    if (auto it = byPath_.find(key); it != byPath_.end()) {
        if (MediaItem* m = p_.findMedia(it->second); m && m->path.empty()) {
            m->hasVideo |= video;  // an offline item learns what it was used for
            m->hasAudio |= audio;
            m->duration = std::max(m->duration, seconds);
        }
        return it->second;
    }
    MediaItem m;
    m.id = p_.newId();
    m.name = name.empty() ? QFileInfo(QString::fromStdString(path)).fileName().toStdString() : name;
    bool found = false;
    if (!path.empty() && QFileInfo::exists(QString::fromStdString(path)) && probe_) {
        MediaItem probed = m;
        if (probe_(path, probed)) {
            probed.id = m.id;
            if (!name.empty()) probed.name = name;
            m = probed;
            found = true;
        }
    }
    if (!found) {
        // Offline: what the timeline tells us, so the clip keeps its length and place.
        m.path = path;
        m.kind = video ? MediaKind::Video : MediaKind::Audio;
        m.hasVideo = video;
        m.hasAudio = audio;
        m.duration = seconds;
        m.width = seq_.width;
        m.height = seq_.height;
        m.fps = seq_.fps;
        res_.offline.push_back(path.empty() ? m.name : path);
    }
    p_.media.push_back(m);
    byPath_[key] = m.id;
    return m.id;
}

Clip* TimelineBuilder::addClip(TrackKind kind, int trackIndex, Id mediaId, FrameTime start, FrameTime duration,
                               double sourceIn, const std::string& name) {
    const MediaItem* m = p_.findMedia(mediaId);
    if (!m || duration <= 0) return nullptr;
    Clip c = makeClip(p_, *m, kind, seq_);
    c.start = start;
    c.duration = duration;
    c.sourceIn = std::max(0.0, sourceIn);
    if (!name.empty()) c.name = name;
    Track& t = track(kind, trackIndex);
    t.clips.push_back(c);
    ++res_.clips;
    return &t.clips.back();
}

Clip* TimelineBuilder::addGenerator(int trackIndex, const std::string& type, FrameTime start, FrameTime duration,
                                    const std::string& name) {
    if (duration <= 0 || !findEffectInfo(type)) return nullptr;
    Clip c = makeGeneratorClip(p_, type, duration);
    c.start = start;
    if (!name.empty()) c.name = name;
    Track& t = track(TrackKind::Video, trackIndex);
    t.clips.push_back(c);
    ++res_.clips;
    return &t.clips.back();
}

void TimelineBuilder::addTransition(TrackKind kind, int trackIndex, Id clipA, Id clipB, FrameTime duration,
                                    const std::string& type) {
    if (duration <= 0) return;
    Transition tr;
    tr.id = p_.newId();
    tr.type = !type.empty() ? type : kind == TrackKind::Video ? "cross_dissolve" : "crossfade";
    tr.clipA = clipA;
    tr.clipB = clipB;
    tr.duration = duration;
    track(kind, trackIndex).transitions.push_back(tr);
}

void TimelineBuilder::linkMatching() {
    // Picture and sound of the same source over the same time belong together.
    for (Track& vt : seq_.videoTracks)
        for (Clip& v : vt.clips) {
            if (v.isGenerator() || v.linkGroup) continue;
            for (Track& at : seq_.audioTracks)
                for (Clip& a : at.clips)
                    if (!a.linkGroup && a.mediaId == v.mediaId && a.start == v.start && a.duration == v.duration &&
                        std::fabs(a.sourceIn - v.sourceIn) < 0.5) {
                        if (!v.linkGroup) v.linkGroup = p_.newId();
                        a.linkGroup = v.linkGroup;
                    }
        }
}

ImportResult TimelineBuilder::finish() {
    if (seq_.videoTracks.empty()) track(TrackKind::Video, 0);
    if (seq_.audioTracks.empty()) track(TrackKind::Audio, 0);
    for (auto* list : {&seq_.videoTracks, &seq_.audioTracks})
        for (Track& t : *list) {
            edit::normalize(t);
            // Overlaps (a tool's stacked takes) would break the one-clip-at-a-time rule: keep the later clip.
            for (size_t i = 1; i < t.clips.size(); ++i)
                if (t.clips[i - 1].end() > t.clips[i].start) {
                    t.clips[i - 1].duration = std::max<FrameTime>(1, t.clips[i].start - t.clips[i - 1].start);
                    res_.warnings.push_back("Overlapping clips on " + t.name + " were trimmed to fit");
                }
        }
    linkMatching();
    res_.sequence = seq_.id;
    p_.sequences.push_back(std::move(seq_));
    p_.activeSequence = res_.sequence;
    res_.ok = true;
    return res_;
}

// ---------------------------------------------------------------------------
// OpenTimelineIO

namespace {

double rtValue(const QJsonValue& v, double rate) {
    const QJsonObject o = v.toObject();
    const double r = o.value("rate").toDouble(rate);
    const double value = o.value("value").toDouble();
    return r > 0 ? value * rate / r : value;  // in `rate` units
}

QString pathFromUrl(const QString& url) {
    if (url.isEmpty()) return {};
    const QUrl u(url);
    if (u.isLocalFile()) return QDir::toNativeSeparators(u.toLocalFile()).replace('\\', '/');
    if (u.scheme().isEmpty()) return url;
    return u.toString();
}

}  // namespace

ImportResult importOtio(Project& p, const std::string& json, const MediaProber& probe) {
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json), &perr);
    ImportResult bad;
    if (!doc.isObject()) {
        bad.error = "Not an OpenTimelineIO file: " + perr.errorString().toStdString();
        return bad;
    }
    const QJsonObject root = doc.object();
    if (!root.value("OTIO_SCHEMA").toString().startsWith("Timeline.")) {
        bad.error = "The file is not an OpenTimelineIO timeline";
        return bad;
    }
    const QJsonObject stack = root.value("tracks").toObject();
    const QJsonArray tracks = stack.value("children").toArray();
    // The rate: Montage's metadata, else the first clip's.
    double rate = root.value("global_start_time").toObject().value("rate").toDouble(0);
    for (const auto& tv : tracks) {
        if (rate > 0) break;
        for (const auto& cv : tv.toObject().value("children").toArray())
            if (double r = cv.toObject().value("source_range").toObject().value("duration").toObject().value("rate").toDouble(0); r > 0) {
                rate = r;
                break;
            }
    }
    if (rate <= 0) rate = 30;
    Rational fps{int(std::lround(rate)), 1};
    if (std::fabs(rate - 29.97) < 0.01) fps = {30000, 1001};
    else if (std::fabs(rate - 23.976) < 0.01) fps = {24000, 1001};
    else if (std::fabs(rate - 59.94) < 0.01) fps = {60000, 1001};
    TimelineBuilder b(p, root.value("name").toString().toStdString(), fps, probe);
    const QJsonObject meta = root.value("metadata").toObject().value("montage").toObject();
    if (meta.contains("width")) {
        b.sequence().width = std::max(16, meta.value("width").toInt(1920));
        b.sequence().height = std::max(16, meta.value("height").toInt(1080));
        b.sequence().sampleRate = meta.value("sample_rate").toInt(48000);
    }
    int vIndex = 0, aIndex = 0;
    for (const auto& tv : tracks) {
        const QJsonObject t = tv.toObject();
        const bool video = t.value("kind").toString() != "Audio";
        const int index = video ? vIndex++ : aIndex++;
        const TrackKind kind = video ? TrackKind::Video : TrackKind::Audio;
        Track& track = b.track(kind, index);
        if (!t.value("name").toString().isEmpty()) track.name = t.value("name").toString().toStdString();
        track.muted = !t.value("enabled").toBool(true);
        double cursor = 0;
        Id prev = 0;
        struct Pending {
            double in = 0, out = 0;
            std::string type;
        };
        std::optional<Pending> pendingTr;
        for (const auto& cv : t.value("children").toArray()) {
            const QJsonObject item = cv.toObject();
            const QString schema = item.value("OTIO_SCHEMA").toString();
            const QJsonObject sr = item.value("source_range").toObject();
            const double dur = rtValue(sr.value("duration"), rate), srcStart = rtValue(sr.value("start_time"), rate);
            if (schema.startsWith("Gap.")) {
                cursor += dur;
                prev = 0;
                continue;
            }
            if (schema.startsWith("Transition.")) {
                pendingTr = Pending{rtValue(item.value("in_offset"), rate), rtValue(item.value("out_offset"), rate),
                                    item.value("transition_type").toString() == "SMPTE_Dissolve" ? std::string() : std::string()};
                continue;
            }
            if (schema.startsWith("Stack.") || schema.startsWith("Track.")) {
                b.warn("Nested stacks are not imported (" + item.value("name").toString().toStdString() + ")");
                cursor += dur;
                prev = 0;
                continue;
            }
            if (!schema.startsWith("Clip.")) {
                cursor += dur;
                continue;
            }
            // Media reference (OTIO 0.15+: media_references[active key]; older: media_reference).
            QJsonObject ref = item.value("media_reference").toObject();
            if (item.contains("media_references")) {
                const QString key = item.value("active_media_reference_key").toString("DEFAULT_MEDIA");
                ref = item.value("media_references").toObject().value(key).toObject();
            }
            const QString refSchema = ref.value("OTIO_SCHEMA").toString();
            const std::string name = item.value("name").toString().toStdString();
            // Speed: a LinearTimeWarp (negative = reversed).
            double scalar = 1;
            for (const auto& ev : item.value("effects").toArray())
                if (ev.toObject().value("OTIO_SCHEMA").toString().startsWith("LinearTimeWarp"))
                    scalar = ev.toObject().value("time_scalar").toDouble(1);
            const FrameTime start = FrameTime(std::llround(cursor)), len = FrameTime(std::llround(dur));
            Clip* clip = nullptr;
            if (refSchema.startsWith("GeneratorReference.") && video) {
                const QString kindName = ref.value("generator_kind").toString();
                const std::string type = kindName == "SolidColor" ? "color" : kindName == "Title" ? "title" : kindName.toLower().toStdString();
                clip = b.addGenerator(index, type, start, len, name);
                if (clip) {
                    const QJsonObject params = ref.value("parameters").toObject();
                    for (auto it = params.begin(); it != params.end(); ++it) {
                        if (it.value().isDouble()) clip->generator.params[it.key().toStdString()] = Param(it.value().toDouble());
                        else if (it.value().isString()) clip->generator.strings[it.key().toStdString()] = it.value().toString().toStdString();
                    }
                } else {
                    b.warn("Generator \"" + kindName.toStdString() + "\" is not available");
                }
            } else {
                const QString path = pathFromUrl(ref.value("target_url").toString());
                const double avail = rtValue(ref.value("available_range").toObject().value("duration"), rate);
                const Id mid = b.media(path.toStdString(), ref.value("name").toString().toStdString(), video, !video,
                                       avail > 0 ? avail / rate : (srcStart + dur * std::fabs(scalar)) / rate);
                // OTIO's source start is where the clip begins in the media; reversed clips start at the far end.
                const double sourceIn = scalar < 0 ? srcStart - dur * std::fabs(scalar) : srcStart;
                clip = b.addClip(kind, index, mid, start, len, sourceIn, name);
                if (clip) {
                    clip->speed = std::max(0.01, std::fabs(scalar));
                    clip->reverse = scalar < 0;
                    // Our sequence rate may differ from the media's rate in source_range units: both are `rate` here.
                }
            }
            if (clip) {
                clip->enabled = item.value("enabled").toBool(true);
                const QJsonObject mm = item.value("metadata").toObject().value("montage").toObject();
                if (mm.contains("blend_mode")) clip->blendMode = mm.value("blend_mode").toString().toStdString();
                if (pendingTr && prev) {
                    b.addTransition(kind, index, prev, clip->id, FrameTime(std::llround(pendingTr->in + pendingTr->out)), {});
                }
                prev = clip->id;
            } else {
                prev = 0;
            }
            pendingTr.reset();
            cursor += dur;
        }
    }
    for (const auto& mv : stack.value("markers").toArray()) {
        const QJsonObject m = mv.toObject();
        const QJsonObject r = m.value("marked_range").toObject();
        Marker mk;
        mk.t = FrameTime(std::llround(rtValue(r.value("start_time"), rate)));
        mk.duration = FrameTime(std::llround(rtValue(r.value("duration"), rate)));
        mk.name = m.value("name").toString().toStdString();
        mk.comment = m.value("comment").toString().toStdString();
        b.sequence().markers.push_back(mk);
    }
    return b.finish();
}

// ---------------------------------------------------------------------------
// CMX 3600 EDL

ImportResult importEdl(Project& p, const std::string& text, Rational fps, const MediaProber& probe, const std::string& mediaDir) {
    const QStringList lines = QString::fromStdString(text).split(QRegularExpression("\\r?\\n"));
    std::string title = "EDL";
    for (const QString& l : lines)
        if (l.startsWith("TITLE:")) title = l.mid(6).trimmed().toStdString();
    TimelineBuilder b(p, title, fps, probe);
    // 001  AX       V     C        01:00:00:00 01:00:05:00 00:00:00:00 00:00:05:00
    static const QRegularExpression eventRe(
        "^(\\d{1,6})\\s+(\\S+)\\s+(\\S+)\\s+(C|D|W\\d{3}|K\\s*B?|KO)\\s*(\\d{1,4})?\\s+"
        "(\\d\\d[:;]\\d\\d[:;]\\d\\d[:;]\\d\\d)\\s+(\\d\\d[:;]\\d\\d[:;]\\d\\d[:;]\\d\\d)\\s+"
        "(\\d\\d[:;]\\d\\d[:;]\\d\\d[:;]\\d\\d)\\s+(\\d\\d[:;]\\d\\d[:;]\\d\\d[:;]\\d\\d)");
    static const QRegularExpression m2Re("^M2\\s+(\\S+)\\s+(-?[\\d.]+)\\s+(\\S+)");
    struct Event {
        QString number, reel, channel, type;
        FrameTime transition = 0;
        FrameTime srcIn = 0, srcOut = 0, recIn = 0, recOut = 0;
        std::string name, file;
        double speed = 1;
    };
    std::vector<Event> events;
    auto tc = [&](const QString& s) {
        FrameTime f = 0;
        parseTimecode(s.toStdString(), fps, f);
        return f;
    };
    for (const QString& raw : lines) {
        const QString l = raw.trimmed();
        if (auto m = eventRe.match(l); m.hasMatch()) {
            Event e;
            e.number = m.captured(1);
            e.reel = m.captured(2);
            e.channel = m.captured(3).toUpper();
            e.type = m.captured(4).left(1);
            e.transition = m.captured(5).toLongLong();
            e.srcIn = tc(m.captured(6));
            e.srcOut = tc(m.captured(7));
            e.recIn = tc(m.captured(8));
            e.recOut = tc(m.captured(9));
            events.push_back(e);
            continue;
        }
        if (events.empty()) continue;
        if (l.startsWith("* FROM CLIP NAME:")) events.back().name = l.mid(17).trimmed().toStdString();
        else if (l.startsWith("* TO CLIP NAME:")) events.back().name = l.mid(15).trimmed().toStdString();
        else if (l.startsWith("* SOURCE FILE:")) events.back().file = l.mid(14).trimmed().toStdString();
        else if (auto m = m2Re.match(l); m.hasMatch() && fps.toDouble() > 0) events.back().speed = m.captured(2).toDouble() / fps.toDouble();
    }
    if (events.empty()) {
        ImportResult bad;
        bad.error = "No EDL events found";
        return bad;
    }
    // Where a named clip lives: the EDL's own comment, else the media folder, else the project.
    auto locate = [&](const Event& e) -> std::string {
        if (!e.file.empty()) return e.file;
        if (!mediaDir.empty() && !e.name.empty()) {
            QDir dir(QString::fromStdString(mediaDir));
            const QString exact = dir.filePath(QString::fromStdString(e.name));
            if (QFileInfo::exists(exact)) return exact.toStdString();
            for (const QFileInfo& fi : dir.entryInfoList(QDir::Files))
                if (fi.completeBaseName() == QFileInfo(QString::fromStdString(e.name)).completeBaseName()) return fi.absoluteFilePath().toStdString();
        }
        for (const MediaItem& m : p.media)
            if (!e.name.empty() && m.name == e.name) return m.path;
        return {};
    };
    // Per channel: the last clip placed, for dissolves.
    std::map<std::string, Id> lastClip;
    for (size_t i = 0; i < events.size(); ++i) {
        const Event& e = events[i];
        if (e.recOut <= e.recIn && e.type == "C") continue;  // the zero-length cut before a dissolve
        std::vector<std::pair<TrackKind, int>> targets;
        const QString ch = e.channel;
        if (ch.contains('V') || ch == "B") targets.push_back({TrackKind::Video, 0});
        if (ch == "A" || ch == "A1" || ch == "AA" || ch == "B" || ch.startsWith("AA/") || ch == "A/V") targets.push_back({TrackKind::Audio, 0});
        if (ch == "A2" || ch == "AA" || ch.startsWith("AA/")) targets.push_back({TrackKind::Audio, 1});
        if (ch == "A3") targets.push_back({TrackKind::Audio, 2});
        if (ch == "A4") targets.push_back({TrackKind::Audio, 3});
        if (targets.empty()) {
            b.warn("Channel " + ch.toStdString() + " of event " + e.number.toStdString() + " is not supported");
            continue;
        }
        const bool black = e.reel == "BL" || e.reel == "BLACK";
        FrameTime recIn = e.recIn, len = e.recOut - e.recIn;
        double srcIn = double(e.srcIn);
        FrameTime dissolve = 0;
        if (e.type == "D" && e.transition > 0) {
            // Centred on the edit: this clip starts half way into the dissolve.
            dissolve = e.transition;
            recIn += dissolve / 2;
            len -= dissolve / 2;
            srcIn += double(dissolve / 2) * e.speed;
        }
        for (auto [kind, index] : targets) {
            const std::string chKey = std::to_string(int(kind)) + ":" + std::to_string(index);
            Clip* c = nullptr;
            if (black) {
                if (kind == TrackKind::Video) c = b.addGenerator(index, "color", recIn, len, "Black");
                if (c) c->generator.params["color.r"] = c->generator.params["color.g"] = c->generator.params["color.b"] = Param(0.0);
            } else {
                const std::string path = locate(e);
                const std::string name = !e.name.empty() ? e.name : e.reel.toStdString();
                const Id mid = b.media(path, name, kind == TrackKind::Video, kind == TrackKind::Audio, double(e.srcOut) / fps.toDouble() + 1);
                // EDL source timecode counts from the media's start timecode.
                double offset = 0;
                if (const MediaItem* m = p.findMedia(mid); m && m->timecode > 0) offset = m->timecode * fps.toDouble();
                c = b.addClip(kind, index, mid, recIn, len, srcIn - offset, name);
            }
            if (!c) continue;
            c->speed = std::max(0.01, std::fabs(e.speed));
            c->reverse = e.speed < 0;
            if (dissolve > 0 && lastClip.count(chKey)) {
                // The outgoing clip runs to the middle of the dissolve.
                if (Clip* prev = edit::clipById(b.sequence(), lastClip[chKey]); prev && prev->end() <= recIn) {
                    prev->duration = recIn - prev->start;
                    b.addTransition(kind, index, prev->id, c->id, dissolve, {});
                }
            }
            lastClip[chKey] = c->id;
        }
    }
    return b.finish();
}

}  // namespace montage
