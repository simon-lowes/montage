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

std::vector<Caption> captionsFromTranscripts(const Project& p, const Sequence& seq, const CaptionRules& rules) {
    const double fps = seq.fpsValue() > 0 ? seq.fpsValue() : 30.0;
    // Every transcribed word heard in the cut, in timeline seconds.
    auto collect = [&](const std::vector<Track>& tracks, bool audio) {
        std::vector<TranscriptWord> words;
        const bool anySolo = audio && std::any_of(tracks.begin(), tracks.end(), [](const Track& t) { return t.solo; });
        for (const Track& tr : tracks) {
            if (tr.muted || (anySolo && !tr.solo)) continue;
            for (const Clip& c : tr.clips) {
                if (!c.enabled || c.isGenerator() || c.speed <= 0) continue;
                const MediaItem* m = p.findMedia(c.mediaId);
                if (!m || !m->transcript) continue;
                const double srcA = c.sourceIn, srcB = c.sourceIn + c.sourceExtent();  // source frames shown
                auto toTimeline = [&](double srcFrame) {
                    const double rel = c.reverse ? (srcB - srcFrame) : (srcFrame - srcA);
                    return double(c.start) + rel / c.speed;
                };
                for (const auto& s : m->transcript->segments)
                    for (const auto& w : s.words) {
                        const double mid = (w.start + w.end) * 0.5 * fps;
                        if (mid < srcA || mid >= srcB) continue;
                        double a = toTimeline(w.start * fps), b = toTimeline(w.end * fps);
                        if (a > b) std::swap(a, b);
                        a = std::clamp(a, double(c.start), double(c.end()));
                        b = std::clamp(b, a, double(c.end()));
                        words.push_back({a / fps, b / fps, w.text, w.probability});
                    }
            }
        }
        std::stable_sort(words.begin(), words.end(),
                         [](const TranscriptWord& x, const TranscriptWord& y) { return x.start < y.start; });
        // The same words from linked or stacked copies of a clip count once.
        std::vector<TranscriptWord> unique;
        for (const auto& w : words) {
            bool dup = false;
            for (auto it = unique.rbegin(); it != unique.rend() && w.start - it->start < 0.15; ++it)
                if (it->text == w.text) dup = true;
            if (!dup) unique.push_back(w);
        }
        return unique;
    };
    std::vector<TranscriptWord> words = collect(seq.audioTracks, true);
    if (words.empty()) words = collect(seq.videoTracks, false);
    if (words.empty()) return {};

    Transcript t;
    TranscriptSegment seg;
    seg.words = std::move(words);
    t.segments.push_back(std::move(seg));
    const auto cues = transcriptCues(t, rules.lineChars * std::max(1, rules.maxLines), rules.maxSeconds);
    std::vector<Caption> out;
    for (const Cue& c : cues) {
        Caption cap;
        cap.start = FrameTime(std::llround(c.start * fps));
        cap.end = std::max(cap.start + 1, FrameTime(std::llround(c.end * fps)));
        cap.text = wrapCaptionText(c.text, rules.lineChars, rules.maxLines);
        out.push_back(std::move(cap));
    }
    // Short captions stay up a little longer when there is room.
    const FrameTime minLen = FrameTime(std::llround(rules.minSeconds * fps));
    for (size_t i = 0; i < out.size(); ++i) {
        const FrameTime limit = i + 1 < out.size() ? out[i + 1].start : std::numeric_limits<FrameTime>::max();
        if (out[i].end - out[i].start < minLen) out[i].end = std::min(out[i].start + minLen, limit);
    }
    normalizeCaptions(out);
    return out;
}

namespace {

std::vector<Cue> toCues(const std::vector<Caption>& captions, Rational fps) {
    const double f = fps.valid() ? fps.toDouble() : 30.0;
    std::vector<Cue> cues;
    for (const Caption& c : captions) cues.push_back({double(c.start) / f, double(c.end) / f, c.text});
    return cues;
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

std::string captionsToSrt(const std::vector<Caption>& captions, Rational fps) { return cuesToSrt(toCues(captions, fps)); }
std::string captionsToVtt(const std::vector<Caption>& captions, Rational fps) { return cuesToVtt(toCues(captions, fps)); }

bool parseSubtitles(const std::string& text, Rational fps, std::vector<Caption>& out, std::string* error) {
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
        for (int i = timing + 1; i < lines.size(); ++i)
            if (const QString l = stripMarkup(lines[i]); !l.isEmpty()) body << l;
        Caption c;
        c.start = FrameTime(std::llround(a * f));
        c.end = FrameTime(std::llround(b * f));
        c.text = body.join('\n').toStdString();
        caps.push_back(std::move(c));
    }
    normalizeCaptions(caps);
    if (caps.empty()) {
        if (error) *error = "No captions found (expected SubRip or WebVTT)";
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
    static const char32_t special[16] = {U'®', U'°', U'½', U'¿', U'™', U'¢', U'£', U'♪',
                                         U'à', 0,     U'è', U'â', U'ê', U'î', U'ô', U'û'};
    for (int i = 0; i < 16; ++i)
        if (special[i] && special[i] == u) return {0, 0x11, uint8_t(0x30 + i)};
    struct Ext {
        char32_t u;
        uint8_t c1, c2;
        char fallback;
    };
    static const Ext ext[] = {
        {U'Á', 0x12, 0x20, 'A'}, {U'É', 0x12, 0x21, 'E'}, {U'Ó', 0x12, 0x22, 'O'}, {U'Ú', 0x12, 0x23, 'U'},
        {U'Ü', 0x12, 0x24, 'U'}, {U'ü', 0x12, 0x25, 'u'}, {U'‘', 0x12, 0x26, '\''}, {U'¡', 0x12, 0x27, '!'},
        {U'*', 0x12, 0x28, ' '}, {U'’', 0x12, 0x29, '\''}, {U'—', 0x12, 0x2a, '-'}, {U'©', 0x12, 0x2b, 'c'},
        {U'•', 0x12, 0x2d, '.'}, {U'“', 0x12, 0x2e, '"'}, {U'”', 0x12, 0x2f, '"'}, {U'À', 0x12, 0x30, 'A'},
        {U'Â', 0x12, 0x31, 'A'}, {U'Ç', 0x12, 0x32, 'C'}, {U'È', 0x12, 0x33, 'E'}, {U'Ê', 0x12, 0x34, 'E'},
        {U'Ë', 0x12, 0x35, 'E'}, {U'ë', 0x12, 0x36, 'e'}, {U'Î', 0x12, 0x37, 'I'}, {U'Ï', 0x12, 0x38, 'I'},
        {U'ï', 0x12, 0x39, 'i'}, {U'Ô', 0x12, 0x3a, 'O'}, {U'Ù', 0x12, 0x3b, 'U'}, {U'ù', 0x12, 0x3c, 'u'},
        {U'Û', 0x12, 0x3d, 'U'}, {U'«', 0x12, 0x3e, '"'}, {U'»', 0x12, 0x3f, '"'}, {U'Ã', 0x13, 0x20, 'A'},
        {U'ã', 0x13, 0x21, 'a'}, {U'Í', 0x13, 0x22, 'I'}, {U'Ì', 0x13, 0x23, 'I'}, {U'ì', 0x13, 0x24, 'i'},
        {U'Ò', 0x13, 0x25, 'O'}, {U'ò', 0x13, 0x26, 'o'}, {U'Õ', 0x13, 0x27, 'O'}, {U'õ', 0x13, 0x28, 'o'},
        {U'{', 0x13, 0x29, '('}, {U'}', 0x13, 0x2a, ')'}, {U'\\', 0x13, 0x2b, '/'}, {U'^', 0x13, 0x2c, ' '},
        {U'_', 0x13, 0x2d, '-'}, {U'|', 0x13, 0x2e, '!'}, {U'~', 0x13, 0x2f, '-'}, {U'Ä', 0x13, 0x30, 'A'},
        {U'ä', 0x13, 0x31, 'a'}, {U'Ö', 0x13, 0x32, 'O'}, {U'ö', 0x13, 0x33, 'o'}, {U'ß', 0x13, 0x34, 's'},
        {U'¥', 0x13, 0x35, 'Y'}, {U'Å', 0x13, 0x38, 'A'}, {U'å', 0x13, 0x39, 'a'}, {U'Ø', 0x13, 0x3a, 'O'},
        {U'ø', 0x13, 0x3b, 'o'}, {U'`', 0x12, 0x26, '\''},
    };
    for (const Ext& e : ext)
        if (e.u == u) return {uint8_t(e.fallback), e.c1, e.c2};
    // Anything else: its letters without accents, if that gives plain ASCII.
    const QString folded = QString(QString::fromUcs4(&u, 1)).normalized(QString::NormalizationForm_KD);
    if (!folded.isEmpty() && folded[0].unicode() >= 0x20 && folded[0].unicode() < 0x7f) return glyphFor(folded[0].unicode());
    return {};
}

// Builds the byte pairs that load one pop-on caption and display it.
std::vector<Pair> popOnPairs(const std::string& text) {
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
    // Up to four rows of 32 characters, at the bottom of the screen.
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
    static const uint8_t rowCode[16][2] = {{0, 0},       {0x11, 0x40}, {0x11, 0x60}, {0x12, 0x40}, {0x12, 0x60}, {0x15, 0x40},
                                           {0x15, 0x60}, {0x16, 0x40}, {0x16, 0x60}, {0x17, 0x40}, {0x17, 0x60}, {0x10, 0x40},
                                           {0x13, 0x40}, {0x13, 0x60}, {0x14, 0x40}, {0x14, 0x60}};
    const int firstRow = 16 - int(fitted.size());
    for (int i = 0; i < fitted.size(); ++i) {
        const QList<uint> chars = fitted[i].toUcs4();
        const int col = std::max(0, (32 - int(chars.size())) / 2);
        const int row = firstRow + i;
        control(rowCode[row][0], uint8_t(rowCode[row][1] + 0x10 + (col / 4) * 2));  // preamble: row, indent
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

std::string captionsToScc(const std::vector<Caption>& captions, Rational fps) {
    const double f = fps.valid() ? fps.toDouble() : 30.0;
    const double sccRate = 30000.0 / 1001.0;
    auto toScc = [&](FrameTime t) { return int64_t(std::llround(double(t) / f * sccRate)); };
    struct Block {
        std::vector<Pair> pairs;
        int64_t show, clear;
    };
    std::vector<Block> blocks;
    for (const Caption& c : captions) {
        Block b{popOnPairs(c.text), toScc(c.start), toScc(c.end)};
        if (b.pairs.size() > 4) blocks.push_back(std::move(b));
    }
    std::string out = "Scenarist_SCC V1.0\n";
    const std::vector<Pair> clearPairs = {pairOf(0x14, 0x2c), pairOf(0x14, 0x2c)};  // erase displayed memory
    int64_t cursor = 0;  // first free frame (one pair per frame)
    for (size_t i = 0; i < blocks.size(); ++i) {
        const Block& b = blocks[i];
        const int64_t n = int64_t(b.pairs.size());
        // Load early enough for the end-of-caption code to land on the start frame.
        const int64_t start = std::max(b.show - (n - 1), cursor);
        out += "\n" + sccTimecode(start) + "\t" + pairsText(b.pairs) + "\n";
        cursor = start + n;
        const int64_t shown = cursor - 1;
        const int64_t nextLoad = i + 1 < blocks.size()
                                     ? blocks[i + 1].show - (int64_t(blocks[i + 1].pairs.size()) - 1)
                                     : std::numeric_limits<int64_t>::max();
        // Clear at the end unless the next caption replaces this one first.
        const int64_t clearAt = std::max(b.clear, cursor);
        if (b.clear > shown && clearAt + 2 <= nextLoad) {
            out += "\n" + sccTimecode(clearAt) + "\t" + pairsText(clearPairs) + "\n";
            cursor = clearAt + 2;
        }
    }
    return out;
}

}  // namespace montage
