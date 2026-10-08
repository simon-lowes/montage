// Montage — English text to phonemes for speech synthesis, in the
// conventions Kokoro was trained on: a port of misaki's lexicon (Hexgrad,
// Apache-2.0) and its pronunciation dictionaries (US or UK, gold then
// silver). Without misaki's part-of-speech tagger, words with several
// pronunciations take their usual one; numbers, money, ordinals, years,
// stems (-s, -ed, -ing) and the small function words that change with
// what follows (a, an, the, to) are handled as misaki does.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace montage {

class Lexicon {
public:
    // From misaki's dictionaries (JSON text: word -> phonemes, or -> {tag: phonemes}).
    static std::shared_ptr<Lexicon> fromJson(const std::string& gold, const std::string& silver, bool british,
                                             std::string* error = nullptr);
    bool british() const { return british_; }
    // Phonemes for running text (punctuation kept, words separated by spaces). Words it cannot
    // say are spelt out letter by letter, or left out if they are not Latin letters.
    std::string phonemize(const std::string& text) const;

    struct Entry {
        std::string phonemes;                      // the usual pronunciation
        std::map<std::string, std::string> byTag;  // others, by part of speech ("VBD", "None"...)
    };

private:
    struct Context {
        int futureVowel = -1;  // -1 unknown (end or punctuation follows), 0 consonant, 1 vowel
        bool futureTo = false;
    };
    struct Result {
        std::string ps;
        bool ok = false;
    };
    const Entry* find(const std::string& word, bool* gold = nullptr) const;
    bool isKnown(const std::string& word) const;
    Result lookup(std::string word, double stress, bool hasStress, const Context& ctx) const;
    Result nnp(const std::string& word) const;
    Result specialCase(const std::string& word, double stress, bool hasStress, const Context& ctx) const;
    Result getWord(const std::string& word, double stress, bool hasStress, const Context& ctx) const;
    Result stemS(const std::string& word, double stress, bool hasStress, const Context& ctx) const;
    Result stemEd(const std::string& word, double stress, bool hasStress, const Context& ctx) const;
    Result stemIng(const std::string& word, double stress, bool hasStress, const Context& ctx) const;
    std::string sSuffix(const std::string& stem) const;
    std::string edSuffix(const std::string& stem) const;
    std::string ingSuffix(const std::string& stem) const;
    Result number(std::string word, const std::string& currency) const;
    Result word(const std::string& token, const std::string& currency, const Context& ctx) const;
    // A word the dictionaries lack: two or three words they know (voice+over), else spelling rules.
    Result compound(const std::string& word, const Context& ctx, int depth = 0) const;

    std::map<std::string, Entry> golds_, silvers_;
    bool british_ = false;
};

// misaki's stress adjustment: -2 removes stress, -1 / 0 lower it, 0.5 / 1 / 2 add it where missing.
std::string applyStress(const std::string& ps, double stress);

// Numbers in words as num2words writes them (cardinal, ordinal, year), e.g. 2026 -> "twenty twenty-six".
std::string numberWords(long long n);
std::string ordinalWords(long long n);
std::string yearWords(long long n);
// A guess at an unknown word's phonemes from its spelling (common English letter patterns, the
// first syllable stressed).
std::string spellingToPhonemes(const std::string& word);

}  // namespace montage
