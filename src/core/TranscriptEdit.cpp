#include "TranscriptEdit.h"

#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <algorithm>
#include <cmath>

namespace montage {

std::vector<TranscriptWord> sequenceTranscriptWords(const Project& p, const Sequence& seq) {
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
                    if (c.ramped()) return double(c.start) + c.localForSource(srcFrame);  // Time Remapping
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

    return words;
}

std::vector<FrameRange> mergeRanges(std::vector<FrameRange> ranges) {
    ranges.erase(std::remove_if(ranges.begin(), ranges.end(), [](const FrameRange& r) { return r.second <= r.first; }),
                 ranges.end());
    std::sort(ranges.begin(), ranges.end());
    std::vector<FrameRange> out;
    for (const auto& r : ranges) {
        if (!out.empty() && r.first <= out.back().second) out.back().second = std::max(out.back().second, r.second);
        else out.push_back(r);
    }
    return out;
}

void rippleCaptions(Sequence& s, FrameTime a, FrameTime b) {
    if (b <= a) return;
    const FrameTime len = b - a;
    for (CaptionTrack& t : s.captionTracks) {
        std::vector<Caption> kept;
        for (Caption c : t.captions) {
            if (c.end <= a) {
                kept.push_back(c);
                continue;
            }
            if (c.start >= b) {
                c.start -= len;
                c.end -= len;
                kept.push_back(c);
                continue;
            }
            // Overlaps the range: keep what is outside it.
            const FrameTime before = std::max<FrameTime>(0, a - c.start), after = std::max<FrameTime>(0, c.end - b);
            if (before + after <= 0) continue;
            c.start = std::min(c.start, a);
            c.end = c.start + before + after;
            kept.push_back(c);
        }
        t.captions = std::move(kept);
        normalizeCaptions(t.captions);
    }
}

edit::Result rippleDeleteRanges(Project& p, Sequence& s, std::vector<FrameRange> ranges) {
    ranges = mergeRanges(std::move(ranges));
    if (ranges.empty()) return edit::Result::fail("Nothing to delete");
    const auto tracks = allTracks(s);
    // From the last range back, so earlier positions stay valid.
    for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) {
        auto r = edit::extractRange(p, s, it->first, it->second, tracks);
        if (!r.ok) return r;
        rippleCaptions(s, it->first, it->second);
    }
    edit::Result res;
    for (const auto& r : ranges) res.applied += r.second - r.first;
    return res;
}

bool isFillerWord(const std::string& word) {
    static const QSet<QString> fillers = {"um", "umm", "ummm", "uh", "uhh", "uhm", "er", "erm", "err", "ah", "ahh",
                                          "hmm", "hm", "mm", "mmm", "mhm", "eh"};
    QString w = QString::fromStdString(word).toLower();
    static const QRegularExpression nonLetters(QStringLiteral("[^\\p{L}]"));
    w.remove(nonLetters);
    return fillers.contains(w);
}

std::vector<FrameRange> fillerWordRanges(const std::vector<TranscriptWord>& words, double fps) {
    std::vector<FrameRange> out;
    for (size_t i = 0; i < words.size(); ++i) {
        if (!isFillerWord(words[i].text)) continue;
        double end = words[i].end;
        // Take the short breath after the filler too, so the next word starts cleanly.
        if (i + 1 < words.size() && words[i + 1].start - end < 0.25) end = words[i + 1].start;
        out.emplace_back(FrameTime(std::llround(words[i].start * fps)), FrameTime(std::llround(end * fps)));
    }
    return mergeRanges(out);
}

std::vector<FrameRange> pauseRanges(const std::vector<TranscriptWord>& words, double fps, double minPause, double keep) {
    std::vector<FrameRange> out;
    for (size_t i = 0; i + 1 < words.size(); ++i) {
        const double gap = words[i + 1].start - words[i].end;
        if (gap <= minPause) continue;
        const FrameTime a = FrameTime(std::ceil((words[i].end + keep / 2) * fps));
        const FrameTime b = FrameTime(std::floor((words[i + 1].start - keep / 2) * fps));
        if (b > a) out.emplace_back(a, b);
    }
    return mergeRanges(out);
}

}  // namespace montage
