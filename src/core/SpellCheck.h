// Montage — spell checking (Premiere's caption and transcript spell check with Learn and Forget, Resolve 21's in
// titles and subtitles): English, American and British, from SCOWL's word lists built in (size 60, the size of the
// usual en_US and en_GB dictionaries). Words are right in their dictionary case, capitalised or in capitals; a
// possessive is right when its stem is; hyphenated words are right when each part is; words with digits, web and
// mail addresses, and words in capitals (acronyms, by default) are left alone. The project's own words (its
// vocabulary, which also prompts speech-to-text) are always right: Learn adds a word to it, Forget takes it out.
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Model.h"

class QString;

namespace montage {

struct Misspelling {
    int start = 0, length = 0;  // in UTF-16 code units (as Qt text editors count), in the text checked
    std::string word;
    std::vector<std::string> suggestions;  // best first
};

class SpellChecker {
public:
    // The checker for a language tag ("en", "en-US", "en_GB", "eng"...), loaded once; null when there is no
    // dictionary for it (only English for now).
    static const SpellChecker* forLanguage(const std::string& language);
    // "en-US" or "en-GB" for a tag, "" when there is no dictionary.
    static std::string dictionaryFor(const std::string& language);

    // `extra`: words that are always right (the project's vocabulary; terms of several words count word by word).
    bool correct(const std::string& word, const std::vector<std::string>& extra = {}) const;
    // Likely corrections, best first, in the word's own case pattern: one edit away (then two), commoner words and
    // run-together words split ("alot" → "a lot") first.
    std::vector<std::string> suggest(const std::string& word, int max = 5) const;
    // The misspelt words of a text, in order; `suggestions` fills each one's (slower).
    std::vector<Misspelling> check(const QString& text, const std::vector<std::string>& extra = {}, bool suggestions = false,
                                   bool ignoreUppercase = true) const;
    std::vector<Misspelling> check(const std::string& text, const std::vector<std::string>& extra = {}, bool suggestions = false,
                                   bool ignoreUppercase = true) const;

    std::string language() const { return language_; }
    size_t size() const { return exact_.size(); }

private:
    explicit SpellChecker(const std::string& dictionary);
    bool known(const std::string& w) const;
    int level(const std::string& lower) const;  // SCOWL size of a lowercase word (10 commonest), 0 when unknown

    std::string language_;
    std::unordered_set<std::string> exact_;               // as listed
    std::unordered_set<std::string> upper_;               // every entry in capitals
    std::unordered_map<std::string, int> lowerLevel_;     // entries that are all lower case: their size
    mutable std::unordered_map<std::string, std::pair<std::string, int>> anyCase_;  // lower case: listed form, size (for suggestions)
};

// A word learned for the project (added to its vocabulary) or forgotten (taken out). False when it was already so.
bool learnWord(Project& p, const std::string& word);
bool forgetWord(Project& p, const std::string& word);

}  // namespace montage
