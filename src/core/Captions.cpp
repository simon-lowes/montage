#include "Captions.h"

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "History.h"
#include "Model.h"
#include "Transcript.h"
#include "TranscriptEdit.h"

namespace montage {

const CaptionTrack* captionTrackFor(const Sequence& seq, Id id) {
    for (const CaptionTrack& t : seq.captionTracks)
        if (id ? t.id == id : t.visible) return &t;
    return nullptr;
}

const Caption* captionAt(const CaptionTrack& track, FrameTime t) {
    auto it = std::upper_bound(track.captions.begin(), track.captions.end(), t,
                               [](FrameTime v, const Caption& c) { return v < c.start; });
    if (it == track.captions.begin()) return nullptr;
    --it;
    return t < it->end ? &*it : nullptr;
}

size_t captionIndexAt(const CaptionTrack& track, FrameTime t) {
    auto it = std::lower_bound(track.captions.begin(), track.captions.end(), t,
                               [](const Caption& c, FrameTime v) { return c.end <= v; });
    return size_t(it - track.captions.begin());
}

int captionKeypad(const Caption& c) {
    const int row = c.vertical == kCaptionTop ? 2 : c.vertical == kCaptionMiddle ? 1 : 0;
    const int col = c.align == kCaptionLeft ? 0 : c.align == kCaptionRight ? 2 : 1;
    return row * 3 + col + 1;
}

void setCaptionKeypad(Caption& c, int keypad) {
    if (keypad < 1 || keypad > 9) keypad = 2;
    const int row = (keypad - 1) / 3, col = (keypad - 1) % 3;
    c.vertical = row == 2 ? kCaptionTop : row == 1 ? kCaptionMiddle : kCaptionBottom;
    c.align = col == 0 ? kCaptionLeft : col == 2 ? kCaptionRight : kCaptionCentre;
}

int keypadFromAss(int alignment, bool legacy) {
    if (!legacy) return alignment >= 1 && alignment <= 9 ? alignment : 0;
    // SSA: 1-3 along the bottom, 4 more for the top, 8 more for the middle.
    if (alignment >= 1 && alignment <= 3) return alignment;
    if (alignment >= 5 && alignment <= 7) return alignment + 2;
    if (alignment >= 9 && alignment <= 11) return alignment - 5;
    return 0;
}

std::string captionPlaceName(const Caption& c) {
    std::string name = c.vertical == kCaptionTop ? "top" : c.vertical == kCaptionMiddle ? "middle" : "bottom";
    if (c.align == kCaptionLeft) name += " left";
    else if (c.align == kCaptionRight) name += " right";
    return name;
}

bool parseCaptionPlace(const std::string& words, int& vertical, int& align) {
    QString w = QString::fromStdString(words).toLower();
    w.replace('-', ' ').replace('_', ' ');
    const QStringList parts = w.split(' ', Qt::SkipEmptyParts);
    if (parts.isEmpty()) return false;
    int v = kCaptionBottom, a = kCaptionCentre;
    for (const QString& part : parts) {
        if (part == QStringLiteral("top")) v = kCaptionTop;
        else if (part == QStringLiteral("middle")) v = kCaptionMiddle;
        else if (part == QStringLiteral("bottom") || part == QStringLiteral("default")) v = kCaptionBottom;
        else if (part == QStringLiteral("left")) a = kCaptionLeft;
        else if (part == QStringLiteral("right")) a = kCaptionRight;
        else if (part == QStringLiteral("centre") || part == QStringLiteral("center")) a = kCaptionCentre;
        else return false;
    }
    vertical = v;
    align = a;
    return true;
}

const std::vector<CaptionLook>& captionLooks() {
    static const std::vector<CaptionLook> looks = [] {
        auto look = [](const char* id, const char* name, auto set) {
            CaptionStyle st;
            set(st);
            return CaptionLook{id, name, st};
        };
        auto rgb = [](double& r, double& g, double& b, double R, double G, double B) { r = R, g = G, b = B; };
        return std::vector<CaptionLook>{
            look("classic", "Classic (boxed)", [](CaptionStyle&) {}),
            look("broadcast", "Broadcast (outline and shadow)", [&](CaptionStyle& s) {
                s.boxOpacity = 0;
                s.outline = 0.05;
                s.shadow = 0.06;
            }),
            look("bold_yellow", "Bold Yellow", [&](CaptionStyle& s) {
                s.boxOpacity = 0;
                s.bold = true;
                rgb(s.textR, s.textG, s.textB, 1, 0.86, 0.1);
                s.outline = 0.07;
                s.shadow = 0.05;
            }),
            look("creator_pop", "Creator Pop", [&](CaptionStyle& s) {
                s.boxOpacity = 0;
                s.bold = true;
                s.allCaps = true;
                s.size = 0.075;
                s.position = 0.72;
                s.outline = 0.09;
                s.shadow = 0.05;
                s.animation = 3;  // pop the spoken word
                rgb(s.hiR, s.hiG, s.hiB, 0.3, 1, 0.35);
            }),
            look("karaoke", "Karaoke", [&](CaptionStyle& s) {
                s.boxOpacity = 0;
                s.bold = true;
                s.outline = 0.06;
                s.animation = 2;  // highlight the spoken word
                rgb(s.hiR, s.hiG, s.hiB, 1, 0.84, 0.1);
            }),
            look("one_word", "One Word", [&](CaptionStyle& s) {
                s.boxOpacity = 0;
                s.allCaps = true;
                s.position = 0.6;
                s.outline = 0.08;
                s.animation = 4;
            }),
            look("minimal", "Minimal", [&](CaptionStyle& s) {
                s.boxOpacity = 0;
                s.size = 0.042;
                s.shadow = 0.08;
                s.shadowOpacity = 0.8;
            }),
            look("paper", "Paper (dark on light)", [&](CaptionStyle& s) {
                rgb(s.textR, s.textG, s.textB, 0.08, 0.08, 0.08);
                rgb(s.boxR, s.boxG, s.boxB, 1, 1, 1);
                s.boxOpacity = 0.92;
                s.bold = true;
            }),
            look("neon", "Neon", [&](CaptionStyle& s) {
                s.boxOpacity = 0;
                s.bold = true;
                rgb(s.textR, s.textG, s.textB, 0.35, 0.95, 1);
                rgb(s.outlineR, s.outlineG, s.outlineB, 0.85, 0.1, 0.75);
                s.outline = 0.06;
                s.shadow = 0.04;
            }),
        };
    }();
    return looks;
}

const CaptionLook* findCaptionLook(const std::string& id) {
    for (const CaptionLook& l : captionLooks())
        if (l.id == id) return &l;
    return nullptr;
}

void normalizeCaptions(std::vector<Caption>& captions) {
    for (auto& c : captions) c.text = QString::fromStdString(c.text).trimmed().toStdString();
    captions.erase(std::remove_if(captions.begin(), captions.end(),
                                  [](const Caption& c) { return c.text.empty() || c.end <= c.start; }),
                   captions.end());
    std::stable_sort(captions.begin(), captions.end(), [](const Caption& a, const Caption& b) { return a.start < b.start; });
    for (size_t i = 0; i + 1 < captions.size(); ++i) captions[i].end = std::min(captions[i].end, captions[i + 1].start);
    captions.erase(std::remove_if(captions.begin(), captions.end(), [](const Caption& c) { return c.end <= c.start; }),
                   captions.end());
}

CaptionTrack translatedTrack(const CaptionTrack& source, const std::vector<std::string>& texts, Id id, const std::string& language,
                             const std::string& languageName) {
    CaptionTrack t = source;
    t.id = id;
    t.language = language;
    t.name = source.name + " (" + languageName + ")";
    for (size_t i = 0; i < t.captions.size(); ++i) {
        Caption& c = t.captions[i];
        // Translations run longer than English: three lines rather than cut.
        c.text = wrapCaptionText(i < texts.size() ? texts[i] : c.text, 42, 3);
        c.wordTimes.clear();
    }
    return t;
}

std::vector<std::string> captionTexts(const CaptionTrack& track) {
    std::vector<std::string> out;
    for (const Caption& c : track.captions) out.push_back(QString::fromStdString(c.text).simplified().toStdString());
    return out;
}

std::string wrapCaptionText(const std::string& textIn, int lineChars, int maxLines) {
    const QStringList words =
        QString::fromStdString(textIn).simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.isEmpty()) return {};
    const QString flat = words.join(QLatin1Char(' '));
    if (flat.size() <= lineChars || maxLines <= 1) return flat.toStdString();
    if (maxLines == 2 || flat.size() <= 2 * lineChars) {
        // Two lines: the split that keeps the longer line shortest, preferring
        // a break after punctuation and a shorter top line (a pyramid).
        int best = -1;
        double bestScore = std::numeric_limits<double>::max();
        int topLen = 0;
        for (int i = 0; i + 1 < words.size(); ++i) {
            topLen += int(words[i].size()) + (i ? 1 : 0);
            const int bottomLen = int(flat.size()) - topLen - 1;
            double score = std::max(topLen, bottomLen);
            if (topLen > lineChars || bottomLen > lineChars) score += 1000;
            const QChar last = words[i].back();
            if (last == ',' || last == '.' || last == '?' || last == '!' || last == ';' || last == ':') score -= 4;
            if (topLen > bottomLen) score += 0.5;
            if (score < bestScore) {
                bestScore = score;
                best = i;
            }
        }
        if (best >= 0 && bestScore < 1000)
            return (words.mid(0, best + 1).join(' ') + '\n' + words.mid(best + 1).join(' ')).toStdString();
    }
    // Longer: fill lines greedily.
    QStringList lines;
    QString line;
    for (const QString& w : words) {
        if (!line.isEmpty() && line.size() + 1 + w.size() > lineChars) {
            lines << line;
            line.clear();
        }
        line += (line.isEmpty() ? QString() : QStringLiteral(" ")) + w;
    }
    if (!line.isEmpty()) lines << line;
    return lines.join('\n').toStdString();
}

namespace {

QStringList captionWords(const Caption& c) {
    static const QRegularExpression space(QStringLiteral("\\s+"));
    return QString::fromStdString(c.text).split(space, Qt::SkipEmptyParts);
}

}  // namespace

std::vector<double> captionWordStarts(const Caption& c) {
    const QStringList words = captionWords(c);
    if (words.isEmpty()) return {};
    if (c.wordTimes.size() == size_t(words.size())) return c.wordTimes;
    // Unknown: longer words take longer to say.
    double total = 0;
    for (const QString& w : words) total += double(w.size()) + 1;
    std::vector<double> out;
    double at = 0;
    for (const QString& w : words) {
        out.push_back(at / total);
        at += double(w.size()) + 1;
    }
    return out;
}

int captionWordAt(const Caption& c, FrameTime t) {
    if (t < c.start) return -1;
    const auto starts = captionWordStarts(c);
    if (starts.empty()) return -1;
    const double f = (double(t - c.start) + 0.5) / double(std::max<FrameTime>(1, c.end - c.start));
    int w = 0;
    for (size_t i = 0; i < starts.size(); ++i)
        if (starts[i] <= f) w = int(i);
    return w;
}

std::vector<Caption> captionsFromTranscripts(const Project& p, const Sequence& seq, const CaptionRules& rules) {
    const double fps = seq.fpsValue() > 0 ? seq.fpsValue() : 30.0;
    std::vector<TranscriptWord> words = sequenceTranscriptWords(p, seq);
    if (words.empty()) return {};

    Transcript t;
    TranscriptSegment seg;
    seg.words = std::move(words);
    t.segments.push_back(std::move(seg));
    const auto cues = transcriptCues(t, rules.lineChars * std::max(1, rules.maxLines), rules.maxSeconds);
    std::vector<Caption> out;
    std::vector<std::vector<double>> wordFrames;  // when each of a caption's words starts, in frames
    const auto& all = t.segments.front().words;
    size_t next = 0;
    for (const Cue& c : cues) {
        Caption cap;
        cap.start = FrameTime(std::llround(c.start * fps));
        cap.end = std::max(cap.start + 1, FrameTime(std::llround(c.end * fps)));
        cap.text = wrapCaptionText(c.text, rules.lineChars, rules.maxLines);
        // The cue's words are the next ones of the transcript.
        const int n = int(captionWords(cap).size());
        std::vector<double> frames;
        for (int k = 0; k < n && next < all.size(); ++k, ++next) frames.push_back(all[next].start * fps);
        wordFrames.push_back(int(frames.size()) == n ? frames : std::vector<double>{});
        out.push_back(std::move(cap));
    }
    // Short captions stay up a little longer when there is room.
    const FrameTime minLen = FrameTime(std::llround(rules.minSeconds * fps));
    for (size_t i = 0; i < out.size(); ++i) {
        const FrameTime limit = i + 1 < out.size() ? out[i + 1].start : std::numeric_limits<FrameTime>::max();
        if (out[i].end - out[i].start < minLen) out[i].end = std::min(out[i].start + minLen, limit);
        const double len = double(std::max<FrameTime>(1, out[i].end - out[i].start));
        for (double f : wordFrames[i]) out[i].wordTimes.push_back(std::clamp((f - double(out[i].start)) / len, 0.0, 0.999));
    }
    normalizeCaptions(out);
    return out;
}

namespace {

std::vector<Cue> toCues(const std::vector<Caption>& captions, Rational fps) {
    const double f = fps.valid() ? fps.toDouble() : 30.0;
    std::vector<Cue> cues;
    for (const Caption& c : captions) cues.push_back({double(c.start) / f, double(c.end) / f, c.text, {}, {}});
    return cues;
}

// An ASS alignment tag in subtitle text ({\an8}, or SSA's {\a6}): its keypad digit, 0 for none.
int alignmentTag(const QString& text) {
    static const QRegularExpression re(QStringLiteral("\\\\(an?)(\\d+)"));
    static const QRegularExpression block(QStringLiteral("\\{([^}]*)\\}"));
    for (auto it = block.globalMatch(text); it.hasNext();) {
        const auto m = re.match(it.next().captured(1));
        if (m.hasMatch()) return keypadFromAss(m.captured(2).toInt(), m.captured(1) == QStringLiteral("a"));
    }
    return 0;
}

// Where WebVTT cue settings put a cue ("line:10%", "line:0", "align:left").
void placeFromVtt(const QString& settings, Caption& c) {
    for (const QString& setting : settings.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
        const QString key = setting.section(':', 0, 0), value = setting.section(':', 1).section(',', 0, 0);
        if (key == QStringLiteral("line")) {
            if (value.endsWith('%')) {
                const double pct = value.chopped(1).toDouble();
                c.vertical = pct < 30 ? kCaptionTop : pct < 70 ? kCaptionMiddle : kCaptionBottom;
            } else if (value != QStringLiteral("auto")) {
                const int line = value.toInt();  // lines from the top, or from the bottom when negative
                c.vertical = line < 0 ? kCaptionBottom : line <= 3 ? kCaptionTop : kCaptionMiddle;
            }
        } else if (key == QStringLiteral("align")) {
            c.align = (value == QStringLiteral("left") || value == QStringLiteral("start"))  ? kCaptionLeft
                      : (value == QStringLiteral("right") || value == QStringLiteral("end")) ? kCaptionRight
                                                                                               : kCaptionCentre;
        }
    }
}

// "01:02:03,456", "01:02:03.456" or "02:03.456" in seconds; -1 if malformed.
double parseCueTime(const QString& s) {
    static const QRegularExpression re(QStringLiteral("^(?:(\\d+):)?(\\d{1,2}):(\\d{1,2})[,.](\\d{1,3})$"));
    const auto m = re.match(s.trimmed());
    if (!m.hasMatch()) return -1;
    const double h = m.captured(1).isEmpty() ? 0 : m.captured(1).toDouble();
    const QString ms = m.captured(4).leftJustified(3, '0');
    return h * 3600 + m.captured(2).toDouble() * 60 + m.captured(3).toDouble() + ms.toDouble() / 1000.0;
}

QString stripMarkup(QString s) {
    static const QRegularExpression tags(QStringLiteral("<[^>]*>")), ass(QStringLiteral("\\{\\\\[^}]*\\}"));
    s.remove(tags);
    s.remove(ass);
    s.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    s.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    s.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    s.replace(QStringLiteral("&lrm;"), QString());
    s.replace(QStringLiteral("&rlm;"), QString());
    s.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    return s.trimmed();
}

}  // namespace

std::string captionsToSrt(const std::vector<Caption>& captions, Rational fps) {
    // A caption out of its usual place leads with the {\an} tag players read in SubRip.
    std::vector<Cue> cues = toCues(captions, fps);
    for (size_t i = 0; i < cues.size(); ++i)
        if (const int k = captionKeypad(captions[i]); k != 2) cues[i].text = "{\\an" + std::to_string(k) + "}" + cues[i].text;
    return cuesToSrt(cues);
}

std::string captionsToVtt(const std::vector<Caption>& captions, Rational fps) {
    std::vector<Cue> cues = toCues(captions, fps);
    for (size_t i = 0; i < cues.size(); ++i) {
        const Caption& c = captions[i];
        std::string& s = cues[i].settings;
        if (c.vertical == kCaptionTop) s = "line:10%";
        else if (c.vertical == kCaptionMiddle) s = "line:50%,center";
        if (c.align == kCaptionLeft) s += std::string(s.empty() ? "" : " ") + "position:10% align:left";
        else if (c.align == kCaptionRight) s += std::string(s.empty() ? "" : " ") + "position:90% align:right";
    }
    return cuesToVtt(cues);
}

bool parseSubtitles(const std::string& text, Rational fps, std::vector<Caption>& out, std::string* error) {
    // The other formats, from what they start with.
    if (text.size() >= 1024 && text.compare(3, 3, "STL") == 0) return parseStl(text, fps, out, error);
    const size_t skip = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    const std::string head = text.substr(skip, 4096);
    if (head.rfind("Scenarist_SCC", 0) == 0) return parseScc(text, fps, out, error);
    if (head.find("<tt") != std::string::npos && head.find("-->") == std::string::npos) return parseTtml(text, fps, out, error);
    if (head.find("[Script Info]") != std::string::npos || head.find("[Events]") != std::string::npos) return parseAss(text, fps, out, error);
    const double f = fps.valid() ? fps.toDouble() : 30.0;
    QString all = QString::fromUtf8(text.data(), qsizetype(text.size()));
    if (all.startsWith(QChar(0xFEFF))) all.remove(0, 1);
    all.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace('\r', '\n');
    std::vector<Caption> caps;
    static const QRegularExpression blank(QStringLiteral("\\n\\s*\\n"));
    for (const QString& block : all.split(blank, Qt::SkipEmptyParts)) {
        const QStringList lines = block.split('\n');
        int timing = -1;
        for (int i = 0; i < lines.size() && i < 3; ++i)
            if (lines[i].contains(QStringLiteral("-->"))) {
                timing = i;
                break;
            }
        if (timing < 0) continue;  // header, NOTE, STYLE, REGION, or junk
        const QStringList parts = lines[timing].split(QStringLiteral("-->"));
        if (parts.size() != 2) continue;
        const double a = parseCueTime(parts[0]);
        const double b = parseCueTime(parts[1].trimmed().section(QRegularExpression(QStringLiteral("\\s+")), 0, 0));
        if (a < 0 || b < 0) continue;
        QStringList body;
        int keypad = 0;
        for (int i = timing + 1; i < lines.size(); ++i) {
            if (!keypad) keypad = alignmentTag(lines[i]);
            if (const QString l = stripMarkup(lines[i]); !l.isEmpty()) body << l;
        }
        Caption c;
        c.start = FrameTime(std::llround(a * f));
        c.end = FrameTime(std::llround(b * f));
        c.text = body.join('\n').toStdString();
        placeFromVtt(parts[1].trimmed().section(QRegularExpression(QStringLiteral("\\s+")), 1), c);
        if (keypad) setCaptionKeypad(c, keypad);
        caps.push_back(std::move(c));
    }
    normalizeCaptions(caps);
    if (caps.empty()) {
        if (error) *error = "No captions found (expected SubRip, WebVTT, SCC, TTML, EBU STL or ASS)";
        return false;
    }
    out = std::move(caps);
    return true;
}

// ---- Scenarist SCC (CEA-608) --------------------------------------------------

namespace {

uint8_t withParity(uint8_t c) {
    c &= 0x7f;
    int ones = 0;
    for (int b = 0; b < 7; ++b) ones += (c >> b) & 1;
    return ones % 2 ? c : uint8_t(c | 0x80);
}

using Pair = uint16_t;
Pair pairOf(uint8_t a, uint8_t b) { return Pair(withParity(a) << 8 | withParity(b)); }

// How one character is sent: a basic character, or a two-byte special or
// extended character (extended ones replace the character before them, so
// they follow a basic fallback).
struct Glyph {
    uint8_t basic = 0;  // 0 = none
    uint8_t c1 = 0, c2 = 0;
};

// The special characters (0x11 0x30-0x3f; 0x39 is a transparent space).
const char32_t kSpecial[16] = {U'®', U'°', U'½', U'¿', U'™', U'¢', U'£', U'♪', U'à', 0, U'è', U'â', U'ê', U'î', U'ô', U'û'};

// The extended characters (0x12 / 0x13, 0x20-0x3f), each with the basic character sent before it.
struct Ext {
    char32_t u;
    uint8_t c1, c2;
    char fallback;
};
const Ext kExt[] = {
    {U'Á', 0x12, 0x20, 'A'}, {U'É', 0x12, 0x21, 'E'}, {U'Ó', 0x12, 0x22, 'O'}, {U'Ú', 0x12, 0x23, 'U'},
    {U'Ü', 0x12, 0x24, 'U'}, {U'ü', 0x12, 0x25, 'u'}, {U'‘', 0x12, 0x26, '\''}, {U'¡', 0x12, 0x27, '!'},
    {U'*', 0x12, 0x28, ' '}, {U'’', 0x12, 0x29, '\''}, {U'—', 0x12, 0x2a, '-'}, {U'©', 0x12, 0x2b, 'c'},
    {U'℠', 0x12, 0x2c, ' '}, {U'•', 0x12, 0x2d, '.'}, {U'“', 0x12, 0x2e, '"'}, {U'”', 0x12, 0x2f, '"'},
    {U'À', 0x12, 0x30, 'A'}, {U'Â', 0x12, 0x31, 'A'}, {U'Ç', 0x12, 0x32, 'C'}, {U'È', 0x12, 0x33, 'E'},
    {U'Ê', 0x12, 0x34, 'E'}, {U'Ë', 0x12, 0x35, 'E'}, {U'ë', 0x12, 0x36, 'e'}, {U'Î', 0x12, 0x37, 'I'},
    {U'Ï', 0x12, 0x38, 'I'}, {U'ï', 0x12, 0x39, 'i'}, {U'Ô', 0x12, 0x3a, 'O'}, {U'Ù', 0x12, 0x3b, 'U'},
    {U'ù', 0x12, 0x3c, 'u'}, {U'Û', 0x12, 0x3d, 'U'}, {U'«', 0x12, 0x3e, '"'}, {U'»', 0x12, 0x3f, '"'},
    {U'Ã', 0x13, 0x20, 'A'}, {U'ã', 0x13, 0x21, 'a'}, {U'Í', 0x13, 0x22, 'I'}, {U'Ì', 0x13, 0x23, 'I'},
    {U'ì', 0x13, 0x24, 'i'}, {U'Ò', 0x13, 0x25, 'O'}, {U'ò', 0x13, 0x26, 'o'}, {U'Õ', 0x13, 0x27, 'O'},
    {U'õ', 0x13, 0x28, 'o'}, {U'{', 0x13, 0x29, '('}, {U'}', 0x13, 0x2a, ')'}, {U'\\', 0x13, 0x2b, '/'},
    {U'^', 0x13, 0x2c, ' '}, {U'_', 0x13, 0x2d, '-'}, {U'|', 0x13, 0x2e, '!'}, {U'~', 0x13, 0x2f, '-'},
    {U'Ä', 0x13, 0x30, 'A'}, {U'ä', 0x13, 0x31, 'a'}, {U'Ö', 0x13, 0x32, 'O'}, {U'ö', 0x13, 0x33, 'o'},
    {U'ß', 0x13, 0x34, 's'}, {U'¥', 0x13, 0x35, 'Y'}, {U'¤', 0x13, 0x36, ' '}, {U'¦', 0x13, 0x37, '!'},
    {U'Å', 0x13, 0x38, 'A'}, {U'å', 0x13, 0x39, 'a'}, {U'Ø', 0x13, 0x3a, 'O'}, {U'ø', 0x13, 0x3b, 'o'},
    {U'┌', 0x13, 0x3c, '+'}, {U'┐', 0x13, 0x3d, '+'}, {U'└', 0x13, 0x3e, '+'}, {U'┘', 0x13, 0x3f, '+'},
    {U'`', 0x12, 0x26, '\''},
};

// The basic characters that differ from ASCII.
char32_t basicChar(uint8_t c) {
    switch (c) {
        case 0x2a: return U'á';
        case 0x5c: return U'é';
        case 0x5e: return U'í';
        case 0x5f: return U'ó';
        case 0x60: return U'ú';
        case 0x7b: return U'ç';
        case 0x7c: return U'÷';
        case 0x7d: return U'Ñ';
        case 0x7e: return U'ñ';
        case 0x7f: return U'█';
        default: return char32_t(c);
    }
}

Glyph glyphFor(char32_t u) {
    // Basic characters that match ASCII.
    if (u >= 0x20 && u < 0x7f && u != '*' && u != '\\' && u != '^' && u != '_' && u != '`' && u != '{' && u != '|' &&
        u != '}' && u != '~')
        return {uint8_t(u)};
    switch (u) {
        // Basic set positions that differ from ASCII.
        case U'á': return {0x2a};
        case U'é': return {0x5c};
        case U'í': return {0x5e};
        case U'ó': return {0x5f};
        case U'ú': return {0x60};
        case U'ç': return {0x7b};
        case U'÷': return {0x7c};
        case U'Ñ': return {0x7d};
        case U'ñ': return {0x7e};
        case U'█': return {0x7f};
        default: break;
    }
    for (int i = 0; i < 16; ++i)
        if (kSpecial[i] && kSpecial[i] == u) return {0, 0x11, uint8_t(0x30 + i)};
    for (const Ext& e : kExt)
        if (e.u == u) return {uint8_t(e.fallback), e.c1, e.c2};
    // Anything else: its letters without accents, if that gives plain ASCII.
    const QString folded = QString(QString::fromUcs4(&u, 1)).normalized(QString::NormalizationForm_KD);
    if (!folded.isEmpty() && folded[0].unicode() >= 0x20 && folded[0].unicode() < 0x7f) return glyphFor(folded[0].unicode());
    return {};
}

// The preamble address codes' first byte and base second byte for rows 1-15.
const uint8_t kRowCode[16][2] = {{0, 0},       {0x11, 0x40}, {0x11, 0x60}, {0x12, 0x40}, {0x12, 0x60}, {0x15, 0x40},
                                 {0x15, 0x60}, {0x16, 0x40}, {0x16, 0x60}, {0x17, 0x40}, {0x17, 0x60}, {0x10, 0x40},
                                 {0x13, 0x40}, {0x13, 0x60}, {0x14, 0x40}, {0x14, 0x60}};

// Builds the byte pairs that load one pop-on caption and display it, in its place.
std::vector<Pair> popOnPairs(const Caption& caption) {
    const std::string& text = caption.text;
    std::vector<Pair> out;
    uint8_t pending = 0;  // a basic character waiting for its partner
    auto flushChar = [&] {
        if (pending) out.push_back(Pair(withParity(pending) << 8 | 0x80));
        pending = 0;
    };
    auto control = [&](uint8_t a, uint8_t b) {
        flushChar();
        out.push_back(pairOf(a, b));
        out.push_back(pairOf(a, b));  // control codes are sent twice
    };
    auto basic = [&](uint8_t c) {
        if (pending) {
            out.push_back(Pair(withParity(pending) << 8 | withParity(c)));
            pending = 0;
        } else {
            pending = c;
        }
    };
    control(0x14, 0x20);  // resume caption loading (pop-on)
    control(0x14, 0x2e);  // erase non-displayed memory
    // Up to four rows of 32 characters, at the bottom of the screen (or the top, or the middle).
    // The caption's own lines when they fit, else re-wrapped.
    QStringList rows = QString::fromStdString(text).split('\n', Qt::SkipEmptyParts);
    for (QString& r : rows) r = r.simplified();
    if (rows.size() > 4 || std::any_of(rows.begin(), rows.end(), [](const QString& r) { return r.size() > 32; })) {
        QString flat = QString::fromStdString(text);
        flat.replace('\n', ' ');
        rows = QString::fromStdString(wrapCaptionText(flat.toStdString(), 32, 4)).split('\n');
    }
    QStringList fitted;
    for (QString r : rows) {
        while (r.size() > 32) {
            fitted << r.left(32);
            r = r.mid(32);
        }
        fitted << r;
    }
    while (fitted.size() > 4) fitted.removeFirst();
    const int n = int(fitted.size());
    const int firstRow = caption.vertical == kCaptionTop ? 2 : caption.vertical == kCaptionMiddle ? 8 - n / 2 : 16 - n;
    for (int i = 0; i < n; ++i) {
        const QList<uint> chars = fitted[i].toUcs4();
        const int len = int(chars.size());
        const int col = caption.align == kCaptionLeft    ? 0
                        : caption.align == kCaptionRight ? std::max(0, 32 - len)
                                                         : std::max(0, (32 - len) / 2);
        const int row = firstRow + i;
        control(kRowCode[row][0], uint8_t(kRowCode[row][1] + 0x10 + (col / 4) * 2));  // preamble: row, indent
        if (col % 4) control(0x17, uint8_t(0x20 + col % 4));                        // tab offset
        for (uint u : chars) {
            const Glyph g = glyphFor(char32_t(u));
            if (g.c1) {
                if (g.basic) basic(g.basic);
                control(g.c1, g.c2);
            } else if (g.basic) {
                basic(g.basic);
            }
        }
    }
    control(0x14, 0x2f);  // end of caption: show it
    return out;
}

std::string sccTimecode(int64_t frame) { return formatTimecode(frame, Rational{30000, 1001}, true); }

std::string pairsText(const std::vector<Pair>& pairs) {
    std::string s;
    char buf[8];
    for (size_t i = 0; i < pairs.size(); ++i) {
        std::snprintf(buf, sizeof buf, "%s%04x", i ? " " : "", unsigned(pairs[i]));
        s += buf;
    }
    return s;
}

}  // namespace

std::vector<Cc608Pair> captionsTo608(const std::vector<Caption>& captions, Rational fps) {
    const double f = fps.valid() ? fps.toDouble() : 30.0;
    const double sccRate = 30000.0 / 1001.0;
    auto toScc = [&](FrameTime t) { return int64_t(std::llround(double(t) / f * sccRate)); };
    struct Block {
        std::vector<Pair> pairs;
        int64_t show, clear;
    };
    std::vector<Block> blocks;
    for (const Caption& c : captions) {
        Block b{popOnPairs(c), toScc(c.start), toScc(c.end)};
        if (b.pairs.size() > 4) blocks.push_back(std::move(b));
    }
    std::vector<Cc608Pair> out;
    int64_t cursor = 0;  // first free frame (one pair per frame)
    bool fresh = true;   // the next pair starts a burst
    auto send = [&](int64_t at, Pair pair) {
        out.push_back({at, pair, fresh || at != cursor});
        fresh = false;
        cursor = at + 1;
    };
    const Pair clear = pairOf(0x14, 0x2c);  // erase displayed memory (sent twice)
    int64_t pendingClear = -1;              // when the caption on screen comes off
    auto flushClear = [&](int64_t at) {
        at = std::max(at, cursor);
        send(at, clear);
        send(at + 1, clear);
        pendingClear = -1;
    };
    auto control = [](Pair p) { return ((p >> 8) & 0x7f) >= 0x10 && ((p >> 8) & 0x7f) <= 0x1f; };
    for (const Block& b : blocks) {
        const int64_t n = int64_t(b.pairs.size());
        if (pendingClear >= b.show) pendingClear = -1;  // this caption replaces the one on screen anyway
        // Load early enough for the end-of-caption code (the last two pairs) to land on the start frame: decoders act
        // on the first of a control code's two copies.
        int64_t start = std::max(b.show - (n - 2), cursor);
        if (pendingClear >= 0 && pendingClear < start) {
            flushClear(pendingClear);
            start = std::max(start, cursor);
        }
        fresh = true;
        if (pendingClear >= 0) {
            // The caption on screen comes off while this one loads (loading fills the memory not shown): the erase
            // goes in between, the load starting two frames earlier, never between a control code's two copies.
            int64_t at = std::max(b.show - (n - 2) - 2, cursor);
            for (int64_t k = 0; k < n; ++k) {
                const bool secondCopy = k > 0 && b.pairs[size_t(k)] == b.pairs[size_t(k - 1)] && control(b.pairs[size_t(k)]);
                if (pendingClear >= 0 && at >= pendingClear && !secondCopy && k <= n - 2) {
                    flushClear(at);
                    at = cursor;
                    fresh = true;
                }
                send(at++, b.pairs[size_t(k)]);
            }
            pendingClear = -1;  // shown by now: the new caption replaced it
        } else {
            for (int64_t k = 0; k < n; ++k) send(start + k, b.pairs[size_t(k)]);
        }
        const int64_t shown = cursor - 2;
        if (b.clear > shown) pendingClear = std::max(b.clear, cursor);
        fresh = true;
    }
    if (pendingClear >= 0) flushClear(pendingClear);
    return out;
}

std::vector<Caption> captionsThroughClip(const Clip& c, const std::vector<Caption>& source) {
    std::vector<Caption> out;
    for (const Caption& cap : source) {
        double a = c.localForSource(double(cap.start)), b = c.localForSource(double(cap.end));
        if (a > b) std::swap(a, b);
        a = std::max(a, 0.0);
        b = std::min(b, double(c.duration));
        Caption t = cap;
        t.start = c.start + FrameTime(std::llround(a));
        t.end = c.start + FrameTime(std::llround(b));
        if (t.end > t.start) out.push_back(std::move(t));
    }
    normalizeCaptions(out);
    return out;
}

std::string captionsToScc(const std::vector<Caption>& captions, Rational fps) {
    std::string out = "Scenarist_SCC V1.0\n";
    std::vector<Pair> line;
    int64_t at = 0;
    auto flush = [&] {
        if (!line.empty()) out += "\n" + sccTimecode(at) + "\t" + pairsText(line) + "\n";
        line.clear();
    };
    for (const Cc608Pair& c : captionsTo608(captions, fps)) {
        if (c.lineStart) {
            flush();
            at = c.frame;
        }
        line.push_back(c.pair);
    }
    flush();
    return out;
}

// ---- Reading SCC ----------------------------------------------------------------

namespace {

// A caption memory: 15 rows of up to 32 characters.
struct Screen608 {
    std::u32string rows[16];
    bool empty() const {
        for (const auto& r : rows)
            if (r.find_first_not_of(U' ') != std::u32string::npos) return false;
        return true;
    }
    void clear() {
        for (auto& r : rows) r.clear();
    }
    static QString line(const std::u32string& r) {
        return QString::fromUcs4(r.data(), qsizetype(r.size())).trimmed();
    }
    std::string text() const {
        QStringList lines;
        for (int i = 1; i < 16; ++i)
            if (const QString l = line(rows[i]); !l.isEmpty()) lines << l;
        return lines.join('\n').toStdString();
    }
    // The caption shown, placed by the rows and columns it is on: the top
    // third or the middle, and left or right when its rows are not centred.
    Caption caption(FrameTime start, FrameTime end) const {
        Caption c{start, end, text(), {}};
        int first = 0, last = 0;
        bool centred = true, sameLeft = true, sameRight = true;
        int left0 = -1, right0 = -1;
        for (int i = 1; i < 16; ++i) {
            const size_t a = rows[i].find_first_not_of(U' ');
            if (a == std::u32string::npos) continue;
            const int l = int(a), r = 32 - int(rows[i].find_last_not_of(U' ') + 1);  // margins left and right
            if (!first) first = i, left0 = l, right0 = r;
            last = i;
            centred = centred && std::abs(l - r) <= 4;  // indents come in fours without tab offsets
            sameLeft = sameLeft && l == left0;
            sameRight = sameRight && r == right0;
        }
        if (!first) return c;
        const double middle = (first + last) / 2.0;
        c.vertical = middle <= 5 ? kCaptionTop : middle <= 10 ? kCaptionMiddle : kCaptionBottom;
        if (!centred) {
            if (sameLeft && (!sameRight || left0 < right0)) c.align = kCaptionLeft;
            else if (sameRight) c.align = kCaptionRight;
        }
        return c;
    }
};

int pacRow(uint8_t a, uint8_t b) {
    for (int r = 1; r < 16; ++r)
        if (kRowCode[r][0] == a && kRowCode[r][1] == (b & 0x60)) return r;
    return 0;
}

char32_t extendedChar(uint8_t a, uint8_t b) {
    for (const Ext& e : kExt)
        if (e.c1 == a && e.c2 == b) return e.u;
    return U' ';
}

// The frame a timecode names at 29.97 fps ("01:00:00;00" drop frame, "01:00:00:00" not).
bool sccFrame(const QString& tc, int64_t& out) {
    static const QRegularExpression re(QStringLiteral("^(\\d+):(\\d+):(\\d+)([:;.,])(\\d+)$"));
    const auto m = re.match(tc.trimmed());
    if (!m.hasMatch()) return false;
    const int64_t h = m.captured(1).toLongLong(), mi = m.captured(2).toLongLong(), se = m.captured(3).toLongLong();
    int64_t frames = ((h * 60 + mi) * 60 + se) * 30 + m.captured(5).toLongLong();
    if (m.captured(4) != QStringLiteral(":")) {
        const int64_t minutes = h * 60 + mi;
        frames -= 2 * (minutes - minutes / 10);
    }
    out = frames;
    return true;
}

}  // namespace

bool captionsFrom608(const std::vector<std::pair<double, uint16_t>>& pairs, Rational fps, std::vector<Caption>& out, std::string* error) {
    const double f = fps.valid() ? fps.toDouble() : 30.0;
    auto frames = [f](double seconds) { return FrameTime(std::llround(seconds * f)); };
    enum class Mode { PopOn, PaintOn, RollUp } mode = Mode::PopOn;
    Screen608 shown, loading;
    int row = 15, col = 0, rollRows = 2;
    double since = -1;      // when what is shown appeared (pop-on, paint-on)
    double lineStart = -1;  // when the roll-up line being written began
    std::vector<Caption> caps;
    std::vector<size_t> open;  // roll-up lines still on screen
    auto commit = [&](double at) {
        if (since >= 0 && !shown.empty() && at > since) caps.push_back(shown.caption(frames(since), frames(at)));
        since = -1;
    };
    auto closeOpen = [&](double at) {
        for (size_t i : open) caps[i].end = std::max(caps[i].start + 1, frames(at));
        open.clear();
    };
    auto put = [&](char32_t c, double at) {
        Screen608& m = mode == Mode::PopOn ? loading : shown;
        if (mode == Mode::PaintOn && since < 0) since = at;
        if (mode == Mode::RollUp && lineStart < 0) lineStart = at;
        std::u32string& r = m.rows[row];
        const size_t i = size_t(std::min(col, 31));
        if (r.size() < i) r.resize(i, U' ');
        if (i < r.size()) r[i] = c;
        else r.push_back(c);
        col = std::min(col + 1, 32);
    };
    auto rollLine = [&](double at) {
        // The line just finished stays up until the next one replaces it (or the screen is cleared).
        if (const QString l = Screen608::line(shown.rows[row]); !l.isEmpty() && lineStart >= 0) {
            caps.push_back({frames(lineStart), -1, l.toStdString(), {}});
            open.push_back(caps.size() - 1);
        }
        for (int r = std::max(1, row - rollRows + 1); r < row; ++r) shown.rows[r] = shown.rows[r + 1];
        shown.rows[row].clear();
        col = 0;
        lineStart = -1;
        (void)at;
    };
    auto misc = [&](uint8_t b, double at) {
        switch (b) {
            case 0x20: mode = Mode::PopOn; break;  // resume caption loading
            case 0x21:                             // backspace
                if (col > 0) {
                    --col;
                    std::u32string& r = (mode == Mode::PopOn ? loading : shown).rows[row];
                    if (size_t(col) < r.size()) r.erase(size_t(col), 1);
                }
                break;
            case 0x24: {  // delete to end of row
                std::u32string& r = (mode == Mode::PopOn ? loading : shown).rows[row];
                if (size_t(col) < r.size()) r.resize(size_t(col));
                break;
            }
            case 0x25:
            case 0x26:
            case 0x27:  // roll-up, two to four rows
                if (mode != Mode::RollUp) {
                    commit(at);
                    shown.clear();
                    row = 15;
                }
                mode = Mode::RollUp;
                rollRows = b - 0x23;
                col = 0;
                break;
            case 0x29: mode = Mode::PaintOn; break;  // resume direct captioning
            case 0x2c:                               // erase displayed memory
                commit(at);
                if (mode == Mode::RollUp && lineStart >= 0) rollLine(at);
                closeOpen(at);
                shown.clear();
                lineStart = -1;
                break;
            case 0x2d:  // carriage return
                if (mode == Mode::RollUp) rollLine(at);
                else row = std::min(row + 1, 15), col = 0;
                break;
            case 0x2e: loading.clear(); break;  // erase non-displayed memory
            case 0x2f:                          // end of caption: show what was loaded
                commit(at);
                closeOpen(at);
                std::swap(shown, loading);
                since = shown.empty() ? -1 : at;
                mode = Mode::PopOn;
                break;
            default: break;
        }
    };

    uint16_t lastControl = 0;
    int channel = 1;
    double last = 0;
    const bool any = !pairs.empty();
    for (const auto& [at, w] : pairs) {
        last = at;
        const uint8_t a = uint8_t((w >> 8) & 0x7f), b = uint8_t(w & 0x7f);
        if (a == 0 && b == 0) continue;  // padding
        if (a >= 0x10 && a <= 0x1f) {
            if ((w & 0x7f7f) == lastControl) {  // control codes are sent twice
                lastControl = 0;
                continue;
            }
            lastControl = uint16_t(w & 0x7f7f);
            channel = (a & 0x08) ? 2 : 1;
            if (channel != 1) continue;
            if ((a == 0x14 || a == 0x15) && b >= 0x20 && b <= 0x2f) misc(b, at);
            else if (a == 0x17 && b >= 0x21 && b <= 0x23) col = std::min(col + (b - 0x20), 32);  // tab offset
            else if (b >= 0x40) {
                if (const int r = pacRow(a, b)) {
                    if (mode == Mode::RollUp && r != row) {
                        // The roll-up window moves to the new base row.
                        for (int i = 0; i < rollRows; ++i)
                            if (row - i >= 1 && r - i >= 1) std::swap(shown.rows[r - i], shown.rows[row - i]);
                    }
                    row = r;
                    col = (b & 0x10) ? ((b & 0x0e) >> 1) * 4 : 0;
                }
            } else if (a == 0x11 && b >= 0x20 && b <= 0x2f) {
                put(U' ', at);  // a mid-row style change shows as a space
            } else if (a == 0x11 && b >= 0x30 && b <= 0x3f) {
                put(kSpecial[b - 0x30] ? kSpecial[b - 0x30] : U' ', at);
            } else if ((a == 0x12 || a == 0x13) && b >= 0x20 && b <= 0x3f) {
                col = std::max(0, col - 1);  // replaces the basic character sent before it
                put(extendedChar(a, b), at);
            }
            continue;
        }
        lastControl = 0;
        if (channel != 1) continue;
        if (a >= 0x20) put(basicChar(a), at);
        if (b >= 0x20) put(basicChar(b), at);
    }
    // Whatever is still up at the end stays a few seconds.
    const double end = last + 3;
    commit(end);
    if (mode == Mode::RollUp && lineStart >= 0) rollLine(end);
    closeOpen(end);
    normalizeCaptions(caps);
    if (caps.empty()) {
        if (error) *error = any ? "No captions found in the CEA-608 data" : "No CEA-608 data";
        return false;
    }
    out = std::move(caps);
    return true;
}

bool parseScc(const std::string& text, Rational fps, std::vector<Caption>& out, std::string* error) {
    QString all = QString::fromUtf8(text.data(), qsizetype(text.size()));
    if (all.startsWith(QChar(0xFEFF))) all.remove(0, 1);
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    std::vector<std::pair<double, uint16_t>> pairs;
    bool any = false;
    for (const QString& rawLine : all.split('\n')) {
        const QStringList parts = rawLine.trimmed().split(ws, Qt::SkipEmptyParts);
        int64_t frame = 0;
        if (parts.size() < 2 || !sccFrame(parts[0], frame)) continue;
        any = true;
        for (int k = 1; k < parts.size(); ++k) {
            bool ok = false;
            const uint16_t w = uint16_t(parts[k].toUInt(&ok, 16));
            if (ok) pairs.push_back({double(frame + k - 1) * 1001.0 / 30000.0, w});  // a pair a frame
        }
    }
    if (!any) {
        if (error) *error = "Not a Scenarist SCC file";
        return false;
    }
    if (!captionsFrom608(pairs, fps, out, error)) {
        if (error) *error = "No captions found in the SCC file";
        return false;
    }
    return true;
}

}  // namespace montage
