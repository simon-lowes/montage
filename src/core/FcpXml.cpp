// Montage — Final Cut Pro XML both ways: FCP 7 XML (xmeml 5, also Premiere
// Pro's and Resolve's exchange format) and FCPXML 1.10 (Final Cut Pro X).
#include <QDomDocument>
#include <QFileInfo>
#include <QUrl>
#include <QXmlStreamWriter>
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <numeric>
#include <optional>
#include <set>

#include "EditOps.h"
#include "Effects.h"
#include "Interpretation.h"
#include "History.h"
#include "ImportBuilder.h"
#include "Interchange.h"
#include "Multicam.h"

namespace montage {

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }
QString fileUrl(const std::string& path) {
    QUrl u = QUrl::fromLocalFile(q(path));
    return u.toString();
}
std::string pathFromUrl(const QString& url) {
    if (url.isEmpty()) return {};
    QUrl u(url);
    if (u.scheme() == "file") {
        // FCP 7 writes file://localhost/...
        u.setHost(QString());
        return u.toLocalFile().toStdString();
    }
    return u.scheme().isEmpty() ? url.toStdString() : u.toString().toStdString();
}

// Timeline frames of the source a clip shows at its first frame / after its last.
double srcStart(const Clip& c) { return c.reverse ? c.sourceIn + c.sourceExtent() : c.sourceIn; }

const Transition* transitionInto(const Track& t, Id clip) {
    for (const auto& tr : t.transitions)
        if (tr.clipB == clip && tr.clipA) return &tr;
    return nullptr;
}
const Transition* transitionOutOf(const Track& t, Id clip) {
    for (const auto& tr : t.transitions)
        if (tr.clipA == clip && tr.clipB) return &tr;
    return nullptr;
}

QDomElement child(const QDomElement& e, const char* name) { return e.firstChildElement(name); }
QString text(const QDomElement& e, const char* name, const QString& def = {}) {
    QDomElement c = e.firstChildElement(name);
    return c.isNull() ? def : c.text().trimmed();
}
double num(const QDomElement& e, const char* name, double def = 0) {
    bool ok = false;
    const double v = text(e, name).toDouble(&ok);
    return ok ? v : def;
}

}  // namespace

// ===========================================================================
// FCP 7 XML (xmeml)

namespace {

struct Fcp7Writer {
    const Project& p;
    const Sequence* cur;  // the sequence being written (the cut, or one nested in it)
    QXmlStreamWriter w;
    QString out;
    std::map<Id, QString> fileIds;   // media -> file id (written in full once)
    std::map<Id, QString> clipItem;  // clip -> clipitem id (in the sequence being written)
    std::map<std::string, QString> nestedIds;  // nested sequences (and multicam parts) -> their ids (written in full where first used)
    std::deque<Sequence> copies;       // those sequences as the exports write them
    int nextItem = 0, depth = 0;

    Fcp7Writer(const Project& pr, const Sequence& sq) : p(pr), cur(&sq), w(&out) {}

    int timebase() const { return int(std::lround(cur->fpsValue())); }
    bool ntsc() const { return cur->fps.den == 1001; }
    void rate() {
        w.writeStartElement("rate");
        w.writeTextElement("timebase", QString::number(timebase()));
        w.writeTextElement("ntsc", ntsc() ? "TRUE" : "FALSE");
        w.writeEndElement();
    }
    void file(const MediaItem& m) {
        auto it = fileIds.find(m.id);
        if (it != fileIds.end()) {
            w.writeEmptyElement("file");
            w.writeAttribute("id", it->second);
            return;
        }
        const QString id = QString("file-%1").arg(fileIds.size() + 1);
        fileIds[m.id] = id;
        w.writeStartElement("file");
        w.writeAttribute("id", id);
        w.writeTextElement("name", q(m.name));
        QString url = fileUrl(uninterpretedPath(m.path));
        url.replace("file:///", "file://localhost/");
        w.writeTextElement("pathurl", url);
        rate();
        w.writeTextElement("duration", QString::number(std::llround(m.duration * cur->fpsValue())));
        w.writeStartElement("media");
        if (m.hasVideo) {
            w.writeStartElement("video");
            w.writeStartElement("samplecharacteristics");
            w.writeTextElement("width", QString::number(m.width));
            w.writeTextElement("height", QString::number(m.height));
            w.writeEndElement();
            w.writeEndElement();
        }
        if (m.hasAudio) {
            w.writeStartElement("audio");
            w.writeTextElement("channelcount", QString::number(std::max(1, m.channels)));
            w.writeEndElement();
        }
        w.writeEndElement();  // media
        w.writeEndElement();  // file
    }
    void speedFilter(const Clip& c) {
        if (c.speed == 1.0 && !c.reverse && !c.ramped()) return;
        const double speed = c.ramped() ? c.sourceExtent() / double(c.duration) : c.speed;
        w.writeStartElement("filter");
        w.writeStartElement("effect");
        w.writeTextElement("name", "Time Remap");
        w.writeTextElement("effectid", "timeremap");
        w.writeTextElement("effectcategory", "motion");
        w.writeTextElement("effecttype", "motion");
        w.writeTextElement("mediatype", "video");
        auto param = [&](const char* id, const QString& v) {
            w.writeStartElement("parameter");
            w.writeTextElement("parameterid", id);
            w.writeTextElement("name", id);
            w.writeTextElement("value", v);
            w.writeEndElement();
        };
        param("speed", QString::number(speed * 100, 'g', 10));
        param("reverse", c.reverse ? "TRUE" : "FALSE");
        w.writeEndElement();
        w.writeEndElement();
    }
    void links(const Clip& c) {
        if (!c.linkGroup) return;
        for (Id other : edit::linkedClips(*cur, c.id)) {
            auto loc = edit::locate(*cur, other);
            if (!loc || !clipItem.count(other)) continue;
            w.writeStartElement("link");
            w.writeTextElement("linkclipref", clipItem[other]);
            w.writeTextElement("mediatype", loc->track.kind == TrackKind::Video ? "video" : "audio");
            w.writeTextElement("trackindex", QString::number(loc->track.index + 1));
            w.writeTextElement("clipindex", QString::number(loc->index + 1));
            w.writeEndElement();
        }
    }
    void generator(const Clip& c, const Track& t) {
        w.writeStartElement("generatoritem");
        w.writeAttribute("id", clipItem[c.id]);
        w.writeTextElement("name", q(c.name));
        w.writeTextElement("enabled", c.enabled ? "TRUE" : "FALSE");
        w.writeTextElement("duration", QString::number(c.duration));
        rate();
        times(c, t);
        w.writeStartElement("effect");
        const bool title = c.generator.type == "title";
        w.writeTextElement("name", title ? "Text" : "Color");
        w.writeTextElement("effectid", title ? "Text" : "Color");
        w.writeTextElement("effectcategory", title ? "Text" : "Matte");
        w.writeTextElement("effecttype", "generator");
        w.writeTextElement("mediatype", "video");
        w.writeStartElement("parameter");
        if (title) {
            w.writeTextElement("parameterid", "str");
            w.writeTextElement("name", "Text");
            w.writeTextElement("value", q(c.generator.s("text")));
        } else {
            w.writeTextElement("parameterid", "fillcolor");
            w.writeTextElement("name", "Color");
            w.writeStartElement("value");
            w.writeTextElement("alpha", "255");
            w.writeTextElement("red", QString::number(std::lround(c.generator.p("color.r", 0) * 255)));
            w.writeTextElement("green", QString::number(std::lround(c.generator.p("color.g", 0) * 255)));
            w.writeTextElement("blue", QString::number(std::lround(c.generator.p("color.b", 0) * 255)));
            w.writeEndElement();
        }
        w.writeEndElement();  // parameter
        w.writeEndElement();  // effect
        w.writeEndElement();
    }
    // start/end/in/out; edges inside a dissolve are -1 (the transitionitem has them), with the source handle included.
    void times(const Clip& c, const Track& t) {
        FrameTime start = c.start, end = c.end();
        double in = srcStart(c), out = in + double(c.duration) * c.speed;
        if (c.ramped()) out = in + c.sourceExtent();
        FrameTime from = 0, to = 0;
        bool startHidden = false, endHidden = false;
        if (const Transition* tr = transitionInto(t, c.id); tr && edit::transitionRange(t, *tr, from, to)) {
            startHidden = true;
            in -= double(c.start - from) * c.speed;
        }
        if (const Transition* tr = transitionOutOf(t, c.id); tr && edit::transitionRange(t, *tr, from, to)) {
            endHidden = true;
            out += double(to - c.end()) * c.speed;
        }
        w.writeTextElement("start", startHidden ? "-1" : QString::number(start));
        w.writeTextElement("end", endHidden ? "-1" : QString::number(end));
        w.writeTextElement("in", QString::number(std::llround(in)));
        w.writeTextElement("out", QString::number(std::llround(out)));
    }
    void clipitem(const Clip& c, const Track& t, bool audio) {
        if (c.isGenerator()) {
            if (!audio) generator(c, t);
            return;
        }
        const MediaItem* m = p.findMedia(c.mediaId);
        if (!m) return;
        w.writeStartElement("clipitem");
        w.writeAttribute("id", clipItem[c.id]);
        w.writeTextElement("masterclipid", QString("masterclip-%1").arg(c.mediaId));
        w.writeTextElement("name", q(c.name));
        w.writeTextElement("enabled", c.enabled ? "TRUE" : "FALSE");
        w.writeTextElement("duration", QString::number(std::llround(m->duration * cur->fpsValue())));
        rate();
        times(c, t);
        const Sequence* nested = m->kind == MediaKind::Sequence ? p.findSequence(m->sequenceId) : nullptr;
        if (nested && depth < 8) {
            // A nested sequence, as Premiere writes one: in full where first used, by its id afterwards. A multicam clip
            // left as one (at another speed) nests only what it shows.
            const std::string key = std::to_string(nested->id) +
                                    (nested->multicam ? (audio ? ":a" + std::to_string(c.audioAngle) : ":v" + std::to_string(c.angle)) : std::string());
            if (auto it = nestedIds.find(key); it != nestedIds.end()) {
                w.writeEmptyElement("sequence");
                w.writeAttribute("id", it->second);
            } else {
                const QString id = QString("sequence-%1").arg(nestedIds.size() + 2);
                nestedIds[key] = id;
                copies.push_back(nested->multicam ? multicamPart(*nested, !audio, c.angle, c.audioAngle)
                                                  : interchangeSequence(flattenedMulticam(p, *nested)));
                sequence(copies.back(), id);
            }
        } else {
            file(*m);
        }
        if (audio) {
            w.writeStartElement("sourcetrack");
            w.writeTextElement("mediatype", "audio");
            w.writeTextElement("trackindex", "1");
            w.writeEndElement();
        }
        speedFilter(c);
        links(c);
        w.writeEndElement();
    }
    void transitions(const Track& t, Id before, bool audio) {
        const Transition* tr = transitionInto(t, before);
        FrameTime from = 0, to = 0;
        if (!tr || !edit::transitionRange(t, *tr, from, to)) return;
        w.writeStartElement("transitionitem");
        rate();
        w.writeTextElement("start", QString::number(from));
        w.writeTextElement("end", QString::number(to));
        w.writeTextElement("alignment", "center");
        w.writeStartElement("effect");
        const char* name = audio ? (tr->type == "crossfade_linear" ? "Cross Fade (0dB)" : "Cross Fade (+3dB)") : "Cross Dissolve";
        w.writeTextElement("name", name);
        w.writeTextElement("effectid", name);
        w.writeTextElement("effectcategory", audio ? "audiotransition" : "Dissolve");
        w.writeTextElement("effecttype", "transition");
        w.writeTextElement("mediatype", audio ? "audio" : "video");
        w.writeEndElement();
        w.writeEndElement();
    }
    void track(const Track& t, bool audio) {
        w.writeStartElement("track");
        w.writeTextElement("enabled", t.muted ? "FALSE" : "TRUE");
        for (const Clip& c : t.clips) {
            transitions(t, c.id, audio);
            clipitem(c, t, audio);
        }
        w.writeEndElement();
    }
    // A sequence element: its settings, tracks and markers.
    void sequence(const Sequence& seq, const QString& id) {
        const Sequence* was = cur;
        const std::map<Id, QString> items = clipItem;
        cur = &seq;
        ++depth;
        for (const auto* list : {&seq.videoTracks, &seq.audioTracks})
            for (const Track& t : *list)
                for (const Clip& c : t.clips) clipItem[c.id] = QString("%1-%2").arg(c.isGenerator() ? "generatoritem" : "clipitem").arg(++nextItem);
        w.writeStartElement("sequence");
        w.writeAttribute("id", id);
        w.writeTextElement("name", q(seq.name));
        w.writeTextElement("duration", QString::number(seq.duration()));
        rate();
        w.writeStartElement("timecode");
        rate();
        w.writeTextElement("string", q(formatTimecode(0, seq.fps)));
        w.writeTextElement("frame", "0");
        w.writeTextElement("displayformat", isDropFrameRate(seq.fps) ? "DF" : "NDF");
        w.writeEndElement();
        w.writeStartElement("media");
        w.writeStartElement("video");
        w.writeStartElement("format");
        w.writeStartElement("samplecharacteristics");
        rate();
        w.writeTextElement("width", QString::number(seq.width));
        w.writeTextElement("height", QString::number(seq.height));
        w.writeTextElement("pixelaspectratio", "square");
        w.writeEndElement();
        w.writeEndElement();
        for (const Track& t : seq.videoTracks) track(t, false);
        w.writeEndElement();  // video
        w.writeStartElement("audio");
        w.writeTextElement("numOutputChannels", "2");
        for (const Track& t : seq.audioTracks) track(t, true);
        w.writeEndElement();  // audio
        w.writeEndElement();  // media
        for (const Marker& m : seq.markers) {
            w.writeStartElement("marker");
            w.writeTextElement("name", q(m.name));
            w.writeTextElement("comment", q(m.comment));
            w.writeTextElement("in", QString::number(m.t));
            w.writeTextElement("out", m.duration > 0 ? QString::number(m.t + m.duration) : "-1");
            w.writeEndElement();
        }
        w.writeEndElement();  // sequence
        --depth;
        cur = was;
        clipItem = items;
    }
    std::string run() {
        const Sequence& top = *cur;
        w.setAutoFormatting(true);
        w.writeStartDocument();
        w.writeDTD("<!DOCTYPE xmeml>");
        w.writeStartElement("xmeml");
        w.writeAttribute("version", "5");
        sequence(top, "sequence-1");
        w.writeEndElement();  // xmeml
        w.writeEndDocument();
        return out.toStdString();
    }
};

Rational fcp7Rate(const QDomElement& rate, Rational def) {
    if (rate.isNull()) return def;
    const int tb = text(rate, "timebase").toInt();
    if (tb <= 0) return def;
    return text(rate, "ntsc").toUpper() == "TRUE" ? Rational{tb * 1000, 1001} : Rational{tb, 1};
}

struct Fcp7Reader {
    Project& p;
    const MediaProber& probe;
    std::map<QString, QDomElement> files;      // file id -> its full description
    std::map<QString, QDomElement> sequences;  // sequence id -> its full description
    std::map<QString, Id> made;                // nested sequence id -> its media item
    std::set<QString> making;
    ImportResult nested;  // what nested builds found

    Fcp7Reader(Project& pr, const QDomDocument& doc, const MediaProber& pb) : p(pr), probe(pb) {
        // Files and sequences are described once and referenced by id afterwards.
        const QDomNodeList fileNodes = doc.elementsByTagName("file");
        for (int i = 0; i < fileNodes.size(); ++i) {
            QDomElement f = fileNodes.at(i).toElement();
            if (f.hasChildNodes() && !f.attribute("id").isEmpty()) files.emplace(f.attribute("id"), f);
        }
        const QDomNodeList seqNodes = doc.elementsByTagName("sequence");
        for (int i = 0; i < seqNodes.size(); ++i) {
            QDomElement e = seqNodes.at(i).toElement();
            if (!child(e, "media").isNull() && !e.attribute("id").isEmpty()) sequences.emplace(e.attribute("id"), e);
        }
    }

    // The media item placing a nested sequence, built once.
    Id nestedMedia(const QDomElement& ref, Rational parentFps) {
        QDomElement def = ref;
        const QString id = ref.attribute("id");
        if (child(def, "media").isNull()) {
            const auto it = sequences.find(id);
            if (it == sequences.end()) return 0;
            def = it->second;
        }
        const QString key = id.isEmpty() ? QString::number(def.lineNumber()) + ":" + QString::number(def.columnNumber()) : id;
        if (auto it = made.find(key); it != made.end()) return it->second;
        if (making.count(key) || making.size() > 16) return 0;
        making.insert(key);
        ImportResult one = build(def, parentFps);
        making.erase(key);
        nested.clips += one.clips;
        nested.offline.insert(nested.offline.end(), one.offline.begin(), one.offline.end());
        nested.warnings.insert(nested.warnings.end(), one.warnings.begin(), one.warnings.end());
        const Sequence* ns = one.ok ? p.findSequence(one.sequence) : nullptr;
        if (!ns) return 0;
        MediaItem item;
        item.id = p.newId();
        item.kind = MediaKind::Sequence;
        item.name = ns->name;
        item.sequenceId = ns->id;
        for (const Track& t : ns->videoTracks) item.hasVideo |= !t.clips.empty();
        for (const Track& t : ns->audioTracks) item.hasAudio |= !t.clips.empty();
        item.width = ns->width;
        item.height = ns->height;
        item.fps = ns->fps;
        item.duration = double(ns->duration()) / std::max(1e-9, ns->fpsValue());
        p.media.push_back(item);
        made[key] = item.id;
        return item.id;
    }

    // One sequence; nested ones are added to the project without becoming the active one.
    ImportResult build(const QDomElement& seqEl, Rational defaultFps) {
        const Id active = p.activeSequence;
        const bool top = making.empty();
        const Rational fps = fcp7Rate(child(seqEl, "rate"), defaultFps);
        TimelineBuilder b(p, text(seqEl, "name").toStdString(), fps, probe);
        const QDomElement media = child(seqEl, "media");
        if (QDomElement sc = child(child(child(media, "video"), "format"), "samplecharacteristics"); !sc.isNull()) {
            b.sequence().width = std::max(16, int(num(sc, "width", 1920)));
            b.sequence().height = std::max(16, int(num(sc, "height", 1080)));
        }
        std::map<QString, Id> clipIds;                // clipitem id -> clip
        std::vector<std::pair<QString, QStringList>> linkSets;
        for (int pass = 0; pass < 2; ++pass) {
            const bool audio = pass == 1;
            const QDomElement kindEl = child(media, audio ? "audio" : "video");
            int ti = 0;
            for (QDomElement t = kindEl.firstChildElement("track"); !t.isNull(); t = t.nextSiblingElement("track"), ++ti) {
                const TrackKind kind = audio ? TrackKind::Audio : TrackKind::Video;
                Track& track = b.track(kind, ti);
                track.muted = text(t, "enabled", "TRUE").toUpper() == "FALSE";
                struct Placed {
                    Id id;
                    FrameTime start, end;  // as written (inside dissolves too)
                };
                std::vector<Placed> placed;
                struct Dissolve {
                    FrameTime from, to;
                    std::string type;
                };
                std::vector<Dissolve> dissolves;
                std::optional<Dissolve> lastTr;
                // Clip edges at -1 come from the neighbouring transitionitem.
                std::vector<QDomElement> items;
                for (QDomElement it = t.firstChildElement(); !it.isNull(); it = it.nextSiblingElement()) items.push_back(it);
                for (size_t k = 0; k < items.size(); ++k) {
                    const QDomElement& it = items[k];
                    if (it.tagName() == "transitionitem") {
                        Dissolve d{FrameTime(num(it, "start")), FrameTime(num(it, "end")), {}};
                        const QString name = text(child(it, "effect"), "name");
                        if (audio) d.type = name.contains("0dB") ? "crossfade_linear" : "crossfade";
                        dissolves.push_back(d);
                        lastTr = d;
                        continue;
                    }
                    if (it.tagName() != "clipitem" && it.tagName() != "generatoritem") continue;
                    FrameTime start = FrameTime(num(it, "start", -1)), end = FrameTime(num(it, "end", -1));
                    double in = num(it, "in"), out = num(it, "out");
                    if (start < 0) start = lastTr ? lastTr->from : 0;
                    if (end < 0)
                        for (size_t n = k + 1; n < items.size(); ++n)
                            if (items[n].tagName() == "transitionitem") {
                                end = FrameTime(num(items[n], "end"));
                                break;
                            }
                    lastTr.reset();
                    if (end <= start) continue;
                    // Speed from a Time Remap filter.
                    double speed = 1;
                    bool reverse = false;
                    for (QDomElement f = it.firstChildElement("filter"); !f.isNull(); f = f.nextSiblingElement("filter")) {
                        const QDomElement e = child(f, "effect");
                        if (text(e, "effectid") != "timeremap") continue;
                        for (QDomElement pr = e.firstChildElement("parameter"); !pr.isNull(); pr = pr.nextSiblingElement("parameter")) {
                            if (text(pr, "parameterid") == "speed") speed = std::max(0.01, std::fabs(num(pr, "value", 100)) / 100);
                            if (text(pr, "parameterid") == "reverse") reverse = text(pr, "value").toUpper() == "TRUE";
                        }
                    }
                    Clip* c = nullptr;
                    if (it.tagName() == "generatoritem") {
                        if (audio) continue;
                        const QDomElement e = child(it, "effect");
                        const QString id = text(e, "effectid");
                        const bool title = id.contains("Text", Qt::CaseInsensitive) || text(e, "effectcategory") == "Text";
                        c = b.addGenerator(ti, title ? "title" : "color", start, end - start, text(it, "name").toStdString());
                        if (c)
                            for (QDomElement pr = e.firstChildElement("parameter"); !pr.isNull(); pr = pr.nextSiblingElement("parameter")) {
                                if (title && text(pr, "parameterid") == "str") c->generator.strings["text"] = text(pr, "value").toStdString();
                                if (!title && text(pr, "parameterid") == "fillcolor") {
                                    const QDomElement v = child(pr, "value");
                                    c->generator.params["color.r"] = Param(num(v, "red") / 255);
                                    c->generator.params["color.g"] = Param(num(v, "green") / 255);
                                    c->generator.params["color.b"] = Param(num(v, "blue") / 255);
                                }
                            }
                    } else if (const QDomElement inner = child(it, "sequence"); !inner.isNull()) {
                        // A nested sequence (Premiere writes it in full where first used, by its id afterwards).
                        const Id mid = nestedMedia(inner, fps);
                        if (!mid) {
                            b.warn("A nested sequence could not be read (" + text(it, "name").toStdString() + ")");
                            continue;
                        }
                        c = b.addClip(kind, ti, mid, start, end - start, reverse ? out - double(end - start) * speed : in, text(it, "name").toStdString());
                        if (c) {
                            c->speed = speed;
                            c->reverse = reverse;
                        }
                    } else {
                        QDomElement f = child(it, "file");
                        if (auto known = files.find(f.attribute("id")); known != files.end()) f = known->second;
                        const std::string path = pathFromUrl(text(f, "pathurl"));
                        const Rational frate = fcp7Rate(child(f, "rate"), fps);
                        const double seconds = num(f, "duration", out) / std::max(1.0, frate.toDouble());
                        const QDomElement fm = child(f, "media");
                        const bool hasV = !child(fm, "video").isNull() || (!audio && fm.isNull());
                        const bool hasA = !child(fm, "audio").isNull() || audio;
                        const Id mid = b.media(path, text(f, "name").toStdString(), hasV || !audio, hasA, seconds);
                        const double srcIn = reverse ? out : in;  // a reversed clip starts at its out point
                        c = b.addClip(kind, ti, mid, start, end - start, reverse ? out - double(end - start) * speed : srcIn,
                                      text(it, "name").toStdString());
                        if (c) {
                            c->speed = speed;
                            c->reverse = reverse;
                        }
                    }
                    if (!c) continue;
                    c->enabled = text(it, "enabled", "TRUE").toUpper() != "FALSE";
                    placed.push_back({c->id, start, end});
                    clipIds[it.attribute("id")] = c->id;
                    QStringList linked;
                    for (QDomElement l = it.firstChildElement("link"); !l.isNull(); l = l.nextSiblingElement("link"))
                        linked << text(l, "linkclipref");
                    if (!linked.isEmpty()) linkSets.push_back({it.attribute("id"), linked});
                }
                // Dissolves: the clips overlap across them; cut both at the middle.
                for (const Dissolve& d : dissolves) {
                    const FrameTime cut = (d.from + d.to) / 2;
                    Clip* a = nullptr;
                    Clip* bc = nullptr;
                    for (const Placed& pl : placed) {
                        if (pl.end == d.to && pl.start < d.from) a = edit::clipById(b.sequence(), pl.id);
                        if (pl.start == d.from && pl.end > d.to) bc = edit::clipById(b.sequence(), pl.id);
                    }
                    if (!a || !bc) continue;
                    a->duration = cut - a->start;
                    const FrameTime shift = cut - bc->start;
                    if (!bc->reverse) bc->sourceIn += double(shift) * bc->speed;
                    bc->start = cut;
                    bc->duration -= shift;
                    b.addTransition(kind, ti, a->id, bc->id, d.to - d.from, d.type);
                }
            }
        }
        // The XML's own links (Premiere links picture and sound this way).
        for (const auto& [id, linked] : linkSets) {
            auto me = clipIds.find(id);
            if (me == clipIds.end()) continue;
            Clip* c = edit::clipById(b.sequence(), me->second);
            if (!c) continue;
            for (const QString& other : linked) {
                auto o = clipIds.find(other);
                if (o == clipIds.end() || o->second == c->id) continue;
                Clip* oc = edit::clipById(b.sequence(), o->second);
                if (!oc) continue;
                if (!c->linkGroup) c->linkGroup = oc->linkGroup ? oc->linkGroup : p.newId();
                oc->linkGroup = c->linkGroup;
            }
        }
        for (QDomElement m = seqEl.firstChildElement("marker"); !m.isNull(); m = m.nextSiblingElement("marker")) {
            Marker mk;
            mk.t = FrameTime(num(m, "in"));
            const double out = num(m, "out", -1);
            mk.duration = out > mk.t ? FrameTime(out) - mk.t : 0;
            mk.name = text(m, "name").toStdString();
            mk.comment = text(m, "comment").toStdString();
            b.sequence().markers.push_back(mk);
        }
        ImportResult r = b.finish();
        if (!top) p.activeSequence = active;
        return r;
    }
};

ImportResult importFcp7(Project& p, const QDomDocument& doc, const MediaProber& probe) {
    QDomElement seqEl = doc.documentElement().firstChildElement("sequence");
    if (seqEl.isNull()) {
        // A project export: the first sequence anywhere.
        QDomNodeList all = doc.elementsByTagName("sequence");
        if (!all.isEmpty()) seqEl = all.at(0).toElement();
    }
    if (seqEl.isNull()) {
        ImportResult r;
        r.error = "The XML holds no sequence";
        return r;
    }
    Fcp7Reader reader(p, doc, probe);
    ImportResult r = reader.build(seqEl, {30, 1});
    r.clips += reader.nested.clips;
    r.offline.insert(r.offline.end(), reader.nested.offline.begin(), reader.nested.offline.end());
    r.warnings.insert(r.warnings.end(), reader.nested.warnings.begin(), reader.nested.warnings.end());
    return r;
}

}  // namespace

namespace {
// QXmlStreamWriter leaves the encoding out when writing to a string; some importers want it.
std::string withEncoding(std::string xml) {
    const std::string bare = "<?xml version=\"1.0\"?>";
    if (xml.rfind(bare, 0) == 0) xml.replace(0, bare.size(), "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    return xml;
}
}  // namespace

std::string exportFcp7Xml(const Project& p, const Sequence& sequence) {
    const Sequence s = interchangeSequence(flattenedMulticam(p, sequence));
    Fcp7Writer w(p, s);
    return withEncoding(w.run());
}

// ===========================================================================
// FCPXML 1.10

namespace {

// Rational seconds for a number of sequence frames, e.g. "1001/30000s".
QString fcpTime(double frames, Rational fps) {
    const long long n = std::llround(frames * fps.den), d = fps.num;
    if (n == 0) return "0s";
    const long long g = std::gcd(std::llabs(n), d);
    if (d / g == 1) return QString("%1s").arg(n / g);
    return QString("%1/%2s").arg(n / g).arg(d / g);
}

// "3003/1000s", "10s" or "1.5s" -> seconds.
double parseFcpTime(const QString& v) {
    QString s = v.trimmed();
    if (s.endsWith('s')) s.chop(1);
    if (s.isEmpty()) return 0;
    const int slash = s.indexOf('/');
    if (slash < 0) return s.toDouble();
    const double d = s.mid(slash + 1).toDouble();
    return d != 0 ? s.left(slash).toDouble() / d : 0;
}

struct FcpxWriter {
    const Project& p;
    const Sequence& s;    // the project's sequence
    const Sequence* cur;  // the sequence being written (s, or one nested in it)
    QXmlStreamWriter w;
    QString out;
    std::map<Id, QString> assets;
    std::map<Id, QString> nested;           // nested and multicam sequences -> their media resources
    std::map<std::string, QString> formats;  // rate and size -> format resource
    std::vector<const Sequence*> nestedOrder;  // innermost first, as resources need them
    std::deque<Sequence> copies;               // those sequences as the exports write them (interchangeSequence)
    QString formatId = "r1", dissolveId, titleId, solidId;
    int nextRes = 2;

    FcpxWriter(const Project& pr, const Sequence& sq) : p(pr), s(sq), cur(&sq), w(&out) {}
    QString t(double frames) const { return fcpTime(frames, cur->fps); }

    // The nested or multicam sequence a clip shows, or null.
    const Sequence* nestedOf(const Clip& c) const {
        if (c.isGenerator()) return nullptr;
        const MediaItem* m = p.findMedia(c.mediaId);
        return m && m->kind == MediaKind::Sequence ? p.findSequence(m->sequenceId) : nullptr;
    }
    void collect(const Sequence& seq, std::set<Id>& open, int depth) {
        if (depth > 8) return;
        open.insert(seq.id);
        for (const auto* list : {&seq.videoTracks, &seq.audioTracks})
            for (const Track& tr : *list)
                for (const Clip& c : tr.clips)
                    if (const Sequence* n = nestedOf(c); n && !open.count(n->id) && !nested.count(n->id)) {
                        collect(*n, open, depth + 1);
                        nested[n->id] = QString();  // (its id comes later)
                        copies.push_back(interchangeSequence(*n));
                        nestedOrder.push_back(&copies.back());
                    }
        open.erase(seq.id);
    }
    QString formatFor(const Sequence& seq) {
        const std::string key = std::to_string(seq.fps.num) + "/" + std::to_string(seq.fps.den) + ":" + std::to_string(seq.width) + "x" +
                                std::to_string(seq.height);
        auto it = formats.find(key);
        if (it != formats.end()) return it->second;
        const QString id = formats.empty() ? formatId : QString("r%1").arg(nextRes++);
        formats[key] = id;
        w.writeEmptyElement("format");
        w.writeAttribute("id", id);
        w.writeAttribute("frameDuration", fcpTime(1, seq.fps));
        w.writeAttribute("width", QString::number(seq.width));
        w.writeAttribute("height", QString::number(seq.height));
        return id;
    }

    void resources() {
        w.writeStartElement("resources");
        formatFor(s);
        std::set<Id> open;
        collect(s, open, 0);
        std::vector<const Sequence*> all{&s};
        all.insert(all.end(), nestedOrder.begin(), nestedOrder.end());
        for (const Sequence* n : nestedOrder) formatFor(*n);
        bool dissolve = false, title = false, solid = false;
        for (const Sequence* seq : all)
            for (const auto* list : {&seq->videoTracks, &seq->audioTracks})
                for (const Track& tr : *list) {
                    dissolve |= !tr.transitions.empty();
                    for (const Clip& c : tr.clips) {
                        if (c.isGenerator()) (c.generator.type == "title" ? title : solid) = true;
                        else if (const MediaItem* m = p.findMedia(c.mediaId); m && m->kind != MediaKind::Sequence && !assets.count(m->id)) {
                            const QString id = QString("r%1").arg(nextRes++);
                            assets[m->id] = id;
                            w.writeStartElement("asset");
                            w.writeAttribute("id", id);
                            w.writeAttribute("name", q(m->name));
                            w.writeAttribute("start", "0s");
                            w.writeAttribute("duration", fcpTime(std::floor(m->duration * seq->fpsValue()), seq->fps));
                            w.writeAttribute("hasVideo", m->hasVideo ? "1" : "0");
                            w.writeAttribute("hasAudio", m->hasAudio ? "1" : "0");
                            if (m->hasVideo) w.writeAttribute("format", formatId);
                            if (m->hasAudio) {
                                w.writeAttribute("audioSources", "1");
                                w.writeAttribute("audioChannels", QString::number(std::max(1, m->channels)));
                                w.writeAttribute("audioRate", QString::number(m->sampleRate > 0 ? m->sampleRate : 48000));
                            }
                            w.writeEmptyElement("media-rep");
                            w.writeAttribute("kind", "original-media");
                            w.writeAttribute("src", fileUrl(m->path));
                            w.writeEndElement();
                        }
                    }
                }
        // Final Cut's own effects, by their identifiers.
        auto effect = [&](QString& id, const char* name, const char* uid) {
            id = QString("r%1").arg(nextRes++);
            w.writeEmptyElement("effect");
            w.writeAttribute("id", id);
            w.writeAttribute("name", name);
            w.writeAttribute("uid", uid);
        };
        if (dissolve) effect(dissolveId, "Cross Dissolve", "FxPlug:4731E73A-8DAC-4113-9A30-AE85B1761265");
        if (title) effect(titleId, "Basic Title", ".../Titles.localized/Bumper:Opener.localized/Basic Title.localized/Basic Title.moti");
        if (solid) effect(solidId, "Custom", ".../Generators.localized/Solids.localized/Custom.localized/Custom.motn");
        // Compound clips' sequences and multicam clips' angles, innermost first.
        for (const Sequence* n : nestedOrder) {
            const QString id = QString("r%1").arg(nextRes++);
            nested[n->id] = id;
            w.writeStartElement("media");
            w.writeAttribute("id", id);
            w.writeAttribute("name", q(n->name));
            if (n->multicam) multicamBody(*n);
            else sequenceElement(*n);
            w.writeEndElement();
        }
        w.writeEndElement();
    }

    void timeMap(const Clip& c) {
        const double speed = c.ramped() ? c.sourceExtent() / double(c.duration) : c.speed;
        if (speed == 1.0 && !c.reverse) return;
        w.writeStartElement("timeMap");
        w.writeEmptyElement("timept");
        w.writeAttribute("time", "0s");
        w.writeAttribute("value", t(c.reverse ? double(c.duration) * speed : 0));
        w.writeAttribute("interp", "linear");
        w.writeEmptyElement("timept");
        w.writeAttribute("time", t(double(c.duration)));
        w.writeAttribute("value", t(c.reverse ? 0 : double(c.duration) * speed));
        w.writeAttribute("interp", "linear");
        w.writeEndElement();
    }

    // A clip connected to (or inside) a storyline element, in that element's time.
    struct Connected {
        const Clip* clip;
        double offset;
        int lane;
    };

    // Multicam angles: each video track is an angle (with its own sound when an audio track carries the same media), and
    // audio tracks of sources with no picture are angles of their own.
    static QString angleId(int index) { return QString("angle-%1").arg(index + 1); }
    static int audioAngleOf(const Sequence& mc, int audioTrack) {
        if (audioTrack < 0 || audioTrack >= int(mc.audioTracks.size())) return -1;
        const int a = audioTrackAngle(mc, audioTrack);
        if (a >= 0) return a;
        int k = int(mc.videoTracks.size());  // audio-only angles follow the picture ones
        for (int i = 0; i < audioTrack; ++i)
            if (audioTrackAngle(mc, i) < 0) ++k;
        return k;
    }

    // One clip element. `lane` 0 is the primary storyline; `offset` is in the parent's time. `audio` is the linked sound
    // carried with it (a multicam clip's audio angle comes from it).
    void clip(const Clip& c, int lane, double offset, bool withAudio, const std::vector<Connected>& connected = {},
              const Clip* audio = nullptr, bool soundOnly = false) {
        const Sequence* inner = nestedOf(c);
        const bool audioOnly = lane < 0 || soundOnly;
        if (c.isGenerator()) {
            const bool title = c.generator.type == "title";
            w.writeStartElement(title ? "title" : "video");
            w.writeAttribute("ref", title ? titleId : solidId);
        } else if (inner && nested.count(inner->id) && inner->multicam) {
            w.writeStartElement("mc-clip");
            w.writeAttribute("ref", nested[inner->id]);
        } else if (inner && nested.count(inner->id)) {
            w.writeStartElement("ref-clip");
            w.writeAttribute("ref", nested[inner->id]);
            if (audioOnly) w.writeAttribute("srcEnable", "audio");
            else if (!withAudio) w.writeAttribute("srcEnable", "video");
        } else {
            w.writeStartElement(withAudio ? "asset-clip" : audioOnly ? "audio" : "video");
            w.writeAttribute("ref", assets[c.mediaId]);
        }
        if (lane != 0) w.writeAttribute("lane", QString::number(lane));
        w.writeAttribute("offset", t(offset));
        w.writeAttribute("name", q(c.name));
        w.writeAttribute("start", c.isGenerator() ? "0s" : t(c.sourceIn));
        w.writeAttribute("duration", t(double(c.duration)));
        if (!c.enabled) w.writeAttribute("enabled", "0");
        if (!c.isGenerator()) timeMap(c);  // (before mc-source, as the DTD orders them)
        if (inner && inner->multicam && nested.count(inner->id)) {
            // Which angle is seen and which heard (Montage's mix of every source becomes the seen angle's sound: for
            // sound on its own, the angle its linked picture shows).
            int shown = c.angle;
            if (audioOnly)
                for (Id other : edit::linkedClips(*cur, c.id))
                    if (const Clip* v = edit::clipById(*cur, other); v && v->mediaId == c.mediaId && edit::locate(*cur, other)->track.kind == TrackKind::Video)
                        shown = v->angle;
            const int video = std::clamp(shown, 0, std::max(0, int(inner->videoTracks.size()) - 1));
            int heard = -1;
            if (audioOnly) heard = audioAngleOf(*inner, c.audioAngle);
            else if (withAudio && audio) heard = audioAngleOf(*inner, audio->audioAngle);
            if ((audioOnly || withAudio) && heard < 0) heard = angleAudioTrack(*inner, video) >= 0 ? video : audioAngleOf(*inner, 0);
            if (!audioOnly && heard == video) {
                w.writeEmptyElement("mc-source");
                w.writeAttribute("angleID", angleId(video));
                w.writeAttribute("srcEnable", "all");
            } else {
                if (!audioOnly) {
                    w.writeEmptyElement("mc-source");
                    w.writeAttribute("angleID", angleId(video));
                    w.writeAttribute("srcEnable", "video");
                }
                if (heard >= 0) {
                    w.writeEmptyElement("mc-source");
                    w.writeAttribute("angleID", angleId(heard));
                    w.writeAttribute("srcEnable", "audio");
                }
            }
        }
        if (c.isGenerator() && c.generator.type == "title") {
            w.writeStartElement("text");
            w.writeStartElement("text-style");
            w.writeAttribute("ref", QString("ts%1").arg(c.id));
            w.writeCharacters(q(c.generator.s("text")));
            w.writeEndElement();
            w.writeEndElement();
            w.writeStartElement("text-style-def");
            w.writeAttribute("id", QString("ts%1").arg(c.id));
            w.writeEmptyElement("text-style");
            w.writeAttribute("font", q(c.generator.s("font", "Helvetica")));
            w.writeAttribute("fontSize", "63");
            w.writeAttribute("fontColor", "1 1 1 1");
            w.writeEndElement();
        }
        for (const Connected& k : connected) clip(*k.clip, k.lane, k.offset, false);
        for (const Marker& m : markersIn(c.start, c.end()))
            marker(m, (c.isGenerator() ? 0 : c.sourceIn) + double(m.t - c.start) * c.speed);
        // Clip markers are already in the clip's own (source) time.
        for (const Marker& m : c.markers)
            if (c.markerFrame(m) >= 0) markerElement(m, double(m.t));
        w.writeEndElement();
    }

    std::vector<Marker> markersIn(FrameTime a, FrameTime b) const {
        std::vector<Marker> out;
        for (const Marker& m : cur->markers)
            if (m.t >= a && m.t < b && !writtenMarkers.count(m.t)) out.push_back(m);
        return out;
    }
    void marker(const Marker& m, double local) {
        writtenMarkers.insert(m.t);
        markerElement(m, local);
    }
    // Chapter markers are FCPXML's own chapter-marker (with a poster frame at their start).
    void markerElement(const Marker& m, double local) {
        w.writeEmptyElement(m.chapter ? "chapter-marker" : "marker");
        w.writeAttribute("start", t(local));
        w.writeAttribute("duration", t(double(std::max<FrameTime>(1, m.duration))));
        w.writeAttribute("value", q(m.name));
        if (!m.comment.empty()) w.writeAttribute("note", q(m.comment));
        if (m.chapter) w.writeAttribute("posterOffset", "0s");
    }
    std::set<FrameTime> writtenMarkers;

    // A sequence element (the project's, or a compound clip's): its settings and its storylines.
    void sequenceElement(const Sequence& seq) {
        const Sequence* was = cur;
        const std::set<FrameTime> marks = writtenMarkers;
        cur = &seq;
        writtenMarkers.clear();
        w.writeStartElement("sequence");
        w.writeAttribute("format", formatFor(seq));
        w.writeAttribute("duration", t(double(seq.duration())));
        w.writeAttribute("tcStart", "0s");
        w.writeAttribute("tcFormat", isDropFrameRate(seq.fps) ? "DF" : "NDF");
        w.writeAttribute("audioLayout", "stereo");
        w.writeAttribute("audioRate", seq.sampleRate == 44100 ? "44.1k" : seq.sampleRate == 96000 ? "96k" : "48k");
        spine(seq);
        w.writeEndElement();
        cur = was;
        writtenMarkers = marks;
    }

    // A multicam clip's angles, each a storyline of its own.
    void multicamBody(const Sequence& mc) {
        const Sequence* was = cur;
        cur = &mc;
        w.writeStartElement("multicam");
        w.writeAttribute("format", formatFor(mc));
        w.writeAttribute("tcStart", "0s");
        w.writeAttribute("tcFormat", isDropFrameRate(mc.fps) ? "DF" : "NDF");
        auto storyline = [&](const Track& tr, const Track* sound) {
            FrameTime cursor = 0;
            for (const Clip& c : tr.clips) {
                if (c.start > cursor) {
                    w.writeEmptyElement("gap");
                    w.writeAttribute("name", "Gap");
                    w.writeAttribute("offset", t(double(cursor)));
                    w.writeAttribute("start", "0s");
                    w.writeAttribute("duration", t(double(c.start - cursor)));
                }
                bool carried = false;
                if (sound)
                    for (const Clip& a : sound->clips) carried |= a.mediaId == c.mediaId && a.start == c.start && a.duration == c.duration;
                clip(c, 0, double(c.start), carried, {}, nullptr, tr.kind == TrackKind::Audio);
                cursor = c.end();
            }
        };
        int index = 0;
        for (size_t v = 0; v < mc.videoTracks.size(); ++v, ++index) {
            w.writeStartElement("mc-angle");
            w.writeAttribute("name", q(mc.videoTracks[v].name));
            w.writeAttribute("angleID", angleId(index));
            const int a = angleAudioTrack(mc, int(v));
            storyline(mc.videoTracks[v], a >= 0 ? &mc.audioTracks[size_t(a)] : nullptr);
            w.writeEndElement();
        }
        for (size_t a = 0; a < mc.audioTracks.size(); ++a) {
            if (audioTrackAngle(mc, int(a)) >= 0) continue;
            w.writeStartElement("mc-angle");
            w.writeAttribute("name", q(mc.audioTracks[a].name));
            w.writeAttribute("angleID", angleId(index++));
            storyline(mc.audioTracks[a], nullptr);
            w.writeEndElement();
        }
        w.writeEndElement();
        cur = was;
    }

    // A sequence's primary storyline (V1, gaps between) and everything connected to it.
    void spine(const Sequence& seq) {
        w.writeStartElement("spine");
        // A1 clips linked to a V1 clip ride in its element.
        static const Track empty;
        const Track& v1 = seq.videoTracks.empty() ? empty : seq.videoTracks[0];
        struct Element {
            const Clip* clip = nullptr;  // null: gap
            FrameTime offset = 0, duration = 0;
            bool withAudio = false;
            const Clip* audio = nullptr;
            std::vector<Connected> connected;
        };
        std::vector<Element> spine;
        std::set<Id> carried;
        FrameTime cursor = 0;
        for (const Clip& c : v1.clips) {
            if (c.start > cursor) spine.push_back({nullptr, cursor, c.start - cursor});
            Element e{&c, c.start, c.duration};
            if (!c.isGenerator())
                for (Id other : edit::linkedClips(seq, c.id))
                    if (auto loc = edit::locate(seq, other); loc && loc->track.kind == TrackKind::Audio && loc->track.index == 0) {
                        const Clip& a = seq.audioTracks[0].clips[loc->index];
                        if (a.mediaId == c.mediaId && a.start == c.start && a.duration == c.duration) {
                            e.withAudio = true;
                            e.audio = &a;
                            carried.insert(a.id);
                        }
                    }
            spine.push_back(e);
            cursor = c.end();
        }
        // Everything else connects to the spine element under its start, in that element's own time.
        auto attach = [&](const Clip& c, int lane) {
            const FrameTime end = spine.empty() ? 0 : spine.back().offset + spine.back().duration;
            if (c.start >= end) spine.push_back({nullptr, end, std::max<FrameTime>(1, c.end() - end)});
            for (Element& e : spine)
                if (c.start >= e.offset && c.start < e.offset + e.duration) {
                    const double local = e.clip && !e.clip->isGenerator() ? e.clip->sourceIn + double(c.start - e.offset) * e.clip->speed
                                                                          : double(c.start - e.offset);
                    e.connected.push_back({&c, local, lane});
                    return;
                }
        };
        for (size_t vi = 1; vi < seq.videoTracks.size(); ++vi)
            for (const Clip& c : seq.videoTracks[vi].clips) attach(c, int(vi));
        for (size_t ai = 0; ai < seq.audioTracks.size(); ++ai)
            for (const Clip& c : seq.audioTracks[ai].clips)
                if (!carried.count(c.id)) attach(c, -std::max(1, int(ai)));  // lane -n reads back as A(n + 1)

        for (const Element& e : spine) {
            if (e.clip) {
                if (const Transition* tr = transitionInto(v1, e.clip->id)) {
                    FrameTime from = 0, to = 0;
                    if (edit::transitionRange(v1, *tr, from, to)) {
                        w.writeStartElement("transition");
                        w.writeAttribute("name", "Cross Dissolve");
                        w.writeAttribute("offset", t(double(from)));
                        w.writeAttribute("duration", t(double(to - from)));
                        w.writeEmptyElement("filter-video");
                        w.writeAttribute("ref", dissolveId);
                        w.writeAttribute("name", "Cross Dissolve");
                        w.writeEndElement();
                    }
                }
                clip(*e.clip, 0, double(e.offset), e.withAudio, e.connected, e.audio);
            } else {
                w.writeStartElement("gap");
                w.writeAttribute("name", "Gap");
                w.writeAttribute("offset", t(double(e.offset)));
                w.writeAttribute("start", "0s");
                w.writeAttribute("duration", t(double(e.duration)));
                for (const Connected& k : e.connected) clip(*k.clip, k.lane, k.offset, false);
                for (const Marker& m : markersIn(e.offset, e.offset + e.duration)) marker(m, double(m.t - e.offset));
                w.writeEndElement();
            }
        }
        w.writeEndElement();  // spine
    }

    std::string run() {
        w.setAutoFormatting(true);
        w.writeStartDocument();
        w.writeDTD("<!DOCTYPE fcpxml>");
        w.writeStartElement("fcpxml");
        w.writeAttribute("version", "1.10");
        resources();
        w.writeStartElement("library");
        w.writeStartElement("event");
        w.writeAttribute("name", "Montage");
        w.writeStartElement("project");
        w.writeAttribute("name", q(s.name));
        sequenceElement(s);
        w.writeEndElement();  // project
        w.writeEndElement();  // event
        w.writeEndElement();  // library
        w.writeEndElement();  // fcpxml
        w.writeEndDocument();
        return out.toStdString();
    }
};

// A format's frame rate (the usual NTSC rates exactly).
Rational fcpxRate(const QDomElement& format) {
    const double frameDur = parseFcpTime(format.attribute("frameDuration", "1/30s"));
    const double rate = frameDur > 0 ? 1 / frameDur : 30;
    if (std::fabs(rate - 29.97) < 0.01) return {30000, 1001};
    if (std::fabs(rate - 23.976) < 0.01) return {24000, 1001};
    if (std::fabs(rate - 59.94) < 0.01) return {60000, 1001};
    return {std::max(1, int(std::lround(rate))), 1};
}

struct FcpxReader {
    Project& p;
    TimelineBuilder* b;
    double fps;
    const MediaProber& probe;
    struct Asset {
        std::string path, name;
        double start = 0, duration = 0;
        bool video = true, audio = true;
    };
    std::map<QString, Asset> assets;
    std::map<QString, QString> effects;     // id -> name
    std::map<QString, QDomElement> formats;  // id -> format
    std::map<QString, QDomElement> medias;   // id -> a compound clip's sequence or a multicam's angles
    // Compound and multicam clips made so far: media resource -> media item, and for multicams each angle's video and
    // audio track (-1 none) by angle id.
    std::map<QString, Id> made;
    std::map<QString, std::map<QString, std::pair<int, int>>> angles;
    std::set<QString> making;
    std::vector<std::string> warnings;
    ImportResult nestedResults;  // what nested builds found (clips, offline files, warnings)
    // Inside a multicam angle, connected sound (a recorder's track beside the camera) waits until every angle has its
    // tracks, then gets tracks of its own (a sound-only source); connected pictures cannot be an angle's.
    struct Deferred {
        QDomElement e;
        double at;
        int lane, angle;
    };
    bool inAngle = false;
    int angleIndex = 0;
    std::vector<Deferred> deferred;

    FcpxReader(Project& pr, TimelineBuilder& bu, double rate, const MediaProber& pb) : p(pr), b(&bu), fps(rate), probe(pb) {}
    FrameTime frames(double seconds) const { return FrameTime(std::llround(seconds * fps)); }

    void readResources(const QDomElement& res) {
        for (QDomElement e = res.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
            if (e.tagName() == "asset") {
                Asset a;
                a.name = e.attribute("name").toStdString();
                a.start = parseFcpTime(e.attribute("start", "0s"));
                a.duration = parseFcpTime(e.attribute("duration", "0s"));
                a.video = e.attribute("hasVideo", "1") == "1";
                a.audio = e.attribute("hasAudio", "0") == "1";
                QString src = e.attribute("src");  // FCPXML 1.8 and earlier
                if (QDomElement rep = e.firstChildElement("media-rep"); !rep.isNull()) src = rep.attribute("src");
                a.path = pathFromUrl(src);
                assets[e.attribute("id")] = a;
            } else if (e.tagName() == "effect") {
                effects[e.attribute("id")] = e.attribute("name");
            } else if (e.tagName() == "format") {
                formats[e.attribute("id")] = e;
            } else if (e.tagName() == "media") {
                medias[e.attribute("id")] = e;
            }
        }
    }

    // A storyline (a spine, or a multicam angle) whose elements are placed by their offsets, from `tcStart`.
    void storyline(const QDomElement& line, double tcStart, int vTrack, int aTrack) {
        Id last = 0;
        double dissolve = 0;
        lastPrimaryAudio = 0;
        for (QDomElement e = line.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
            const double offset = parseFcpTime(e.attribute("offset", "0s")) - tcStart;
            if (e.tagName() == "transition") {
                dissolve = parseFcpTime(e.attribute("duration"));
                continue;
            }
            if (e.tagName() == "gap") {
                last = 0;
                lastPrimaryAudio = 0;
            }
            element(e, offset, vTrack, aTrack, last, dissolve);
        }
    }

    // Builds the sequence of a compound clip (`<sequence>`) or multicam clip (`<multicam>`) once, and the media item
    // that places it; 0 if it cannot be read.
    Id nestedMedia(const QString& ref) {
        if (auto it = made.find(ref); it != made.end()) return it->second;
        const auto m = medias.find(ref);
        if (m == medias.end()) return 0;
        if (making.count(ref) || making.size() > 16) {
            warnings.push_back("A compound clip that contains itself was left out");
            return 0;
        }
        const QDomElement body = !m->second.firstChildElement("multicam").isNull() ? m->second.firstChildElement("multicam")
                                                                                   : m->second.firstChildElement("sequence");
        if (body.isNull()) return 0;
        making.insert(ref);
        const auto format = formats.find(body.attribute("format"));
        const Rational rate = format != formats.end() ? fcpxRate(format->second) : b->sequence().fps;
        const std::string name = m->second.attribute("name").toStdString();
        TimelineBuilder nb(p, name, rate, probe);
        if (format != formats.end() && format->second.hasAttribute("width")) {
            nb.sequence().width = std::max(16, format->second.attribute("width").toInt());
            nb.sequence().height = std::max(16, format->second.attribute("height").toInt());
        } else {
            nb.sequence().width = b->sequence().width;
            nb.sequence().height = b->sequence().height;
        }
        nb.sequence().sampleRate = b->sequence().sampleRate;
        TimelineBuilder* outer = b;
        const double outerFps = fps;
        const Id outerAudio = lastPrimaryAudio;
        b = &nb;
        fps = rate.toDouble();
        const double tcStart = parseFcpTime(body.attribute("tcStart", "0s"));
        const bool multicam = body.tagName() == "multicam";
        if (multicam) {
            // Each angle: its picture a video track (an angle in Montage), its sound an audio track.
            int v = 0, a = 0;
            auto& ids = angles[ref];
            const bool wasInAngle = inAngle;
            std::vector<Deferred> outerDeferred;
            outerDeferred.swap(deferred);
            inAngle = true;
            angleIndex = 0;
            std::vector<QString> angleNames;
            for (QDomElement angle = body.firstChildElement("mc-angle"); !angle.isNull(); angle = angle.nextSiblingElement("mc-angle"), ++angleIndex) {
                angleNames.push_back(angle.attribute("name"));
                storyline(angle, tcStart, v, a);
                const auto used = [&](const std::vector<Track>& list, int i) { return int(list.size()) > i && !list[size_t(i)].clips.empty(); };
                const QString angleName = angle.attribute("name");
                std::pair<int, int> tracks{-1, -1};
                if (used(nb.sequence().videoTracks, v)) {
                    if (!angleName.isEmpty()) nb.sequence().videoTracks[size_t(v)].name = angleName.toStdString();
                    tracks.first = v++;
                }
                if (used(nb.sequence().audioTracks, a)) {
                    if (!angleName.isEmpty()) nb.sequence().audioTracks[size_t(a)].name = angleName.toStdString();
                    tracks.second = a++;
                }
                ids[angle.attribute("angleID")] = tracks;
            }
            inAngle = wasInAngle;
            // The angles' connected sound, a track for each angle's lane after all the angles' own.
            std::map<std::pair<int, int>, int> trackFor;
            for (const Deferred& d : deferred) {
                auto [it, added] = trackFor.try_emplace({d.angle, d.lane}, a);
                if (added) {
                    const QString n = d.angle < int(angleNames.size()) ? angleNames[size_t(d.angle)] : QString();
                    nb.track(TrackKind::Audio, a).name = (n.isEmpty() ? QStringLiteral("Angle %1").arg(d.angle + 1) : n).toStdString() + " (connected)";
                    ++a;
                }
                Id ignore = 0;
                double none = 0;
                element(d.e, d.at, -1, it->second, ignore, none);
            }
            deferred.swap(outerDeferred);
        } else {
            storyline(body.firstChildElement("spine"), tcStart, 0, 0);
        }
        b = outer;
        fps = outerFps;
        lastPrimaryAudio = outerAudio;
        // Its picture and sound, before finish() adds empty tracks.
        bool video = false, audio = false;
        for (const Track& t : nb.sequence().videoTracks) video |= !t.clips.empty();
        for (const Track& t : nb.sequence().audioTracks) audio |= !t.clips.empty();
        nb.sequence().multicam = multicam;
        const Id active = p.activeSequence;
        ImportResult one = nb.finish();
        p.activeSequence = active;
        making.erase(ref);
        nestedResults.clips += one.clips;
        nestedResults.offline.insert(nestedResults.offline.end(), one.offline.begin(), one.offline.end());
        nestedResults.warnings.insert(nestedResults.warnings.end(), one.warnings.begin(), one.warnings.end());
        const Sequence* ns = p.findSequence(one.sequence);
        if (!ns) return 0;
        MediaItem item;
        item.id = p.newId();
        item.kind = MediaKind::Sequence;
        item.name = name.empty() ? ns->name : name;
        item.sequenceId = ns->id;
        item.hasVideo = video;
        item.hasAudio = audio;
        item.width = ns->width;
        item.height = ns->height;
        item.fps = ns->fps;
        item.duration = double(ns->duration()) / std::max(1e-9, ns->fpsValue());
        p.media.push_back(item);
        made[ref] = item.id;
        return item.id;
    }

    // Speed from a two-point timeMap (anything curvier: its average).
    static double speedOf(const QDomElement& e, bool& reverse) {
        reverse = false;
        const QDomElement tm = e.firstChildElement("timeMap");
        if (tm.isNull()) return 1;
        QDomElement first = tm.firstChildElement("timept"), last = first;
        for (QDomElement t = first; !t.isNull(); t = t.nextSiblingElement("timept")) last = t;
        const double dt = parseFcpTime(last.attribute("time")) - parseFcpTime(first.attribute("time"));
        const double dv = parseFcpTime(last.attribute("value")) - parseFcpTime(first.attribute("value"));
        if (dt <= 0) return 1;
        reverse = dv < 0;
        return std::max(0.01, std::fabs(dv / dt));
    }

    // An element at timeline seconds `at` (where its own time `start` shows).
    Id lastPrimaryAudio = 0;  // the storyline's sound, which its transitions cross-fade too

    void element(const QDomElement& e, double at, int vTrack, int aTrack, Id& lastPrimary, double& pendingDissolve) {
        const QString tag = e.tagName();
        const double start = parseFcpTime(e.attribute("start", "0s"));
        const double dur = parseFcpTime(e.attribute("duration", "0s"));
        const FrameTime s0 = frames(at), len = frames(dur);
        const std::string name = e.attribute("name").toStdString();
        Id placed = 0, placedAudio = 0;
        bool reverse = false;
        const double speed = speedOf(e, reverse);
        auto place = [&](Id mid, double origin, TrackKind kind, int track) -> Clip* {
            Clip* c = b->addClip(kind, track, mid, s0, len, (start - origin) * fps, name);
            if (c) {
                c->speed = speed;
                c->reverse = reverse;
                c->enabled = e.attribute("enabled", "1") != "0";
            }
            return c;
        };
        auto mediaClip = [&](const Asset& a, TrackKind kind, int track) -> Clip* {
            return place(b->media(a.path, a.name, a.video, a.audio, a.duration), a.start, kind, track);
        };
        auto link = [&](Id v, Id au) {
            if (!v || !au) return;
            const Id group = p.newId();
            edit::clipById(b->sequence(), v)->linkGroup = group;
            edit::clipById(b->sequence(), au)->linkGroup = group;
        };
        if (tag == "asset-clip" || tag == "video" || tag == "audio") {
            auto it = assets.find(e.attribute("ref"));
            if (it != assets.end()) {
                const Asset& a = it->second;
                Id v = 0, au = 0;
                if (tag != "audio" && a.video && vTrack >= 0)
                    if (Clip* c = mediaClip(a, TrackKind::Video, vTrack)) v = c->id;
                if ((tag == "audio" || (tag == "asset-clip" && a.audio)) && aTrack >= 0)
                    if (Clip* c = mediaClip(a, TrackKind::Audio, aTrack)) au = c->id;
                link(v, au);
                placed = v ? v : au;
                placedAudio = au;
            } else if (tag == "video" && effects.count(e.attribute("ref")) && vTrack >= 0) {
                // A generator (Final Cut's solids and the like).
                if (Clip* c = b->addGenerator(vTrack, "color", s0, len, name)) placed = c->id;
            }
        } else if (tag == "title" && vTrack >= 0) {
            if (Clip* c = b->addGenerator(vTrack, "title", s0, len, name)) {
                QString words;
                const QDomNodeList styles = e.firstChildElement("text").elementsByTagName("text-style");
                for (int i = 0; i < styles.size(); ++i) words += styles.at(i).toElement().text();
                c->generator.strings["text"] = words.toStdString();
                placed = c->id;
            }
        } else if (tag == "clip" || tag == "sync-clip") {
            // A container: its own story inside, shown from `start`.
            for (QDomElement k = e.firstChildElement(); !k.isNull(); k = k.nextSiblingElement()) {
                if (k.attribute("lane").toInt() != 0 || (k.tagName() != "video" && k.tagName() != "audio" && k.tagName() != "asset-clip"))
                    continue;
                const double kOffset = parseFcpTime(k.attribute("offset", "0s"));
                Id dummy = 0;
                double none = 0;
                element(k, at + (kOffset - start), k.tagName() == "audio" ? -1 : vTrack, aTrack, dummy, none);
            }
        } else if (tag == "ref-clip") {
            // A compound clip: a nested sequence, its picture and sound as srcEnable says.
            if (const Id mid = nestedMedia(e.attribute("ref"))) {
                const MediaItem* m = p.findMedia(mid);
                const Sequence* ns = m ? p.findSequence(m->sequenceId) : nullptr;
                const double origin = ns ? parseFcpTime(medias[e.attribute("ref")].firstChildElement("sequence").attribute("tcStart", "0s")) : 0;
                const QString enable = e.attribute("srcEnable", "all");
                Id v = 0, au = 0;
                if (m && m->hasVideo && enable != "audio" && vTrack >= 0)
                    if (Clip* c = place(mid, origin, TrackKind::Video, vTrack)) v = c->id;
                if (m && m->hasAudio && enable != "video" && aTrack >= 0)
                    if (Clip* c = place(mid, origin, TrackKind::Audio, aTrack)) au = c->id;
                link(v, au);
                placed = v ? v : au;
                placedAudio = au;
            } else {
                b->warn("A compound clip from Final Cut could not be read (" + name + ")");
            }
        } else if (tag == "mc-clip") {
            // A multicam clip: the angle seen and the angle heard (mc-source), else the first angle and all the sound.
            if (const Id mid = nestedMedia(e.attribute("ref"))) {
                const QString ref = e.attribute("ref");
                const double origin = parseFcpTime(medias[ref].firstChildElement("multicam").attribute("tcStart", "0s"));
                const auto& ids = angles[ref];
                int seen = -1, heard = -2;  // -2: no sound (none chosen, or the angle chosen has none)
                bool anySource = false, anyKnown = false;
                for (QDomElement src = e.firstChildElement("mc-source"); !src.isNull(); src = src.nextSiblingElement("mc-source")) {
                    anySource = true;
                    const auto it = ids.find(src.attribute("angleID"));
                    if (it == ids.end()) continue;
                    anyKnown = true;
                    const QString enable = src.attribute("srcEnable", "all");
                    if (enable != "audio" && it->second.first >= 0) seen = it->second.first;
                    if (enable != "video" && it->second.second >= 0) heard = it->second.second;
                }
                if (anySource && !anyKnown) b->warn("A multicam clip names angles its multicam does not have; the first angle is shown (" + name + ")");
                if (!anyKnown) {
                    seen = 0;
                    heard = -1;  // every source's sound
                }
                const MediaItem* m = p.findMedia(mid);
                Id v = 0, au = 0;
                if (m && m->hasVideo && seen >= 0 && vTrack >= 0)
                    if (Clip* c = place(mid, origin, TrackKind::Video, vTrack)) {
                        c->angle = seen;
                        v = c->id;
                    }
                if (m && m->hasAudio && heard > -2 && aTrack >= 0)
                    if (Clip* c = place(mid, origin, TrackKind::Audio, aTrack)) {
                        c->audioAngle = heard;
                        au = c->id;
                    }
                link(v, au);
                placed = v ? v : au;
                placedAudio = au;
            } else {
                b->warn("A multicam clip from Final Cut could not be read (" + name + ")");
            }
        }
        if (lane(e) == 0 && placed) {
            if (pendingDissolve > 0 && lastPrimary)
                b->addTransition(TrackKind::Video, std::max(0, vTrack), lastPrimary, placed, frames(pendingDissolve), {});
            if (pendingDissolve > 0 && lastPrimaryAudio && placedAudio)
                b->addTransition(TrackKind::Audio, std::max(0, aTrack), lastPrimaryAudio, placedAudio, frames(pendingDissolve), "crossfade");
            pendingDissolve = 0;
            lastPrimary = placed;
            lastPrimaryAudio = placedAudio;
        }
        // Connected clips, positioned in this element's own time.
        for (QDomElement k = e.firstChildElement(); !k.isNull(); k = k.nextSiblingElement()) {
            const int l = lane(k);
            if (l == 0) continue;
            const double kAt = at + (parseFcpTime(k.attribute("offset", "0s")) - start) / std::max(0.01, speed);
            if (inAngle) {
                if (l < 0) deferred.push_back({k, kAt, l, angleIndex});
                else b->warn("A picture connected inside a multicam angle was left out (" + k.attribute("name").toStdString() + ")");
                continue;
            }
            Id ignore = 0;
            double none = 0;
            // Lane n > 0 is V(n + 1); lane -n is A(n + 1) (A1 carries the storyline's own sound).
            element(k, kAt, l > 0 ? l : -1, l < 0 ? -l : -1, ignore, none);
        }
        for (const char* tag : {"marker", "chapter-marker"})
            for (QDomElement m = e.firstChildElement(tag); !m.isNull(); m = m.nextSiblingElement(tag)) {
                Marker mk;
                mk.t = frames(at + parseFcpTime(m.attribute("start")) - start);
                mk.name = m.attribute("value").toStdString();
                mk.comment = m.attribute("note").toStdString();
                mk.chapter = QLatin1String(tag) == QLatin1String("chapter-marker");
                b->sequence().markers.push_back(mk);
            }
    }
    static int lane(const QDomElement& e) { return e.attribute("lane", "0").toInt(); }
};

ImportResult importFcpx(Project& p, const QDomDocument& doc, const MediaProber& probe) {
    const QDomElement root = doc.documentElement();
    // The project's sequence (compound clips' sequences sit in the resources).
    QDomElement seq;
    const QDomNodeList all = root.elementsByTagName("sequence");
    for (int i = 0; i < all.size() && seq.isNull(); ++i)
        if (all.at(i).parentNode().toElement().tagName() == "project") seq = all.at(i).toElement();
    if (seq.isNull()) seq = all.at(0).toElement();
    if (seq.isNull()) {
        ImportResult r;
        r.error = "The FCPXML holds no project sequence";
        return r;
    }
    // The sequence's format gives the rate and frame size.
    const QDomElement res = root.firstChildElement("resources");
    QDomElement format;
    for (QDomElement f = res.firstChildElement("format"); !f.isNull(); f = f.nextSiblingElement("format"))
        if (f.attribute("id") == seq.attribute("format")) format = f;
    const Rational fps = fcpxRate(format);
    QString name = seq.parentNode().toElement().attribute("name");
    TimelineBuilder b(p, name.toStdString(), fps, probe);
    if (format.hasAttribute("width")) {
        b.sequence().width = std::max(16, format.attribute("width").toInt());
        b.sequence().height = std::max(16, format.attribute("height").toInt());
    }
    FcpxReader r(p, b, fps.toDouble(), probe);
    r.readResources(res);
    r.storyline(seq.firstChildElement("spine"), parseFcpTime(seq.attribute("tcStart", "0s")), 0, 0);
    for (const std::string& w : r.warnings) b.warn(w);
    for (const std::string& w : r.nestedResults.warnings) b.warn(w);
    ImportResult result = b.finish();
    result.clips += r.nestedResults.clips;
    result.offline.insert(result.offline.end(), r.nestedResults.offline.begin(), r.nestedResults.offline.end());
    return result;
}

}  // namespace

std::string exportFcpXml(const Project& p, const Sequence& sequence) {
    const Sequence s = interchangeSequence(sequence);
    FcpxWriter w(p, s);
    return withEncoding(w.run());
}

ImportResult importXmlTimeline(Project& p, const std::string& xml, const MediaProber& probe) {
    QDomDocument doc;
    QString err;
    int line = 0;
    if (!doc.setContent(QByteArray::fromStdString(xml), &err, &line)) {
        ImportResult r;
        r.error = "Not valid XML (line " + std::to_string(line) + "): " + err.toStdString();
        return r;
    }
    const QString root = doc.documentElement().tagName();
    if (root == "xmeml") return importFcp7(p, doc, probe);
    if (root == "fcpxml") return importFcpx(p, doc, probe);
    ImportResult r;
    r.error = "Not a Final Cut Pro XML file (root element <" + root.toStdString() + ">)";
    return r;
}

}  // namespace montage
