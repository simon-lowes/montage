#include "Cdl.h"

#include <QDomDocument>
#include <QFileInfo>
#include <QRegularExpression>
#include <QXmlStreamWriter>
#include <algorithm>
#include <cmath>

#include "Effects.h"

namespace montage {

namespace {

const char* const kChannels[3] = {"r", "g", "b"};

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

// Three numbers from "a b c" (or one, given three times).
bool triple(const QString& text, double out[3]) {
    const QStringList parts = text.simplified().split(' ', Qt::SkipEmptyParts);
    if (parts.size() != 3 && parts.size() != 1) return false;
    for (int i = 0; i < 3; ++i) {
        bool ok = false;
        out[i] = parts[parts.size() == 3 ? i : 0].toDouble(&ok);
        if (!ok || !std::isfinite(out[i])) return false;
    }
    return true;
}

// An element's name without a namespace prefix.
QString nameOf(const QDomElement& e) {
    const QString t = e.tagName();
    const int colon = t.indexOf(':');
    return colon < 0 ? t : t.mid(colon + 1);
}

// The first child element with any of these names (CDL 1.0 wrote SATNode, 1.01 SatNode).
QDomElement childNamed(const QDomElement& e, std::initializer_list<const char*> names) {
    for (QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
        for (const char* n : names)
            if (nameOf(c) == QLatin1String(n)) return c;
    return {};
}

// Every ColorCorrection at or below `c`.
void collect(const QDomElement& c, std::vector<Cdl>& out, std::string& problem) {
    if (nameOf(c) == "ColorCorrectionRef") {
        problem = "The file refers to corrections kept in another file (ColorCorrectionRef " + c.attribute("ref").toStdString() +
                  "): import that .ccc instead";
        return;
    }
    if (nameOf(c) != "ColorCorrection") {
        for (QDomElement k = c.firstChildElement(); !k.isNull(); k = k.nextSiblingElement()) collect(k, out, problem);
        return;
    }
    Cdl cdl;
    cdl.id = c.attribute("id").toStdString();
    if (QDomElement d = childNamed(c, {"Description"}); !d.isNull()) cdl.description = d.text().trimmed().toStdString();
    const QDomElement sop = childNamed(c, {"SOPNode"});
    if (!sop.isNull()) {
        if (QDomElement d = childNamed(sop, {"Description"}); !d.isNull() && cdl.description.empty()) cdl.description = d.text().trimmed().toStdString();
        const QDomElement slope = childNamed(sop, {"Slope"}), offset = childNamed(sop, {"Offset"}), power = childNamed(sop, {"Power"});
        if ((!slope.isNull() && !triple(slope.text(), cdl.slope)) || (!offset.isNull() && !triple(offset.text(), cdl.offset)) ||
            (!power.isNull() && !triple(power.text(), cdl.power))) {
            problem = "A ColorCorrection (" + cdl.id + ") has unreadable slope, offset or power";
            return;
        }
    }
    if (QDomElement sat = childNamed(c, {"SatNode", "SATNode"}); !sat.isNull()) {
        bool ok = false;
        const double v = childNamed(sat, {"Saturation"}).text().trimmed().toDouble(&ok);
        if (!ok || !std::isfinite(v)) {
            problem = "A ColorCorrection (" + cdl.id + ") has an unreadable saturation";
            return;
        }
        cdl.saturation = v;
    }
    for (double& p : cdl.power) p = std::max(1e-6, p);  // (a power of 0 or less is not a CDL)
    cdl.saturation = std::max(0.0, cdl.saturation);
    out.push_back(cdl);
}

}  // namespace

bool Cdl::identity() const {
    Cdl plain;
    plain.id = id;
    return sameGrade(plain);
}

bool Cdl::sameGrade(const Cdl& o) const {
    auto near = [](double a, double b) { return std::fabs(a - b) < 5e-7; };
    for (int i = 0; i < 3; ++i)
        if (!near(slope[i], o.slope[i]) || !near(offset[i], o.offset[i]) || !near(power[i], o.power[i])) return false;
    return near(saturation, o.saturation);
}

void applyCdl(const Cdl& c, float& r, float& g, float& b) {
    float v[3] = {r, g, b};
    for (int i = 0; i < 3; ++i) {
        const float x = clamp01(v[i] * float(c.slope[i]) + float(c.offset[i]));
        v[i] = c.power[i] == 1.0 ? x : std::pow(x, float(c.power[i]));
    }
    if (c.saturation != 1.0) {
        const float luma = 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2];
        for (float& x : v) x = clamp01(luma + float(c.saturation) * (x - luma));
    }
    r = v[0];
    g = v[1];
    b = v[2];
}

std::string cdlNumber(double v) {
    if (std::fabs(v) < 5e-7) v = 0;  // no "-0.000000"
    return QString::number(v, 'f', 6).toStdString();
}

std::string cdlSopText(const Cdl& c) {
    auto group = [](const double v[3]) { return "(" + cdlNumber(v[0]) + " " + cdlNumber(v[1]) + " " + cdlNumber(v[2]) + ")"; };
    return group(c.slope) + group(c.offset) + group(c.power);
}

bool parseCdlSop(const std::string& text, Cdl& out) {
    static const QRegularExpression re(R"(\(([^()]*)\)\s*\(([^()]*)\)\s*\(([^()]*)\))");
    const QRegularExpressionMatch m = re.match(QString::fromStdString(text));
    if (!m.hasMatch()) return false;
    Cdl c = out;
    if (!triple(m.captured(1), c.slope) || !triple(m.captured(2), c.offset) || !triple(m.captured(3), c.power)) return false;
    for (double& p : c.power) p = std::max(1e-6, p);
    out = c;
    return true;
}

std::vector<Cdl> parseCdlXml(const std::string& xml, std::string* error) {
    QDomDocument doc;
    QString err;
    int line = 0;
    if (!doc.setContent(QByteArray::fromStdString(xml), &err, &line)) {
        if (error) *error = "Not a CDL file: " + err.toStdString() + " (line " + std::to_string(line) + ")";
        return {};
    }
    std::vector<Cdl> out;
    std::string problem;
    collect(doc.documentElement(), out, problem);
    if (error) *error = out.empty() && problem.empty() ? "The file holds no ColorCorrection" : problem;
    return out;
}

std::string withoutMediaExtension(const std::string& name) {
    static const QStringList media{"mov", "mp4", "m4v", "mxf", "avi", "mkv", "mts", "m2ts", "braw", "r3d", "ari", "arx", "crm", "dng",
                                   "cine", "nev", "rmf", "mpg", "mpeg", "wav", "bwf", "aif", "aiff", "mp3", "dpx", "exr", "tif", "tiff"};
    const QString n = QString::fromStdString(name);
    const int dot = n.lastIndexOf('.');
    if (dot <= 0 || !media.contains(n.mid(dot + 1).toLower())) return name;
    return n.left(dot).toStdString();
}

const Cdl* matchCdl(const std::vector<Cdl>& cdls, const std::vector<std::string>& names) {
    auto same = [](const std::string& a, const std::string& b) { return !a.empty() && QString::fromStdString(a).compare(QString::fromStdString(b), Qt::CaseInsensitive) == 0; };
    for (const Cdl& c : cdls)
        for (const std::string& n : names)
            if (same(c.id, n)) return &c;
    for (const Cdl& c : cdls)
        for (const std::string& n : names)
            if (same(withoutMediaExtension(c.id), withoutMediaExtension(n))) return &c;
    return nullptr;
}

CdlFormat cdlFormatFor(const std::string& path) {
    const QString ext = QFileInfo(QString::fromStdString(path)).suffix().toLower();
    if (ext == "cc") return CdlFormat::Cc;
    if (ext == "ccc") return CdlFormat::Ccc;
    return CdlFormat::Cdl;
}

std::string writeCdlXml(const std::vector<Cdl>& cdls, CdlFormat format) {
    QString out;
    QXmlStreamWriter w(&out);
    w.setAutoFormatting(true);
    w.writeStartDocument();
    const QString ns = "urn:ASC:CDL:v1.01";
    auto correction = [&](const Cdl& c, bool root) {
        w.writeStartElement("ColorCorrection");
        if (root) w.writeDefaultNamespace(ns);
        if (!c.id.empty()) w.writeAttribute("id", QString::fromStdString(c.id));
        if (!c.description.empty()) w.writeTextElement("Description", QString::fromStdString(c.description));
        w.writeStartElement("SOPNode");
        auto three = [](const double v[3]) {
            return QString::fromStdString(cdlNumber(v[0]) + " " + cdlNumber(v[1]) + " " + cdlNumber(v[2]));
        };
        w.writeTextElement("Slope", three(c.slope));
        w.writeTextElement("Offset", three(c.offset));
        w.writeTextElement("Power", three(c.power));
        w.writeEndElement();
        w.writeStartElement("SatNode");
        w.writeTextElement("Saturation", QString::fromStdString(cdlNumber(c.saturation)));
        w.writeEndElement();
        w.writeEndElement();
    };
    if (format == CdlFormat::Cc) {
        correction(cdls.empty() ? Cdl{} : cdls.front(), true);
    } else if (format == CdlFormat::Ccc) {
        w.writeStartElement("ColorCorrectionCollection");
        w.writeDefaultNamespace(ns);
        for (const Cdl& c : cdls) correction(c, false);
        w.writeEndElement();
    } else {
        w.writeStartElement("ColorDecisionList");
        w.writeDefaultNamespace(ns);
        for (const Cdl& c : cdls) {
            w.writeStartElement("ColorDecision");
            correction(c, false);
            w.writeEndElement();
        }
        w.writeEndElement();
    }
    w.writeEndDocument();
    std::string s = out.toStdString();
    const std::string bare = "<?xml version=\"1.0\"?>";
    if (s.rfind(bare, 0) == 0) s.replace(0, bare.size(), "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    return s;
}

bool clipCdl(const Clip& c, FrameTime t, Cdl& out) {
    for (const Effect& e : c.effects) {
        if (e.type != "cdl" || !e.enabled) continue;
        Cdl cdl;
        cdl.id = e.s("id");
        cdl.description = e.s("description");
        for (int i = 0; i < 3; ++i) {
            const std::string ch = kChannels[i];
            cdl.slope[i] = e.p("slope." + ch, t, 1);
            cdl.offset[i] = e.p("offset." + ch, t, 0);
            cdl.power[i] = std::max(1e-6, e.p("power." + ch, t, 1));
        }
        cdl.saturation = std::max(0.0, e.p("saturation", t, 1));
        out = cdl;
        return true;
    }
    return false;
}

void setClipCdl(Project& p, Clip& c, const Cdl& cdl) {
    Effect* e = nullptr;
    for (Effect& x : c.effects)
        if (x.type == "cdl") {
            e = &x;
            break;
        }
    if (!e) {
        // First in the chain: a CDL is made on the camera's own picture, before any transform out of it.
        c.effects.insert(c.effects.begin(), makeEffect(p, "cdl"));
        e = &c.effects.front();
    }
    e->enabled = true;
    for (int i = 0; i < 3; ++i) {
        const std::string ch = kChannels[i];
        e->params["slope." + ch] = Param(cdl.slope[i]);
        e->params["offset." + ch] = Param(cdl.offset[i]);
        e->params["power." + ch] = Param(cdl.power[i]);
    }
    e->params["saturation"] = Param(cdl.saturation);
    e->strings["id"] = cdl.id;
    if (!cdl.description.empty()) e->strings["description"] = cdl.description;
}

bool mediaCdl(const MediaItem& m, Cdl& out) {
    const auto sop = m.metadata.find("asc_sop");
    if (sop == m.metadata.end()) return false;
    Cdl c;
    if (!parseCdlSop(sop->second, c)) return false;
    if (const auto sat = m.metadata.find("asc_sat"); sat != m.metadata.end()) {
        bool ok = false;
        const double v = QString::fromStdString(sat->second).trimmed().toDouble(&ok);
        if (ok && std::isfinite(v)) c.saturation = std::max(0.0, v);
    }
    c.id = m.name;
    out = c;
    return true;
}

void setMediaCdl(MediaItem& m, const Cdl& cdl) {
    m.metadata["asc_sop"] = cdlSopText(cdl);
    m.metadata["asc_sat"] = cdlNumber(cdl.saturation);
}

}  // namespace montage
