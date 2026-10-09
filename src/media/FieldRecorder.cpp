#include "FieldRecorder.h"

#include <QFile>
#include <QString>
#include <QRegularExpression>
#include <QStringList>
#include <QXmlStreamReader>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>

namespace montage {

namespace {

uint32_t le32(const char* p) {
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return uint32_t(u[0]) | uint32_t(u[1]) << 8 | uint32_t(u[2]) << 16 | uint32_t(u[3]) << 24;
}
uint64_t le64(const char* p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }

// "25/1", "30000/1001", "25" or "29.97" as a rate.
Rational parseRate(const QString& text) {
    const QStringList nd = text.trimmed().split('/');
    if (nd.size() == 2 && nd[0].toInt() > 0 && nd[1].toInt() > 0) return Rational{nd[0].toInt(), nd[1].toInt()};
    const double v = text.trimmed().toDouble();
    if (v <= 0) return Rational{0, 1};
    for (int base : {24, 30, 60})
        if (std::abs(v - base * 1000.0 / 1001.0) < 0.01) return Rational{base * 1000, 1001};
    return Rational{int(std::lround(v)), 1};
}

// The iXML document: the fields kept, by element path.
void readIxml(const QByteArray& xml, FieldRecording& f, double& stampSamples, int& stampRate) {
    QXmlStreamReader x(xml);
    QStringList path;
    uint64_t hi = 0, lo = 0;
    bool haveHi = false, haveLo = false;
    int trackIndex = -1;
    QString trackName;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            path << x.name().toString().toUpper();
            if (path.last() == QStringLiteral("TRACK")) trackIndex = -1, trackName.clear();
        } else if (x.isEndElement()) {
            if (!path.isEmpty() && path.last() == QStringLiteral("TRACK") && !trackName.isEmpty()) {
                const size_t at = trackIndex > 0 ? size_t(trackIndex - 1) : f.trackNames.size();
                if (f.trackNames.size() <= at) f.trackNames.resize(at + 1);
                f.trackNames[at] = trackName.toStdString();
            }
            if (!path.isEmpty()) path.removeLast();
        } else if (x.isCharacters() && !x.isWhitespace() && !path.isEmpty()) {
            const QString v = x.text().toString().trimmed();
            const QString& tag = path.last();
            const QString parent = path.size() >= 2 ? path[path.size() - 2] : QString();
            if (parent == QStringLiteral("BWFXML")) {
                if (tag == QStringLiteral("PROJECT")) f.project = v.toStdString();
                else if (tag == QStringLiteral("SCENE")) f.scene = v.toStdString();
                else if (tag == QStringLiteral("TAKE")) f.take = v.toStdString();
                else if (tag == QStringLiteral("TAPE")) f.tape = v.toStdString();
                else if (tag == QStringLiteral("NOTE")) f.note = v.toStdString();
                else if (tag == QStringLiteral("CIRCLED")) f.circled = v.compare(QStringLiteral("TRUE"), Qt::CaseInsensitive) == 0;
            } else if (parent == QStringLiteral("SPEED")) {
                if (tag == QStringLiteral("TIMECODE_RATE")) f.timecodeRate = parseRate(v);
                else if (tag == QStringLiteral("TIMECODE_FLAG")) f.dropFrame = v.compare(QStringLiteral("DF"), Qt::CaseInsensitive) == 0;
                else if (tag == QStringLiteral("TIMESTAMP_SAMPLES_SINCE_MIDNIGHT_HI")) hi = v.toULongLong(), haveHi = true;
                else if (tag == QStringLiteral("TIMESTAMP_SAMPLES_SINCE_MIDNIGHT_LO")) lo = v.toULongLong(), haveLo = true;
                else if (tag == QStringLiteral("TIMESTAMP_SAMPLE_RATE")) stampRate = v.toInt();
            } else if (parent == QStringLiteral("TRACK")) {
                if (tag == QStringLiteral("CHANNEL_INDEX")) trackIndex = v.toInt();
                else if (tag == QStringLiteral("NAME")) trackName = v;
            }
        }
    }
    if (haveHi || haveLo) stampSamples = double((hi << 32) | (lo & 0xffffffffu));
}

// bext's description: "sSCENE=12A" lines (Sound Devices and others), and sTRK1=Boom channel names.
void readDescription(const QString& text, FieldRecording& f) {
    std::map<int, std::string> tracks;
    for (QString line : text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts)) {
        line = line.trimmed();
        const int eq = line.indexOf('=');
        if (eq <= 1) continue;
        const QString key = line.left(eq).toUpper(), value = line.mid(eq + 1).trimmed();
        if (key == QStringLiteral("SSCENE")) f.scene = value.toStdString();
        else if (key == QStringLiteral("STAKE")) f.take = value.toStdString();
        else if (key == QStringLiteral("STAPE")) f.tape = value.toStdString();
        else if (key == QStringLiteral("SNOTE")) f.note = value.toStdString();
        else if (key == QStringLiteral("SPROJECT")) f.project = value.toStdString();
        else if (key == QStringLiteral("SCIRCLED")) f.circled = value.compare(QStringLiteral("TRUE"), Qt::CaseInsensitive) == 0;
        else if (key.startsWith(QStringLiteral("STRK")) && key.mid(4).toInt() > 0) tracks[key.mid(4).toInt()] = value.toStdString();
    }
    for (const auto& [i, name] : tracks) {
        if (f.trackNames.size() < size_t(i)) f.trackNames.resize(size_t(i));
        f.trackNames[size_t(i - 1)] = name;
    }
}

}  // namespace

bool readFieldRecording(const std::string& path, FieldRecording& out) {
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly)) return false;
    char head[12];
    if (file.read(head, 12) != 12) return false;
    const bool rf64 = std::memcmp(head, "RF64", 4) == 0 || std::memcmp(head, "BW64", 4) == 0;
    if ((!rf64 && std::memcmp(head, "RIFF", 4) != 0) || std::memcmp(head + 8, "WAVE", 4) != 0) return false;
    FieldRecording f;
    int sampleRate = 0;
    uint64_t dataSize64 = 0;
    double bextSamples = -1, ixmlSamples = -1;
    int ixmlRate = 0;
    QByteArray bext, ixml;
    while (!file.atEnd()) {
        char ch[8];
        if (file.read(ch, 8) != 8) break;
        uint64_t size = le32(ch + 4);
        const qint64 body = file.pos();
        if (std::memcmp(ch, "data", 4) == 0 && rf64 && size == 0xFFFFFFFFu) size = dataSize64;
        if (std::memcmp(ch, "ds64", 4) == 0 && size >= 16) {
            const QByteArray d = file.read(16);
            if (d.size() == 16) dataSize64 = le64(d.constData() + 8);
        } else if (std::memcmp(ch, "fmt ", 4) == 0 && size >= 8) {
            const QByteArray d = file.read(8);
            if (d.size() == 8) sampleRate = int(le32(d.constData() + 4));
        } else if (std::memcmp(ch, "bext", 4) == 0 && size >= 346 && size < (1u << 20)) {
            bext = file.read(qint64(size));
        } else if (std::memcmp(ch, "iXML", 4) == 0 && size < (8u << 20)) {
            ixml = file.read(qint64(size));
        }
        if (!file.seek(body + qint64(size + (size & 1)))) break;  // chunks are padded to even sizes
    }
    if (bext.size() >= 346) {
        readDescription(QString::fromLatin1(bext.constData(), qsizetype(strnlen(bext.constData(), 256))), f);
        bextSamples = double(le64(bext.constData() + 338));
    }
    if (!ixml.isEmpty()) readIxml(ixml, f, ixmlSamples, ixmlRate);
    // The stamp: iXML's when it gives one, else bext's (both count samples at the file's rate unless told otherwise).
    if (ixmlSamples >= 0 && (ixmlRate > 0 || sampleRate > 0)) f.startSeconds = ixmlSamples / double(ixmlRate > 0 ? ixmlRate : sampleRate);
    else if (!bext.isEmpty() && sampleRate > 0) f.startSeconds = bextSamples / double(sampleRate);
    if (!f.any() && bext.isEmpty() && ixml.isEmpty()) return false;
    out = std::move(f);
    return true;
}

void applyFieldRecording(const FieldRecording& f, MediaItem& m) {
    if (m.timecode < 0 && f.startSeconds >= 0) m.timecode = f.startSeconds;
    auto put = [&](const char* key, const std::string& v) {
        if (!v.empty() && m.metadata[key].empty()) m.metadata[key] = v;
        if (m.metadata[key].empty()) m.metadata.erase(key);
    };
    put("scene", f.scene);
    put("take", f.take);
    put("tape", f.tape);
    put("project", f.project);
    put("comment", f.note);
    std::string tracks;
    for (size_t i = 0; i < f.trackNames.size(); ++i)
        tracks += (i ? ", " : "") + (f.trackNames[i].empty() ? "Track " + std::to_string(i + 1) : f.trackNames[i]);
    put("tracks", tracks);
    if (f.circled) put("circled", "Yes");
    if (f.timecodeRate.valid()) put("timecode_rate", std::to_string(f.timecodeRate.num) + "/" + std::to_string(f.timecodeRate.den) + (f.dropFrame ? " DF" : ""));
}

}  // namespace montage
