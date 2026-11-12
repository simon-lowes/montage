#include "FieldRecorder.h"

#include <QDateTime>
#include <QFile>
#include <QSaveFile>
#include <QXmlStreamWriter>
#include <QString>
#include <QRegularExpression>
#include <QStringList>
#include <QXmlStreamReader>
#include <algorithm>
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
            } else if (parent == QStringLiteral("USER") || parent == QStringLiteral("ASWG")) {
                f.user[tag.toStdString()] = v.toStdString();
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
        // ASCII by the standard; libraries write UTF-8, read as such where it is.
        const QByteArray raw(bext.constData(), qsizetype(strnlen(bext.constData(), 256)));
        QString description = QString::fromUtf8(raw);
        if (description.contains(QChar::ReplacementCharacter)) description = QString::fromLatin1(raw);
        readDescription(description, f);
        f.description = description.trimmed().toStdString();
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

namespace {

void putLe32(char* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = char((v >> (8 * i)) & 0xff);
}
void putLe64(char* p, uint64_t v) {
    putLe32(p, uint32_t(v & 0xffffffffu));
    putLe32(p + 4, uint32_t(v >> 32));
}
// `text` in a fixed field of `size` bytes, cut short and NUL-padded.
void putText(QByteArray& b, int at, int size, const std::string& text) {
    const int n = std::min(size, int(text.size()));
    std::memcpy(b.data() + at, text.data(), size_t(n));
}
QString rateText(Rational r) { return QStringLiteral("%1/%2").arg(r.num).arg(r.den); }

struct WaveFormat {
    int channels = 0, sampleRate = 0, bits = 0;
};

uint64_t timeReference(const BroadcastStamp& st, const WaveFormat& fmt) {
    return uint64_t(std::max<long long>(0, std::llround(st.seconds * double(fmt.sampleRate))));
}

QByteArray bextBody(const BroadcastStamp& st, const WaveFormat& fmt) {
    QByteArray b(602, '\0');
    putText(b, 0, 256, st.description);
    putText(b, 256, 32, "Montage");
    const QDateTime now = QDateTime::currentDateTime();
    putText(b, 320, 10, now.toString(QStringLiteral("yyyy-MM-dd")).toStdString());
    putText(b, 330, 8, now.toString(QStringLiteral("hh:mm:ss")).toStdString());
    putLe64(b.data() + 338, timeReference(st, fmt));
    b[346] = 1;  // version 1 (with a UMID field, left empty)
    const char* mode = fmt.channels == 1 ? "mono" : fmt.channels == 2 ? "stereo" : "multitrack";
    b += QStringLiteral("A=PCM,F=%1,W=%2,M=%3,T=Montage\r\n").arg(fmt.sampleRate).arg(fmt.bits).arg(QLatin1String(mode)).toLatin1();
    return b;
}

QByteArray ixmlBody(const BroadcastStamp& st, const WaveFormat& fmt) {
    QByteArray out;
    QXmlStreamWriter x(&out);
    x.setAutoFormatting(true);
    x.writeStartDocument();
    x.writeStartElement(QStringLiteral("BWFXML"));
    x.writeTextElement(QStringLiteral("IXML_VERSION"), QStringLiteral("2.10"));
    auto field = [&](const char* tag, const std::string& v) {
        if (!v.empty()) x.writeTextElement(QLatin1String(tag), QString::fromStdString(v));
    };
    field("PROJECT", st.project);
    field("SCENE", st.scene);
    field("TAKE", st.take);
    field("TAPE", st.tape);
    field("NOTE", st.note);
    const uint64_t ref = timeReference(st, fmt);
    x.writeStartElement(QStringLiteral("SPEED"));
    const QString rate = rateText(st.timecodeRate.valid() ? st.timecodeRate : Rational{25, 1});
    x.writeTextElement(QStringLiteral("MASTER_SPEED"), rate);
    x.writeTextElement(QStringLiteral("CURRENT_SPEED"), rate);
    x.writeTextElement(QStringLiteral("TIMECODE_RATE"), rate);
    x.writeTextElement(QStringLiteral("TIMECODE_FLAG"), st.dropFrame ? QStringLiteral("DF") : QStringLiteral("NDF"));
    x.writeTextElement(QStringLiteral("FILE_SAMPLE_RATE"), QString::number(fmt.sampleRate));
    x.writeTextElement(QStringLiteral("AUDIO_BIT_DEPTH"), QString::number(fmt.bits));
    x.writeTextElement(QStringLiteral("TIMESTAMP_SAMPLES_SINCE_MIDNIGHT_HI"), QString::number(ref >> 32));
    x.writeTextElement(QStringLiteral("TIMESTAMP_SAMPLES_SINCE_MIDNIGHT_LO"), QString::number(ref & 0xffffffffu));
    x.writeTextElement(QStringLiteral("TIMESTAMP_SAMPLE_RATE"), QString::number(fmt.sampleRate));
    x.writeEndElement();
    x.writeStartElement(QStringLiteral("BEXT"));
    x.writeTextElement(QStringLiteral("BWF_DESCRIPTION"), QString::fromStdString(st.description));
    x.writeTextElement(QStringLiteral("BWF_ORIGINATOR"), QStringLiteral("Montage"));
    x.writeTextElement(QStringLiteral("BWF_TIME_REFERENCE_LOW"), QString::number(ref & 0xffffffffu));
    x.writeTextElement(QStringLiteral("BWF_TIME_REFERENCE_HIGH"), QString::number(ref >> 32));
    x.writeEndElement();
    x.writeEndElement();
    x.writeEndDocument();
    return out;
}

}  // namespace

bool writeBroadcastStamp(const std::string& path, const BroadcastStamp& stamp, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const QString file = QString::fromStdString(path);
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly)) return fail("Cannot read " + path);
    char head[12];
    if (in.read(head, 12) != 12) return fail("Not a WAV file");
    const bool rf64 = std::memcmp(head, "RF64", 4) == 0 || std::memcmp(head, "BW64", 4) == 0;
    if ((!rf64 && std::memcmp(head, "RIFF", 4) != 0) || std::memcmp(head + 8, "WAVE", 4) != 0) return fail("Not a WAV file");
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly)) return fail("Cannot write " + path);
    out.write(head, 12);
    WaveFormat fmt;
    uint64_t dataSize64 = 0;
    qint64 ds64At = -1;  // where the copy's ds64 body is
    bool wroteStamp = false, sawData = false;
    auto chunk = [&](const char* id, const QByteArray& body) {
        char h[8];
        std::memcpy(h, id, 4);
        putLe32(h + 4, uint32_t(body.size()));
        out.write(h, 8);
        out.write(body);
        if (body.size() & 1) out.write("\0", 1);
    };
    QByteArray buffer(1 << 20, Qt::Uninitialized);
    while (!in.atEnd()) {
        char ch[8];
        if (in.read(ch, 8) != 8) break;
        uint64_t size = le32(ch + 4);
        const bool data = std::memcmp(ch, "data", 4) == 0;
        if (data && rf64 && size == 0xFFFFFFFFu) size = dataSize64;
        const uint64_t padded = size + (size & 1);
        if (std::memcmp(ch, "bext", 4) == 0 || std::memcmp(ch, "iXML", 4) == 0) {  // replaced
            if (!in.seek(in.pos() + qint64(padded))) break;
            continue;
        }
        if (data && !wroteStamp) {
            if (fmt.sampleRate <= 0) return fail("The WAV file has no format before its sound");
            chunk("bext", bextBody(stamp, fmt));
            chunk("iXML", ixmlBody(stamp, fmt));
            wroteStamp = true;
        }
        if (std::memcmp(ch, "ds64", 4) == 0) ds64At = out.pos() + 8;
        if (std::memcmp(ch, "fmt ", 4) == 0 && size >= 16) {
            const QByteArray f = in.peek(16);
            if (f.size() == 16) {
                fmt.channels = int(uint8_t(f[2])) | int(uint8_t(f[3])) << 8;
                fmt.sampleRate = int(le32(f.constData() + 4));
                fmt.bits = int(uint8_t(f[14])) | int(uint8_t(f[15])) << 8;
            }
        }
        if (std::memcmp(ch, "ds64", 4) == 0 && size >= 16) {
            const QByteArray d = in.peek(16);
            if (d.size() == 16) dataSize64 = le64(d.constData() + 8);
        }
        out.write(ch, 8);
        uint64_t left = padded;
        while (left > 0) {  // (a chunk cut short by the file's end is copied as far as it goes)
            const qint64 n = in.read(buffer.data(), qint64(std::min<uint64_t>(left, uint64_t(buffer.size()))));
            if (n <= 0) break;
            if (out.write(buffer.constData(), n) != n) return fail("Cannot write " + path);
            left -= uint64_t(n);
        }
        if (left > 0) break;
        if (data) sawData = true;
    }
    if (!sawData) return fail("The WAV file has no sound");
    const uint64_t riff = uint64_t(out.pos()) - 8;
    char v[8];
    if (rf64) {
        if (ds64At < 0) return fail("The RF64 file has no ds64 chunk");
        putLe64(v, riff);
        out.seek(ds64At);
        out.write(v, 8);
    } else {
        if (riff > 0xFFFFFFFFu) return fail("The WAV file would be larger than 4 GB");
        putLe32(v, uint32_t(riff));
        out.seek(4);
        out.write(v, 4);
    }
    in.close();
    if (!out.commit()) return fail("Cannot write " + path);
    return true;
}

}  // namespace montage
