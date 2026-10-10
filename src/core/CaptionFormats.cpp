// Montage — the caption formats beyond SubRip, WebVTT and SCC: TTML / IMSC,
// EBU STL and Advanced SubStation Alpha, written and read.

#include <QDate>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>

#include "Captions.h"
#include "Model.h"

namespace montage {

namespace {

double frameRate(Rational fps, double fallback) { return fps.valid() ? fps.toDouble() : fallback; }

std::string xmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

int byte01(double v) { return std::clamp(int(std::lround(v * 255)), 0, 255); }

// Lines of a caption, trimmed, without empty ones.
QStringList captionLines(const std::string& text) {
    QStringList lines;
    for (const QString& l : QString::fromStdString(text).split('\n'))
        if (const QString t = l.simplified(); !t.isEmpty()) lines << t;
    return lines;
}

FrameTime toFrames(double seconds, double f) { return FrameTime(std::llround(seconds * f)); }

}  // namespace

// ---- TTML / IMSC 1.1 -------------------------------------------------------------

namespace {

std::string ttmlClock(double seconds) {
    const int64_t ms = std::max<int64_t>(0, std::llround(seconds * 1000));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld.%03lld", static_cast<long long>(ms / 3600000),
                  static_cast<long long>(ms / 60000 % 60), static_cast<long long>(ms / 1000 % 60), static_cast<long long>(ms % 1000));
    return buf;
}

// A TTML time expression in seconds: clock time ("00:00:01.500", "00:00:01:12" with frames) or an
// offset ("1.5s", "1500ms", "36f", "10t", "0.5m", "1h"). -1 if malformed.
double ttmlTime(const QString& text, double frameRate, double subFrameRate, double tickRate) {
    const QString s = text.trimmed();
    static const QRegularExpression clock(QStringLiteral("^(\\d+):(\\d{2}):(\\d{2})(?:(\\.\\d+)|:(\\d+)(?:\\.(\\d+))?)?$"));
    if (const auto m = clock.match(s); m.hasMatch()) {
        double t = m.captured(1).toDouble() * 3600 + m.captured(2).toDouble() * 60 + m.captured(3).toDouble();
        if (!m.captured(4).isEmpty()) t += m.captured(4).toDouble();
        if (!m.captured(5).isEmpty()) t += m.captured(5).toDouble() / frameRate;
        if (!m.captured(6).isEmpty()) t += m.captured(6).toDouble() / (subFrameRate * frameRate);
        return t;
    }
    static const QRegularExpression offset(QStringLiteral("^(\\d+(?:\\.\\d+)?)(h|ms|m|s|f|t)$"));
    if (const auto m = offset.match(s); m.hasMatch()) {
        const double v = m.captured(1).toDouble();
        const QString unit = m.captured(2);
        if (unit == QStringLiteral("h")) return v * 3600;
        if (unit == QStringLiteral("m")) return v * 60;
        if (unit == QStringLiteral("s")) return v;
        if (unit == QStringLiteral("ms")) return v / 1000;
        if (unit == QStringLiteral("f")) return v / frameRate;
        return v / tickRate;
    }
    return -1;
}

// A name without its prefix: TTML has had several namespaces, and files are read without checking them
// (an undeclared prefix is a common slip that should not lose the captions).
QString local(QStringView qualified) { return qualified.mid(qualified.lastIndexOf(':') + 1).toString(); }

QString attr(const QXmlStreamAttributes& attrs, const char* name) {
    for (const QXmlStreamAttribute& a : attrs)
        if (local(a.qualifiedName()) == QLatin1String(name)) return a.value().toString();
    return {};
}

}  // namespace

std::string captionsToTtml(const std::vector<Caption>& captions, Rational fps, const std::string& language, const CaptionStyle& style) {
    const double f = frameRate(fps, 30);
    char colour[16], box[16];
    std::snprintf(colour, sizeof colour, "#%02x%02x%02x", byte01(style.textR), byte01(style.textG), byte01(style.textB));
    std::snprintf(box, sizeof box, "#%02x%02x%02x%02x", byte01(style.boxR), byte01(style.boxG), byte01(style.boxB),
                  byte01(style.boxOpacity));
    // Font size in cells of a 32 x 15 grid; the region ends where the captions' bottom sits.
    const int fontSize = std::clamp(int(std::lround(style.size * 15 * 100)), 20, 300);
    const int bottom = std::clamp(int(std::lround(style.position * 100)), 20, 100);
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:ttp=\"http://www.w3.org/ns/ttml#parameter\" "
           "xmlns:tts=\"http://www.w3.org/ns/ttml#styling\" xmlns:ttm=\"http://www.w3.org/ns/ttml#metadata\" "
           "ttp:timeBase=\"media\" ttp:cellResolution=\"32 15\" "
           "ttp:profile=\"http://www.w3.org/ns/ttml/profile/imsc1.1/text\" xml:lang=\"" +
           xmlEscape(language.empty() ? "en" : language) + "\">\n";
    out += "  <head>\n    <styling>\n";
    out += "      <style xml:id=\"caption\" tts:fontFamily=\"proportionalSansSerif\" tts:fontSize=\"" + std::to_string(fontSize) +
           "%\" tts:lineHeight=\"125%\" tts:textAlign=\"center\" tts:color=\"" + colour + "\"" +
           (style.boxOpacity > 0 ? std::string(" tts:backgroundColor=\"") + box + "\"" : std::string()) +
           (style.bold ? " tts:fontWeight=\"bold\"" : "") + "/>\n";
    out += "      <style xml:id=\"left\" tts:textAlign=\"left\"/>\n      <style xml:id=\"right\" tts:textAlign=\"right\"/>\n";
    out += "    </styling>\n    <layout>\n";
    // Captions sit at the bottom; ones moved to the top keep the same margin from it.
    out += "      <region xml:id=\"bottom\" tts:origin=\"10% 10%\" tts:extent=\"80% " + std::to_string(bottom - 10) +
           "%\" tts:displayAlign=\"after\" tts:textAlign=\"center\"/>\n";
    out += "      <region xml:id=\"top\" tts:origin=\"10% " + std::to_string(100 - bottom) + "%\" tts:extent=\"80% " +
           std::to_string(bottom - 10) + "%\" tts:displayAlign=\"before\" tts:textAlign=\"center\"/>\n";
    out += "      <region xml:id=\"middle\" tts:origin=\"10% 10%\" tts:extent=\"80% 80%\" tts:displayAlign=\"center\" "
           "tts:textAlign=\"center\"/>\n";
    out += "    </layout>\n  </head>\n  <body region=\"bottom\">\n    <div>\n";
    int n = 0;
    for (const Caption& c : captions) {
        const QStringList lines = captionLines(c.text);
        if (lines.isEmpty() || c.end <= c.start) continue;
        std::string body;
        for (int i = 0; i < lines.size(); ++i) body += (i ? "<br/>" : "") + xmlEscape(lines[i].toStdString());
        std::string place;
        if (c.vertical == kCaptionTop) place += " region=\"top\"";
        else if (c.vertical == kCaptionMiddle) place += " region=\"middle\"";
        if (c.align == kCaptionLeft) place += " style=\"left\"";
        else if (c.align == kCaptionRight) place += " style=\"right\"";
        out += "      <p xml:id=\"c" + std::to_string(++n) + "\" begin=\"" + ttmlClock(double(c.start) / f) + "\" end=\"" +
               ttmlClock(double(c.end) / f) + "\"" + place + "><span style=\"caption\">" + body + "</span></p>\n";
    }
    out += "    </div>\n  </body>\n</tt>\n";
    return out;
}

bool parseTtml(const std::string& text, Rational fps, std::vector<Caption>& out, std::string* error) {
    const double f = frameRate(fps, 30);
    QXmlStreamReader x(QByteArray(text.data(), qsizetype(text.size())));
    x.setNamespaceProcessing(false);
    double rate = 30, sub = 1, ticks = 1;
    bool tickSet = false;
    struct Span {
        double begin, end;
        QString region;  // the region its content goes to (inherited)
        int align;       // its text alignment (inherited), -1 if none was given
    };
    std::vector<Span> stack;
    bool inP = false;
    QString buf;
    double pBegin = 0, pEnd = 0;
    Caption placed;  // the paragraph's place
    // Where each region puts text (the third of the frame it lands in) and each style's alignment.
    std::map<QString, int> regionPlace, regionAlign, styleAlign;
    auto alignOf = [](const QString& v) {
        if (v == QStringLiteral("left") || v == QStringLiteral("start")) return int(kCaptionLeft);
        if (v == QStringLiteral("right") || v == QStringLiteral("end")) return int(kCaptionRight);
        return v.isEmpty() ? -1 : int(kCaptionCentre);
    };
    auto percent = [](const QString& pair, int i) {
        const QStringList v = pair.split(' ', Qt::SkipEmptyParts);
        return i < v.size() && v[i].endsWith('%') ? v[i].chopped(1).toDouble() : -1.0;
    };
    bool sawTt = false;
    std::vector<Caption> caps;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            const QString name = local(x.qualifiedName());
            const QXmlStreamAttributes a = x.attributes();
            if (name == QStringLiteral("tt")) {
                sawTt = true;
                if (const QString v = attr(a, "frameRate"); !v.isEmpty()) rate = std::max(1.0, v.toDouble());
                if (const QString v = attr(a, "frameRateMultiplier"); !v.isEmpty()) {
                    const QStringList nd = v.split(' ', Qt::SkipEmptyParts);
                    if (nd.size() == 2 && nd[1].toDouble() > 0) rate *= nd[0].toDouble() / nd[1].toDouble();
                }
                if (const QString v = attr(a, "subFrameRate"); !v.isEmpty()) sub = std::max(1.0, v.toDouble());
                if (const QString v = attr(a, "tickRate"); !v.isEmpty()) ticks = std::max(1.0, v.toDouble()), tickSet = true;
                if (!tickSet && !attr(a, "frameRate").isEmpty()) ticks = rate * sub;
            }
            // Times nest: an element's begin and end count from its parent's begin.
            const double parentBegin = stack.empty() ? 0 : stack.back().begin;
            const double parentEnd = stack.empty() ? std::numeric_limits<double>::infinity() : stack.back().end;
            const QString b = attr(a, "begin"), e = attr(a, "end"), d = attr(a, "dur");
            const double tb = b.isEmpty() ? 0 : ttmlTime(b, rate, sub, ticks);
            const double begin = parentBegin + std::max(0.0, tb);
            double end = parentEnd;
            if (!e.isEmpty() && ttmlTime(e, rate, sub, ticks) >= 0) end = std::min(parentEnd, parentBegin + ttmlTime(e, rate, sub, ticks));
            else if (!d.isEmpty() && ttmlTime(d, rate, sub, ticks) >= 0) end = std::min(parentEnd, begin + ttmlTime(d, rate, sub, ticks));
            const QString id = attr(a, "id");
            if (name == QStringLiteral("region") && !id.isEmpty()) {
                const double y = percent(attr(a, "origin"), 1), h = percent(attr(a, "extent"), 1);
                if (y >= 0) {
                    const QString display = attr(a, "displayAlign");
                    const double at = display == QStringLiteral("after")    ? y + std::max(0.0, h)
                                      : display == QStringLiteral("center") ? y + std::max(0.0, h) / 2
                                                                            : y;
                    regionPlace[id] = at < 33 ? kCaptionTop : at < 67 ? kCaptionMiddle : kCaptionBottom;
                }
                if (const int al = alignOf(attr(a, "textAlign")); al >= 0) regionAlign[id] = al;
            } else if (name == QStringLiteral("style") && !id.isEmpty()) {
                if (const int al = alignOf(attr(a, "textAlign")); al >= 0) styleAlign[id] = al;
            }
            Span span{begin, end, stack.empty() ? QString() : stack.back().region, stack.empty() ? -1 : stack.back().align};
            if (const QString r = attr(a, "region"); !r.isEmpty()) span.region = r;
            for (const QString& st : attr(a, "style").split(' ', Qt::SkipEmptyParts))
                if (auto it = styleAlign.find(st); it != styleAlign.end()) span.align = it->second;
            if (const int al = alignOf(attr(a, "textAlign")); al >= 0) span.align = al;
            stack.push_back(span);
            if (name == QStringLiteral("p")) {
                inP = true;
                buf.clear();
                pBegin = begin;
                pEnd = end;
                placed = {};
                if (auto it = regionPlace.find(span.region); it != regionPlace.end()) placed.vertical = it->second;
                int al = span.align;
                if (al < 0)
                    if (auto it = regionAlign.find(span.region); it != regionAlign.end()) al = it->second;
                if (al >= 0) placed.align = al;
            } else if (name == QStringLiteral("br") && inP) {
                buf += '\n';
            }
        } else if (x.isCharacters() && inP) {
            QString t = x.text().toString();
            t.replace('\n', ' ').replace('\t', ' ').replace('\r', ' ');
            buf += t;
        } else if (x.isEndElement()) {
            if (!stack.empty()) stack.pop_back();
            if (local(x.qualifiedName()) == QStringLiteral("p") && inP) {
                inP = false;
                QStringList lines;
                for (const QString& l : buf.split('\n'))
                    if (const QString s = l.simplified(); !s.isEmpty()) lines << s;
                if (!lines.isEmpty() && std::isfinite(pEnd) && pEnd > pBegin) {
                    Caption c = placed;
                    c.start = toFrames(pBegin, f);
                    c.end = toFrames(pEnd, f);
                    c.text = lines.join('\n').toStdString();
                    caps.push_back(std::move(c));
                }
            }
        }
    }
    normalizeCaptions(caps);
    if (caps.empty()) {
        if (error)
            *error = x.hasError() ? "The TTML file is not well-formed: " + x.errorString().toStdString()
                     : sawTt     ? "No timed captions found in the TTML file"
                                 : "Not a TTML file";
        return false;
    }
    out = std::move(caps);
    return true;
}

// ---- EBU STL (Tech 3264) -----------------------------------------------------------

namespace {

// ISO 6937 (the Latin character code table 00): the characters above 0xA0 that are not accented letters.
const std::map<char32_t, uint8_t>& iso6937Specials() {
    static const std::map<char32_t, uint8_t> m = {
        {U'¡', 0xA1}, {U'¢', 0xA2}, {U'£', 0xA3}, {U'¥', 0xA5}, {U'§', 0xA7}, {U'¤', 0xA8}, {U'‘', 0xA9}, {U'“', 0xAA},
        {U'«', 0xAB}, {U'←', 0xAC}, {U'↑', 0xAD}, {U'→', 0xAE}, {U'↓', 0xAF}, {U'°', 0xB0}, {U'±', 0xB1}, {U'²', 0xB2},
        {U'³', 0xB3}, {U'×', 0xB4}, {U'µ', 0xB5}, {U'¶', 0xB6}, {U'·', 0xB7}, {U'÷', 0xB8}, {U'’', 0xB9}, {U'”', 0xBA},
        {U'»', 0xBB}, {U'¼', 0xBC}, {U'½', 0xBD}, {U'¾', 0xBE}, {U'¿', 0xBF}, {U'―', 0xD0}, {U'—', 0xD0}, {U'¹', 0xD1},
        {U'®', 0xD2}, {U'©', 0xD3}, {U'™', 0xD4}, {U'♪', 0xD5}, {U'¬', 0xD6}, {U'¦', 0xD7}, {U'⅛', 0xDC}, {U'⅜', 0xDD},
        {U'⅝', 0xDE}, {U'⅞', 0xDF}, {U'Ω', 0xE0}, {U'Æ', 0xE1}, {U'Đ', 0xE2}, {U'ª', 0xE3}, {U'Ħ', 0xE4}, {U'Ĳ', 0xE6},
        {U'Ŀ', 0xE7}, {U'Ł', 0xE8}, {U'Ø', 0xE9}, {U'Œ', 0xEA}, {U'º', 0xEB}, {U'Þ', 0xEC}, {U'Ŧ', 0xED}, {U'Ŋ', 0xEE},
        {U'ŉ', 0xEF}, {U'ĸ', 0xF0}, {U'æ', 0xF1}, {U'đ', 0xF2}, {U'ð', 0xF3}, {U'ħ', 0xF4}, {U'ı', 0xF5}, {U'ĳ', 0xF6},
        {U'ŀ', 0xF7}, {U'ł', 0xF8}, {U'ø', 0xF9}, {U'œ', 0xFA}, {U'ß', 0xFB}, {U'þ', 0xFC}, {U'ŧ', 0xFD}, {U'ŋ', 0xFE},
    };
    return m;
}

// The non-spacing diacritics, sent before the letter they go on.
const std::map<char32_t, uint8_t>& iso6937Diacritics() {
    static const std::map<char32_t, uint8_t> m = {
        {0x300, 0xC1}, {0x301, 0xC2}, {0x302, 0xC3}, {0x303, 0xC4}, {0x304, 0xC5}, {0x306, 0xC6}, {0x307, 0xC7},
        {0x308, 0xC8}, {0x30A, 0xCA}, {0x327, 0xCB}, {0x30B, 0xCD}, {0x328, 0xCE}, {0x30C, 0xCF},
    };
    return m;
}

std::string toIso6937(const QString& text) {
    std::string out;
    for (uint u : text.toUcs4()) {
        if (u >= 0x20 && u < 0x7f) {
            out += char(u);
            continue;
        }
        if (auto it = iso6937Specials().find(char32_t(u)); it != iso6937Specials().end()) {
            out += char(it->second);
            continue;
        }
        if (u == 0x2013) {
            out += '-';
            continue;
        }
        if (u == 0x2026) {
            out += "...";
            continue;
        }
        // An accented letter: its diacritic, then the letter.
        const char32_t one = char32_t(u);
        const QList<uint> d = QString::fromUcs4(&one, 1).normalized(QString::NormalizationForm_D).toUcs4();
        if (d.size() == 2 && d[0] < 0x7f)
            if (auto it = iso6937Diacritics().find(char32_t(d[1])); it != iso6937Diacritics().end()) {
                out += char(it->second);
                out += char(d[0]);
                continue;
            }
        if (!d.isEmpty() && d[0] >= 0x20 && d[0] < 0x7f) out += char(d[0]);  // the letter without what it cannot carry
    }
    return out;
}

QString fromIso6937(const std::string& bytes) {
    QString out;
    for (size_t i = 0; i < bytes.size(); ++i) {
        const uint8_t c = uint8_t(bytes[i]);
        if (c == 0x8A) {
            out += '\n';
        } else if (c >= 0x20 && c < 0x7f) {
            out += QChar(char16_t(c));
        } else if (c >= 0xC1 && c <= 0xCF && i + 1 < bytes.size()) {
            // A diacritic on the next letter.
            char32_t mark = 0;
            for (const auto& [m, b] : iso6937Diacritics())
                if (b == c) mark = m;
            const uint8_t base = uint8_t(bytes[++i]);
            if (base >= 0x20 && base < 0x7f) {
                const char32_t pair[2] = {char32_t(base), mark};
                out += mark ? QString::fromUcs4(pair, 2).normalized(QString::NormalizationForm_C) : QString(QChar(char16_t(base)));
            }
        } else if (c >= 0xA0) {
            if (c == 0xA4) out += '$';
            else if (c == 0xA6) out += '#';
            else
                for (const auto& [u, b] : iso6937Specials())
                    if (b == c) {
                        out += QString::fromUcs4(&u, 1);
                        break;
                    }
        }
        // Teletext controls (0x00-0x1f), styles (0x80-0x85) and unused space (0x8f) are not text.
    }
    return out;
}

// EBU's language codes (Tech 3264 appendix 3) for ISO 639-1 codes.
const std::map<std::string, std::string>& ebuLanguages() {
    static const std::map<std::string, std::string> m = {
        {"sq", "01"}, {"br", "02"}, {"ca", "03"}, {"hr", "04"}, {"cy", "05"}, {"cs", "06"}, {"da", "07"}, {"de", "08"},
        {"en", "09"}, {"es", "0A"}, {"eo", "0B"}, {"et", "0C"}, {"eu", "0D"}, {"fo", "0E"}, {"fr", "0F"}, {"fy", "10"},
        {"ga", "11"}, {"gd", "12"}, {"gl", "13"}, {"is", "14"}, {"it", "15"}, {"la", "17"}, {"lv", "18"}, {"lb", "19"},
        {"lt", "1A"}, {"hu", "1B"}, {"mt", "1C"}, {"nl", "1D"}, {"no", "1E"}, {"nb", "1E"}, {"nn", "1E"}, {"oc", "1F"},
        {"pl", "20"}, {"pt", "21"}, {"ro", "22"}, {"rm", "23"}, {"sr", "24"}, {"sk", "25"}, {"sl", "26"}, {"fi", "27"},
        {"sv", "28"}, {"tr", "29"},
    };
    return m;
}

std::string stlTimecodeText(int64_t frames, int rate) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02lld%02lld%02lld%02lld", static_cast<long long>(frames / (3600LL * rate) % 100),
                  static_cast<long long>(frames / (60LL * rate) % 60), static_cast<long long>(frames / rate % 60),
                  static_cast<long long>(frames % rate));
    return buf;
}

void putTimecode(std::string& block, size_t at, int64_t frames, int rate) {
    block[at] = char(frames / (3600LL * rate) % 100);
    block[at + 1] = char(frames / (60LL * rate) % 60);
    block[at + 2] = char(frames / rate % 60);
    block[at + 3] = char(frames % rate);
}

}  // namespace

std::string captionsToStl(const std::vector<Caption>& captions, Rational fps, const std::string& language, const std::string& title) {
    const double f = frameRate(fps, 25);
    const int rate = (std::fabs(f - 25) < 0.5 || std::fabs(f - 50) < 0.5) ? 25 : 30;
    auto stlFrames = [&](FrameTime t) { return int64_t(std::llround(double(t) / f * rate)); };
    // The subtitles' text fields: centred double-height teletext lines, up to three of 35 characters.
    struct Sub {
        int64_t in, out;
        int lines;
        std::string tf;
        int vertical, align;
    };
    std::vector<Sub> subs;
    for (const Caption& c : captions) {
        QStringList lines = captionLines(c.text);
        if (lines.isEmpty() || c.end <= c.start) continue;
        if (lines.size() > 3 || std::any_of(lines.begin(), lines.end(), [](const QString& l) { return l.size() > 35; }))
            lines = captionLines(wrapCaptionText(lines.join(' ').toStdString(), 35, 3));
        while (lines.size() > 3) lines.removeLast();
        std::string tf;
        for (int i = 0; i < lines.size(); ++i) {
            if (i) tf += "\x8A\x8A";
            tf += "\x0D\x0B\x0B" + toIso6937(lines[i]) + "\x0A\x0A";
        }
        subs.push_back({stlFrames(c.start), stlFrames(c.end), int(lines.size()), tf, c.vertical, c.align});
    }
    size_t blocks = 0;
    for (const Sub& s : subs) blocks += std::max<size_t>(1, (s.tf.size() + 111) / 112);

    std::string gsi(1024, ' ');
    auto field = [&gsi](size_t at, size_t len, std::string v) {
        v.resize(len, ' ');
        gsi.replace(at, len, v);
    };
    auto number = [](size_t v, int width) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%0*zu", width, v);
        return std::string(buf);
    };
    const QString today = QDate::currentDate().toString(QStringLiteral("yyMMdd"));
    std::string lang = "00";
    if (auto it = ebuLanguages().find(language.substr(0, 2)); it != ebuLanguages().end()) lang = it->second;
    const std::string name = toIso6937(QString::fromStdString(title));
    field(0, 3, "850");                                  // code page of the GSI block
    field(3, 8, rate == 25 ? "STL25.01" : "STL30.01");   // disk format
    field(11, 1, "1");                                   // display standard: level-1 teletext
    field(12, 2, "00");                                  // character code table: Latin
    field(14, 2, lang);                                  // language
    field(16, 32, name);                                 // original programme title
    field(80, 32, name);                                 // translated programme title
    field(224, 6, today.toStdString());                  // creation date
    field(230, 6, today.toStdString());                  // revision date
    field(236, 2, "00");                                 // revision number
    field(238, 5, number(blocks, 5));                    // TTI blocks
    field(243, 5, number(subs.size(), 5));               // subtitles
    field(248, 3, "001");                                // subtitle groups
    field(251, 2, "40");                                 // characters per row
    field(253, 2, "23");                                 // rows
    field(255, 1, "1");                                  // time codes are for intended use
    field(256, 8, "00000000");                           // start of programme
    field(264, 8, stlTimecodeText(subs.empty() ? 0 : subs.front().in, rate));  // first in-cue
    field(272, 1, "1");                                  // disks
    field(273, 1, "1");                                  // this disk
    std::string out = gsi;
    for (size_t n = 0; n < subs.size(); ++n) {
        const Sub& s = subs[n];
        const size_t parts = std::max<size_t>(1, (s.tf.size() + 111) / 112);
        for (size_t k = 0; k < parts; ++k) {
            std::string tti(128, '\0');
            tti[0] = 0;  // subtitle group
            tti[1] = char(n & 0xff);
            tti[2] = char((n >> 8) & 0xff);
            tti[3] = char(k + 1 == parts ? 0xFF : k);  // extension block number
            tti[4] = 0;                                // cumulative status: none
            putTimecode(tti, 5, s.in, rate);
            putTimecode(tti, 9, s.out, rate);
            // Vertical position: the bottom rows (or the top, or the middle), each line double height.
            tti[13] = char(s.vertical == kCaptionTop      ? 1
                           : s.vertical == kCaptionMiddle ? 12 - (s.lines - 1)
                                                          : std::max(1, 22 - 2 * (s.lines - 1)));
            tti[14] = char(s.align == kCaptionLeft ? 1 : s.align == kCaptionRight ? 3 : 2);  // justification
            tti[15] = 0;                                          // not a comment
            std::string tf = s.tf.substr(k * 112, 112);
            tf.resize(112, '\x8F');
            tti.replace(16, 112, tf);
            out += tti;
        }
    }
    return out;
}

bool parseStl(const std::string& data, Rational fps, std::vector<Caption>& out, std::string* error) {
    if (data.size() < 1024 || data.compare(3, 3, "STL") != 0) {
        if (error) *error = "Not an EBU STL file";
        return false;
    }
    const double f = frameRate(fps, 25);
    int rate = std::atoi(data.substr(6, 2).c_str());
    if (rate <= 0 || rate > 60) rate = 25;
    const bool latin = data.substr(12, 2) == "00" || data.substr(12, 2) == "  ";
    auto seconds = [rate](const uint8_t* tc) { return tc[0] * 3600.0 + tc[1] * 60.0 + tc[2] + double(tc[3]) / rate; };
    double programmeStart = 0;
    {
        const std::string tcp = data.substr(256, 8);
        if (std::all_of(tcp.begin(), tcp.end(), [](char c) { return c >= '0' && c <= '9'; }))
            programmeStart = std::stoi(tcp.substr(0, 2)) * 3600.0 + std::stoi(tcp.substr(2, 2)) * 60.0 + std::stoi(tcp.substr(4, 2)) +
                             double(std::stoi(tcp.substr(6, 2))) / rate;
    }
    struct Sub {
        double in, out;
        std::string tf;
        int row, justification;
    };
    std::vector<Sub> subs;
    int current = -1;
    bool done = true;
    for (size_t off = 1024; off + 128 <= data.size(); off += 128) {
        const auto* b = reinterpret_cast<const uint8_t*>(data.data() + off);
        const int sn = b[1] | (b[2] << 8);
        const uint8_t ebn = b[3];
        if (ebn == 0xFE || b[15] == 1) continue;  // user data, comments
        if (sn != current || done) {
            subs.push_back({seconds(b + 5), seconds(b + 9), {}, b[13], b[14]});
            current = sn;
        }
        subs.back().tf.append(reinterpret_cast<const char*>(b + 16), 112);
        done = ebn == 0xFF;
    }
    // Times count from the start of programme when the subtitles do.
    double minIn = std::numeric_limits<double>::infinity();
    for (const Sub& s : subs) minIn = std::min(minIn, s.in);
    const double zero = (programmeStart > 0 && minIn >= programmeStart) ? programmeStart : 0;
    std::vector<Caption> caps;
    for (const Sub& s : subs) {
        QString text;
        if (latin) {
            text = fromIso6937(s.tf);
        } else {
            for (char c : s.tf) {
                const uint8_t u = uint8_t(c);
                if (u == 0x8A) text += '\n';
                else if (u >= 0x20 && u < 0x7f) text += QChar(char16_t(u));
            }
        }
        QStringList lines;
        for (const QString& l : text.split('\n'))
            if (const QString t = l.simplified(); !t.isEmpty()) lines << t;
        if (lines.isEmpty()) continue;
        Caption c{toFrames(s.in - zero, f), toFrames(s.out - zero, f), lines.join('\n').toStdString(), {}};
        // Rows 0-23: the top few, the middle, the bottom; justification 1 left, 2 centred, 3 right.
        c.vertical = s.row <= 6 ? kCaptionTop : s.row <= 14 ? kCaptionMiddle : kCaptionBottom;
        c.align = s.justification == 1 ? kCaptionLeft : s.justification == 3 ? kCaptionRight : kCaptionCentre;
        caps.push_back(std::move(c));
    }
    normalizeCaptions(caps);
    if (caps.empty()) {
        if (error) *error = "No subtitles found in the STL file";
        return false;
    }
    out = std::move(caps);
    return true;
}

// ---- Advanced SubStation Alpha ---------------------------------------------------

namespace {

std::string assTime(double seconds) {
    const int64_t cs = std::max<int64_t>(0, std::llround(seconds * 100));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld.%02lld", static_cast<long long>(cs / 360000), static_cast<long long>(cs / 6000 % 60),
                  static_cast<long long>(cs / 100 % 60), static_cast<long long>(cs % 100));
    return buf;
}

// &HAABBGGRR, alpha 00 opaque.
std::string assColour(double r, double g, double b, double opacity) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "&H%02X%02X%02X%02X", 255 - byte01(opacity), byte01(b), byte01(g), byte01(r));
    return buf;
}

double assSeconds(const QString& s) {
    static const QRegularExpression re(QStringLiteral("^(\\d+):(\\d{1,2}):(\\d{1,2})(?:[.,](\\d+))?$"));
    const auto m = re.match(s.trimmed());
    if (!m.hasMatch()) return -1;
    double t = m.captured(1).toDouble() * 3600 + m.captured(2).toDouble() * 60 + m.captured(3).toDouble();
    if (!m.captured(4).isEmpty()) t += (QStringLiteral("0.") + m.captured(4)).toDouble();
    return t;
}

}  // namespace

std::string captionsToAss(const std::vector<Caption>& captions, Rational fps, const CaptionStyle& style, int width, int height) {
    const double f = frameRate(fps, 30);
    const int W = width > 0 ? width : 1920, H = height > 0 ? height : 1080;
    const int size = std::max(1, int(std::lround(style.size * H)));
    const bool box = style.boxOpacity > 0;
    const std::string text = assColour(style.textR, style.textG, style.textB, 1);
    const std::string back = box ? assColour(style.boxR, style.boxG, style.boxB, style.boxOpacity) : assColour(0, 0, 0, 0.5);
    const std::string outline = box ? back : assColour(style.outlineR, style.outlineG, style.outlineB, 1);
    // Without a box, ASS draws the shadow in the back colour.
    const std::string shadowColour = assColour(0, 0, 0, style.shadowOpacity);
    const int border = box ? std::max(1, int(std::lround(size * 0.15))) : int(std::lround(style.outline * size));
    std::string font = style.font.empty() ? "Sans Serif" : style.font;
    std::replace(font.begin(), font.end(), ',', ' ');
    std::string out = "[Script Info]\n; Written by Montage\nScriptType: v4.00+\nPlayResX: " + std::to_string(W) +
                      "\nPlayResY: " + std::to_string(H) + "\nWrapStyle: 0\nScaledBorderAndShadow: yes\n\n";
    out += "[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, "
           "Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, "
           "MarginV, Encoding\n";
    const int shadow = box ? 0 : int(std::lround(style.shadow * size));
    out += "Style: Default," + font + "," + std::to_string(size) + "," + text + "," + text + "," + outline + "," +
           (box || shadow == 0 ? back : shadowColour) + "," + (style.bold ? "-1" : "0") + ",0,0,0,100,100,0,0," + (box ? "3" : "1") + "," +
           std::to_string(border) + "," + std::to_string(shadow) + ",2," +
           std::to_string(W / 20) + "," + std::to_string(W / 20) + "," +
           std::to_string(std::max(0, int(std::lround((1 - style.position) * H)))) + ",1\n\n";
    out += "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
    for (const Caption& c : captions) {
        const QStringList lines = captionLines(c.text);
        if (lines.isEmpty() || c.end <= c.start) continue;
        QString t = lines.join(QStringLiteral("\\N"));
        if (style.allCaps) t = t.toUpper();  // the line breaks are \N already
        t.replace('{', '(').replace('}', ')');  // braces open override tags
        if (const int k = captionKeypad(c); k != 2) t.prepend(QStringLiteral("{\\an%1}").arg(k));
        out += "Dialogue: 0," + assTime(double(c.start) / f) + "," + assTime(double(c.end) / f) + ",Default,,0,0,0,," +
               t.toStdString() + "\n";
    }
    return out;
}

bool parseAss(const std::string& text, Rational fps, std::vector<Caption>& out, std::string* error) {
    const double f = frameRate(fps, 30);
    QString all = QString::fromUtf8(text.data(), qsizetype(text.size()));
    if (all.startsWith(QChar(0xFEFF))) all.remove(0, 1);
    QStringList format = {"layer", "start", "end", "style", "name", "marginl", "marginr", "marginv", "effect", "text"};
    QStringList styleFormat;
    std::map<QString, int> styleKeypad;  // each style's alignment
    bool events = false, styles = false, legacy = false;
    static const QRegularExpression an(QStringLiteral("\\\\(an?)(\\d+)"));
    static const QRegularExpression overrides(QStringLiteral("\\{[^}]*\\}"));
    std::vector<Caption> caps;
    for (QString line : all.split('\n')) {
        line = line.trimmed();
        if (line.startsWith('[')) {
            events = line.compare(QStringLiteral("[Events]"), Qt::CaseInsensitive) == 0;
            styles = line.contains(QStringLiteral("Styles]"), Qt::CaseInsensitive);
            if (styles) legacy = !line.contains('+');  // SSA's [V4 Styles] number alignments their own way
            continue;
        }
        if (styles) {
            if (line.startsWith(QStringLiteral("Format:"), Qt::CaseInsensitive)) {
                styleFormat.clear();
                for (const QString& name : line.mid(7).split(',')) styleFormat << name.trimmed().toLower();
            } else if (line.startsWith(QStringLiteral("Style:"), Qt::CaseInsensitive)) {
                const QStringList v = line.mid(6).split(',');
                const int ni = styleFormat.indexOf(QStringLiteral("name")), ai = styleFormat.indexOf(QStringLiteral("alignment"));
                if (ni >= 0 && ai >= 0 && ai < v.size() && ni < v.size())
                    if (const int k = keypadFromAss(v[ai].trimmed().toInt(), legacy)) styleKeypad[v[ni].trimmed()] = k;
            }
            continue;
        }
        if (!events) continue;
        if (line.startsWith(QStringLiteral("Format:"), Qt::CaseInsensitive)) {
            format.clear();
            for (const QString& name : line.mid(7).split(',')) format << name.trimmed().toLower();
            continue;
        }
        if (!line.startsWith(QStringLiteral("Dialogue:"), Qt::CaseInsensitive)) continue;  // comments, pictures, sounds
        const QString rest = line.mid(9);
        // The text is the last field and may hold commas.
        QStringList fields;
        int from = 0;
        for (int i = 0; i + 1 < format.size(); ++i) {
            const int comma = rest.indexOf(',', from);
            if (comma < 0) break;
            fields << rest.mid(from, comma - from).trimmed();
            from = comma + 1;
        }
        fields << rest.mid(from);
        if (fields.size() != format.size()) continue;
        const double a = assSeconds(fields[format.indexOf(QStringLiteral("start"))]);
        const double b = assSeconds(fields[format.indexOf(QStringLiteral("end"))]);
        const int ti = format.indexOf(QStringLiteral("text"));
        if (a < 0 || b <= a || ti < 0) continue;
        QString t = fields[ti];
        // Its place: the style's, or an alignment tag in the text.
        int keypad = 2;
        if (const int si = format.indexOf(QStringLiteral("style")); si >= 0)
            if (auto it = styleKeypad.find(fields[si].trimmed().remove('*')); it != styleKeypad.end()) keypad = it->second;
        for (auto it = overrides.globalMatch(t); it.hasNext();)
            if (const auto m = an.match(it.next().captured()); m.hasMatch())
                if (const int k = keypadFromAss(m.captured(2).toInt(), m.captured(1) == QStringLiteral("a"))) keypad = k;
        t.remove(overrides);
        t.replace(QStringLiteral("\\N"), QStringLiteral("\n")).replace(QStringLiteral("\\n"), QStringLiteral("\n"));
        t.replace(QStringLiteral("\\h"), QStringLiteral(" "));
        QStringList lines;
        for (const QString& l : t.split('\n'))
            if (const QString s = l.simplified(); !s.isEmpty()) lines << s;
        if (lines.isEmpty()) continue;
        Caption c{toFrames(a, f), toFrames(b, f), lines.join('\n').toStdString(), {}};
        setCaptionKeypad(c, keypad);
        caps.push_back(std::move(c));
    }
    normalizeCaptions(caps);
    if (caps.empty()) {
        if (error) *error = "No dialogue found in the ASS file";
        return false;
    }
    out = std::move(caps);
    return true;
}

// ---- By extension ------------------------------------------------------------------

namespace {
std::string formatOf(std::string ext) {
    if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == "xml" || ext == "dfxp" || ext == "imsc" || ext == "ttml") return "ttml";
    if (ext == "ssa" || ext == "ass") return "ass";
    if (ext == "srt" || ext == "vtt" || ext == "scc" || ext == "stl") return ext;
    return {};
}
}  // namespace

bool captionFormatKnown(const std::string& extension) { return !formatOf(extension).empty(); }

std::string exportCaptions(const CaptionTrack& track, const Sequence& seq, const std::string& extension) {
    const std::string f = formatOf(extension);
    if (f == "srt") return captionsToSrt(track.captions, seq.fps);
    if (f == "vtt") return captionsToVtt(track.captions, seq.fps);
    if (f == "scc") return captionsToScc(track.captions, seq.fps);
    if (f == "ttml") return captionsToTtml(track.captions, seq.fps, track.language, track.style);
    if (f == "stl") return captionsToStl(track.captions, seq.fps, track.language, seq.name);
    if (f == "ass") return captionsToAss(track.captions, seq.fps, track.style, seq.width, seq.height);
    return {};
}

}  // namespace montage
