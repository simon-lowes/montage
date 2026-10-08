// Montage — Final Cut Pro XML both ways: FCP 7 XML (xmeml 5, also Premiere
// Pro's and Resolve's exchange format) and FCPXML 1.10 (Final Cut Pro X).
#include <QDomDocument>
#include <QFileInfo>
#include <QUrl>
#include <QXmlStreamWriter>
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <optional>
#include <set>

#include "EditOps.h"
#include "Effects.h"
#include "History.h"
#include "ImportBuilder.h"
#include "Interchange.h"

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
    const Sequence& s;
    QXmlStreamWriter w;
    QString out;
    std::map<Id, QString> fileIds;   // media -> file id (written in full once)
    std::map<Id, QString> clipItem;  // clip -> clipitem id

    Fcp7Writer(const Project& pr, const Sequence& sq) : p(pr), s(sq), w(&out) {}

    int timebase() const { return int(std::lround(s.fpsValue())); }
    bool ntsc() const { return s.fps.den == 1001; }
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
        QString url = fileUrl(m.path);
        url.replace("file:///", "file://localhost/");
        w.writeTextElement("pathurl", url);
        rate();
        w.writeTextElement("duration", QString::number(std::llround(m.duration * s.fpsValue())));
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
        for (Id other : edit::linkedClips(s, c.id)) {
            auto loc = edit::locate(s, other);
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
        w.writeTextElement("duration", QString::number(std::llround(m->duration * s.fpsValue())));
        rate();
        times(c, t);
        file(*m);
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
    std::string run() {
        int n = 0;
        for (const auto* list : {&s.videoTracks, &s.audioTracks})
            for (const Track& t : *list)
                for (const Clip& c : t.clips) clipItem[c.id] = QString("%1-%2").arg(c.isGenerator() ? "generatoritem" : "clipitem").arg(++n);
        w.setAutoFormatting(true);
        w.writeStartDocument();
        w.writeDTD("<!DOCTYPE xmeml>");
        w.writeStartElement("xmeml");
        w.writeAttribute("version", "5");
        w.writeStartElement("sequence");
        w.writeAttribute("id", "sequence-1");
        w.writeTextElement("name", q(s.name));
        w.writeTextElement("duration", QString::number(s.duration()));
        rate();
        w.writeStartElement("timecode");
        rate();
        w.writeTextElement("string", q(formatTimecode(0, s.fps)));
        w.writeTextElement("frame", "0");
        w.writeTextElement("displayformat", isDropFrameRate(s.fps) ? "DF" : "NDF");
        w.writeEndElement();
        w.writeStartElement("media");
        w.writeStartElement("video");
        w.writeStartElement("format");
        w.writeStartElement("samplecharacteristics");
        rate();
        w.writeTextElement("width", QString::number(s.width));
        w.writeTextElement("height", QString::number(s.height));
        w.writeTextElement("pixelaspectratio", "square");
        w.writeEndElement();
        w.writeEndElement();
        for (const Track& t : s.videoTracks) track(t, false);
        w.writeEndElement();  // video
        w.writeStartElement("audio");
        w.writeTextElement("numOutputChannels", "2");
        for (const Track& t : s.audioTracks) track(t, true);
        w.writeEndElement();  // audio
        w.writeEndElement();  // media
        for (const Marker& m : s.markers) {
            w.writeStartElement("marker");
            w.writeTextElement("name", q(m.name));
            w.writeTextElement("comment", q(m.comment));
            w.writeTextElement("in", QString::number(m.t));
            w.writeTextElement("out", m.duration > 0 ? QString::number(m.t + m.duration) : "-1");
            w.writeEndElement();
        }
        w.writeEndElement();  // sequence
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
    const Rational fps = fcp7Rate(child(seqEl, "rate"), {30, 1});
    TimelineBuilder b(p, text(seqEl, "name").toStdString(), fps, probe);
    const QDomElement media = child(seqEl, "media");
    if (QDomElement sc = child(child(child(media, "video"), "format"), "samplecharacteristics"); !sc.isNull()) {
        b.sequence().width = std::max(16, int(num(sc, "width", 1920)));
        b.sequence().height = std::max(16, int(num(sc, "height", 1080)));
    }
    // Files are described once and referenced by id afterwards.
    std::map<QString, QDomElement> files;
    QDomNodeList fileNodes = seqEl.elementsByTagName("file");
    for (int i = 0; i < fileNodes.size(); ++i) {
        QDomElement f = fileNodes.at(i).toElement();
        if (f.hasChildNodes() && !f.attribute("id").isEmpty()) files.emplace(f.attribute("id"), f);
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
    return b.finish();
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
    const Sequence s = interchangeSequence(sequence);
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
    const Sequence& s;
    QXmlStreamWriter w;
    QString out;
    std::map<Id, QString> assets;
    QString formatId = "r1", dissolveId, titleId, solidId;
    int nextRes = 2;

    FcpxWriter(const Project& pr, const Sequence& sq) : p(pr), s(sq), w(&out) {}
    QString t(double frames) const { return fcpTime(frames, s.fps); }

    void resources() {
        w.writeStartElement("resources");
        w.writeEmptyElement("format");
        w.writeAttribute("id", formatId);
        w.writeAttribute("frameDuration", fcpTime(1, s.fps));
        w.writeAttribute("width", QString::number(s.width));
        w.writeAttribute("height", QString::number(s.height));
        bool dissolve = false, title = false, solid = false;
        for (const auto* list : {&s.videoTracks, &s.audioTracks})
            for (const Track& tr : *list) {
                dissolve |= !tr.transitions.empty();
                for (const Clip& c : tr.clips) {
                    if (c.isGenerator()) (c.generator.type == "title" ? title : solid) = true;
                    else if (const MediaItem* m = p.findMedia(c.mediaId); m && !assets.count(m->id)) {
                        const QString id = QString("r%1").arg(nextRes++);
                        assets[m->id] = id;
                        w.writeStartElement("asset");
                        w.writeAttribute("id", id);
                        w.writeAttribute("name", q(m->name));
                        w.writeAttribute("start", "0s");
                        w.writeAttribute("duration", t(std::floor(m->duration * s.fpsValue())));
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

    // One clip element. `lane` 0 is the primary storyline; `offset` is in the parent's time.
    void clip(const Clip& c, int lane, double offset, bool withAudio, const std::vector<Connected>& connected = {}) {
        if (c.isGenerator()) {
            const bool title = c.generator.type == "title";
            w.writeStartElement(title ? "title" : "video");
            w.writeAttribute("ref", title ? titleId : solidId);
        } else {
            w.writeStartElement(withAudio ? "asset-clip" : lane < 0 ? "audio" : "video");
            w.writeAttribute("ref", assets[c.mediaId]);
        }
        if (lane != 0) w.writeAttribute("lane", QString::number(lane));
        w.writeAttribute("offset", t(offset));
        w.writeAttribute("name", q(c.name));
        w.writeAttribute("start", c.isGenerator() ? "0s" : t(c.sourceIn));
        w.writeAttribute("duration", t(double(c.duration)));
        if (!c.enabled) w.writeAttribute("enabled", "0");
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
        if (!c.isGenerator()) timeMap(c);
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
        for (const Marker& m : s.markers)
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
        w.writeStartElement("sequence");
        w.writeAttribute("format", formatId);
        w.writeAttribute("duration", t(double(s.duration())));
        w.writeAttribute("tcStart", "0s");
        w.writeAttribute("tcFormat", isDropFrameRate(s.fps) ? "DF" : "NDF");
        w.writeAttribute("audioLayout", "stereo");
        w.writeAttribute("audioRate", s.sampleRate == 44100 ? "44.1k" : s.sampleRate == 96000 ? "96k" : "48k");
        w.writeStartElement("spine");

        // The primary storyline: V1, gaps between. A1 clips linked to a V1 clip ride in its asset-clip.
        static const Track empty;
        const Track& v1 = s.videoTracks.empty() ? empty : s.videoTracks[0];
        struct Element {
            const Clip* clip = nullptr;  // null: gap
            FrameTime offset = 0, duration = 0;
            bool withAudio = false;
            std::vector<Connected> connected;
        };
        std::vector<Element> spine;
        std::set<Id> carried;
        FrameTime cursor = 0;
        for (const Clip& c : v1.clips) {
            if (c.start > cursor) spine.push_back({nullptr, cursor, c.start - cursor});
            Element e{&c, c.start, c.duration};
            if (!c.isGenerator())
                for (Id other : edit::linkedClips(s, c.id))
                    if (auto loc = edit::locate(s, other); loc && loc->track.kind == TrackKind::Audio && loc->track.index == 0) {
                        const Clip& a = s.audioTracks[0].clips[loc->index];
                        if (a.mediaId == c.mediaId && a.start == c.start && a.duration == c.duration) {
                            e.withAudio = true;
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
        for (size_t vi = 1; vi < s.videoTracks.size(); ++vi)
            for (const Clip& c : s.videoTracks[vi].clips) attach(c, int(vi));
        for (size_t ai = 0; ai < s.audioTracks.size(); ++ai)
            for (const Clip& c : s.audioTracks[ai].clips)
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
                clip(*e.clip, 0, double(e.offset), e.withAudio, e.connected);
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
        w.writeEndElement();  // sequence
        w.writeEndElement();  // project
        w.writeEndElement();  // event
        w.writeEndElement();  // library
        w.writeEndElement();  // fcpxml
        w.writeEndDocument();
        return out.toStdString();
    }
};

struct FcpxReader {
    Project& p;
    TimelineBuilder& b;
    double fps;
    struct Asset {
        std::string path, name;
        double start = 0, duration = 0;
        bool video = true, audio = true;
    };
    std::map<QString, Asset> assets;
    std::map<QString, QString> effects;  // id -> name

    FcpxReader(Project& pr, TimelineBuilder& bu, double rate) : p(pr), b(bu), fps(rate) {}
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
            }
        }
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
        auto mediaClip = [&](const Asset& a, TrackKind kind, int track) -> Clip* {
            const Id mid = b.media(a.path, a.name, a.video, a.audio, a.duration);
            Clip* c = b.addClip(kind, track, mid, s0, len, (start - a.start) * fps, name);
            if (c) {
                c->speed = speed;
                c->reverse = reverse;
                c->enabled = e.attribute("enabled", "1") != "0";
            }
            return c;
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
                if (v && au) {
                    const Id group = p.newId();
                    edit::clipById(b.sequence(), v)->linkGroup = group;
                    edit::clipById(b.sequence(), au)->linkGroup = group;
                }
                placed = v ? v : au;
                placedAudio = au;
            } else if (tag == "video" && effects.count(e.attribute("ref")) && vTrack >= 0) {
                // A generator (Final Cut's solids and the like).
                if (Clip* c = b.addGenerator(vTrack, "color", s0, len, name)) placed = c->id;
            }
        } else if (tag == "title" && vTrack >= 0) {
            if (Clip* c = b.addGenerator(vTrack, "title", s0, len, name)) {
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
        } else if (tag == "ref-clip" || tag == "mc-clip") {
            b.warn("Compound and multicam clips from Final Cut are not imported (" + name + ")");
        }
        if (lane(e) == 0 && placed) {
            if (pendingDissolve > 0 && lastPrimary)
                b.addTransition(TrackKind::Video, 0, lastPrimary, placed, frames(pendingDissolve), {});
            if (pendingDissolve > 0 && lastPrimaryAudio && placedAudio)
                b.addTransition(TrackKind::Audio, 0, lastPrimaryAudio, placedAudio, frames(pendingDissolve), "crossfade");
            pendingDissolve = 0;
            lastPrimary = placed;
            lastPrimaryAudio = placedAudio;
        }
        // Connected clips, positioned in this element's own time.
        for (QDomElement k = e.firstChildElement(); !k.isNull(); k = k.nextSiblingElement()) {
            const int l = lane(k);
            if (l == 0) continue;
            const double kAt = at + (parseFcpTime(k.attribute("offset", "0s")) - start) / std::max(0.01, speed);
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
                b.sequence().markers.push_back(mk);
            }
    }
    static int lane(const QDomElement& e) { return e.attribute("lane", "0").toInt(); }
};

ImportResult importFcpx(Project& p, const QDomDocument& doc, const MediaProber& probe) {
    const QDomElement root = doc.documentElement();
    const QDomElement seq = root.elementsByTagName("sequence").at(0).toElement();
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
    const double frameDur = parseFcpTime(format.attribute("frameDuration", "1/30s"));
    const double rate = frameDur > 0 ? 1 / frameDur : 30;
    Rational fps{int(std::lround(rate)), 1};
    if (std::fabs(rate - 29.97) < 0.01) fps = {30000, 1001};
    else if (std::fabs(rate - 23.976) < 0.01) fps = {24000, 1001};
    else if (std::fabs(rate - 59.94) < 0.01) fps = {60000, 1001};
    QString name = seq.parentNode().toElement().attribute("name");
    TimelineBuilder b(p, name.toStdString(), fps, probe);
    if (format.hasAttribute("width")) {
        b.sequence().width = std::max(16, format.attribute("width").toInt());
        b.sequence().height = std::max(16, format.attribute("height").toInt());
    }
    FcpxReader r(p, b, fps.toDouble());
    r.readResources(res);
    const double tcStart = parseFcpTime(seq.attribute("tcStart", "0s"));
    Id last = 0;
    double dissolve = 0;
    const QDomElement spine = seq.firstChildElement("spine");
    for (QDomElement e = spine.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
        const double offset = parseFcpTime(e.attribute("offset", "0s")) - tcStart;
        if (e.tagName() == "transition") {
            dissolve = parseFcpTime(e.attribute("duration"));
            continue;
        }
        if (e.tagName() == "gap") {
            last = 0;
            r.lastPrimaryAudio = 0;
        }
        r.element(e, offset, 0, 0, last, dissolve);
    }
    return b.finish();
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
