#include "Amf.h"

#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <QXmlStreamWriter>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "Clf.h"
#include "ColorSpace.h"
#include "Processing.h"
#include "core/Effects.h"
#include "core/Interpretation.h"
#include "media/ImageSequence.h"

namespace montage {

namespace {

const char* kAces = "urn:ampas:aces:amf:v2.0";
const char* kCdl = "urn:ASC:CDL:v1.01";
const std::string kIdPrefix = "urn:ampas:aces:transformId:v2.0:";
const std::string kGamutCompress = "Look.Academy.ReferenceGamutCompress.a2.v1";
// The id Montage gives the CLF it writes for a space ACES has no transform for; read back as that space.
const QString kClfIdPrefix = QStringLiteral("urn:montage:clf:");
const QString kClfIdSuffix = QStringLiteral("_to_ACES2065-1");

QDomElement child(const QDomElement& e, const char* name) {
    for (QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
        if (c.localName() == QLatin1String(name)) return c;
    return {};
}

std::string text(const QDomElement& e, const char* name) { return child(e, name).text().trimmed().toStdString(); }

bool appliedOf(const QDomElement& e) {
    const QString a = e.attribute(QStringLiteral("applied")).trimmed().toLower();
    return a == QLatin1String("true") || a == QLatin1String("1");
}

// The name part of a transform id ("IDT.ARRI.Alexa-v3-logC-EI800.a1.v2" from "urn:ampas:aces:transformId:v1.5:...").
QString idName(const std::string& id) {
    QString s = QString::fromStdString(id).trimmed();
    static const QRegularExpression prefix(QStringLiteral("^urn:ampas:aces:transformId:v[0-9.]+:"));
    s.remove(prefix);
    return s;
}

std::string absolute(const std::string& file, const std::string& folder) {
    if (file.empty() || folder.empty()) return file;
    QString f = QString::fromStdString(file);
    if (f.startsWith(QLatin1String("file://"))) f = f.mid(7);
    return QDir::cleanPath(QDir(QString::fromStdString(folder)).absoluteFilePath(f)).toStdString();
}

// The space a CLF Montage wrote stands for, else "".
std::string montageClfSpace(const std::string& path) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    QDomDocument doc;
    if (!doc.setContent(&f, true)) return {};
    const QString id = doc.documentElement().attribute(QStringLiteral("id"));
    if (!id.startsWith(kClfIdPrefix) || !id.endsWith(kClfIdSuffix)) return {};
    const std::string space = id.mid(kClfIdPrefix.size(), id.size() - kClfIdPrefix.size() - kClfIdSuffix.size()).toStdString();
    return findColorSpace(space) ? space : std::string();
}

// A transform as an AMF names it (its id, or a file), with the space it stands for.
struct Named {
    std::string id, file;
};
Named namedTransform(const QDomElement& e, const std::string& folder) {
    Named n;
    n.id = text(e, "transformId");
    if (const QDomElement f = child(e, "file"); !f.isNull()) n.file = absolute(f.text().trimmed().toStdString(), folder);
    return n;
}

bool readTriple(const QDomElement& parent, const char* name, double out[3]) {
    const QDomElement e = child(parent, name);
    if (e.isNull()) return true;
    const QStringList parts = e.text().simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() != 3) return false;
    for (int i = 0; i < 3; ++i) {
        bool ok = false;
        out[i] = parts[i].toDouble(&ok);
        if (!ok || !std::isfinite(out[i])) return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Transform ids

std::string acesInputSpace(const std::string& transformId, bool* exact) {
    if (exact) *exact = true;
    const QString n = idName(transformId);
    auto has = [&](const char* s) { return n.contains(QLatin1String(s), Qt::CaseInsensitive); };
    auto near = [&](const char* space) {
        if (exact) *exact = false;
        return std::string(space);
    };
    if (n.startsWith(QLatin1String("Inv"))) {
        double peak = 0;
        return acesOutputSpace(transformId, &peak);  // an inverse output transform: pictures made for that display
    }
    if (has("ACEScct_to_ACES")) return "acescct";
    if (has("ACEScg_to_ACES")) return "acescg";
    if (has("ACEScc_to_ACES")) return near("acescct");  // (ACEScc: ACEScct without its toe)
    if (has("LogC4")) return "logc4-awg4";
    if (has("LogC3") || has("logC-EI") || has("LogC_EI")) {
        // LogC3's curve differs by exposure index; Montage's is EI 800's.
        static const QRegularExpression ei(QStringLiteral("EI(\\d+)"), QRegularExpression::CaseInsensitiveOption);
        const auto m = ei.match(n);
        return m.hasMatch() && m.captured(1) != QLatin1String("800") ? near("logc3-awg3") : std::string("logc3-awg3");
    }
    if (has("SLog3") && has("SGamut3Cine")) return has("Venice") ? near("slog3-sgamut3cine") : std::string("slog3-sgamut3cine");
    if (has("SLog3") && has("SGamut3")) return has("Venice") ? near("slog3-sgamut3") : std::string("slog3-sgamut3");
    if (has("VLog_VGamut")) return "vlog-vgamut";
    if ((has("CLog3") || has("CanonLog3")) && (has("CGamut") || has("CinemaGamut"))) return "clog3-cinemagamut";
    if (has("Log3G10")) return "log3g10-rwg";
    if (has("BMDFilm_WideGamut_Gen5")) return "bmdfilm5-bmdwg";
    if (has("AppleLog_BT2020")) return "applelog-rec2020";
    if (has("DLog_DGamut")) return "dlog-dgamut";
    return {};
}

std::string acesOutputSpace(const std::string& transformId, double* peakNits) {
    QString n = idName(transformId);
    if (n.startsWith(QLatin1String("Inv"))) n = n.mid(3);
    double peak = 100;
    std::string space;
    // ACES 2: "Output.Academy.<limiting>_<peak>nit_in_<container>_<encoding>.a2.v1".
    static const QRegularExpression v2(QStringLiteral("^Output\\.[^.]+\\.[^_]+_(\\d+)nit_in_([^_]+)_([^.]+)\\.a"));
    // ACES 1: "ODT.Academy.Rec709_100nits_dim", "RRTODT.Academy.Rec2020_1000nits_15nits_ST2084".
    static const QRegularExpression hdr1(QStringLiteral("^(?:RRT)?ODT\\.[^.]+\\.(Rec2020|P3D65)_(\\d+)nits_[^_]+_(ST2084|HLG)\\."));
    if (const auto m = v2.match(n); m.hasMatch()) {
        peak = m.captured(1).toDouble();
        const QString container = m.captured(2), eotf = m.captured(3);
        const bool sdrCurve = eotf == QLatin1String("BT1886") || eotf == QLatin1String("sRGB-Piecewise") || eotf == QLatin1String("Gamma2pt2");
        if (container == QLatin1String("Rec709-D65") && sdrCurve) space = eotf == QLatin1String("BT1886") ? "rec709" : "srgb";
        else if (container == QLatin1String("Rec2100-D65") && eotf == QLatin1String("ST2084")) space = "rec2100pq";
        else if (container == QLatin1String("Rec2100-D65") && eotf == QLatin1String("HLG")) space = "rec2100hlg";
        else if (container == QLatin1String("P3-D65") && eotf == QLatin1String("ST2084")) space = "p3d65pq";
        else if (container == QLatin1String("P3-D65") && eotf != QLatin1String("Gamma2pt6") && sdrCurve) space = "p3d65";
    } else if (const auto h = hdr1.match(n); h.hasMatch()) {
        peak = h.captured(2).toDouble();
        space = h.captured(3) == QLatin1String("HLG") ? "rec2100hlg" : h.captured(1) == QLatin1String("P3D65") ? "p3d65pq" : "rec2100pq";
    } else if (n.startsWith(QLatin1String("ODT."))) {
        if (n.contains(QLatin1String(".Rec709_"))) space = "rec709";
        else if (n.contains(QLatin1String(".RGBmonitor_"))) space = "srgb";
        else if (n.contains(QLatin1String(".Rec2020_100nits")) || n.contains(QLatin1String(".Rec2020_Rec709limited"))) space = "rec2020";
        else if (n.contains(QLatin1String(".DisplayP3_"))) space = "p3d65";
    }
    if (space == "rec2100hlg") peak = 1000;
    if (peakNits) *peakNits = space.empty() ? 0 : peak;
    return space;
}

std::string acesInputTransformId(const std::string& spaceId) {
    static const std::map<std::string, std::string> ids{
        {"acescct", "CSC.Academy.ACEScct_to_ACES.a2.v1"},
        {"acescg", "CSC.Academy.ACEScg_to_ACES.a2.v1"},
        {"logc3-awg3", "CSC.Arri.LogC3_to_ACES.a2.v1"},
        {"logc4-awg4", "CSC.Arri.LogC4_to_ACES.a2.v1"},
        {"slog3-sgamut3cine", "CSC.Sony.SLog3_SGamut3Cine_to_ACES.a2.v1"},
        {"slog3-sgamut3", "CSC.Sony.SLog3_SGamut3_to_ACES.a2.v1"},
        {"vlog-vgamut", "CSC.Panasonic.VLog_VGamut_to_ACES.a2.v1"},
        {"clog3-cinemagamut", "CSC.Canon.CLog3_CGamut_to_ACES.a1.v1"},
        {"log3g10-rwg", "CSC.Red.Log3G10_RWG_to_ACES.a2.v1"},
        {"bmdfilm5-bmdwg", "CSC.Blackmagic.BMDFilm_WideGamut_Gen5_to_ACES.a2.v1"},
        {"applelog-rec2020", "CSC.Apple.AppleLog_BT2020_to_ACES.a2.v1"},
        {"dlog-dgamut", "CSC.DJI.DLog_DGamut_to_ACES.a1.v1"},
    };
    const auto it = ids.find(spaceId);
    return it == ids.end() ? std::string() : kIdPrefix + it->second;
}

std::string acesOutputTransformId(const std::string& spaceId, double peakNits) {
    auto nearest = [&](std::initializer_list<int> peaks) {
        int best = *peaks.begin();
        for (int p : peaks)
            if (std::fabs(std::log(p / std::max(1.0, peakNits))) < std::fabs(std::log(best / std::max(1.0, peakNits)))) best = p;
        return std::to_string(best);
    };
    std::string name;
    if (spaceId == "rec709") name = "Output.Academy.Rec709-D65_100nit_in_Rec709-D65_BT1886.a2.v1";
    else if (spaceId == "srgb") name = "Output.Academy.Rec709-D65_100nit_in_Rec709-D65_sRGB-Piecewise.a2.v1";
    else if (spaceId == "p3d65") name = "Output.Academy.P3-D65_100nit_in_P3-D65_sRGB-Piecewise.a2.v1";
    else if (spaceId == "rec2100pq") name = "Output.Academy.Rec2100-D65_" + nearest({500, 1000, 2000, 4000}) + "nit_in_Rec2100-D65_ST2084.a2.v1";
    else if (spaceId == "rec2100hlg") name = "Output.Academy.P3-D65_1000nit_in_Rec2100-D65_HLG.a2.v1";
    else if (spaceId == "p3d65pq") name = "Output.Academy.P3-D65_" + nearest({108, 500, 1000, 2000, 4000}) + "nit_in_P3-D65_ST2084.a2.v1";
    return name.empty() ? std::string() : kIdPrefix + name;
}

std::string clfToAces(const std::string& spaceId) {
    const ColorSpace* cs = findColorSpace(spaceId);
    if (!cs) return {};
    std::string out;
    char buf[160];
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<ProcessList compCLFversion=\"3.0\" id=\"" + (kClfIdPrefix.toStdString() + spaceId + kClfIdSuffix.toStdString()) +
           "\" name=\"" + spaceId + " to ACES2065-1\">\n";
    QString label = QString::fromStdString(cs->label).toHtmlEscaped();
    out += "    <Description>" + label.toStdString() + " to ACES2065-1 (written by Montage)</Description>\n";
    out += "    <InputDescriptor>" + label.toStdString() + "</InputDescriptor>\n";
    out += "    <OutputDescriptor>ACES2065-1</OutputDescriptor>\n";
    if (cs->transfer != Transfer::Linear) {
        const int n = 4096;
        out += "    <LUT1D inBitDepth=\"32f\" outBitDepth=\"32f\">\n        <Array dim=\"" + std::to_string(n) + " 1\">\n";
        for (int i = 0; i < n; ++i) {
            std::snprintf(buf, sizeof buf, "            %.9g\n", toLinear(cs->transfer, double(i) / (n - 1)));
            out += buf;
        }
        out += "        </Array>\n    </LUT1D>\n";
    }
    double m[9];
    primariesMatrix(cs->primaries, Primaries::Ap0, m);
    out += "    <Matrix inBitDepth=\"32f\" outBitDepth=\"32f\">\n        <Array dim=\"3 3\">\n";
    for (int r = 0; r < 3; ++r) {
        std::snprintf(buf, sizeof buf, "            %.10f %.10f %.10f\n", m[r * 3], m[r * 3 + 1], m[r * 3 + 2]);
        out += buf;
    }
    out += "        </Array>\n    </Matrix>\n</ProcessList>\n";
    return out;
}

// ---------------------------------------------------------------------------
// Reading

bool parseAmf(const std::string& xml, const std::string& folder, AmfDocument& out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    QDomDocument doc;
    if (!doc.setContent(QByteArray::fromStdString(xml), true)) return fail("Not an XML file");
    const QDomElement root = doc.documentElement();
    if (root.localName() != QLatin1String("acesMetadataFile")) return fail("Not an ACES Metadata File (no acesMetadataFile)");
    AmfDocument a;
    const QDomElement info = child(root, "amfInfo");
    a.description = text(info, "description");
    a.uuid = text(info, "uuid");
    if (const QDomElement clip = child(root, "clipId"); !clip.isNull()) {
        a.clipName = text(clip, "clipName");
        a.clipFile = text(clip, "file");
        a.clipUuid = text(clip, "uuid");
        if (const QDomElement seq = child(clip, "sequence"); !seq.isNull()) {
            QString pattern = seq.text().trimmed();
            const QString idx = seq.attribute(QStringLiteral("idx"), QStringLiteral("#"));
            if (!idx.isEmpty() && idx != QLatin1String("#")) pattern.replace(idx, QStringLiteral("#"));
            a.clipSequence = pattern.toStdString();
            a.sequenceMin = seq.attribute(QStringLiteral("min")).toInt();
            a.sequenceMax = seq.attribute(QStringLiteral("max")).toInt();
        }
    }
    const QDomElement pipe = child(root, "pipeline");
    if (pipe.isNull()) return fail("The ACES Metadata File has no pipeline");
    for (QDomElement e = pipe.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
        const QString kind = e.localName();
        if (kind == QLatin1String("inputTransform")) {
            a.hasInput = true;
            a.inputApplied = appliedOf(e);
            Named n = namedTransform(e, folder);
            for (const char* inverse : {"inverseOutputTransform", "inverseOutputDeviceTransform"})
                if (n.id.empty() && n.file.empty())
                    if (const QDomElement inv = child(e, inverse); !inv.isNull()) n = namedTransform(inv, folder);
            a.inputTransformId = n.id;
            a.inputFile = n.file;
            a.inputSpace = !n.id.empty() ? acesInputSpace(n.id) : !n.file.empty() ? montageClfSpace(n.file) : std::string();
        } else if (kind == QLatin1String("lookTransform")) {
            AmfLook look;
            look.applied = appliedOf(e);
            look.description = text(e, "description");
            const QDomElement cws = child(e, "cdlWorkingSpace");
            if (!cws.isNull()) {
                look.kind = AmfLook::Kind::Cdl;
                const Named from = namedTransform(child(cws, "fromCdlWorkingSpace"), folder);
                look.cdlSpaceId = !from.id.empty() ? from.id : from.file;
                look.cdlSpace = !from.id.empty() ? acesInputSpace(from.id) : !from.file.empty() ? montageClfSpace(from.file) : std::string();
                QDomElement sop = child(e, "SOPNode"), sat = child(e, "SatNode");
                if (sop.isNull()) sop = child(e, "ASC_SOP");
                if (sat.isNull()) sat = child(e, "ASC_SAT");
                if (const QDomElement ref = child(e, "ColorCorrectionRef"); !ref.isNull() && sop.isNull()) {
                    // The correction kept in a .cc/.ccc/.cdl file beside it.
                    const std::string file = absolute(text(e, "file"), folder);
                    QFile f(QString::fromStdString(file));
                    std::vector<Cdl> cdls;
                    if (f.open(QIODevice::ReadOnly)) cdls = parseCdlXml(f.readAll().toStdString());
                    const Cdl* found = matchCdl(cdls, {ref.attribute(QStringLiteral("ref")).toStdString()});
                    if (!found && cdls.size() == 1) found = &cdls.front();
                    if (!found) return fail("The CDL " + ref.attribute(QStringLiteral("ref")).toStdString() + " is not in " + file);
                    look.cdl = *found;
                } else {
                    if (!readTriple(sop, "Slope", look.cdl.slope) || !readTriple(sop, "Offset", look.cdl.offset) ||
                        !readTriple(sop, "Power", look.cdl.power))
                        return fail("A look's CDL has numbers that cannot be read");
                    if (const QDomElement s = child(sat, "Saturation"); !s.isNull()) {
                        bool ok = false;
                        look.cdl.saturation = s.text().trimmed().toDouble(&ok);
                        if (!ok || !std::isfinite(look.cdl.saturation)) return fail("A look's CDL saturation cannot be read");
                    }
                }
                look.cdl.description = look.description;
            } else {
                const Named n = namedTransform(e, folder);
                if (!n.file.empty()) {
                    look.kind = AmfLook::Kind::File;
                    look.file = n.file;
                } else if (!n.id.empty()) {
                    look.kind = AmfLook::Kind::TransformId;
                    look.transformId = n.id;
                } else {
                    return fail("A look names no transform");
                }
            }
            a.looks.push_back(look);
        } else if (kind == QLatin1String("outputTransform")) {
            a.hasOutput = true;
            a.outputApplied = appliedOf(e);
            Named n = namedTransform(e, folder);
            if (n.id.empty() && n.file.empty()) n = namedTransform(child(e, "outputDeviceTransform"), folder);  // ACES 1: RRT + ODT
            a.outputTransformId = !n.id.empty() ? n.id : n.file;
            a.outputSpace = acesOutputSpace(n.id, &a.outputPeakNits);
        }
    }
    out = std::move(a);
    return true;
}

bool readAmf(const std::string& path, AmfDocument& out, std::string* error) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "Cannot read " + path;
        return false;
    }
    return parseAmf(f.readAll().toStdString(), QFileInfo(f).absolutePath().toStdString(), out, error);
}

// ---------------------------------------------------------------------------
// Writing

std::string writeAmf(const AmfDocument& a) {
    QByteArray bytes;
    QXmlStreamWriter w(&bytes);
    w.setAutoFormatting(true);
    w.setAutoFormattingIndent(4);
    w.writeStartDocument();
    w.writeNamespace(QString::fromLatin1(kAces), QStringLiteral("aces"));
    w.writeNamespace(QString::fromLatin1(kCdl), QStringLiteral("cdl"));
    const QString A = QString::fromLatin1(kAces), C = QString::fromLatin1(kCdl);
    auto el = [&](const char* name, const std::string& value) { w.writeTextElement(A, QString::fromLatin1(name), QString::fromStdString(value)); };
    const std::string now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
    auto uuid = [] { return "urn:uuid:" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(); };
    auto info = [&](const std::string& description, const std::string& id) {
        if (!description.empty()) el("description", description);
        w.writeStartElement(A, QStringLiteral("dateTime"));
        el("creationDateTime", now);
        el("modificationDateTime", now);
        w.writeEndElement();
        el("uuid", id.empty() ? uuid() : id);
    };
    auto named = [&](const std::string& id, const std::string& file) {
        if (!id.empty()) el("transformId", id);
        else el("file", file);
    };
    w.writeStartElement(A, QStringLiteral("acesMetadataFile"));
    w.writeAttribute(QStringLiteral("version"), QStringLiteral("2.0"));
    w.writeStartElement(A, QStringLiteral("amfInfo"));
    info(a.description, a.uuid);
    w.writeEndElement();
    if (!a.clipName.empty() && (!a.clipSequence.empty() || !a.clipFile.empty() || !a.clipUuid.empty())) {
        w.writeStartElement(A, QStringLiteral("clipId"));
        el("clipName", a.clipName);
        if (!a.clipSequence.empty()) {
            w.writeStartElement(A, QStringLiteral("sequence"));
            w.writeAttribute(QStringLiteral("idx"), QStringLiteral("#"));
            w.writeAttribute(QStringLiteral("min"), QString::number(a.sequenceMin));
            w.writeAttribute(QStringLiteral("max"), QString::number(a.sequenceMax));
            w.writeCharacters(QString::fromStdString(a.clipSequence));
            w.writeEndElement();
        } else if (!a.clipFile.empty()) {
            el("file", a.clipFile);
        } else {
            el("uuid", a.clipUuid);
        }
        w.writeEndElement();
    }
    w.writeStartElement(A, QStringLiteral("pipeline"));
    w.writeStartElement(A, QStringLiteral("pipelineInfo"));
    info({}, {});
    w.writeStartElement(A, QStringLiteral("systemVersion"));
    el("majorVersion", "2");
    el("minorVersion", "0");
    el("patchVersion", "0");
    w.writeEndElement();
    w.writeEndElement();
    if (a.hasInput && (!a.inputTransformId.empty() || !a.inputFile.empty())) {
        w.writeStartElement(A, QStringLiteral("inputTransform"));
        w.writeAttribute(QStringLiteral("applied"), a.inputApplied ? QStringLiteral("true") : QStringLiteral("false"));
        if (idName(a.inputTransformId).startsWith(QLatin1String("InvOutput."))) {
            w.writeStartElement(A, QStringLiteral("inverseOutputTransform"));
            el("transformId", a.inputTransformId);
            w.writeEndElement();
        } else {
            named(a.inputTransformId, a.inputFile);
        }
        w.writeEndElement();
    }
    for (const AmfLook& l : a.looks) {
        w.writeStartElement(A, QStringLiteral("lookTransform"));
        w.writeAttribute(QStringLiteral("applied"), l.applied ? QStringLiteral("true") : QStringLiteral("false"));
        if (!l.description.empty()) el("description", l.description);
        if (l.kind == AmfLook::Kind::Cdl) {
            w.writeStartElement(A, QStringLiteral("cdlWorkingSpace"));
            w.writeStartElement(A, QStringLiteral("fromCdlWorkingSpace"));
            const bool isId = QString::fromStdString(l.cdlSpaceId).startsWith(QLatin1String("urn:ampas:aces:transformId:"));
            named(isId ? l.cdlSpaceId : std::string(), isId ? std::string() : l.cdlSpaceId);
            w.writeEndElement();
            w.writeEndElement();
            auto triple = [&](const char* name, const double v[3]) {
                w.writeTextElement(C, QString::fromLatin1(name),
                                   QString::fromStdString(cdlNumber(v[0]) + " " + cdlNumber(v[1]) + " " + cdlNumber(v[2])));
            };
            w.writeStartElement(C, QStringLiteral("SOPNode"));
            triple("Slope", l.cdl.slope);
            triple("Offset", l.cdl.offset);
            triple("Power", l.cdl.power);
            w.writeEndElement();
            w.writeStartElement(C, QStringLiteral("SatNode"));
            w.writeTextElement(C, QStringLiteral("Saturation"), QString::fromStdString(cdlNumber(l.cdl.saturation)));
            w.writeEndElement();
        } else {
            named(l.kind == AmfLook::Kind::TransformId ? l.transformId : std::string(), l.file);
        }
        w.writeEndElement();
    }
    if (a.hasOutput && !a.outputTransformId.empty()) {
        w.writeStartElement(A, QStringLiteral("outputTransform"));
        w.writeAttribute(QStringLiteral("applied"), a.outputApplied ? QStringLiteral("true") : QStringLiteral("false"));
        el("transformId", a.outputTransformId);
        w.writeEndElement();
    }
    w.writeEndElement();  // pipeline
    w.writeEndElement();  // acesMetadataFile
    w.writeEndDocument();
    return bytes.toStdString();
}

// ---------------------------------------------------------------------------
// Clips

namespace {

const std::string kAmfTag = "amf";  // on the effects an AMF gave a clip (replaced by the next one)

// Names a clip goes by, for matching an AMF's clipId: the clip's, its media's, the file's and the tape's.
std::vector<std::string> clipNames(const Project& p, const Clip& c) {
    std::vector<std::string> names{c.name};
    if (const MediaItem* m = p.findMedia(c.mediaId)) {
        names.push_back(m->name);
        if (!m->path.empty()) {
            const std::string path = uninterpretedPath(m->path);
            ImageSequence seq;
            if (parseImageSequencePath(path, seq)) names.push_back(QFileInfo(QString::fromStdString(seq.pattern)).fileName().toStdString());
            else names.push_back(QFileInfo(QString::fromStdString(path)).fileName().toStdString());
        }
        for (const char* key : {"tape", "amf_name"})
            if (auto it = m->metadata.find(key); it != m->metadata.end()) names.push_back(it->second);
    }
    return names;
}

// A file name with its frame number as one '#' ("shot_%04d.exr", "shot_####.exr", "shot_[1001-1048].exr" alike).
QString frameless(QString s) {
    static const QRegularExpression number(QStringLiteral("%0?\\d*d|#+|\\[\\d+-\\d+\\]"));
    return s.replace(number, QStringLiteral("#")).toLower();
}

bool matches(const AmfDocument& a, const Project& p, const Clip& c) {
    const MediaItem* m = p.findMedia(c.mediaId);
    if (!a.clipUuid.empty() && m)
        if (auto it = m->metadata.find("amf_uuid"); it != m->metadata.end() && QString::fromStdString(it->second).compare(QString::fromStdString(a.clipUuid), Qt::CaseInsensitive) == 0)
            return true;
    const std::vector<std::string> names = clipNames(p, c);
    for (const std::string& want : {a.clipName, a.clipFile, a.clipSequence}) {
        if (want.empty()) continue;
        const QString w = frameless(QFileInfo(QString::fromStdString(want)).fileName());
        for (const std::string& n : names) {
            if (n.empty()) continue;
            const QString have = frameless(QString::fromStdString(n));
            if (have == w || frameless(QString::fromStdString(withoutMediaExtension(n))) == frameless(QString::fromStdString(withoutMediaExtension(want))))
                return true;
        }
    }
    return false;
}

void note(std::vector<std::string>& notes, const std::string& n) {
    if (std::find(notes.begin(), notes.end(), n) == notes.end()) notes.push_back(n);
}

const ColorSpace* spaceByLabel(const std::string& label) {
    for (const auto& cs : colorSpaces())
        if (cs.label == label) return &cs;
    return nullptr;
}

}  // namespace

AmfImportResult applyAmf(Project& p, Sequence& seq, const std::vector<Id>& clips, const AmfDocument& amf, const AmfImportOptions& o) {
    AmfImportResult r;
    std::vector<Clip*> targets;
    for (Track& t : seq.videoTracks)
        for (Clip& c : t.clips) {
            if (c.isGenerator() || !c.mediaId) continue;
            const bool chosen = clips.empty() ? matches(amf, p, c) : std::find(clips.begin(), clips.end(), c.id) != clips.end();
            if (chosen) targets.push_back(&c);
        }
    if (targets.empty()) {
        note(r.notes, clips.empty() ? "No clip here is " + (amf.clipName.empty() ? amf.clipFile : amf.clipName) + ": select the clips to give it"
                                    : std::string("Select clips of footage"));
        return r;
    }
    // The input transform: the space the media is read as (already in ACES when it was applied).
    std::string inputSpace;
    if (amf.hasInput) {
        inputSpace = amf.inputApplied ? std::string("aces2065-1") : amf.inputSpace;
        bool exact = true;
        if (!amf.inputApplied && !amf.inputTransformId.empty()) acesInputSpace(amf.inputTransformId, &exact);
        const std::string named = !amf.inputTransformId.empty() ? idName(amf.inputTransformId).toStdString() : amf.inputFile;
        if (inputSpace.empty()) note(r.notes, "The input transform " + named + " is not one Montage has; the media is read as before");
        else if (!exact) note(r.notes, "The input transform " + named + " is read as " + findColorSpace(inputSpace)->label);
    }
    // The looks not yet in the pictures, as effects.
    std::vector<Effect> looks;
    for (const AmfLook& l : amf.looks) {
        if (l.applied) continue;
        Effect e;
        if (l.kind == AmfLook::Kind::Cdl) {
            e = makeEffect(p, "cdl");
            for (int i = 0; i < 3; ++i) {
                const std::string ch = i == 0 ? "r" : i == 1 ? "g" : "b";
                e.params["slope." + ch] = Param(l.cdl.slope[i]);
                e.params["offset." + ch] = Param(l.cdl.offset[i]);
                e.params["power." + ch] = Param(l.cdl.power[i]);
            }
            e.params["saturation"] = Param(l.cdl.saturation);
            e.strings["id"] = l.cdl.id.empty() ? (amf.clipName.empty() ? l.description : amf.clipName) : l.cdl.id;
            if (!l.description.empty()) e.strings["description"] = l.description;
            const ColorSpace* space = findColorSpace(l.cdlSpace.empty() ? "acescct" : l.cdlSpace);
            if (l.cdlSpace.empty())
                note(r.notes, "The CDL's working space " + (l.cdlSpaceId.empty() ? std::string("(none named)") : idName(l.cdlSpaceId).toStdString()) +
                                  " is not one Montage has; it is applied in ACEScct");
            e.strings["space"] = space->label;
        } else if (l.kind == AmfLook::Kind::File) {
            std::string why;
            const QString f = QString::fromStdString(l.file);
            const bool clf = f.endsWith(QLatin1String(".clf"), Qt::CaseInsensitive) || f.endsWith(QLatin1String(".ctf"), Qt::CaseInsensitive);
            const bool ok = clf ? bool(loadClf(l.file, &why)) : bool(loadCubeLut(l.file, &why));
            if (!ok) {
                note(r.notes, "The look " + QFileInfo(f).fileName().toStdString() + " was left out: " + (why.empty() ? "it cannot be read" : why));
                continue;
            }
            e = makeEffect(p, "lut");
            e.strings["path"] = l.file;
            if (!l.description.empty()) e.strings["description"] = l.description;
            e.strings["space"] = findColorSpace("aces2065-1")->label;  // (a look works on ACES2065-1)
        } else {
            const QString n = idName(l.transformId);
            if (!n.contains(QLatin1String("GamutCompress"), Qt::CaseInsensitive)) {
                note(r.notes, "The look " + n.toStdString() + " is not one Montage has; it was left out");
                continue;
            }
            e = makeEffect(p, "aces_gamut_compress");
            e.params["direction"] = Param(n.startsWith(QLatin1String("Inv")) ? 1.0 : 0.0);
        }
        e.strings[kAmfTag] = amf.uuid.empty() ? std::string("1") : amf.uuid;
        looks.push_back(e);
    }
    std::set<Id> media;
    for (Clip* c : targets) {
        // An earlier AMF's looks give way to this one's, first in the chain (they are made on the camera's picture).
        c->effects.erase(std::remove_if(c->effects.begin(), c->effects.end(), [](const Effect& e) { return e.strings.count(kAmfTag) > 0; }),
                         c->effects.end());
        std::vector<Effect> mine = looks;
        for (Effect& e : mine) e.id = p.newId();
        c->effects.insert(c->effects.begin(), mine.begin(), mine.end());
        media.insert(c->mediaId);
        ++r.clips;
    }
    for (MediaItem& m : p.media) {
        if (!media.count(m.id)) continue;
        if (!inputSpace.empty()) m.colorOverride = inputSpace == m.colorSpace ? std::string() : inputSpace;
        if (!amf.clipName.empty()) m.metadata["amf_name"] = amf.clipName;
        if (!amf.clipUuid.empty()) m.metadata["amf_uuid"] = amf.clipUuid;
    }
    if (amf.hasOutput) {
        if (amf.outputSpace.empty()) {
            note(r.notes, "The output transform " + idName(amf.outputTransformId).toStdString() + " is for a display Montage does not deliver to");
        } else if (o.setSequenceSpace) {
            seq.colorSpace = amf.outputSpace;
            if (findColorSpace(amf.outputSpace)->hdr()) seq.hdrPeakNits = amf.outputPeakNits;
        }
    }
    return r;
}

bool clipAmf(const Project& p, const Sequence& seq, const Clip& clip, const std::string& folder, AmfDocument& out,
             std::vector<std::string>* notes, const std::string& pictureSpace, std::string* error) {
    const MediaItem* m = p.findMedia(clip.mediaId);
    if (!m || clip.isGenerator()) {
        if (error) *error = "An ACES Metadata File is for a clip of footage";
        return false;
    }
    std::vector<std::string> said;
    const QDir dir(QString::fromStdString(folder));
    // A CLF taking a space into ACES, written beside the AMF once (for a space ACES has no id for).
    auto clfFor = [&](const std::string& space) -> std::string {
        const QString name = QString::fromStdString(space) + QStringLiteral("_to_ACES2065-1.clf");
        QSaveFile f(dir.filePath(name));
        const std::string clf = clfToAces(space);
        if (clf.empty() || !f.open(QIODevice::WriteOnly) || f.write(clf.data(), qint64(clf.size())) != qint64(clf.size()) || !f.commit()) {
            note(said, "Cannot write " + dir.filePath(name).toStdString());
            return {};
        }
        return name.toStdString();
    };
    AmfDocument a;
    a.description = clip.name.empty() ? m->name : clip.name;
    if (auto it = m->metadata.find("amf_uuid"); it != m->metadata.end()) a.clipUuid = it->second;
    const auto named = m->metadata.find("amf_name");
    a.clipName = named != m->metadata.end() ? named->second : withoutMediaExtension(m->name.empty() ? clip.name : m->name);
    const std::string path = uninterpretedPath(m->path);
    ImageSequence frames;
    if (parseImageSequencePath(path, frames)) {
        a.clipSequence = frameless(QFileInfo(QString::fromStdString(frames.pattern)).fileName()).toStdString();
        // (the number as wide as it is written: "#" for each digit)
        static const QRegularExpression width(QStringLiteral("%0(\\d+)d"));
        const auto w = width.match(QString::fromStdString(frames.pattern));
        if (w.hasMatch()) {
            QString pat = QFileInfo(QString::fromStdString(frames.pattern)).fileName();
            pat.replace(width, QString(w.captured(1).toInt(), QLatin1Char('#')));
            a.clipSequence = pat.toStdString();
        }
        a.sequenceMin = frames.first;
        a.sequenceMax = frames.last;
    } else if (!path.empty()) {
        a.clipFile = QFileInfo(QString::fromStdString(path)).fileName().toStdString();
    }
    // The input transform: the pictures' space into ACES.
    const std::string space = !pictureSpace.empty() ? pictureSpace : !m->colorOverride.empty() ? m->colorOverride : m->colorSpace.empty() ? "rec709" : m->colorSpace;
    const ColorSpace* cs = findColorSpace(space);
    if (cs && cs->id != "aces2065-1") {
        a.hasInput = true;
        a.inputTransformId = acesInputTransformId(cs->id);
        if (a.inputTransformId.empty() && !cs->sceneReferred) {
            a.inputTransformId = acesOutputTransformId(cs->id, seq.hdrPeakNits);  // pictures made for a display: its inverse
            if (!a.inputTransformId.empty()) a.inputTransformId.replace(a.inputTransformId.find(":Output."), 8, ":InvOutput.");
        }
        if (a.inputTransformId.empty()) a.inputFile = clfFor(cs->id);
        if (a.inputTransformId.empty() && a.inputFile.empty()) a.hasInput = false;
    }
    // The looks: its CDL, CLF and cube looks in ACES2065-1, the gamut compression; what else grades it is not carried.
    const ColorSpace& working = sequenceColorSpace(seq);
    for (const Effect& e : clip.effects) {
        if (!e.enabled) continue;
        AmfLook l;
        if (e.type == "cdl") {
            Clip holder;
            holder.effects.push_back(e);
            clipCdl(holder, 0, l.cdl);
            const ColorSpace* in = spaceByLabel(e.s("space"));
            if (!in) in = &working;
            l.kind = AmfLook::Kind::Cdl;
            l.description = !e.s("description").empty() ? e.s("description") : e.s("id");
            l.cdlSpace = in->id;
            l.cdlSpaceId = in->id == "aces2065-1" ? std::string() : acesInputTransformId(in->id);
            if (l.cdlSpaceId.empty()) l.cdlSpaceId = clfFor(in->id);
            if (l.cdlSpaceId.empty()) continue;
        } else if (e.type == "lut") {
            const std::string file = e.s("path");
            if (file.empty()) continue;
            const ColorSpace* in = spaceByLabel(e.s("space"));
            if (!in || in->id != "aces2065-1") {
                note(said, "The LUT " + QFileInfo(QString::fromStdString(file)).fileName().toStdString() +
                               " is applied in " + (in ? in->label : std::string("the working space")) +
                               "; an AMF's looks work in ACES2065-1, so it was left out");
                continue;
            }
            l.kind = AmfLook::Kind::File;
            // Beside the AMF, so the two travel together.
            const QFileInfo src(QString::fromStdString(file));
            const QString there = dir.filePath(src.fileName());
            if (QFileInfo(there).absoluteFilePath() != src.absoluteFilePath()) {
                QFile::remove(there);
                if (!QFile::copy(src.absoluteFilePath(), there)) note(said, "Cannot copy " + file + " beside the AMF");
            }
            l.file = src.fileName().toStdString();
            l.description = !e.s("description").empty() ? e.s("description") : src.completeBaseName().toStdString();
        } else if (e.type == "aces_gamut_compress") {
            l.kind = AmfLook::Kind::TransformId;
            l.description = "ACES Reference Gamut Compression";
            l.transformId = kIdPrefix + (e.p("direction", 0) >= 0.5 ? "Inv" : "") + kGamutCompress;
        } else {
            const EffectInfo* info = findEffectInfo(e.type);
            if (info && info->group == "Color" && info->category == EffectCategory::VideoFilter)
                note(said, info->displayName + " is not carried (an AMF holds CDLs, CLF looks and gamut compression)");
            continue;
        }
        a.looks.push_back(l);
    }
    // The output transform: the sequence's display.
    a.outputTransformId = acesOutputTransformId(seq.colorSpace, seq.hdrPeakNits);
    a.hasOutput = !a.outputTransformId.empty();
    if (!a.hasOutput) note(said, "ACES has no output transform for " + working.label);
    out = std::move(a);
    if (notes) *notes = std::move(said);
    return true;
}

bool writeClipAmf(const Project& p, const Sequence& seq, const Clip& clip, const std::string& path, std::vector<std::string>* notes,
                  const std::string& pictureSpace, std::string* error) {
    const QFileInfo fi(QString::fromStdString(path));
    if (!QDir().mkpath(fi.absolutePath())) {
        if (error) *error = "Cannot make the folder " + fi.absolutePath().toStdString();
        return false;
    }
    AmfDocument a;
    if (!clipAmf(p, seq, clip, fi.absolutePath().toStdString(), a, notes, pictureSpace, error)) return false;
    const std::string xml = writeAmf(a);
    QSaveFile f(fi.absoluteFilePath());
    if (!f.open(QIODevice::WriteOnly) || f.write(xml.data(), qint64(xml.size())) != qint64(xml.size()) || !f.commit()) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    return true;
}

}  // namespace montage
