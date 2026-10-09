#include "CaptionTools.h"

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace montage {

namespace {

double rate(Rational fps) { return fps.valid() ? fps.toDouble() : 30.0; }

// Characters on screen: line breaks do not count.
int characters(const std::string& text) {
    QString t = QString::fromStdString(text);
    t.remove('\n');
    return int(t.toUcs4().size());
}

std::vector<bool> chosen(size_t n, const std::vector<size_t>& indices) {
    std::vector<bool> on(n, indices.empty());
    for (size_t i : indices)
        if (i < n) on[i] = true;
    return on;
}

std::string number(double v, int decimals) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

}  // namespace

double captionCps(const Caption& c, Rational fps) {
    const double seconds = double(c.end - c.start) / rate(fps);
    return seconds > 0 ? characters(c.text) / seconds : 0.0;
}

std::vector<unsigned> checkCaptions(const std::vector<Caption>& captions, Rational fps, const CaptionLimits& limits) {
    const double f = rate(fps);
    std::vector<unsigned> out(captions.size(), 0);
    for (size_t i = 0; i < captions.size(); ++i) {
        const Caption& c = captions[i];
        unsigned issues = 0;
        if (captionCps(c, fps) > limits.maxCps + 1e-9) issues |= kCaptionTooFast;
        const QStringList lines = QString::fromStdString(c.text).split('\n');
        if (std::any_of(lines.begin(), lines.end(), [&](const QString& l) { return int(l.toUcs4().size()) > limits.maxLineChars; }))
            issues |= kCaptionLineTooLong;
        if (int(lines.size()) > limits.maxLines) issues |= kCaptionTooManyLines;
        const double seconds = double(c.end - c.start) / f;
        if (seconds + 1e-9 < limits.minSeconds) issues |= kCaptionTooShort;
        if (seconds - 1e-9 > limits.maxSeconds) issues |= kCaptionTooLong;
        if (i + 1 < captions.size() && captions[i + 1].start - c.end < limits.minGapFrames) issues |= kCaptionGapTooSmall;
        out[i] = issues;
    }
    return out;
}

std::string describeCaptionIssues(unsigned issues, const Caption& c, Rational fps, const CaptionLimits& limits) {
    std::vector<std::string> parts;
    const double seconds = double(c.end - c.start) / rate(fps);
    if (issues & kCaptionTooFast)
        parts.push_back(number(captionCps(c, fps), 1) + " characters a second (" + number(limits.maxCps, 0) + " at most)");
    if (issues & kCaptionLineTooLong) {
        int longest = 0;
        for (const QString& l : QString::fromStdString(c.text).split('\n')) longest = std::max(longest, int(l.toUcs4().size()));
        parts.push_back("a line of " + std::to_string(longest) + " characters (" + std::to_string(limits.maxLineChars) + " at most)");
    }
    if (issues & kCaptionTooManyLines)
        parts.push_back(std::to_string(QString::fromStdString(c.text).split('\n').size()) + " lines (" + std::to_string(limits.maxLines) +
                        " at most)");
    if (issues & kCaptionTooShort) parts.push_back("on screen " + number(seconds, 2) + " s (" + number(limits.minSeconds, 2) + " s at least)");
    if (issues & kCaptionTooLong) parts.push_back("on screen " + number(seconds, 1) + " s (" + number(limits.maxSeconds, 0) + " s at most)");
    if (issues & kCaptionGapTooSmall)
        parts.push_back("the next one follows too closely (" + std::to_string(limits.minGapFrames) + " frames apart at least)");
    std::string out;
    for (const std::string& p : parts) out += (out.empty() ? "" : "\n") + p;
    return out;
}

int fixCaptionTiming(std::vector<Caption>& captions, Rational fps, const CaptionLimits& limits) {
    const double f = rate(fps);
    const FrameTime gap = std::max(0, limits.minGapFrames);
    const FrameTime chain = FrameTime(std::llround(limits.chainSeconds * f));
    const FrameTime longest = FrameTime(std::llround(limits.maxSeconds * f));
    int changed = 0;
    for (size_t i = 0; i < captions.size(); ++i) {
        Caption& c = captions[i];
        const bool hasNext = i + 1 < captions.size();
        const FrameTime limit = hasNext ? captions[i + 1].start - gap : std::numeric_limits<FrameTime>::max();
        // As long as it needs to be read, within the longest a caption may stay.
        const FrameTime needed = std::max<FrameTime>(FrameTime(std::ceil(limits.minSeconds * f - 1e-9)),
                                                     FrameTime(std::ceil(characters(c.text) / std::max(1.0, limits.maxCps) * f - 1e-9)));
        FrameTime end = c.end;
        if (end - c.start < needed) end = std::max(end, std::min(c.start + needed, c.start + longest));
        end = std::min(end, limit);
        // A short pause before the next caption closes up to the minimum gap (a chained pair reads better).
        if (hasNext && limit - end > 0 && limit + gap - end < chain) end = limit;
        end = std::max(end, c.start + 1);
        if (end != c.end) {
            c.end = end;
            ++changed;
        }
    }
    return changed;
}

bool shiftCaptions(std::vector<Caption>& captions, const std::vector<size_t>& indices, FrameTime delta) {
    const std::vector<bool> on = chosen(captions.size(), indices);
    // Not before the start: the earliest chosen caption stops at frame 0.
    for (size_t i = 0; i < captions.size(); ++i)
        if (on[i]) delta = std::max(delta, -captions[i].start);
    if (delta == 0) return false;
    bool any = false;
    for (size_t i = 0; i < captions.size(); ++i)
        if (on[i]) {
            captions[i].start += delta;
            captions[i].end += delta;
            any = true;
        }
    if (any) normalizeCaptions(captions);
    return any;
}

bool syncCaptions(std::vector<Caption>& captions, FrameTime fromA, FrameTime toA, FrameTime fromB, FrameTime toB) {
    if (fromA == fromB) return false;
    const double scale = double(toB - toA) / double(fromB - fromA);
    auto map = [&](FrameTime t) { return std::max<FrameTime>(0, FrameTime(std::llround(double(toA) + double(t - fromA) * scale))); };
    for (Caption& c : captions) {
        const FrameTime a = map(c.start), b = map(c.end);
        c.start = a;
        c.end = std::max(b, a + 1);
    }
    normalizeCaptions(captions);
    return true;
}

int replaceInCaptions(std::vector<Caption>& captions, const std::vector<size_t>& indices, const std::string& find,
                      const std::string& replace, bool caseSensitive, bool wholeWords) {
    if (find.empty()) return 0;
    QString pattern = QRegularExpression::escape(QString::fromStdString(find));
    if (wholeWords) pattern = QStringLiteral("\\b") + pattern + QStringLiteral("\\b");
    QRegularExpression re(pattern, QRegularExpression::UseUnicodePropertiesOption |
                                       (caseSensitive ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption));
    const QString with = QString::fromStdString(replace);
    const std::vector<bool> on = chosen(captions.size(), indices);
    int count = 0;
    for (size_t i = 0; i < captions.size(); ++i) {
        if (!on[i]) continue;
        QString text = QString::fromStdString(captions[i].text);
        int n = 0;
        for (auto it = re.globalMatch(text); it.hasNext(); it.next()) ++n;
        if (!n) continue;
        // Literal replacement: no backreferences in what the user typed.
        QString out;
        qsizetype from = 0;
        for (auto it = re.globalMatch(text); it.hasNext();) {
            const auto m = it.next();
            out += text.mid(from, m.capturedStart() - from) + with;
            from = m.capturedEnd();
        }
        out += text.mid(from);
        captions[i].text = out.toStdString();
        captions[i].wordTimes.clear();  // the words changed
        count += n;
    }
    if (count) normalizeCaptions(captions);
    return count;
}

}  // namespace montage
