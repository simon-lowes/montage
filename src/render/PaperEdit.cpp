#include "PaperEdit.h"

#include <algorithm>
#include <cctype>

#include "Highlights.h"

namespace montage {

namespace {

// A word as compared: lower case, letters, digits and apostrophes only.
std::string plain(const std::string& w) {
    std::string out;
    for (unsigned char c : w)
        if (std::isalnum(c) || c == '\'') out += char(std::tolower(c));
    return out;
}

}  // namespace

std::optional<PaperLine> findLine(const Project& p, Id media, const std::string& phrase, double after) {
    const MediaItem* m = p.findMedia(media);
    if (!m || !m->transcript) return std::nullopt;
    std::vector<std::string> want;
    std::string cur;
    for (char c : phrase + " ") {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!plain(cur).empty()) want.push_back(plain(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (want.empty()) return std::nullopt;
    std::vector<const TranscriptWord*> words;
    for (const TranscriptSegment& seg : m->transcript->segments)
        for (const TranscriptWord& w : seg.words)
            if (!plain(w.text).empty()) words.push_back(&w);
    for (size_t i = 0; i + want.size() <= words.size(); ++i) {
        if (words[i]->start < after) continue;
        bool match = true;
        for (size_t k = 0; k < want.size() && match; ++k) match = plain(words[i + k]->text) == want[k];
        if (!match) continue;
        PaperLine line;
        line.media = media;
        line.in = words[i]->start;
        line.out = words[i + want.size() - 1]->end;
        for (size_t k = 0; k < want.size(); ++k) line.text += (k ? " " : "") + words[i + k]->text;
        return line;
    }
    return std::nullopt;
}

Id makePaperEdit(Project& p, const std::vector<PaperLine>& lines, const std::string& name, double handle) {
    std::vector<HighlightMoment> moments;
    for (const PaperLine& l : lines) {
        const MediaItem* m = p.findMedia(l.media);
        if (!m || l.out <= l.in) continue;
        HighlightMoment h;
        h.media = l.media;
        h.in = std::max(0.0, l.in - handle);
        h.out = m->duration > 0 ? std::min(m->duration, l.out + handle) : l.out + handle;
        moments.push_back(h);
    }
    if (moments.empty()) return 0;
    return makeHighlightSequence(p, moments, name);
}

}  // namespace montage
