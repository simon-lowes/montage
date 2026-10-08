#include "AutoBroll.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "core/TranscriptEdit.h"
#include "core/VisualIndex.h"

namespace montage {

namespace {

struct Sentence {
    std::string text;
    double start = 0, end = 0;
};

bool endsSentence(const std::string& w) {
    for (auto it = w.rbegin(); it != w.rend(); ++it) {
        if (*it == '.' || *it == '!' || *it == '?') return true;
        if (std::isalnum(static_cast<unsigned char>(*it))) return false;
    }
    return false;
}

// Sentences by their closing punctuation, or a pause of most of a second.
std::vector<Sentence> sentencesOf(const std::vector<TranscriptWord>& words) {
    std::vector<Sentence> out;
    Sentence cur;
    for (size_t i = 0; i < words.size(); ++i) {
        const TranscriptWord& w = words[i];
        if (cur.text.empty()) cur.start = w.start;
        else cur.text += ' ';
        cur.text += w.text;
        cur.end = w.end;
        const bool pause = i + 1 < words.size() && words[i + 1].start - w.end > 0.8;
        if (endsSentence(w.text) || pause || i + 1 == words.size()) {
            out.push_back(cur);
            cur = {};
        }
    }
    return out;
}

}  // namespace

std::vector<BrollPick> planBroll(const Project& p, const Sequence& s, const std::vector<Id>& candidates,
                                 const std::function<std::vector<float>(const std::string&)>& embed, const BrollOptions& o,
                                 std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return std::vector<BrollPick>{};
    };
    const std::vector<Sentence> sentences = sentencesOf(sequenceTranscriptWords(p, s));
    if (sentences.empty()) return fail("Nothing in the sequence is transcribed: transcribe the dialogue first");
    const double fps = s.fpsValue();
    struct Best {
        size_t sentence;
        ShotMatch shot;
    };
    std::vector<Best> best;
    for (size_t i = 0; i < sentences.size(); ++i) {
        const Sentence& st = sentences[i];
        if (st.end - st.start < o.minSeconds) continue;
        const std::vector<float> q = embed(st.text);
        if (q.empty()) continue;
        for (const ShotMatch& m : findShots(p, q, 40))
            if (std::find(candidates.begin(), candidates.end(), Id(m.media)) != candidates.end() && m.score >= o.minScore) {
                best.push_back({i, m});
                break;
            }
    }
    if (best.empty()) return fail("None of the footage looks like what is being said (index the footage first)");
    // The best-matching sentences, as many as the coverage allows, then in timeline order.
    std::sort(best.begin(), best.end(), [](const Best& a, const Best& b) { return a.shot.score > b.shot.score; });
    const size_t eligible = size_t(std::count_if(sentences.begin(), sentences.end(), [&](const Sentence& x) { return x.end - x.start >= o.minSeconds; }));
    const size_t keep = std::max<size_t>(1, size_t(std::lround(std::clamp(o.coverage, 0.0, 1.0) * double(eligible))));
    if (best.size() > keep) best.resize(keep);
    std::sort(best.begin(), best.end(), [](const Best& a, const Best& b) { return a.sentence < b.sentence; });
    std::vector<BrollPick> out;
    for (const Best& b : best) {
        const Sentence& st = sentences[b.sentence];
        const MediaItem* m = p.findMedia(Id(b.shot.media));
        if (!m) continue;
        const double len = std::min({o.maxSeconds, st.end - st.start, m->duration});
        BrollPick pick;
        pick.sentence = st.text;
        pick.at = FrameTime(std::llround(st.start * fps));
        pick.length = std::max<FrameTime>(1, FrameTime(std::llround(len * fps)));
        pick.media = m->id;
        // Centred on the moment that matched best, kept inside the footage.
        pick.in = std::clamp(b.shot.best - len / 2, 0.0, std::max(0.0, m->duration - len));
        pick.score = b.shot.score;
        out.push_back(pick);
    }
    return out;
}

edit::Result placeBroll(Project& p, Sequence& s, const std::vector<BrollPick>& picks, int videoTrack) {
    while (int(s.videoTracks.size()) <= videoTrack) edit::addTrack(p, s, TrackKind::Video);
    edit::Result all;
    all.ok = true;
    const double fps = s.fpsValue();
    for (const BrollPick& b : picks) {
        const double in = std::round(b.in * fps);
        const edit::Result r = edit::placeMedia(p, s, b.media, b.at, in, in + double(b.length), {TrackKind::Video, videoTrack},
                                                {TrackKind::Audio, -1}, false);
        if (!r.ok) return r;
        all.created.insert(all.created.end(), r.created.begin(), r.created.end());
    }
    if (all.created.empty()) return edit::Result::fail("No cutaways to place");
    return all;
}

}  // namespace montage
