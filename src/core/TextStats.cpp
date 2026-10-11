#include "TextStats.h"

#include <QString>
#include <QStringList>
#include <set>

namespace montage {

namespace {

const QString kClosers = QStringLiteral("\"')]”’»");

QString withoutClosers(const std::string& word) {
    QString q = QString::fromStdString(word).trimmed();
    while (!q.isEmpty() && kClosers.contains(q.back())) q.chop(1);
    return q;
}

}  // namespace

std::string wordKey(const std::string& word) {
    QString out;
    for (QChar c : QString::fromStdString(word)) {
        if (c == QChar(0x2019)) c = QLatin1Char('\'');
        if (c.isLetterOrNumber() || c == QLatin1Char('\'')) out += c.toLower();
    }
    while (out.startsWith(QLatin1Char('\''))) out.remove(0, 1);
    while (out.endsWith(QLatin1Char('\''))) out.chop(1);
    return out.toStdString();
}

std::vector<std::string> wordKeys(const std::string& text) {
    std::vector<std::string> out;
    for (const QString& part : QString::fromStdString(text).simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts))
        if (std::string k = wordKey(part.toStdString()); !k.empty()) out.push_back(std::move(k));
    return out;
}

bool isStopWord(const std::string& k) {
    static const std::set<std::string> stop = {
        "the",   "and",    "but",   "for",    "with",   "about", "are",   "was",    "were",  "been",   "its",    "it's",
        "this",  "that",   "these", "those",  "you",    "they",  "she",   "his",    "her",   "him",    "them",   "your",
        "our",   "their",  "what",  "how",    "why",    "when",  "who",   "where",  "which", "does",   "did",    "have",
        "has",   "had",    "can",   "will",   "would",  "should", "could", "not",   "from",  "into",   "than",   "then",
        "there", "here",   "just",  "really", "very",   "all",   "any",   "some",   "more",  "most",   "also",   "too",
        "out",   "off",    "over",  "under",  "again",  "once",  "only",  "own",    "same",  "such",   "both",   "each",
        "few",   "other",  "now",   "well",   "yeah",   "okay",  "like",  "know",   "think", "mean",   "going",  "get",
        "got",   "one",    "lot",   "thing",  "things", "way",   "kind",  "sort",   "much",  "many",   "said",   "say",
        "see",   "make",   "made",  "let",    "let's",  "i'm",   "i've",  "i'd",    "i'll",  "you're", "you've", "we're",
        "we've", "they're", "that's", "there's", "don't", "didn't", "doesn't", "isn't", "wasn't", "aren't", "can't", "won't",
        "being", "because", "while", "after",  "before", "during", "until", "through", "something", "anything", "everything",
        "nothing", "someone", "anyone", "everyone", "maybe", "actually", "basically", "right", "even", "still", "back",
        "into",  "onto",   "upon",  "yes",    "um",     "uh",    "gonna", "wanna",  "want",  "need",   "come",   "came",
        "go",    "went",   "take",  "took",   "put",    "use",   "used",  "first",  "last",  "next",   "good",   "great",
        "pretty", "quite", "little", "big",   "new",    "old",   "sure",  "today",  "time",  "times"};
    return k.size() < 3 || stop.count(k) > 0;
}

std::string stemWord(std::string k) {
    for (const char* suffix : {"ing", "ies", "es", "ed", "s"}) {
        const size_t n = std::char_traits<char>::length(suffix);
        if (k.size() > n + 3 && k.compare(k.size() - n, n, suffix) == 0) {
            k.resize(k.size() - n);
            if (std::string(suffix) == "ies") k += 'y';
            break;
        }
    }
    return k;
}

bool endsSentence(const std::string& word) {
    const QString q = withoutClosers(word);
    if (q.isEmpty()) return false;
    static const QString enders = QStringLiteral(".?!…。？！");
    if (!enders.contains(q.back())) return false;
    static const std::set<std::string> abbreviations = {"mr", "mrs", "ms", "dr", "prof", "st", "vs", "eg", "ie", "jr", "sr"};
    return !(q.back() == QLatin1Char('.') && abbreviations.count(wordKey(q.toStdString())));
}

bool endsQuestion(const std::string& word) {
    const QString q = withoutClosers(word);
    return q.endsWith(QLatin1Char('?')) || q.endsWith(QChar(0xff1f));
}

std::vector<std::pair<size_t, size_t>> sentenceSpans(const std::vector<TranscriptWord>& words, double pause) {
    std::vector<std::pair<size_t, size_t>> out;
    for (size_t i = 0, first = 0; i < words.size(); ++i)
        if (endsSentence(words[i].text) || i + 1 == words.size() || words[i + 1].start - words[i].end >= pause) {
            out.push_back({first, i});
            first = i + 1;
        }
    return out;
}

}  // namespace montage
