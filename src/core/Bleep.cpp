#include "Bleep.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>

#include "Captions.h"
#include "Effects.h"

namespace montage {

namespace {

std::string bare(const std::string& word) {
    std::string out;
    for (char c : word)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '\'') out += char(std::tolower(static_cast<unsigned char>(c)));
    while (!out.empty() && out.back() == '\'') out.pop_back();
    return out;
}

std::vector<std::string> splitWords(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == ' ' || c == '\n' || c == '\t') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Masks the word at index `w` of `text`, keeping the spaces and line breaks.
std::string maskNth(const std::string& text, size_t w) {
    std::string out;
    size_t index = 0;
    std::string cur;
    auto flush = [&]() {
        if (cur.empty()) return;
        out += index == w ? maskWord(cur) : cur;
        ++index;
        cur.clear();
    };
    for (char c : text) {
        if (c == ' ' || c == '\n' || c == '\t') {
            flush();
            out += c;
        } else {
            cur += c;
        }
    }
    flush();
    return out;
}

}  // namespace

std::vector<SecondsRange> bleepRanges(const Effect& e) {
    std::vector<SecondsRange> out;
    std::stringstream ss(e.s("ranges"));
    std::string item;
    while (std::getline(ss, item, ';')) {
        const size_t dash = item.find('-', 1);
        if (dash == std::string::npos) continue;
        const double a = std::atof(item.substr(0, dash).c_str()), b = std::atof(item.substr(dash + 1).c_str());
        if (b > a) out.push_back({a, b});
    }
    std::sort(out.begin(), out.end());
    return out;
}

void setBleepRanges(Effect& e, std::vector<SecondsRange> ranges) {
    std::sort(ranges.begin(), ranges.end());
    std::vector<SecondsRange> merged;
    for (const SecondsRange& r : ranges) {
        if (r.second <= r.first) continue;
        if (!merged.empty() && r.first <= merged.back().second) merged.back().second = std::max(merged.back().second, r.second);
        else merged.push_back(r);
    }
    std::string text;
    for (const SecondsRange& r : merged) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s%.3f-%.3f", text.empty() ? "" : ";", r.first, r.second);
        text += buf;
    }
    e.strings["ranges"] = text;
}

std::string maskWord(const std::string& word) {
    // Leading and trailing punctuation stay; of the letters, the first stays.
    size_t a = 0, b = word.size();
    while (a < b && !std::isalnum(static_cast<unsigned char>(word[a]))) ++a;
    while (b > a && !std::isalnum(static_cast<unsigned char>(word[b - 1]))) --b;
    if (b <= a) return word;
    std::string out = word.substr(0, a + 1);
    for (size_t i = a + 1; i < b; ++i) out += std::isalnum(static_cast<unsigned char>(word[i])) ? '*' : word[i];
    return out + word.substr(b);
}

bool isProfanity(const std::string& word) {
    static const std::set<std::string> words = {
        "fuck",     "fucking",  "fucked",   "fucker",   "fuckers",  "fucks",     "motherfucker", "motherfucking",
        "shit",     "shitty",   "shits",    "bullshit", "horseshit", "bitch",    "bitches",      "bastard",
        "bastards", "asshole",  "assholes", "arsehole", "dick",     "dickhead",  "cunt",         "cunts",
        "cock",     "cocksucker", "piss",   "pissed",   "prick",    "twat",      "wanker",       "bollocks",
        "goddamn",  "goddamned", "damn",    "dammit",   "crap",     "slut",      "whore",        "douchebag"};
    return words.count(bare(word)) > 0;
}

std::vector<TranscriptWord> profanity(const std::vector<TranscriptWord>& words) {
    std::vector<TranscriptWord> out;
    for (const TranscriptWord& w : words)
        if (isProfanity(w.text)) out.push_back(w);
    return out;
}

edit::Result bleepWords(Project& p, Sequence& s, const std::vector<TranscriptWord>& words, bool maskCaptions) {
    if (words.empty()) return edit::Result::fail("No words to bleep");
    const double fps = s.fpsValue();
    edit::Result res;
    for (Track& t : s.audioTracks) {
        if (t.muted || t.locked) continue;
        for (Clip& c : t.clips) {
            const MediaItem* m = c.enabled ? p.findMedia(c.mediaId) : nullptr;
            if (!m || !m->transcript || !m->hasAudio) continue;
            std::vector<SecondsRange> add;
            for (const TranscriptWord& w : words) {
                // The word's part of this clip, in clip frames, then in source seconds.
                const double a = std::max(w.start * fps, double(c.start)) - double(c.start);
                const double b = std::min(w.end * fps, double(c.end())) - double(c.start);
                if (b <= a) continue;
                const double sa = c.sourceAt(a) / fps, sb = c.sourceAt(b) / fps;
                add.push_back({std::min(sa, sb), std::max(sa, sb)});
            }
            if (add.empty()) continue;
            Effect* fx = nullptr;
            for (Effect& e : c.effects)
                if (e.type == "bleep") fx = &e;
            if (!fx) {
                c.effects.push_back(makeEffect(p, "bleep"));
                fx = &c.effects.back();
            }
            std::vector<SecondsRange> all = bleepRanges(*fx);
            all.insert(all.end(), add.begin(), add.end());
            setBleepRanges(*fx, all);
            res.created.push_back(c.id);
        }
    }
    if (res.created.empty()) return edit::Result::fail("No transcribed audio under those words");
    if (maskCaptions) {
        for (CaptionTrack& ct : s.captionTracks)
            for (Caption& cap : ct.captions) {
                const std::vector<std::string> capWords = splitWords(cap.text);
                for (const TranscriptWord& w : words) {
                    const double mid = (w.start + w.end) / 2 * fps;
                    if (mid < double(cap.start) || mid >= double(cap.end)) continue;
                    // By when each caption word is said, else the first unmasked one that matches.
                    size_t hit = capWords.size();
                    if (cap.wordTimes.size() == capWords.size() && cap.end > cap.start) {
                        double best = 1e18;
                        for (size_t k = 0; k < capWords.size(); ++k) {
                            const double at = double(cap.start) + cap.wordTimes[k] * double(cap.end - cap.start);
                            if (bare(capWords[k]) == bare(w.text) && std::fabs(at - mid) < best) best = std::fabs(at - mid), hit = k;
                        }
                    }
                    const std::vector<std::string> now = splitWords(cap.text);
                    for (size_t k = 0; hit == capWords.size() && k < now.size(); ++k)
                        if (bare(now[k]) == bare(w.text)) hit = k;
                    if (hit < capWords.size()) cap.text = maskNth(cap.text, hit);
                }
            }
    }
    return res;
}

}  // namespace montage
