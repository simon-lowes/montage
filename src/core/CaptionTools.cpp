#include "CaptionTools.h"

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>

#include "EditOps.h"

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

// Frames a caption needs on screen to be read: the shortest a caption may be, or its characters at the most a second.
FrameTime framesToRead(const Caption& c, double f, const CaptionLimits& limits) {
    return std::max<FrameTime>(FrameTime(std::ceil(limits.minSeconds * f - 1e-9)),
                               FrameTime(std::ceil(characters(c.text) / std::max(1.0, limits.maxCps) * f - 1e-9)));
}

// When the caption's speech ends, from its word times (the last word's start and its letters at the reading speed); -1
// when its word times are not known.
double speechEnd(const Caption& c, double f, const CaptionLimits& limits) {
    const QStringList words = QString::fromStdString(c.text).simplified().split(' ', Qt::SkipEmptyParts);
    if (words.isEmpty() || c.wordTimes.size() != size_t(words.size())) return -1;
    const double last = double(c.start) + c.wordTimes.back() * double(c.end - c.start);
    return last + double(words.back().toUcs4().size()) / std::max(1.0, limits.maxCps) * f;
}

// The shot change at or before `t` and the one after it (-1 and max when there is none).
std::pair<FrameTime, FrameTime> shotChangesAround(const std::vector<FrameTime>& cuts, FrameTime t) {
    const auto it = std::upper_bound(cuts.begin(), cuts.end(), t);
    return {it == cuts.begin() ? FrameTime(-1) : *(it - 1), it == cuts.end() ? std::numeric_limits<FrameTime>::max() : *it};
}

}  // namespace

int shotChangeZone(Rational fps, const CaptionLimits& limits) {
    if (!(limits.shotZoneSeconds > 0)) return 0;
    return std::max(1, int(std::floor(std::min(limits.shotZoneSeconds, 10.0) * rate(fps) + 0.5 - 1e-6)));
}

std::vector<FrameTime> captionShotChanges(const Sequence& seq) {
    const FrameTime last = seq.duration();
    std::set<FrameTime> out;
    const std::vector<Id> through = edit::throughEdits(seq);
    auto picture = [](const Clip& c) { return c.enabled && !c.isGenerator(); };
    for (size_t ti = 0; ti < seq.videoTracks.size(); ++ti) {
        const Track& t = seq.videoTracks[ti];
        if (t.muted) continue;
        // (edits the picture runs straight through or dissolves across)
        std::set<FrameTime> smooth;
        for (const Clip& c : t.clips)
            if (std::find(through.begin(), through.end(), c.id) != through.end()) smooth.insert(c.end());
        for (const Transition& tr : t.transitions) {
            if (const Clip* b = tr.clipB ? edit::clipById(seq, tr.clipB) : nullptr) smooth.insert(b->start);
            else if (const Clip* a = tr.clipA ? edit::clipById(seq, tr.clipA) : nullptr) smooth.insert(a->end());
        }
        auto hidden = [&](FrameTime f) {
            for (size_t up = ti + 1; up < seq.videoTracks.size(); ++up) {
                if (seq.videoTracks[up].muted) continue;
                for (const Clip& c : seq.videoTracks[up].clips)
                    if (picture(c) && c.start <= f - 1 && f < c.end()) return true;
            }
            return false;
        };
        for (const Clip& c : t.clips) {
            if (!picture(c)) continue;
            for (const FrameTime f : {c.start, c.end()})
                if (f > 0 && f < last && !smooth.count(f) && !hidden(f)) out.insert(f);
        }
    }
    for (const Marker& m : seq.markers)
        if (m.shotChange && m.t > 0) out.insert(m.t);  // (wherever it is: captions may run on past the picture)
    return {out.begin(), out.end()};
}

double captionCps(const Caption& c, Rational fps) {
    const double seconds = double(c.end - c.start) / rate(fps);
    return seconds > 0 ? characters(c.text) / seconds : 0.0;
}

std::vector<unsigned> checkCaptions(const std::vector<Caption>& captions, Rational fps, const CaptionLimits& limits,
                                    const std::vector<FrameTime>& shotChanges) {
    const double f = rate(fps);
    const FrameTime zone = shotChanges.empty() ? 0 : shotChangeZone(fps, limits), gap = std::max(0, limits.minGapFrames);
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
        if (zone > 0) {
            // Starting on a shot change, or half a second (the zone) from one; ending the gap before one, or the zone
            // from one; across one only where the speech goes on.
            const auto [before, after] = shotChangesAround(shotChanges, c.start);
            if ((before >= 0 && c.start != before && c.start - before < zone) || after - c.start < zone) issues |= kCaptionNearShotChange;
            for (auto it = std::lower_bound(shotChanges.begin(), shotChanges.end(), c.end - zone + 1);
                 it != shotChanges.end() && *it < c.end + zone; ++it)
                if (*it > c.start && c.end != *it - gap) issues |= kCaptionNearShotChange;
            const double spoken = speechEnd(c, f, limits);
            for (auto it = std::lower_bound(shotChanges.begin(), shotChanges.end(), c.start + zone); it != shotChanges.end() && *it + zone <= c.end; ++it)
                if (spoken < 0 || spoken <= double(*it)) issues |= kCaptionCrossesShotChange;
        }
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
    if (issues & kCaptionNearShotChange)
        parts.push_back("starts or ends within " + std::to_string(shotChangeZone(fps, limits)) + " frames of a shot change (start on it, or end " +
                        std::to_string(limits.minGapFrames) + " frames before it)");
    if (issues & kCaptionCrossesShotChange) parts.push_back("stays up across a shot change (keep it there only if the speech does)");
    std::string out;
    for (const std::string& p : parts) out += (out.empty() ? "" : "\n") + p;
    return out;
}

int fixCaptionTiming(std::vector<Caption>& captions, Rational fps, const CaptionLimits& limits, const std::vector<FrameTime>& shotChanges) {
    const double f = rate(fps);
    const FrameTime gap = std::max(0, limits.minGapFrames);
    const FrameTime chain = FrameTime(std::llround(limits.chainSeconds * f));
    const FrameTime longest = FrameTime(std::llround(limits.maxSeconds * f));
    const FrameTime zone = shotChanges.empty() ? 0 : shotChangeZone(fps, limits);
    const std::vector<Caption> before = captions;
    // Starts within the zone after a shot change go back to it (the caption before ending the gap before it).
    if (zone > 0)
        for (size_t i = 0; i < captions.size(); ++i) {
            Caption& c = captions[i];
            const FrameTime cut = shotChangesAround(shotChanges, c.start).first;
            if (cut < 0 || c.start == cut || c.start - cut >= zone || c.end - cut > longest) continue;
            if (i > 0 && captions[i - 1].end > cut - gap) {
                Caption& prev = captions[i - 1];
                // (not when the one before would be left too short to read)
                if (cut - gap - prev.start < std::min(framesToRead(prev, f, limits), prev.end - prev.start)) continue;
                prev.end = cut - gap;
            }
            const double length = double(c.end - c.start);
            for (double& w : c.wordTimes) w = (double(c.start - cut) + w * length) / double(c.end - cut);  // (the words stay where they are said)
            c.start = cut;
        }
    for (size_t i = 0; i < captions.size(); ++i) {
        Caption& c = captions[i];
        const bool hasNext = i + 1 < captions.size();
        const FrameTime limit = hasNext ? captions[i + 1].start - gap : std::numeric_limits<FrameTime>::max();
        // As long as it needs to be read, within the longest a caption may stay.
        const FrameTime needed = framesToRead(c, f, limits);
        FrameTime end = c.end;
        if (end - c.start < needed) end = std::max(end, std::min(c.start + needed, c.start + longest));
        end = std::min(end, limit);
        // A short pause before the next caption closes up to the minimum gap (a chained pair reads better).
        if (hasNext && limit - end > 0 && limit + gap - end < chain) end = limit;
        // (not lengthened up to or over a shot change: to the gap before it at most)
        if (zone > 0 && end > c.end)
            if (const FrameTime cut = shotChangesAround(shotChanges, c.end).second; end > cut - gap) end = std::max(c.end, cut - gap);
        end = std::max(end, c.start + 1);
        c.end = end;
    }
    // Ends: to the gap before the shot change they are near, or the speech has finished by.
    if (zone > 0)
        for (size_t i = 0; i < captions.size(); ++i) {
            Caption& c = captions[i];
            const FrameTime limit = i + 1 < captions.size() ? captions[i + 1].start - gap : std::numeric_limits<FrameTime>::max();
            const double spoken = speechEnd(c, f, limits);
            FrameTime to = -1;
            for (auto it = std::lower_bound(shotChanges.begin(), shotChanges.end(), c.start + 1); it != shotChanges.end() && *it < c.end + zone; ++it) {
                const FrameTime cut = *it, at = cut - gap;
                if (c.end == at) break;
                if (c.end < at) {
                    // (ending within the zone before it: on to the gap before it)
                    if (cut - c.end < zone && at <= limit && at - c.start <= longest) to = at;
                    break;
                }
                // Ending on it, within the zone after it, or long after it with the speech over by it: back to the gap before it.
                const bool near = c.end < cut + zone, spokenBy = spoken >= 0 && spoken <= double(cut) && c.start + zone <= cut;
                if ((near || spokenBy) && at - c.start >= framesToRead(c, f, limits) && at > c.start) {
                    to = at;
                    break;
                }
            }
            if (to >= 0 && to != c.end) {
                const double length = double(to - c.start);
                for (double& w : c.wordTimes) w = std::min(1.0, w * double(c.end - c.start) / length);
                c.end = to;
            }
        }
    int changed = 0;
    for (size_t i = 0; i < captions.size(); ++i) changed += captions[i].start != before[i].start || captions[i].end != before[i].end;
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

bool placeCaptions(std::vector<Caption>& captions, const std::vector<size_t>& indices, int vertical, int align) {
    const std::vector<bool> on = chosen(captions.size(), indices);
    bool any = false;
    for (size_t i = 0; i < captions.size(); ++i)
        if (on[i]) {
            Caption& c = captions[i];
            const int v = vertical < 0 ? c.vertical : vertical, a = align < 0 ? c.align : align;
            if (c.vertical == v && c.align == a) continue;
            c.vertical = v;
            c.align = a;
            any = true;
        }
    return any;
}

int raiseCaptionsOverTitles(std::vector<Caption>& captions, const Sequence& seq) {
    // Where low titles are on screen.
    std::vector<std::pair<FrameTime, FrameTime>> spans;
    for (const Track& track : seq.videoTracks) {
        if (track.muted) continue;
        for (const Clip& c : track.clips) {
            if (!c.enabled || (c.generator.type != "title" && c.generator.type != "title3d")) continue;
            // Lower Left, Centre or Right, or placed (with the clip's own position) below the middle fifth.
            const int anchor = int(std::lround(c.generator.p("anchor", 0)));
            const double y = c.generator.p("pos_y", 0) + c.motion.p("pos_y", 0);
            const bool low = (anchor >= 1 && anchor <= 3) || (anchor == 0 && y > 0.1 * seq.height);
            if (low && int(std::lround(c.generator.p("motion", 0))) != 1) spans.emplace_back(c.start, c.end());
        }
    }
    int moved = 0;
    for (Caption& cap : captions) {
        if (cap.vertical == kCaptionTop) continue;
        const bool covered = std::any_of(spans.begin(), spans.end(), [&](const auto& s) { return s.first < cap.end && cap.start < s.second; });
        if (covered) {
            cap.vertical = kCaptionTop;
            ++moved;
        }
    }
    return moved;
}

}  // namespace montage
