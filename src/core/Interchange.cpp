#include "Interchange.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>

#include "EditOps.h"
#include "Effects.h"
#include "History.h"

namespace montage {

// ---------------------------------------------------------------------------
// EDL

namespace {

std::string reelFor(const Clip& c) { return c.isGenerator() ? "BL" : "AX"; }

const Clip* findClip(const Track& t, Id id) {
    for (const auto& c : t.clips)
        if (c.id == id) return &c;
    return nullptr;
}

struct EdlWriter {
    const Project& p;
    const Sequence& s;
    std::ostringstream out;
    int event = 0;

    std::string tc(FrameTime f) const { return formatTimecode(std::max<FrameTime>(0, f), s.fps); }
    // Source timecode of timeline frame t inside clip c.
    FrameTime src(const Clip& c, FrameTime t) const { return FrameTime(std::llround(c.sourceFrameAt(t))); }

    void comment(const Clip& c) {
        out << "* FROM CLIP NAME: " << c.name << "\n";
        if (const MediaItem* m = p.findMedia(c.mediaId); m && !m->path.empty()) out << "* SOURCE FILE: " << m->path << "\n";
        if (c.speed != 1.0 || c.reverse)
            out << "M2   " << reelFor(c) << "       " << std::fixed << std::setprecision(1)
                << (c.reverse ? -1 : 1) * c.speed * s.fpsValue() << "    " << tc(src(c, c.start)) << "\n";
    }

    void line(const std::string& reel, const std::string& channel, const std::string& type, FrameTime srcIn,
              FrameTime srcOut, FrameTime recIn, FrameTime recOut) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "%03d  %-8s %-5s %-8s %s %s %s %s\n", event, reel.c_str(), channel.c_str(), type.c_str(),
                      tc(srcIn).c_str(), tc(srcOut).c_str(), tc(recIn).c_str(), tc(recOut).c_str());
        out << buf;
    }

    void writeTrack(const Track& t, const std::string& channel) {
        for (size_t i = 0; i < t.clips.size(); ++i) {
            const Clip& c = t.clips[i];
            if (!c.enabled) continue;
            FrameTime recIn = c.start, recOut = c.end();
            // A dissolve into this clip starts the event early; the outgoing clip ends at the dissolve start.
            const Transition* in = nullptr;
            const Transition* outTr = nullptr;
            for (const auto& tr : t.transitions) {
                if (tr.clipB == c.id && tr.clipA) in = &tr;
                if (tr.clipA == c.id && tr.clipB) outTr = &tr;
            }
            FrameTime from = 0, to = 0;
            if (outTr && edit::transitionRange(t, *outTr, from, to)) recOut = from;
            if (in && edit::transitionRange(t, *in, from, to)) {
                const Clip* a = findClip(t, in->clipA);
                ++event;
                // Zero-length cut to the outgoing source, then the dissolve.
                if (a) line(reelFor(*a), channel, "C", src(*a, from), src(*a, from), from, from);
                char type[16];
                std::snprintf(type, sizeof type, "D    %03lld", (long long)(to - from));
                line(reelFor(c), channel, type, src(c, from), src(c, recOut), from, recOut);
                if (a) out << "* FROM CLIP NAME: " << a->name << "\n";
                out << "* TO CLIP NAME: " << c.name << "\n";
                continue;
            }
            ++event;
            line(reelFor(c), channel, "C", src(c, recIn), src(c, recIn) + (recOut - recIn), recIn, recOut);
            comment(c);
        }
    }
};

}  // namespace

std::string exportEdl(const Project& p, const Sequence& s, int videoTrack) {
    EdlWriter w{p, s, {}, 0};
    w.out << "TITLE: " << s.name << "\n";
    w.out << "FCM: " << (isDropFrameRate(s.fps) ? "DROP FRAME" : "NON-DROP FRAME") << "\n\n";
    if (const Track* v = trackAt(s, {TrackKind::Video, videoTrack})) w.writeTrack(*v, "V");
    for (size_t i = 0; i < s.audioTracks.size() && i < 4; ++i) {
        std::string ch = i == 0 ? "A" : "A" + std::to_string(i + 1);
        w.writeTrack(s.audioTracks[i], ch);
    }
    return w.out.str();
}

// ---------------------------------------------------------------------------
// OpenTimelineIO

namespace {

QJsonObject rt(double value, double rate) {
    return QJsonObject{{"OTIO_SCHEMA", "RationalTime.1"}, {"rate", rate}, {"value", value}};
}

QJsonObject range(double start, double duration, double rate) {
    return QJsonObject{{"OTIO_SCHEMA", "TimeRange.1"}, {"start_time", rt(start, rate)}, {"duration", rt(duration, rate)}};
}

QJsonObject item(const char* schema, const QString& name) {
    return QJsonObject{{"OTIO_SCHEMA", schema}, {"name", name},       {"effects", QJsonArray()},
                       {"markers", QJsonArray()}, {"metadata", QJsonObject()}, {"enabled", true}};
}

QString otioColor(int label) {
    static const char* colors[] = {"RED", "PURPLE", "BLUE", "CYAN", "PINK", "BLUE", "GREEN", "PINK", "ORANGE", "YELLOW", "ORANGE", "RED"};
    return colors[std::clamp(label, 0, 11)];
}

QJsonObject clipJson(const Project& p, const Sequence& s, const Clip& c) {
    const double rate = s.fpsValue();
    QJsonObject o = item("Clip.2", QString::fromStdString(c.name));
    o["enabled"] = c.enabled;
    double srcStart = c.reverse ? c.sourceIn + c.sourceExtent() : c.sourceIn;
    o["source_range"] = range(srcStart, double(c.duration), rate);
    QJsonObject ref;
    if (c.isGenerator()) {
        QString kind = c.generator.type == "color" ? "SolidColor" : (c.generator.type == "title" ? "Title" : QString::fromStdString(c.generator.type));
        QJsonObject params;
        for (const auto& [k, v] : c.generator.params) params[QString::fromStdString(k)] = v.at(0);
        for (const auto& [k, v] : c.generator.strings) params[QString::fromStdString(k)] = QString::fromStdString(v);
        ref = QJsonObject{{"OTIO_SCHEMA", "GeneratorReference.1"}, {"name", kind}, {"generator_kind", kind},
                          {"parameters", params}, {"metadata", QJsonObject()}, {"available_range", QJsonValue()}};
    } else if (const MediaItem* m = p.findMedia(c.mediaId)) {
        ref = QJsonObject{{"OTIO_SCHEMA", "ExternalReference.1"}, {"name", QString::fromStdString(m->name)},
                          {"target_url", QUrl::fromLocalFile(QString::fromStdString(m->path)).toString()},
                          {"metadata", QJsonObject()}};
        if (m->duration > 0) ref["available_range"] = range(0, std::floor(m->duration * rate), rate);
        else ref["available_range"] = QJsonValue();
    } else {
        ref = QJsonObject{{"OTIO_SCHEMA", "MissingReference.1"}, {"name", ""}, {"metadata", QJsonObject()}, {"available_range", QJsonValue()}};
    }
    o["media_references"] = QJsonObject{{"DEFAULT_MEDIA", ref}};
    o["active_media_reference_key"] = "DEFAULT_MEDIA";
    if (c.speed != 1.0 || c.reverse) {
        QJsonArray fx;
        fx.append(QJsonObject{{"OTIO_SCHEMA", "LinearTimeWarp.1"}, {"name", ""}, {"effect_name", "LinearTimeWarp"},
                              {"time_scalar", (c.reverse ? -1.0 : 1.0) * c.speed}, {"metadata", QJsonObject()}});
        o["effects"] = fx;
    }
    QJsonObject meta;
    QJsonArray effects;
    for (const auto& e : c.effects) effects.append(QString::fromStdString(e.type));
    meta["montage"] = QJsonObject{{"clip_id", double(c.id)}, {"effects", effects}, {"blend_mode", QString::fromStdString(c.blendMode)}};
    o["metadata"] = meta;
    return o;
}

QJsonObject trackJson(const Project& p, const Sequence& s, const Track& t) {
    const double rate = s.fpsValue();
    QJsonObject o = item("Track.1", QString::fromStdString(t.name));
    o.remove("enabled");
    o["enabled"] = !t.muted;
    o["kind"] = t.kind == TrackKind::Video ? "Video" : "Audio";
    o["source_range"] = QJsonValue();
    QJsonArray children;
    FrameTime cursor = 0;
    for (const Clip& c : t.clips) {
        if (c.start > cursor) {
            QJsonObject gap = item("Gap.1", "");
            gap["source_range"] = range(0, double(c.start - cursor), rate);
            children.append(gap);
        }
        // A transition into this clip sits between it and the previous item.
        for (const auto& tr : t.transitions) {
            if (tr.clipB != c.id) continue;
            FrameTime from, to;
            if (!edit::transitionRange(t, tr, from, to)) continue;
            QString type = (tr.type == "cross_dissolve" || tr.type.rfind("crossfade", 0) == 0) ? "SMPTE_Dissolve" : "Custom_Transition";
            children.append(QJsonObject{{"OTIO_SCHEMA", "Transition.1"}, {"name", QString::fromStdString(tr.type)},
                                        {"transition_type", type}, {"in_offset", rt(double(c.start - from), rate)},
                                        {"out_offset", rt(double(to - c.start), rate)}, {"metadata", QJsonObject()}});
        }
        children.append(clipJson(p, s, c));
        cursor = c.end();
    }
    o["children"] = children;
    return o;
}

}  // namespace

std::string exportOtio(const Project& p, const Sequence& s) {
    const double rate = s.fpsValue();
    QJsonArray tracks;
    for (const auto& t : s.videoTracks) tracks.append(trackJson(p, s, t));
    for (const auto& t : s.audioTracks) tracks.append(trackJson(p, s, t));
    QJsonArray markers;
    for (const auto& m : s.markers)
        markers.append(QJsonObject{{"OTIO_SCHEMA", "Marker.2"}, {"name", QString::fromStdString(m.name)},
                                   {"comment", QString::fromStdString(m.comment)}, {"color", otioColor(m.color)},
                                   {"marked_range", range(double(m.t), double(m.duration), rate)}, {"metadata", QJsonObject()}});
    QJsonObject stack = item("Stack.1", "tracks");
    stack["children"] = tracks;
    stack["markers"] = markers;
    stack["source_range"] = QJsonValue();
    QJsonObject root{{"OTIO_SCHEMA", "Timeline.1"},
                     {"name", QString::fromStdString(s.name)},
                     {"global_start_time", rt(0, rate)},
                     {"tracks", stack},
                     {"metadata", QJsonObject{{"montage", QJsonObject{{"width", s.width}, {"height", s.height}, {"sample_rate", s.sampleRate}}}}}};
    return QJsonDocument(root).toJson(QJsonDocument::Indented).toStdString();
}

}  // namespace montage
