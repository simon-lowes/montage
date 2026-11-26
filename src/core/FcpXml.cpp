// Montage — Final Cut Pro XML both ways: FCP 7 XML (xmeml 5, also Premiere
// Pro's and Resolve's exchange format) and FCPXML 1.10 (Final Cut Pro X).
#include <QDomDocument>
#include <QRegularExpression>
#include <QFileInfo>
#include <QLocale>
#include <QUrl>
#include <QXmlStreamWriter>
#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <map>
#include <numeric>
#include <optional>
#include <set>

#include "Captions.h"
#include "EditOps.h"
#include "Effects.h"
#include "render/Titles.h"
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

// ---- What each clip does: its adjustments, their keyframes, and what Montage carries through unused ----------------
//
// FCPXML 1.10's published reference gives the adjustments' units: a transform's position and
// anchor and a crop's or trim's edges in percent of the frame's height (y upwards), its scale as factors, its rotation in
// degrees counter-clockwise; opacity 0..1; volume in dB; a stereo pan from -100 (left) to 100. 1.15 is not documented
// beyond its DTD; what Montage knows of it comes from comparing its DTD with 1.14's:
// keyframes' interp gains hold, bezier and passthrough, frame sampling gains vsa-motion-blur, the primitive video
// element gains adjust-natural-motion-blur, masks gain mask-semantic and every anchored item a flags attribute.

// A number as FCPXML writes it (no exponent for ordinary values, no "-0").
QString fnum(double v) {
    if (std::fabs(v) < 1e-12) v = 0;
    return QString::number(v, 'g', 12);
}

// "-6dB", "0.5", "10 -25": the numbers in a value.
std::vector<double> numbers(const QString& v) {
    std::vector<double> out;
    for (QString part : v.simplified().split(' ', Qt::SkipEmptyParts)) {
        if (part.endsWith("dB", Qt::CaseInsensitive)) part.chop(2);
        bool ok = false;
        const double d = part.toDouble(&ok);
        if (ok) out.push_back(d);
    }
    return out;
}

// Blend modes by FCPXML's numbers (its adjust-blend reference); the ones Montage has.
const std::vector<std::pair<int, const char*>>& fcpBlendModes() {
    static const std::vector<std::pair<int, const char*>> modes = {
        {0, "normal"},       {2, "subtract"},    {3, "darken"},      {4, "multiply"},    {5, "color_burn"},
        {8, "add"},          {9, "lighten"},     {10, "screen"},     {11, "color_dodge"}, {14, "overlay"},
        {15, "soft_light"},  {16, "hard_light"}, {20, "hard_mix"},   {22, "difference"}};
    return modes;
}

// Where an element goes among a clip element's children, as the DTD orders them (title: param, text, text-style-def,
// note; the others: note, timing, the video adjustments, the audio ones, mc-source, anchored items, markers, sources,
// filters, metadata). Montage writes its own children in these places and the carried ones between them.
int childRank(const QString& tag) {
    static const std::map<QString, int> ranks = {
        {"param", 0},           {"text", 1},                {"text-style-def", 2},      {"note", 3},
        {"conform-rate", 4},    {"timeMap", 5},             {"object-tracker", 6},      {"adjust-crop", 7},
        {"adjust-corners", 8},  {"adjust-conform", 9},      {"adjust-transform", 10},   {"adjust-blend", 11},
        {"adjust-stabilization", 12}, {"adjust-rollingShutter", 13}, {"adjust-360-transform", 14},
        {"adjust-reorient", 15}, {"adjust-orientation", 16}, {"adjust-cinematic", 17}, {"adjust-natural-motion-blur", 19},
        {"adjust-volume", 20},  {"adjust-panner", 21},      {"mc-source", 22},          {"marker", 24},
        {"chapter-marker", 24}, {"rating", 24},             {"keyword", 24},            {"analysis-marker", 24},
        {"audio-channel-source", 25}, {"audio-role-source", 25}, {"sync-source", 25}, {"filter-video", 26},
        {"filter-video-mask", 26}, {"filter-audio", 27},   {"metadata", 28},           {"reserved", 29}};
    if (auto it = ranks.find(tag); it != ranks.end()) return it->second;
    static const QStringList anchors = {"audio", "video", "clip", "title", "caption", "mc-clip", "ref-clip", "sync-clip", "asset-clip", "audition", "spine"};
    if (anchors.contains(tag)) return 23;
    if (tag.startsWith("adjust-")) return 18;  // a later version's adjustment: after the ones 1.10 knows
    return 30;
}

// What a clip element held that Montage does not use (Clip::foreign["fcpxml"]): its attributes, its elements, the
// effect resources its filters name, and a blend mode Montage has none of. Kept as a small document:
//   <carried blend="6"><attrs flags="1" videoRole="titles"/><res>effect...</res><el>elements...</el></carried>
struct Carried {
    QDomDocument doc;
    std::vector<std::pair<QString, QString>> attrs;
    std::vector<QDomElement> elements;
    std::map<QString, QDomElement> effects;  // the effect resources its filters name, by the id they had
    int blend = -1;
    bool empty() const { return attrs.empty() && elements.empty() && blend < 0; }
};

Carried parseCarried(const std::string& xml) {
    Carried c;
    if (xml.empty() || !c.doc.setContent(QString::fromStdString(xml))) return c;
    const QDomElement root = c.doc.documentElement();
    c.blend = root.attribute("blend", "-1").toInt();
    const QDomNamedNodeMap a = root.firstChildElement("attrs").attributes();
    for (int i = 0; i < a.count(); ++i) c.attrs.emplace_back(a.item(i).nodeName(), a.item(i).nodeValue());
    std::sort(c.attrs.begin(), c.attrs.end());
    for (QDomElement e = root.firstChildElement("res").firstChildElement(); !e.isNull(); e = e.nextSiblingElement())
        c.effects[e.attribute("id")] = e;
    for (QDomElement e = root.firstChildElement("el").firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) c.elements.push_back(e);
    return c;
}

// An element as it was, through a stream writer (`ref` attributes renamed by `refs`).
void writeDom(QXmlStreamWriter& w, const QDomElement& e, const std::map<QString, QString>& refs) {
    w.writeStartElement(e.tagName());
    const QDomNamedNodeMap a = e.attributes();
    std::vector<std::pair<QString, QString>> attrs;
    for (int i = 0; i < a.count(); ++i) attrs.emplace_back(a.item(i).nodeName(), a.item(i).nodeValue());
    std::sort(attrs.begin(), attrs.end());
    for (auto& [k, v] : attrs) {
        if (k == "ref")
            if (auto it = refs.find(v); it != refs.end()) v = it->second;
        w.writeAttribute(k, v);
    }
    for (QDomNode n = e.firstChild(); !n.isNull(); n = n.nextSibling()) {
        if (n.isElement()) writeDom(w, n.toElement(), refs);
        else if (n.isCDATASection()) w.writeCDATA(n.nodeValue());
        else if (n.isText() && !n.nodeValue().trimmed().isEmpty()) w.writeCharacters(n.nodeValue());
    }
    w.writeEndElement();
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

    int minor = 15;  // the version written, 1.<minor>: 10 or 15

    FcpxWriter(const Project& pr, const Sequence& sq, int versionMinor) : p(pr), s(sq), cur(&sq), w(&out), minor(versionMinor) {}
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
        // The effects carried filters name (the format's own, or a plug-in's), once each.
        for (const Sequence* seq : all)
            for (const auto* list : {&seq->videoTracks, &seq->audioTracks})
                for (const Track& tr : *list)
                    for (const Clip& c : tr.clips)
                        for (const auto& [id, e] : carried(c).effects) {
                            const QString key = effectKey(e);
                            if (carriedEffectIds.count(key)) continue;
                            const QString nid = QString("r%1").arg(nextRes++);
                            carriedEffectIds[key] = nid;
                            QDomElement copy = e.cloneNode(true).toElement();
                            copy.setAttribute("id", nid);
                            writeDom(w, copy, {});
                        }
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

    static bool motionBlurred(const Clip& c) {
        for (const Effect& e : c.effects)
            if (e.type == "motion_blur" && e.enabled) return true;
        return false;
    }
    void timeMap(const Clip& c) {
        const double speed = c.ramped() ? c.sourceExtent() / double(c.duration) : c.speed;
        if (speed == 1.0 && !c.reverse) return;
        w.writeStartElement("timeMap");
        // Frame sampling: Montage's Optical Flow with Motion Blur is 1.15's own sampling, AI frames its learned one.
        const int sampling = c.timing.empty() ? 0 : int(c.timing.p("sampling", 0));
        QString fs;
        if (sampling == 1) fs = "frame-blending";
        else if (sampling == 2) fs = minor >= 15 && motionBlurred(c) ? "vsa-motion-blur" : "optical-flow";
        else if (sampling == 3) fs = minor >= 15 ? "optical-flow-frc" : "optical-flow";
        if (!fs.isEmpty()) w.writeAttribute("frameSampling", fs);
        w.writeAttribute("preservesPitch", !c.timing.empty() && c.timing.p("maintain_pitch", 0) > 0.5 ? "1" : "0");
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

    // ---- Adjustments and their keyframes ----
    using ValueAt = std::function<QString(double)>;
    // An adjustment's attribute: the Montage parameters it is made from and its value at a clip frame.
    struct Attr {
        QString name;
        std::vector<const Param*> params;
        ValueAt value;
        QString def;
    };
    static const Param* param(const Effect& e, const char* n) {
        auto it = e.params.find(n);
        return it == e.params.end() ? nullptr : &it->second;
    }
    static bool keyed(const std::vector<const Param*>& ps) {
        return std::any_of(ps.begin(), ps.end(), [](const Param* q) { return q && q->animated(); });
    }
    static bool used(const std::vector<Attr>& attrs) {
        return std::any_of(attrs.begin(), attrs.end(), [](const Attr& a) { return keyed(a.params) || a.value(0) != a.def; });
    }
    QString interpName(Interp i) const {
        switch (i) {
            case Interp::Hold: return minor >= 15 ? "hold" : "linear";
            case Interp::Smooth: return "ease";
            case Interp::Bezier: return minor >= 15 ? "bezier" : "ease";
            case Interp::Linear: return "linear";
            default: break;
        }
        // The easing curves as FCPXML's three eases (each family's own shape is not in FCPXML).
        const int form = (int(i) - int(Interp::ExpoIn)) % 3;
        return form == 0 ? "easeIn" : form == 1 ? "easeOut" : "ease";
    }
    // A keyframe's time in the element's own time (sound keys between frames to the sample).
    QString keyTime(double frames) const {
        if (std::fabs(frames - std::round(frames)) < 1e-9) return t(std::round(frames));
        return fcpTime(frames * 48000.0 * double(cur->fps.den) / double(cur->fps.num), Rational{48000, 1});
    }
    // An attribute's keyframes, at the keys of every Montage parameter it is made from. `local0`: the element's time of
    // the clip's first frame.
    void keyframes(const Attr& a, double local0) {
        std::vector<std::pair<double, Interp>> times;
        for (const Param* q : a.params)
            if (q)
                for (const Keyframe& k : q->keys)
                    if (std::none_of(times.begin(), times.end(), [&](const auto& t) { return std::fabs(t.first - k.time()) < 1e-9; }))
                        times.emplace_back(k.time(), k.interp);
        std::sort(times.begin(), times.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        w.writeStartElement("param");
        w.writeAttribute("name", a.name);
        w.writeStartElement("keyframeAnimation");
        auto key = [&](double at, Interp interp) {
            w.writeEmptyElement("keyframe");
            w.writeAttribute("time", keyTime(local0 + at));
            w.writeAttribute("value", a.value(at));
            if (const QString i = interpName(interp); i != "linear") w.writeAttribute("interp", i);
        };
        for (size_t i = 0; i < times.size(); ++i) {
            key(times[i].first, times[i].second);
            // (1.10 has no hold: the value is kept until a frame before the next key.)
            if (times[i].second == Interp::Hold && minor < 15 && i + 1 < times.size() && times[i + 1].first - times[i].first > 1)
                key(times[i + 1].first - 1, Interp::Linear);
        }
        w.writeEndElement();
        w.writeEndElement();
    }
    // An adjustment element: each attribute where it differs from its default, keyed ones as params.
    void adjustment(const QString& tag, const std::vector<Attr>& attrs, double local0,
                    const std::vector<std::pair<QString, QString>>& extra = {}, const std::function<void()>& children = {}) {
        w.writeStartElement(tag);
        for (const auto& [k, v] : extra) w.writeAttribute(k, v);
        for (const Attr& a : attrs)
            if (!keyed(a.params))
                if (const QString v = a.value(0); v != a.def) w.writeAttribute(a.name, v);
        if (children) children();
        for (const Attr& a : attrs)
            if (keyed(a.params)) keyframes(a, local0);
        w.writeEndElement();
    }
    // The size of what a clip shows, in its media's pixels (the sequence's for generators and media of unknown size).
    void mediaSize(const Clip& c, double& mw, double& mh) const {
        mw = cur->width, mh = cur->height;
        if (const MediaItem* m = c.isGenerator() ? nullptr : p.findMedia(c.mediaId); m && m->width > 0 && m->height > 0)
            mw = m->width, mh = m->height;
    }
    static int fcpBlend(const std::string& mode) {
        for (const auto& [n, name] : fcpBlendModes())
            if (mode == name) return n;
        return 0;
    }
    // The picture's adjustments in DTD order (crop as FCPXML's trim, fit, transform, blend; 1.15's motion blur on a
    // primitive video element), each with the carried elements that go before it.
    void videoAdjustments(const Clip& c, double local0, bool primitive, const std::function<void(int)>& flush) {
        const Effect& m = c.motion;
        if (m.empty()) return;
        double mw = 1, mh = 1;
        mediaSize(c, mw, mh);
        const double H = std::max(1, cur->height);
        const int fit = int(m.p("fit", 0));
        const double aspect = fit == 2 ? double(cur->width) / H : mw / mh;  // the picture's width over its height as fitted
        auto P = [&](const char* n) { return param(m, n); };
        auto edge = [&](const char* n, const char* name, double k) {
            return Attr{name, {P(n)}, [&m, n, k](double f) { return fnum(m.pAt(n, f) * k); }, "0"};
        };
        const std::vector<Attr> trim = {edge("crop_left", "left", aspect), edge("crop_top", "top", 1), edge("crop_right", "right", aspect),
                                        edge("crop_bottom", "bottom", 1)};
        if (used(trim)) {
            flush(7);
            w.writeStartElement("adjust-crop");
            w.writeAttribute("mode", "trim");
            adjustment("trim-rect", trim, local0);
            w.writeEndElement();
        }
        if (fit == 1 || fit == 3) {
            flush(9);
            w.writeEmptyElement("adjust-conform");
            w.writeAttribute("type", fit == 1 ? "fill" : "none");
        }
        const std::vector<Attr> transform = {
            {"position", {P("pos_x"), P("pos_y")}, [&](double f) { return fnum(m.pAt("pos_x", f) / H * 100) + " " + fnum(-m.pAt("pos_y", f) / H * 100); }, "0 0"},
            {"scale", {P("scale"), P("scale_x"), P("scale_y"), P("flip_h"), P("flip_v")},
             [&](double f) {
                 const double s = m.pAt("scale", f, 100) / 100;
                 return fnum((m.pAt("flip_h", f) > 0.5 ? -s : s) * m.pAt("scale_x", f, 100) / 100) + " " +
                        fnum((m.pAt("flip_v", f) > 0.5 ? -s : s) * m.pAt("scale_y", f, 100) / 100);
             },
             "1 1"},
            {"rotation", {P("rotation")}, [&](double f) { return fnum(-m.pAt("rotation", f)); }, "0"},
            {"anchor", {P("anchor_x"), P("anchor_y")}, [&](double f) { return fnum(m.pAt("anchor_x", f) / mh * 100) + " " + fnum(-m.pAt("anchor_y", f) / mh * 100); }, "0 0"}};
        if (used(transform)) {
            flush(10);
            adjustment("adjust-transform", transform, local0);
        }
        const std::vector<Attr> blend = {{"amount", {P("opacity")}, [&](double f) { return fnum(m.pAt("opacity", f, 100) / 100); }, "1"}};
        const int mode = c.blendMode != "normal" ? fcpBlend(c.blendMode) : std::max(0, carried(c).blend);
        if (used(blend) || mode != 0) {
            flush(11);
            adjustment("adjust-blend", blend, local0, mode != 0 ? std::vector<std::pair<QString, QString>>{{"mode", QString::number(mode)}} : std::vector<std::pair<QString, QString>>{});
        }
        if (primitive && minor >= 15)
            for (const Effect& e : c.effects)
                if (e.type == "motion_blur") {
                    // (Its amount as a 180° shutter's: undocumented, so taken as Montage's own scale.)
                    flush(19);
                    w.writeEmptyElement("adjust-natural-motion-blur");
                    if (!e.enabled) w.writeAttribute("enabled", "0");
                    w.writeAttribute("amount", fnum(e.p("shutter", 0, 180) / 180));
                    break;
                }
    }
    static bool panned(const Clip& a) {
        const Param* pan = param(a.audio, "pan");
        return pan && (pan->animated() || pan->value != 0);
    }
    // The sound's volume and pan (the audio element takes no pan: a panned sound is written as an asset clip).
    void audioAdjustments(const Clip& a, double local0, bool panner, const std::function<void(int)>& flush) {
        if (a.audio.empty()) return;
        const Effect& v = a.audio;
        const std::vector<Attr> volume = {{"amount", {param(v, "gain_db")}, [&](double f) { return fnum(v.pAt("gain_db", f)) + "dB"; }, "0dB"}};
        if (used(volume)) {
            flush(20);
            adjustment("adjust-volume", volume, local0);
        }
        const std::vector<Attr> pan = {{"amount", {param(v, "pan")}, [&](double f) { return fnum(v.pAt("pan", f) * 100); }, "0"}};
        if (panner && used(pan)) {
            flush(21);
            adjustment("adjust-panner", pan, local0);
        }
    }
    // Montage's audio roles as FCPXML's (its three own in lower case).
    static QString fcpRole(const std::string& role) {
        QString r = q(role);
        const QString main = r.section('.', 0, 0);
        if (main == "Dialogue" || main == "Music" || main == "Effects") r = main.toLower() + r.mid(main.size());
        return r;
    }

    // The carried part of a clip (Clip::foreign["fcpxml"]), read once.
    std::map<Id, Carried> carriedCache;
    std::map<QString, QString> carriedEffectIds;  // a carried effect resource's uid (or name) -> its id here
    const Carried& carried(const Clip& c) {
        auto it = carriedCache.find(c.id);
        if (it == carriedCache.end()) {
            const auto f = c.foreign.find("fcpxml");
            it = carriedCache.emplace(c.id, parseCarried(f == c.foreign.end() ? std::string() : f->second)).first;
        }
        return it->second;
    }
    static QString effectKey(const QDomElement& e) { return e.attribute("uid").isEmpty() ? "name:" + e.attribute("name") : e.attribute("uid"); }
    // Carried elements a version can take (1.15's own are left out of 1.10).
    bool writable(const QDomElement& e) const {
        if (minor >= 15) return true;
        return e.tagName() != "adjust-natural-motion-blur" && e.elementsByTagName("mask-semantic").isEmpty();
    }

    // Captions on a storyline element, in its time.
    struct CaptionAt {
        const CaptionTrack* track;
        const Caption* caption;
        int lane;
        double local;
    };
    int captionStyles = 0;
    void captionElement(const CaptionAt& k) {
        const Caption& c = *k.caption;
        const CaptionTrack& tr = *k.track;
        QString name = q(tr.name);
        name.remove('.').remove('?');
        if (name.isEmpty() || name == "_") name = "Captions";
        const QString format = tr.format.empty() ? QString("ITT") : q(tr.format);
        const QString id = QString("tsc%1").arg(++captionStyles);
        w.writeStartElement("caption");
        w.writeAttribute("lane", QString::number(k.lane));
        w.writeAttribute("offset", t(k.local));
        w.writeAttribute("name", q(c.text).section('\n', 0, 0));
        w.writeAttribute("duration", t(double(c.end - c.start)));
        w.writeAttribute("role", QString("%1?captionFormat=%2.%3").arg(name, format, tr.language.empty() ? QString("en") : q(tr.language)));
        w.writeStartElement("text");
        if (format == "ITT" && c.vertical == kCaptionTop) w.writeAttribute("placement", "top");
        w.writeStartElement("text-style");
        w.writeAttribute("ref", id);
        w.writeCharacters(q(c.text));
        w.writeEndElement();
        w.writeEndElement();
        w.writeStartElement("text-style-def");
        w.writeAttribute("id", id);
        w.writeEmptyElement("text-style");
        w.writeAttribute("font", q(tr.style.font));
        int r = 0, g = 0, b = 0;
        if (captionColour(c, r, g, b)) w.writeAttribute("fontColor", QString("%1 %2 %3 1").arg(fnum(r / 255.0), fnum(g / 255.0), fnum(b / 255.0)));
        if (tr.style.bold) w.writeAttribute("bold", "1");
        if (c.italic) w.writeAttribute("italic", "1");
        w.writeEndElement();
        w.writeEndElement();
    }

    // A clip connected to (or inside) a storyline element, in that element's time.
    struct Connected {
        const Clip* clip;
        double offset;
        int lane;
        double scale = 1;  // the element's time a timeline frame (its speed)
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
              const Clip* audio = nullptr, bool soundOnly = false, double scale = 1, const std::vector<CaptionAt>& captions = {}) {
        const Sequence* inner = nestedOf(c);
        const bool audioOnly = lane < 0 || soundOnly;
        // The sound whose volume, pan and role the element carries.
        const Clip* sound = audioOnly ? &c : withAudio ? audio : nullptr;
        bool primitive = false, panner = true, mc = false;
        QString roleAttr;
        if (c.isGenerator()) {
            const bool title = c.generator.type == "title";
            w.writeStartElement(title ? "title" : "video");
            w.writeAttribute("ref", title ? titleId : solidId);
            primitive = !title;
        } else if (inner && nested.count(inner->id) && inner->multicam) {
            w.writeStartElement("mc-clip");
            w.writeAttribute("ref", nested[inner->id]);
            mc = true;
        } else if (inner && nested.count(inner->id)) {
            w.writeStartElement("ref-clip");
            w.writeAttribute("ref", nested[inner->id]);
            if (audioOnly) w.writeAttribute("srcEnable", "audio");
            else if (!withAudio) w.writeAttribute("srcEnable", "video");
        } else {
            const bool asAsset = withAudio || (audioOnly && panned(c));
            w.writeStartElement(asAsset ? "asset-clip" : audioOnly ? "audio" : "video");
            w.writeAttribute("ref", assets[c.mediaId]);
            if (asAsset && audioOnly) w.writeAttribute("srcEnable", "audio");
            primitive = !asAsset && !audioOnly;
            panner = asAsset;
            roleAttr = asAsset ? "audioRole" : audioOnly ? "role" : QString();
        }
        if (lane != 0) w.writeAttribute("lane", QString::number(lane));
        // Sound placed to the sample (its head and tail, parts of its first and last frames left silent), connected at
        // its own speed, or in a storyline of sound alone: its times in samples, as Final Cut Pro edits sound on its own.
        // (In a storyline with pictures, or retimed, it keeps whole frames, its neighbours and time map in frames too.)
        const bool sampled = (lane < 0 || storylineSamples) && !c.isGenerator() && !inner && c.speed == 1.0 && !c.reverse && !c.ramped();
        const double head = sampled ? c.head : 0, tail = sampled ? c.tail : 0;
        auto ts = [&](double frames) {
            if (head == 0 && tail == 0) return t(frames);
            const int rate = cur->sampleRate > 0 ? cur->sampleRate : 48000;
            return fcpTime(frames * double(rate) * cur->fps.den / cur->fps.num, Rational{rate, 1});
        };
        w.writeAttribute("offset", ts(offset + head * scale));
        w.writeAttribute("name", q(c.name));
        w.writeAttribute("start", c.isGenerator() ? "0s" : ts(c.sourceIn + head));
        w.writeAttribute("duration", ts(double(c.duration) - head - tail));
        if (!c.enabled) w.writeAttribute("enabled", "0");
        if (!roleAttr.isEmpty() && sound && !sound->role.empty()) w.writeAttribute(roleAttr, fcpRole(sound->role));
        // What the clip (and its sound) brought from FCPXML and Montage does not use, written back where it was.
        std::vector<QDomElement> extra;
        std::map<QString, QString> refs;
        std::set<QString> attrsWritten;
        std::vector<const Clip*> from{&c};
        if (sound && sound != &c) from.push_back(sound);
        for (const Clip* f : from) {
            const Carried& k = carried(*f);
            for (const auto& [name, value] : k.attrs)
                if (!attrsWritten.count(name) && (minor >= 15 || name != "flags")) {
                    w.writeAttribute(name, value);
                    attrsWritten.insert(name);
                }
            for (const QDomElement& e : k.elements)
                if (writable(e)) extra.push_back(e);
            for (const auto& [id, e] : k.effects)
                if (auto it = carriedEffectIds.find(effectKey(e)); it != carriedEffectIds.end()) refs[id] = it->second;
        }
        std::stable_sort(extra.begin(), extra.end(), [](const QDomElement& x, const QDomElement& y) { return childRank(x.tagName()) < childRank(y.tagName()); });
        size_t next = 0;
        auto flush = [&](int before) {
            for (; next < extra.size() && childRank(extra[next].tagName()) < before; ++next) writeDom(w, extra[next], refs);
        };
        const double local0 = c.isGenerator() ? 0 : c.sourceIn;
        if (c.isGenerator() && c.generator.type == "title") {
            flush(1);
            // The text in stretches of one style each (its styled runs, render/Titles.h), each with its style's def.
            // (Spaces alone go with the stretch before them: a reader may drop a text-style holding only white space.)
            std::vector<TitleStyleSpan> spans;
            for (TitleStyleSpan& sp : titleStyleSpans(c.generator)) {
                if (!spans.empty() && sp.text.trimmed().isEmpty()) spans.back().text += sp.text;
                else if (!spans.empty() && spans.back().text.trimmed().isEmpty()) spans.back() = {spans.back().text + sp.text, sp.style};
                else spans.push_back(std::move(sp));
            }
            std::vector<TextRun> defs;
            w.writeStartElement("text");
            for (const TitleStyleSpan& sp : spans) {
                size_t k = size_t(std::find(defs.begin(), defs.end(), sp.style) - defs.begin());
                if (k == defs.size()) defs.push_back(sp.style);
                w.writeStartElement("text-style");
                w.writeAttribute("ref", k ? QString("ts%1_%2").arg(c.id).arg(k) : QString("ts%1").arg(c.id));
                w.writeCharacters(sp.text);
                w.writeEndElement();
            }
            w.writeEndElement();
            for (size_t k = 0; k < defs.size(); ++k) {
                const TextRun& st = defs[k];
                auto num = [](double v) { return QString::number(v, 'g', QLocale::FloatingPointShortest); };
                auto rgba = [&](const std::array<double, 3>& v) { return QString("%1 %2 %3 1").arg(num(v[0]), num(v[1]), num(v[2])); };
                w.writeStartElement("text-style-def");
                w.writeAttribute("id", k ? QString("ts%1_%2").arg(c.id).arg(k) : QString("ts%1").arg(c.id));
                w.writeEmptyElement("text-style");
                w.writeAttribute("font", q(st.font));
                w.writeAttribute("fontSize", num(st.size * 63.0 / 96.0));  // a title's 96 px as Final Cut's 63
                w.writeAttribute("fontColor", rgba(st.color.value_or(std::array<double, 3>{1, 1, 1})));
                w.writeAttribute("bold", st.bold > 0 ? "1" : "0");
                w.writeAttribute("italic", st.italic > 0 ? "1" : "0");
                if (st.outline > 0) {
                    w.writeAttribute("strokeColor", rgba(st.outlineColor.value_or(std::array<double, 3>{0, 0, 0})));
                    w.writeAttribute("strokeWidth", num(st.outline));
                }
                if (st.tracking && *st.tracking != 0) w.writeAttribute("kerning", num(*st.tracking));
                if (st.baseline != 0) w.writeAttribute("baseline", num(st.baseline));
                w.writeEndElement();
            }
        }
        if (!c.isGenerator()) {
            flush(5);
            timeMap(c);  // (before mc-source, as the DTD orders them)
        }
        // A multicam clip's picture adjustments go in the angle it shows (its mc-source), its sound's on the clip.
        if (!audioOnly && !mc) videoAdjustments(c, local0, primitive, flush);
        if (sound) audioAdjustments(*sound, local0, panner, flush);
        if (inner && inner->multicam && nested.count(inner->id)) {
            flush(22);
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
            auto source = [&](int angle, const char* enable, bool picture) {
                w.writeStartElement("mc-source");
                w.writeAttribute("angleID", angleId(angle));
                w.writeAttribute("srcEnable", enable);
                if (picture) videoAdjustments(c, local0, false, [](int) {});
                w.writeEndElement();
            };
            if (!audioOnly && heard == video) {
                source(video, "all", true);
            } else {
                if (!audioOnly) source(video, "video", true);
                if (heard >= 0) source(heard, "audio", false);
            }
        }
        flush(23);
        for (const Connected& k : connected) clip(*k.clip, k.lane, k.offset, false, {}, nullptr, false, k.scale);
        for (const CaptionAt& k : captions) captionElement(k);
        flush(24);
        for (const Marker& m : markersIn(c.start, c.end()))
            marker(m, (c.isGenerator() ? 0 : c.sourceIn) + double(m.t - c.start) * c.speed);
        // Clip markers are already in the clip's own (source) time.
        for (const Marker& m : c.markers)
            if (c.markerFrame(m) >= 0) markerElement(m, double(m.t));
        flush(1000);
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

    bool storylineSamples = false;  // writing an angle of sound alone (its clips' times in samples)

    // A multicam clip's angles, each a storyline of its own.
    void multicamBody(const Sequence& mc) {
        const Sequence* was = cur;
        cur = &mc;
        w.writeStartElement("multicam");
        w.writeAttribute("format", formatFor(mc));
        w.writeAttribute("tcStart", "0s");
        w.writeAttribute("tcFormat", isDropFrameRate(mc.fps) ? "DF" : "NDF");
        auto storyline = [&](const Track& tr, const Track* sound) {
            // An angle of sound alone keeps its clips' heads and tails: their times in samples, as clip() writes them,
            // and the gaps between running exactly from one to the next (in units of a sample and of a frame both).
            const bool samples = tr.kind == TrackKind::Audio;
            const long long rate = mc.sampleRate > 0 ? mc.sampleRate : 48000, perFrame = mc.fps.den * rate, perSample = mc.fps.num;
            const long long unit = rate * mc.fps.num;  // a second
            auto at = [&](long long u) {
                if (u == 0) return QString("0s");
                const long long g = std::gcd(u, unit);
                return unit / g == 1 ? QString("%1s").arg(u / g) : QString("%1/%2s").arg(u / g).arg(unit / g);
            };
            auto sampleUnits = [&](double frames) { return std::llround(frames * double(rate) * mc.fps.den / mc.fps.num) * perSample; };
            long long cursor = 0;
            for (const Clip& c : tr.clips) {
                // (As clip() places it: retimed sound keeps whole frames, and so does sound with no head or tail.)
                const bool keep = samples && !c.isGenerator() && !nestedOf(c) && c.speed == 1.0 && !c.reverse && !c.ramped() &&
                                  (c.head != 0 || c.tail != 0);
                const long long from = keep ? sampleUnits(double(c.start) + c.head) : c.start * perFrame;
                if (from > cursor) {
                    w.writeEmptyElement("gap");
                    w.writeAttribute("name", "Gap");
                    w.writeAttribute("offset", at(cursor));
                    w.writeAttribute("start", "0s");
                    w.writeAttribute("duration", at(from - cursor));
                }
                bool carried = false;
                if (sound)
                    for (const Clip& a : sound->clips) carried |= a.mediaId == c.mediaId && a.start == c.start && a.duration == c.duration;
                storylineSamples = samples;
                clip(c, 0, double(c.start), carried, {}, nullptr, samples);
                storylineSamples = false;
                cursor = from + (keep ? sampleUnits(double(c.duration) - c.head - c.tail) : c.duration * perFrame);
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
            std::vector<CaptionAt> captions;
        };
        auto gap = [](FrameTime at, FrameTime length) {
            Element g;
            g.offset = at, g.duration = length;
            return g;
        };
        std::vector<Element> spine;
        std::set<Id> riding;
        FrameTime cursor = 0;
        for (const Clip& c : v1.clips) {
            if (c.start > cursor) spine.push_back(gap(cursor, c.start - cursor));
            Element e = gap(c.start, c.duration);
            e.clip = &c;
            if (!c.isGenerator())
                for (Id other : edit::linkedClips(seq, c.id))
                    if (auto loc = edit::locate(seq, other); loc && loc->track.kind == TrackKind::Audio && loc->track.index == 0) {
                        const Clip& a = seq.audioTracks[0].clips[loc->index];
                        // (Not sound placed to the sample: it is connected, with its times in samples.)
                        if (a.mediaId == c.mediaId && a.start == c.start && a.duration == c.duration && a.head == 0 && a.tail == 0) {
                            e.withAudio = true;
                            e.audio = &a;
                            riding.insert(a.id);
                        }
                    }
            spine.push_back(e);
            cursor = c.end();
        }
        // Everything else connects to the spine element under its start, in that element's own time: or, read from
        // FCPXML, to the clip it was connected to there while that clip is still in the primary storyline.
        auto localIn = [](const Element& e, FrameTime at) {
            return e.clip && !e.clip->isGenerator() ? e.clip->sourceIn + double(at - e.offset) * e.clip->speed : double(at - e.offset);
        };
        auto under = [&](FrameTime start, FrameTime finish) -> Element* {
            const FrameTime end = spine.empty() ? 0 : spine.back().offset + spine.back().duration;
            if (start >= end) spine.push_back(gap(end, std::max<FrameTime>(1, finish - end)));
            for (Element& e : spine)
                if (start >= e.offset && start < e.offset + e.duration) return &e;
            return nullptr;
        };
        auto attach = [&](const Clip& c, int lane) {
            Element* to = nullptr;
            if (auto f = c.foreign.find("fcpxml-parent"); f != c.foreign.end())
                for (Element& e : spine)
                    if (e.clip && std::to_string(e.clip->id) == f->second) to = &e;
            if (!to) to = under(c.start, c.end());
            if (to) to->connected.push_back({&c, localIn(*to, c.start), lane, to->clip && !to->clip->isGenerator() ? to->clip->speed : 1.0});
        };
        for (size_t vi = 1; vi < seq.videoTracks.size(); ++vi)
            for (const Clip& c : seq.videoTracks[vi].clips) attach(c, int(vi));
        for (size_t ai = 0; ai < seq.audioTracks.size(); ++ai)
            for (const Clip& c : seq.audioTracks[ai].clips)
                if (!riding.count(c.id)) attach(c, -std::max(1, int(ai)));  // lane -n reads back as A(n + 1)
        // Caption tracks in lanes above the pictures.
        for (size_t ti = 0; ti < seq.captionTracks.size(); ++ti)
            for (const Caption& cap : seq.captionTracks[ti].captions)
                if (cap.end > cap.start)
                    if (Element* e = under(cap.start, cap.end))
                        e->captions.push_back({&seq.captionTracks[ti], &cap, int(std::max<size_t>(1, seq.videoTracks.size()) + ti), localIn(*e, cap.start)});

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
                clip(*e.clip, 0, double(e.offset), e.withAudio, e.connected, e.audio, false, 1, e.captions);
            } else {
                w.writeStartElement("gap");
                w.writeAttribute("name", "Gap");
                w.writeAttribute("offset", t(double(e.offset)));
                w.writeAttribute("start", "0s");
                w.writeAttribute("duration", t(double(e.duration)));
                for (const Connected& k : e.connected) clip(*k.clip, k.lane, k.offset, false, {}, nullptr, false, k.scale);
                for (const CaptionAt& k : e.captions) captionElement(k);
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
        w.writeAttribute("version", QString("1.%1").arg(minor));
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
                effectEls[e.attribute("id")] = e;
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

    // ---- What each clip does ----
    std::map<QString, QDomElement> effectEls;  // effect resources by id (carried filters name them)
    std::set<std::string> said;                // warnings given once
    void warnOnce(const std::string& w) {
        if (said.insert(w).second) b->warn(w);
    }
    struct Anim {
        std::vector<double> value;
        struct Key {
            double t;  // clip frames
            std::vector<double> v;
            Interp interp;
        };
        std::vector<Key> keys;
    };
    Interp interpFrom(const QString& v, const std::string& clipName) {
        if (v == "ease") return Interp::Smooth;
        if (v == "easeIn") return Interp::SineIn;
        if (v == "easeOut") return Interp::SineOut;
        if (v == "hold") return Interp::Hold;      // 1.15
        if (v == "bezier") return Interp::Bezier;  // 1.15 (its handles are not in the file: Montage's automatic ones)
        if (v != "linear" && !v.isEmpty())
            warnOnce("Keyframes interpolated as \"" + v.toStdString() + "\" were read as linear (" + clipName + ")");
        return Interp::Linear;
    }
    // An adjustment's attribute: its value (the attribute, or its param's) and its keys, at clip frames (`start` the
    // element's own time of its first frame; `head` the silent part of a sound's first frame).
    Anim anim(const QDomElement& adj, const QString& name, const QString& def, double start, double head, const std::string& clipName) {
        Anim a;
        a.value = numbers(adj.attribute(name, def));
        for (QDomElement pe = adj.firstChildElement("param"); !pe.isNull(); pe = pe.nextSiblingElement("param")) {
            if (pe.attribute("name") != name) continue;
            if (pe.hasAttribute("value")) a.value = numbers(pe.attribute("value"));
            for (QDomElement k = pe.firstChildElement("keyframeAnimation").firstChildElement("keyframe"); !k.isNull(); k = k.nextSiblingElement("keyframe")) {
                double t = (parseFcpTime(k.attribute("time")) - start) * fps + head;
                if (std::fabs(t - std::round(t)) < 1e-6) t = std::round(t);
                a.keys.push_back({t, numbers(k.attribute("value")), interpFrom(k.attribute("interp", "linear"), clipName)});
            }
        }
        if (a.value.empty()) a.value = numbers(def);
        return a;
    }
    static void setParam(Effect& e, const std::string& name, const Anim& a, size_t dim, const std::function<double(double)>& f) {
        Param& q = e.params[name];
        q.keys.clear();
        if (dim < a.value.size()) q.value = f(a.value[dim]);
        for (const Anim::Key& k : a.keys)
            if (dim < k.v.size()) q.addKeyAt(k.t, f(k.v[dim]), k.interp);
        if (!q.keys.empty()) q.value = q.keys.front().v;
    }
    static void scaleParam(Param& q, double k) {
        q.value *= k;
        for (Keyframe& key : q.keys) key.v *= k, key.inDv *= k, key.outDv *= k;
    }
    static void offsetParam(Param& q, double d) {
        q.value += d;
        for (Keyframe& key : q.keys) key.v += d;
    }
    static bool addMotionBlur(Project& p, Clip& c, double shutter, bool enabled) {
        for (Effect& e : c.effects)
            if (e.type == "motion_blur") return false;
        Effect e = makeEffect(p, "motion_blur");
        e.params["shutter"].value = std::clamp(shutter, 0.0, 720.0);
        e.enabled = enabled;
        c.effects.push_back(std::move(e));
        return true;
    }

    // The picture's adjustments (`e` the clip element, or a multicam clip's angle shown, its mc-source).
    void readVideoAdjustments(const QDomElement& e, Id id, double start, const std::string& name, int& blendCarried) {
        Clip* c = edit::clipById(b->sequence(), id);
        if (!c || c->motion.empty()) return;
        Effect& m = c->motion;
        const double W = b->sequence().width, H = std::max(1, b->sequence().height);
        double mw = W, mh = H;
        if (const MediaItem* mi = c->isGenerator() ? nullptr : p.findMedia(c->mediaId); mi && mi->width > 0 && mi->height > 0) mw = mi->width, mh = mi->height;
        if (const QDomElement conform = e.firstChildElement("adjust-conform"); !conform.isNull()) {
            const QString type = conform.attribute("type", "fit");
            m.params["fit"].value = type == "fill" ? 1 : type == "none" ? 3 : 0;
        }
        const int fit = int(m.p("fit", 0));
        const double aspect = fit == 2 ? W / H : mw / mh;
        const double fitScale = fit == 1 ? std::max(W / mw, H / mh) : fit == 2 ? H / mh : fit == 3 ? 1.0 : std::min(W / mw, H / mh);
        const double fittedH = mh * fitScale;
        auto on = [](const QDomElement& x) { return !x.isNull() && x.attribute("enabled", "1") != "0"; };
        if (const QDomElement tr = e.firstChildElement("adjust-transform"); on(tr)) {
            const Anim pos = anim(tr, "position", "0 0", start, 0, name);
            setParam(m, "pos_x", pos, 0, [&](double v) { return v * H / 100; });
            setParam(m, "pos_y", pos, 1, [&](double v) { return -v * H / 100; });
            const Anim sc = anim(tr, "scale", "1 1", start, 0, name);
            auto even = [](const std::vector<double>& v) { return v.size() < 2 || std::fabs(std::fabs(v[0]) - std::fabs(v[1])) < 1e-12; };
            bool uniform = even(sc.value);
            for (const Anim::Key& k : sc.keys) uniform &= even(k.v);
            const std::vector<double>& first = sc.keys.empty() ? sc.value : sc.keys.front().v;
            m.params["flip_h"].value = !first.empty() && first[0] < 0 ? 1 : 0;
            m.params["flip_v"].value = first.size() > 1 && first[1] < 0 ? 1 : 0;
            auto pct = [](double v) { return std::fabs(v) * 100; };
            if (uniform) {
                setParam(m, "scale", sc, 0, pct);
                m.params["scale_x"] = Param(100);
                m.params["scale_y"] = Param(100);
            } else {
                m.params["scale"] = Param(100);
                setParam(m, "scale_x", sc, 0, pct);
                setParam(m, "scale_y", sc, 1, pct);
            }
            setParam(m, "rotation", anim(tr, "rotation", "0", start, 0, name), 0, [](double v) { return -v; });
            const Anim an = anim(tr, "anchor", "0 0", start, 0, name);
            setParam(m, "anchor_x", an, 0, [&](double v) { return v * mh / 100; });
            setParam(m, "anchor_y", an, 1, [&](double v) { return -v * mh / 100; });
        } else if (!tr.isNull()) {
            warnOnce("A turned-off transform was left out (" + name + ")");
        }
        // A crop: FCPXML's trim is Montage's crop; its crop (the rest scaled up to fill the frame) and its pan and zoom
        // (from one such rectangle to another over the clip) are Montage's scale and position.
        auto fill = [&](double l, double t, double r, double bm, double& k, double& dx, double& dy) {
            const double rw = 1 - (l + r) / (100 * aspect), rh = 1 - (t + bm) / 100;
            k = 1 / std::max(0.01, std::max(rw, rh));
            dx = (l - r) / 200 * fittedH;  // the rectangle's centre from the picture's, in sequence pixels
            dy = (t - bm) / 200 * fittedH;
        };
        if (const QDomElement cr = e.firstChildElement("adjust-crop"); on(cr)) {
            const QString mode = cr.attribute("mode", "trim");
            auto edges = [&](const QDomElement& rect) {
                setParam(m, "crop_left", anim(rect, "left", "0", start, 0, name), 0, [&](double v) { return v / aspect; });
                setParam(m, "crop_top", anim(rect, "top", "0", start, 0, name), 0, [](double v) { return v; });
                setParam(m, "crop_right", anim(rect, "right", "0", start, 0, name), 0, [&](double v) { return v / aspect; });
                setParam(m, "crop_bottom", anim(rect, "bottom", "0", start, 0, name), 0, [](double v) { return v; });
            };
            const double s0 = m.p("scale", 0, 100) / 100;
            if (mode == "trim") {
                edges(cr.firstChildElement("trim-rect"));
            } else if (mode == "crop") {
                const QDomElement rect = cr.firstChildElement("crop-rect");
                edges(rect);
                double k = 1, dx = 0, dy = 0;
                fill(rect.attribute("left").toDouble(), rect.attribute("top").toDouble(), rect.attribute("right").toDouble(),
                     rect.attribute("bottom").toDouble(), k, dx, dy);
                scaleParam(m.params["scale"], k);
                offsetParam(m.params["pos_x"], -dx * k * s0);
                offsetParam(m.params["pos_y"], -dy * k * s0);
            } else if (mode == "pan") {
                const QDomElement from = cr.firstChildElement("pan-rect"), to = from.nextSiblingElement("pan-rect");
                if (!from.isNull() && !to.isNull()) {
                    const double last = double(std::max<FrameTime>(1, c->duration - 1));
                    const double x0 = m.p("pos_x", 0), y0 = m.p("pos_y", 0);
                    Param& sp = m.params["scale"];
                    Param& px = m.params["pos_x"];
                    Param& py = m.params["pos_y"];
                    sp.keys.clear(), px.keys.clear(), py.keys.clear();
                    for (const auto& [rect, at] : {std::pair{from, 0.0}, std::pair{to, last}}) {
                        double k = 1, dx = 0, dy = 0;
                        fill(rect.attribute("left").toDouble(), rect.attribute("top").toDouble(), rect.attribute("right").toDouble(),
                             rect.attribute("bottom").toDouble(), k, dx, dy);
                        sp.addKeyAt(at, s0 * k * 100, Interp::Smooth);
                        px.addKeyAt(at, x0 - dx * k * s0, Interp::Smooth);
                        py.addKeyAt(at, y0 - dy * k * s0, Interp::Smooth);
                    }
                }
            }
        }
        if (const QDomElement bl = e.firstChildElement("adjust-blend"); !bl.isNull()) {
            setParam(m, "opacity", anim(bl, "amount", "1", start, 0, name), 0, [](double v) { return v * 100; });
            if (bl.hasAttribute("mode")) {
                // (Parsers scan for the number only, the format's reference says: "14 (Overlay)".)
                const int mode = numbers(bl.attribute("mode").section('(', 0, 0)).empty() ? 0 : int(numbers(bl.attribute("mode").section('(', 0, 0))[0]);
                std::string blend;
                for (const auto& [n, nm] : fcpBlendModes())
                    if (n == mode) blend = nm;
                if (!blend.empty()) {
                    c->blendMode = blend;
                } else {
                    blendCarried = mode;
                    warnOnce("Blend mode " + std::to_string(mode) + " is not one of Montage's; the clip blends normally (" + name + ")");
                }
            }
        }
        // 1.15's natural motion blur (its amount taken as a 180° shutter's: the format does not document it).
        if (const QDomElement nb = e.firstChildElement("adjust-natural-motion-blur"); !nb.isNull())
            addMotionBlur(p, *c, 180 * (nb.hasAttribute("amount") ? nb.attribute("amount").toDouble() : 1.0), nb.attribute("enabled", "1") != "0");
    }

    void readAudioAdjustments(const QDomElement& e, Id id, double start, const std::string& name) {
        Clip* c = edit::clipById(b->sequence(), id);
        if (!c || c->audio.empty()) return;
        if (const QDomElement v = e.firstChildElement("adjust-volume"); !v.isNull())
            setParam(c->audio, "gain_db", anim(v, "amount", "0dB", start, c->head, name), 0, [](double x) { return x; });
        if (const QDomElement pn = e.firstChildElement("adjust-panner"); !pn.isNull()) {
            const QString mode = pn.attribute("mode", "0").section('(', 0, 0).trimmed();
            if (mode != "0" && mode != "1") warnOnce("A surround pan mode (" + mode.toStdString() + ") was read as a stereo pan (" + name + ")");
            setParam(c->audio, "pan", anim(pn, "amount", "0", start, c->head, name), 0, [](double x) { return std::clamp(x / 100, -1.0, 1.0); });
        }
    }

    // Frame sampling and pitch from the time map.
    void readTiming(const QDomElement& e, Id video, Id audio, const std::string& name) {
        const QDomElement tm = e.firstChildElement("timeMap");
        if (tm.isNull()) return;
        const QString fs = tm.attribute("frameSampling", "floor");
        int sampling = 0;
        if (fs == "frame-blending") sampling = 1;
        else if (fs == "optical-flow" || fs == "optical-flow-classic" || fs == "vsa-motion-blur") sampling = 2;
        else if (fs == "optical-flow-frc") sampling = 3;
        else if (fs != "floor" && fs != "nearest-neighbor") warnOnce("Frame sampling \"" + fs.toStdString() + "\" was read as nearest frame (" + name + ")");
        for (Id id : {video, audio}) {
            Clip* c = id ? edit::clipById(b->sequence(), id) : nullptr;
            if (!c) continue;
            if (c->timing.empty()) c->timing = makeEffect(p, "time");
            c->timing.params["sampling"].value = sampling;
            if (tm.hasAttribute("preservesPitch")) c->timing.params["maintain_pitch"].value = tm.attribute("preservesPitch") == "1" ? 1 : 0;
            // 1.15's sampling with motion blur: optical flow and Montage's Motion Blur.
            if (fs == "vsa-motion-blur" && id == video) addMotionBlur(p, *c, 180, true);
        }
    }

    // FCPXML's roles as Montage's: its own three capitalised; a subrole kept unless it is the one made by default
    // ("dialogue-1").
    static std::string montageRole(const QString& fcp) {
        QString main = fcp.section('.', 0, 0), sub = fcp.section('.', 1);
        if (main == "dialogue" || main == "music" || main == "effects") main[0] = main[0].toUpper();
        if (sub.isEmpty() || QRegularExpression("^" + QRegularExpression::escape(main) + "-\\d+$", QRegularExpression::CaseInsensitiveOption).match(sub).hasMatch())
            return main.toStdString();
        return (main + "." + sub).toStdString();
    }

    // A caption element: a caption on the track its role names (made the first time: "ITT Subtitles?captionFormat=ITT.en"
    // is a track "ITT Subtitles" for ITT, in English).
    void readCaption(const QDomElement& k, double at) {
        const QString role = k.attribute("role");
        const QString main = role.section('.', 0, 0), language = role.section('.', 1);
        const std::string name = main.section('?', 0, 0).toStdString(), format = main.section("captionFormat=", 1).toStdString();
        auto& tracks = b->sequence().captionTracks;
        CaptionTrack* track = nullptr;
        for (CaptionTrack& t : tracks)
            if (t.name == name && t.format == format && t.language == language.toStdString()) track = &t;
        if (!track) {
            CaptionTrack t;
            t.id = p.newId();
            t.name = name.empty() ? "Captions" : name;
            t.format = format;
            t.language = language.isEmpty() ? "en" : language.toStdString();
            tracks.push_back(std::move(t));
            track = &tracks.back();
        }
        std::map<QString, QDomElement> defs;
        for (QDomElement d = k.firstChildElement("text-style-def"); !d.isNull(); d = d.nextSiblingElement("text-style-def"))
            defs[d.attribute("id")] = d.firstChildElement("text-style");
        Caption cap;
        cap.start = frames(at);
        cap.end = frames(at + parseFcpTime(k.attribute("duration", "0s")));
        QStringList blocks;
        for (QDomElement t = k.firstChildElement("text"); !t.isNull(); t = t.nextSiblingElement("text")) {
            blocks << t.text();
            if (t.attribute("placement") == "top") cap.vertical = kCaptionTop;
            for (QDomElement st = t.firstChildElement("text-style"); !st.isNull(); st = st.nextSiblingElement("text-style")) {
                QDomElement style = defs.count(st.attribute("ref")) ? defs[st.attribute("ref")] : st;
                if (style.attribute("italic") == "1") cap.italic = true;
                if (const std::vector<double> c = numbers(style.attribute("fontColor")); c.size() >= 3 && !(c[0] == 1 && c[1] == 1 && c[2] == 1))
                    cap.colour = QString("#%1%2%3")
                                     .arg(int(std::lround(c[0] * 255)), 2, 16, QChar('0'))
                                     .arg(int(std::lround(c[1] * 255)), 2, 16, QChar('0'))
                                     .arg(int(std::lround(c[2] * 255)), 2, 16, QChar('0'))
                                     .toStdString();
            }
        }
        cap.text = blocks.join('\n').toStdString();
        track->captions.push_back(std::move(cap));
        normalizeCaptions(track->captions);
    }

    // What a clip element holds that Montage does not use, kept with its clip (Clip::foreign["fcpxml"]) to be written
    // back: its other attributes (1.15's flags among them), its elements (filters, masks, metadata...) and the effect
    // resources they name. The report names each effect not used, clip by clip.
    void carry(const QDomElement& e, Id id, const std::string& name, int blend) {
        Clip* c = id ? edit::clipById(b->sequence(), id) : nullptr;
        if (!c) return;
        static const QStringList knownAttrs = {"ref", "lane", "offset", "name", "start", "duration", "enabled", "srcEnable", "audioRole", "format"};
        static const QStringList knownElements = {"timeMap", "adjust-transform", "adjust-crop", "adjust-blend", "adjust-volume", "adjust-panner",
                                                  "adjust-conform", "adjust-natural-motion-blur", "mc-source", "text", "text-style-def",
                                                  "marker", "chapter-marker"};
        QDomDocument d;
        QDomElement root = d.createElement("carried");
        d.appendChild(root);
        if (blend >= 0) root.setAttribute("blend", blend);
        QDomElement attrs = d.createElement("attrs"), res = d.createElement("res"), els = d.createElement("el");
        bool any = blend >= 0;
        const QDomNamedNodeMap a = e.attributes();
        for (int i = 0; i < a.count(); ++i) {
            const QString n = a.item(i).nodeName();
            if (knownAttrs.contains(n) || (n == "role" && e.tagName() == "audio")) continue;
            attrs.setAttribute(n, a.item(i).nodeValue());
            any = true;
        }
        std::set<QString> named;
        for (QDomElement k = e.firstChildElement(); !k.isNull(); k = k.nextSiblingElement()) {
            const QString tag = k.tagName();
            if (knownElements.contains(tag) || childRank(tag) == 23) continue;
            els.appendChild(d.importNode(k, true));
            any = true;
            // The effects it names, and what the report says of it.
            // (it and every element in it; elementsByTagName("*") matches none in older Qt)
            QList<QDomElement> sub{k};
            for (qsizetype i = 0; i < sub.size(); ++i)
                for (QDomElement x = sub[i].firstChildElement(); !x.isNull(); x = x.nextSiblingElement()) sub << x;
            for (const QDomElement& x : sub) {
                const QString ref = x.attribute("ref");
                if (auto it = effectEls.find(ref); it != effectEls.end() && !named.count(ref)) {
                    named.insert(ref);
                    res.appendChild(d.importNode(it->second, true));
                }
                if (x.tagName() == "filter-video" || x.tagName() == "filter-audio") {
                    QString effect = x.attribute("name");
                    if (effect.isEmpty() && effectEls.count(ref)) effect = effectEls[ref].attribute("name");
                    warnOnce("The effect \"" + effect.toStdString() + "\" on \"" + name + "\" was not used (kept to be written back)");
                }
            }
            if (tag.startsWith("adjust-")) warnOnce("The adjustment " + tag.toStdString() + " on \"" + name + "\" was not used (kept to be written back)");
        }
        if (!any) return;
        root.appendChild(attrs);
        root.appendChild(res);
        root.appendChild(els);
        c->foreign["fcpxml"] = d.toString(-1).toStdString();
    }

    // An element at timeline seconds `at` (where its own time `start` shows).
    Id lastPrimaryAudio = 0;  // the storyline's sound, which its transitions cross-fade too

    // `parent`: the clip of the element it is connected to (0 for none, or a gap).
    void element(const QDomElement& e, double at, int vTrack, int aTrack, Id& lastPrimary, double& pendingDissolve, Id parent = 0) {
        const QString tag = e.tagName();
        const double start = parseFcpTime(e.attribute("start", "0s"));
        const double dur = parseFcpTime(e.attribute("duration", "0s"));
        const FrameTime s0 = frames(at), len = frames(dur);
        const std::string name = e.attribute("name").toStdString();
        Id placed = 0, placedAudio = 0;
        QDomElement seenSource;  // a multicam clip's angle shown (its picture's adjustments)
        bool reverse = false;
        const double speed = speedOf(e, reverse);
        auto place = [&](Id mid, double origin, TrackKind kind, int track) -> Clip* {
            // Sound edited to the sample (times between frames): whole frames round it, the silent parts of its first
            // and last frames its head and tail.
            // (Within half a sample of a frame, on it.)
            const double near = 0.5 * fps / 48000;
            const double from = at * fps, to = (at + dur) * fps, first = std::floor(from + near), last = std::ceil(to - near);
            const bool sampled = kind == TrackKind::Audio && dur > 0 && (from - first > near || last - to > near);
            const double head = sampled ? from - first : 0, tail = sampled ? last - to : 0;
            const double in = (start - origin) * fps - (reverse ? tail : head) * speed;
            Clip* c = sampled ? b->addClip(kind, track, mid, FrameTime(first), FrameTime(last - first), in, name)
                              : b->addClip(kind, track, mid, s0, len, (start - origin) * fps, name);
            if (c) {
                c->speed = speed;
                c->reverse = reverse;
                c->enabled = e.attribute("enabled", "1") != "0";
                c->head = std::clamp(head, 0.0, 1.0), c->tail = std::clamp(tail, 0.0, 1.0);
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
                const QString enable = e.attribute("srcEnable", "all");
                if (tag != "audio" && a.video && enable != "audio" && vTrack >= 0)
                    if (Clip* c = mediaClip(a, TrackKind::Video, vTrack)) v = c->id;
                if ((tag == "audio" || (tag == "asset-clip" && a.audio && enable != "video")) && aTrack >= 0)
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
                // Its text in stretches, each with the style its def gives (the most used one the title's own, the
                // others styled runs).
                std::map<QString, QDomElement> defs;
                const QDomNodeList defList = e.elementsByTagName("text-style-def");
                for (int i = 0; i < defList.size(); ++i) {
                    const QDomElement d = defList.at(i).toElement();
                    defs[d.attribute("id")] = d.firstChildElement("text-style");
                }
                std::vector<std::pair<QString, std::optional<TextRun>>> spans;
                const QDomNodeList styles = e.firstChildElement("text").elementsByTagName("text-style");
                for (int i = 0; i < styles.size(); ++i) {
                    const QDomElement st = styles.at(i).toElement();
                    std::optional<TextRun> style;
                    if (auto it = defs.find(st.attribute("ref")); it != defs.end() && !it->second.isNull()) {
                        const QDomElement d = it->second;
                        TextRun r;
                        auto rgb = [](const QString& v) -> std::optional<std::array<double, 3>> {
                            const QStringList parts = v.simplified().split(' ');
                            if (parts.size() < 3) return std::nullopt;
                            return std::array<double, 3>{parts[0].toDouble(), parts[1].toDouble(), parts[2].toDouble()};
                        };
                        if (d.hasAttribute("font")) r.font = d.attribute("font").toStdString();
                        if (d.hasAttribute("fontSize")) r.size = d.attribute("fontSize").toDouble() * 96.0 / 63.0;
                        if (d.hasAttribute("fontColor")) r.color = rgb(d.attribute("fontColor"));
                        const QString face = d.attribute("fontFace");
                        if (d.hasAttribute("bold")) r.bold = d.attribute("bold").toInt() != 0;
                        else if (!face.isEmpty()) r.bold = face.contains("Bold", Qt::CaseInsensitive);
                        if (d.hasAttribute("italic")) r.italic = d.attribute("italic").toInt() != 0;
                        else if (!face.isEmpty()) r.italic = face.contains("Italic", Qt::CaseInsensitive) || face.contains("Oblique", Qt::CaseInsensitive);
                        if (d.hasAttribute("strokeWidth") && d.attribute("strokeWidth").toDouble() > 0) {
                            r.outline = d.attribute("strokeWidth").toDouble();
                            r.outlineColor = rgb(d.attribute("strokeColor", "0 0 0 1"));
                        } else if (d.hasAttribute("bold") || d.hasAttribute("font")) {
                            r.outline = 0;
                        }
                        if (d.hasAttribute("kerning")) r.tracking = d.attribute("kerning").toDouble();
                        r.baseline = d.attribute("baseline").toDouble();
                        style = r;
                    }
                    spans.emplace_back(st.text(), style);
                }
                applyTitleStyleSpans(c->generator, spans);
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
                    if (enable != "audio" && it->second.first >= 0) seen = it->second.first, seenSource = src;
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
        // What the clip does: its picture's and sound's adjustments, retiming, role; what Montage does not use, kept;
        // the clip it is connected to.
        if (placed) {
            const Id video = placed != placedAudio ? placed : 0;
            int blend = -1;
            if (video) readVideoAdjustments(tag == "mc-clip" ? seenSource : e, video, start, name, blend);
            if (placedAudio) readAudioAdjustments(e, placedAudio, start, name);
            readTiming(e, video, placedAudio, name);
            const QString role = e.attribute(tag == "audio" ? "role" : "audioRole");
            if (Clip* a = placedAudio && !role.isEmpty() ? edit::clipById(b->sequence(), placedAudio) : nullptr) a->role = montageRole(role);
            carry(e, placed, name, blend);
            if (parent)
                for (Id id : {video, placedAudio})
                    if (Clip* c = id ? edit::clipById(b->sequence(), id) : nullptr) c->foreign["fcpxml-parent"] = std::to_string(parent);
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
            if (k.tagName() == "caption") {
                if (!inAngle) readCaption(k, kAt);
                continue;
            }
            if (inAngle) {
                if (l < 0) deferred.push_back({k, kAt, l, angleIndex});
                else b->warn("A picture connected inside a multicam angle was left out (" + k.attribute("name").toStdString() + ")");
                continue;
            }
            Id ignore = 0;
            double none = 0;
            // Lane n > 0 is V(n + 1); lane -n is A(n + 1) (A1 carries the storyline's own sound).
            element(k, kAt, l > 0 ? l : -1, l < 0 ? -l : -1, ignore, none, placed);
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

std::string exportFcpXml(const Project& p, const Sequence& sequence, const std::string& version) {
    const Sequence s = interchangeSequence(sequence);
    FcpxWriter w(p, s, version == "1.10" ? 10 : 15);
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
