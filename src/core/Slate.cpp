#include "Slate.h"

#include "MediaLog.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <vector>

namespace montage {

namespace {

struct Token {
    std::string text;  // lower case, letters and digits only
    double start = 0;
};

const std::map<std::string, int>& numberWords() {
    static const std::map<std::string, int> m = {
        {"zero", 0}, {"oh", 0}, {"one", 1}, {"two", 2}, {"three", 3}, {"four", 4}, {"five", 5}, {"six", 6}, {"seven", 7}, {"eight", 8},
        {"nine", 9}, {"ten", 10}, {"eleven", 11}, {"twelve", 12}, {"thirteen", 13}, {"fourteen", 14}, {"fifteen", 15}, {"sixteen", 16},
        {"seventeen", 17}, {"eighteen", 18}, {"nineteen", 19}, {"twenty", 20}, {"thirty", 30}, {"forty", 40}, {"fifty", 50},
        {"sixty", 60}, {"seventy", 70}, {"eighty", 80}, {"ninety", 90}};
    return m;
}

// Phonetic alphabets (ICAO and the older ones still heard on sets) to letters.
char phoneticLetter(const std::string& w) {
    static const std::map<std::string, char> m = {
        {"alpha", 'A'}, {"alfa", 'A'}, {"apple", 'A'}, {"able", 'A'}, {"bravo", 'B'}, {"baker", 'B'}, {"boy", 'B'}, {"charlie", 'C'},
        {"charley", 'C'}, {"delta", 'D'}, {"dog", 'D'}, {"david", 'D'}, {"echo", 'E'}, {"easy", 'E'}, {"edward", 'E'}, {"foxtrot", 'F'},
        {"fox", 'F'}, {"frank", 'F'}, {"golf", 'G'}, {"george", 'G'}, {"hotel", 'H'}, {"henry", 'H'}, {"india", 'I'}, {"item", 'I'},
        {"juliet", 'J'}, {"juliett", 'J'}, {"jig", 'J'}, {"kilo", 'K'}, {"king", 'K'}, {"lima", 'L'}, {"love", 'L'}, {"mike", 'M'},
        {"mary", 'M'}, {"november", 'N'}, {"nan", 'N'}, {"oscar", 'O'}, {"oboe", 'O'}, {"papa", 'P'}, {"peter", 'P'}, {"quebec", 'Q'},
        {"queen", 'Q'}, {"romeo", 'R'}, {"roger", 'R'}, {"sierra", 'S'}, {"sugar", 'S'}, {"tango", 'T'}, {"tare", 'T'}, {"uniform", 'U'},
        {"uncle", 'U'}, {"victor", 'V'}, {"whiskey", 'W'}, {"whisky", 'W'}, {"william", 'W'}, {"xray", 'X'}, {"yankee", 'Y'},
        {"yoke", 'Y'}, {"zulu", 'Z'}, {"zebra", 'Z'}};
    const auto it = m.find(w);
    return it == m.end() ? 0 : it->second;
}

bool isDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

// A number at token i ("12", "twelve", "twenty one", "one hundred and four"); its value and how many tokens it took.
bool numberAt(const std::vector<Token>& t, size_t i, int& value, size_t& used) {
    if (i >= t.size()) return false;
    if (isDigits(t[i].text) && t[i].text.size() <= 5) {
        value = std::stoi(t[i].text), used = 1;
        return true;
    }
    const auto& words = numberWords();
    auto word = [&](size_t k, int& v) {
        if (k >= t.size()) return false;
        const auto it = words.find(t[k].text);
        if (it == words.end()) return false;
        v = it->second;
        return true;
    };
    int v = 0;
    if (!word(i, v)) return false;
    size_t k = i + 1;
    int total = v;
    if (k < t.size() && t[k].text == "hundred" && v > 0 && v < 10) {
        total = v * 100;
        ++k;
        if (k < t.size() && t[k].text == "and") ++k;
        int rest = 0;
        if (word(k, rest)) {
            total += rest;
            ++k;
            if (rest >= 20 && rest % 10 == 0 && word(k, rest) && rest < 10) total += rest, ++k;
        }
    } else if (v >= 20 && v % 10 == 0) {
        int unit = 0;
        if (word(k, unit) && unit > 0 && unit < 10) total += unit, ++k;
    }
    value = total, used = k - i;
    return true;
}

// A shot letter at token i: a single letter, or a phonetic word.
char letterAt(const std::vector<Token>& t, size_t i) {
    if (i >= t.size()) return 0;
    const std::string& w = t[i].text;
    if (w.size() == 1 && std::isalpha(static_cast<unsigned char>(w[0]))) return char(std::toupper(static_cast<unsigned char>(w[0])));
    return phoneticLetter(w);
}

}  // namespace

std::optional<SlateInfo> slateFromTranscript(const Transcript& tr, double withinSeconds) {
    // The words at the head of the take, as plain lower-case tokens ("12B," -> "12", "b").
    std::vector<Token> tokens;
    for (const TranscriptSegment& seg : tr.segments)
        for (const TranscriptWord& w : seg.words) {
            if (w.start > withinSeconds) break;
            std::string cur;
            auto flush = [&] {
                if (!cur.empty()) tokens.push_back({cur, w.start});
                cur.clear();
            };
            for (size_t k = 0; k < w.text.size(); ++k) {
                const unsigned char c = static_cast<unsigned char>(w.text[k]);
                if (!std::isalnum(c)) {
                    flush();
                    continue;
                }
                // A change between digits and letters splits ("12b" -> "12", "b").
                if (!cur.empty() && bool(std::isdigit(static_cast<unsigned char>(cur.back()))) != bool(std::isdigit(c))) flush();
                cur += char(std::tolower(c));
            }
            flush();
        }
    SlateInfo s;
    bool any = false;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string& w = tokens[i].text;
        int n = 0;
        size_t used = 0;
        auto mark = [&] {
            if (s.at < 0) s.at = tokens[i].start;
            any = true;
        };
        if ((w == "scene" || w == "slate") && numberAt(tokens, i + 1, n, used)) {
            if (s.scene.empty() || w == "scene") s.scene = std::to_string(n);
            if (const char l = letterAt(tokens, i + 1 + used)) s.shot = std::string(1, l), ++used;
            mark();
            i += used;
        } else if (w == "shot") {
            if (const char l = letterAt(tokens, i + 1)) s.shot = std::string(1, l), ++i, mark();
            else if (numberAt(tokens, i + 1, n, used)) s.shot = std::to_string(n), i += used, mark();
        } else if (w == "take" && numberAt(tokens, i + 1, n, used)) {
            s.take = std::to_string(n);
            mark();
            i += used;
        } else if (s.scene.empty() && isDigits(w) && i + 2 < tokens.size() && letterAt(tokens, i + 1) && tokens[i + 2].text == "take") {
            // "12 B take 3" with no "scene": the number and letter before "take".
            s.scene = w, s.shot = std::string(1, letterAt(tokens, i + 1));
            mark();
            i += 1;
        }
    }
    if (!any || (s.scene.empty() && s.take.empty())) return std::nullopt;
    return s;
}

int logFromSlates(Project& p, const std::vector<Id>& media) {
    int logged = 0;
    for (MediaItem& m : p.media) {
        if (!media.empty() && std::find(media.begin(), media.end(), m.id) == media.end()) continue;
        if (!m.transcript) continue;
        const auto s = slateFromTranscript(*m.transcript);
        if (!s) continue;
        if (!s->scene.empty()) setMediaField(m, "scene", s->scene);
        if (!s->shot.empty()) setMediaField(m, "shot", s->shot);
        if (!s->take.empty()) setMediaField(m, "take", s->take);
        ++logged;
    }
    return logged;
}

}  // namespace montage
