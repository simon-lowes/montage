#include "G2p.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace montage {

namespace {

const QString kVowels = QStringLiteral("AIOQWYaiuæɑɒɔəɛɜɪʊʌᵻ");
const QString kConsonants = QStringLiteral("bdfhjklmnpstvwzðŋɡɹɾʃʒʤʧθ");
const QString kUsTaus = QStringLiteral("AIOWYiuæɑəɛɪɹʊʌ");
const QString kPuncts = QStringLiteral(";:,.!?—…\"“”");
const QString kNonQuotePuncts = QStringLiteral(";:,.!?—…");
const QChar kPrimary(0x02C8), kSecondary(0x02CC);

bool isVowel(QChar c) { return kVowels.contains(c); }

QString qs(const std::string& s) { return QString::fromStdString(s); }
std::string ss(const QString& s) { return s.toStdString(); }

bool isLower(const std::string& w) { return qs(w) == qs(w).toLower(); }
bool isUpper(const std::string& w) { return qs(w) == qs(w).toUpper(); }
std::string lower(const std::string& w) { return ss(qs(w).toLower()); }
std::string capitalize(const std::string& w) {
    QString q = qs(w).toLower();
    if (!q.isEmpty()) q[0] = q[0].toUpper();
    return ss(q);
}
bool isAlpha(const std::string& w) {
    const QString q = qs(w);
    return !q.isEmpty() && std::all_of(q.begin(), q.end(), [](QChar c) { return c.isLetter(); });
}
// Letters, apostrophe and hyphen in ASCII: the dictionaries' alphabet.
bool lexiconChars(const std::string& w) {
    return std::all_of(w.begin(), w.end(), [](char c) { return c == '\'' || c == '-' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); });
}

void parseDictionary(const QJsonObject& o, std::map<std::string, Lexicon::Entry>& out) {
    for (auto it = o.begin(); it != o.end(); ++it) {
        Lexicon::Entry e;
        if (it.value().isString()) {
            e.phonemes = ss(it.value().toString());
        } else if (it.value().isObject()) {
            const QJsonObject tags = it.value().toObject();
            e.phonemes = ss(tags.value("DEFAULT").toString());
            for (auto t = tags.begin(); t != tags.end(); ++t)
                if (t.key() != "DEFAULT" && t.value().isString()) e.byTag[ss(t.key())] = ss(t.value().toString());
        } else {
            continue;
        }
        out[ss(it.key())] = std::move(e);
    }
    // As misaki grows it: lower-case words also capitalised, capitalised words also in lower case.
    std::map<std::string, Lexicon::Entry> extra;
    for (const auto& [k, v] : out) {
        if (qs(k).size() < 2) continue;
        if (isLower(k)) {
            if (k != capitalize(k)) extra.emplace(capitalize(k), v);
        } else if (k == capitalize(k)) {
            extra.emplace(lower(k), v);
        }
    }
    for (auto& [k, v] : extra) out.emplace(k, std::move(v));  // existing entries win
}

const char* kOnes[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
                       "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
const char* kTens[] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};

std::string under1000(int n) {
    std::string out;
    if (n >= 100) {
        out = std::string(kOnes[n / 100]) + " hundred";
        n %= 100;
        if (n) out += " and ";
        else return out;
    }
    if (n < 20) return out + kOnes[n];
    out += kTens[n / 10];
    if (n % 10) out += std::string("-") + kOnes[n % 10];
    return out;
}

}  // namespace

std::string numberWords(long long n) {
    if (n < 0) return "minus " + numberWords(-n);
    if (n < 1000) return under1000(int(n));
    static const std::pair<long long, const char*> scales[] = {
        {1000000000000LL, "trillion"}, {1000000000LL, "billion"}, {1000000LL, "million"}, {1000LL, "thousand"}};
    std::string out;
    for (const auto& [value, name] : scales) {
        if (n >= value) {
            out += (out.empty() ? "" : ", ") + numberWords(n / value) + " " + name;
            n %= value;
        }
    }
    if (n > 0) out += (n < 100 ? " and " : ", ") + under1000(int(n));
    return out;
}

std::string ordinalWords(long long n) {
    std::string w = numberWords(n);
    // The last word becomes ordinal.
    size_t at = w.find_last_of(" -");
    std::string head = at == std::string::npos ? "" : w.substr(0, at + 1), last = at == std::string::npos ? w : w.substr(at + 1);
    static const std::map<std::string, std::string> irregular{{"one", "first"},   {"two", "second"}, {"three", "third"}, {"five", "fifth"},
                                                              {"eight", "eighth"}, {"nine", "ninth"}, {"twelve", "twelfth"}};
    if (auto it = irregular.find(last); it != irregular.end()) last = it->second;
    else if (!last.empty() && last.back() == 'y') last = last.substr(0, last.size() - 1) + "ieth";
    else last += "th";
    return head + last;
}

std::string yearWords(long long n) {
    if (n < 1000 || n > 9999) return numberWords(n);
    const long long hi = n / 100, lo = n % 100;
    if (lo == 0) return n % 1000 == 0 ? numberWords(n) : numberWords(hi) + " hundred";
    if (hi % 10 == 0 && lo < 10) return numberWords(n);  // 2005: two thousand and five
    return numberWords(hi) + " " + (lo < 10 ? "oh-" + numberWords(lo) : numberWords(lo));
}

std::string applyStress(const std::string& in, double stress) {
    QString ps = qs(in);
    auto restress = [](const QString& p) {
        // Each stress mark moves to just before the vowel it belongs to.
        struct Item {
            double pos;
            QChar c;
        };
        std::vector<Item> items;
        for (int i = 0; i < p.size(); ++i) items.push_back({double(i), p[i]});
        for (int i = 0; i < p.size(); ++i) {
            if (p[i] != kPrimary && p[i] != kSecondary) continue;
            for (int j = i; j < p.size(); ++j)
                if (isVowel(p[j])) {
                    items[size_t(i)].pos = j - 0.5;
                    break;
                }
        }
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.pos < b.pos; });
        QString out;
        for (const Item& it : items) out += it.c;
        return out;
    };
    const bool hasPrimary = ps.contains(kPrimary), hasSecondary = ps.contains(kSecondary), hasVowel = std::any_of(ps.begin(), ps.end(), isVowel);
    if (stress < -1) {
        ps.remove(kPrimary).remove(kSecondary);
    } else if (stress == -1 || ((stress == 0 || stress == -0.5) && hasPrimary)) {
        ps.remove(kSecondary).replace(kPrimary, kSecondary);
    } else if ((stress == 0 || stress == 0.5 || stress == 1) && !hasPrimary && !hasSecondary) {
        if (hasVowel) ps = restress(kSecondary + ps);
    } else if (stress >= 1 && !hasPrimary && hasSecondary) {
        ps.replace(kSecondary, kPrimary);
    } else if (stress > 1 && !hasPrimary && !hasSecondary) {
        if (hasVowel) ps = restress(kPrimary + ps);
    }
    return ss(ps);
}

std::shared_ptr<Lexicon> Lexicon::fromJson(const std::string& gold, const std::string& silver, bool british, std::string* error) {
    auto lex = std::make_shared<Lexicon>();
    lex->british_ = british;
    for (const auto& [text, out] : {std::pair{&gold, &lex->golds_}, std::pair{&silver, &lex->silvers_}}) {
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(*text), &pe);
        if (!doc.isObject()) {
            if (error) *error = "Pronunciation dictionary: " + ss(pe.errorString());
            return nullptr;
        }
        parseDictionary(doc.object(), *out);
    }
    if (lex->golds_.empty()) {
        if (error) *error = "The pronunciation dictionary is empty";
        return nullptr;
    }
    return lex;
}

const Lexicon::Entry* Lexicon::find(const std::string& word, bool* gold) const {
    if (auto it = golds_.find(word); it != golds_.end()) {
        if (gold) *gold = true;
        return &it->second;
    }
    if (auto it = silvers_.find(word); it != silvers_.end()) {
        if (gold) *gold = false;
        return &it->second;
    }
    return nullptr;
}

bool Lexicon::isKnown(const std::string& word) const {
    static const std::set<std::string> symbols{"%", "&", "+", "@"};
    if (golds_.count(word) || silvers_.count(word) || symbols.count(word)) return true;
    if (!isAlpha(word) || !lexiconChars(word)) return false;
    if (qs(word).size() == 1) return true;
    if (isUpper(word) && golds_.count(lower(word))) return true;
    const std::string rest = word.substr(1);
    return rest == ss(qs(rest).toUpper());
}

Lexicon::Result Lexicon::nnp(const std::string& word) const {
    // Spelt out: each letter's name, the last one stressed.
    QString ps;
    for (QChar c : qs(word)) {
        if (!c.isLetter()) continue;
        auto it = golds_.find(ss(QString(c.toUpper())));
        if (it == golds_.end()) return {};
        ps += qs(it->second.phonemes);
    }
    if (ps.isEmpty()) return {};
    ps = qs(applyStress(ss(ps), 0));
    const int last = ps.lastIndexOf(kSecondary);
    if (last >= 0) ps[last] = kPrimary;
    return {ss(ps), true};
}

Lexicon::Result Lexicon::lookup(std::string word, double stress, bool hasStress, const Context& ctx) const {
    bool nnpWord = false;
    if (isUpper(word) && !golds_.count(word)) {
        word = lower(word);
        nnpWord = false;  // misaki spells capitals only when tagged as proper nouns; we have no tagger
    }
    bool gold = false;
    const Entry* e = find(word, &gold);
    if (!e) return nnp(word);
    std::string ps = e->phonemes;
    if (ctx.futureVowel < 0) {
        if (auto it = e->byTag.find("None"); it != e->byTag.end()) ps = it->second;
    }
    if (ps.empty() || nnpWord) return nnp(word);
    return {hasStress ? applyStress(ps, stress) : ps, true};
}

Lexicon::Result Lexicon::specialCase(const std::string& word, double stress, bool hasStress, const Context& ctx) const {
    static const std::map<std::string, std::string> symbols{{"%", "percent"}, {"&", "and"}, {"+", "plus"}, {"@", "at"}};
    if (auto it = symbols.find(word); it != symbols.end()) return lookup(it->second, 0, false, ctx);
    // Initials with dots: U.S., e.g.
    const QString q = qs(word);
    QString inner = q;
    while (inner.startsWith('.')) inner.remove(0, 1);
    while (inner.endsWith('.')) inner.chop(1);
    if (inner.contains('.')) {
        QString letters = q;
        letters.remove('.');
        bool shortParts = true;
        for (const QString& part : q.split('.', Qt::SkipEmptyParts)) shortParts &= part.size() < 3;
        if (shortParts && isAlpha(ss(letters))) return nnp(ss(letters));
    }
    if (word == "a") return {"ɐ", true};
    if (word == "am" || word == "Am") {
        if (ctx.futureVowel < 0 || word != "am" || (hasStress && stress > 0)) return lookup("am", 0, false, ctx);
        return {"ɐm", true};
    }
    if (word == "an" || word == "An" || word == "AN") return {"ɐn", true};
    if (word == "I") return {"ˌI", true};
    if (word == "to" || word == "To") {
        if (ctx.futureVowel < 0) return lookup("to", 0, false, ctx);
        return {ctx.futureVowel ? "tʊ" : "tə", true};
    }
    if (word == "the" || word == "The") return {ctx.futureVowel == 1 ? "ði" : "ðə", true};
    if (word == "vs" || word == "vs." || word == "Vs" || word == "VS") return lookup("versus", 0, false, ctx);
    return {};
}

std::string Lexicon::sSuffix(const std::string& stem) const {
    if (stem.empty()) return {};
    const QString s = qs(stem);
    const QChar last = s.back();
    if (QStringLiteral("ptkfθ").contains(last)) return stem + "s";
    if (QStringLiteral("szʃʒʧʤ").contains(last)) return stem + (british_ ? "ɪz" : "ᵻz");
    return stem + "z";
}

std::string Lexicon::edSuffix(const std::string& stem) const {
    if (stem.empty()) return {};
    QString s = qs(stem);
    const QChar last = s.back();
    if (QStringLiteral("pkfθʃsʧ").contains(last)) return stem + "t";
    if (last == 'd') return stem + (british_ ? "ɪd" : "ᵻd");
    if (last != 't') return stem + "d";
    if (british_ || s.size() < 2) return stem + "ɪd";
    if (kUsTaus.contains(s[s.size() - 2])) return ss(s.left(s.size() - 1)) + "ɾᵻd";
    return stem + "ᵻd";
}

std::string Lexicon::ingSuffix(const std::string& stem) const {
    if (stem.empty()) return {};
    const QString s = qs(stem);
    if (british_) {
        if (QStringLiteral("əː").contains(s.back())) return {};
    } else if (s.size() > 1 && s.back() == 't' && kUsTaus.contains(s[s.size() - 2])) {
        return ss(s.left(s.size() - 1)) + "ɾɪŋ";
    }
    return stem + "ɪŋ";
}

Lexicon::Result Lexicon::stemS(const std::string& word, double stress, bool hasStress, const Context& ctx) const {
    auto ends = [&](const char* suf) { return word.size() >= strlen(suf) && word.compare(word.size() - strlen(suf), strlen(suf), suf) == 0; };
    std::string stem;
    if (word.size() > 2 && ends("s") && !ends("ss") && isKnown(word.substr(0, word.size() - 1))) stem = word.substr(0, word.size() - 1);
    else if ((ends("'s") || (word.size() > 4 && ends("es"))) && isKnown(word.substr(0, word.size() - 2))) stem = word.substr(0, word.size() - 2);
    else if (word.size() > 4 && ends("ies") && isKnown(word.substr(0, word.size() - 3) + "y")) stem = word.substr(0, word.size() - 3) + "y";
    else return {};
    const Result r = lookup(stem, stress, hasStress, ctx);
    if (!r.ok) return {};
    return {sSuffix(r.ps), true};
}

Lexicon::Result Lexicon::stemEd(const std::string& word, double stress, bool hasStress, const Context& ctx) const {
    auto ends = [&](const char* suf) { return word.size() >= strlen(suf) && word.compare(word.size() - strlen(suf), strlen(suf), suf) == 0; };
    std::string stem;
    if (ends("d") && !ends("dd") && isKnown(word.substr(0, word.size() - 1))) stem = word.substr(0, word.size() - 1);
    else if (ends("ed") && !ends("eed") && isKnown(word.substr(0, word.size() - 2))) stem = word.substr(0, word.size() - 2);
    else return {};
    const Result r = lookup(stem, stress, hasStress, ctx);
    if (!r.ok) return {};
    return {edSuffix(r.ps), true};
}

Lexicon::Result Lexicon::stemIng(const std::string& word, double stress, bool hasStress, const Context& ctx) const {
    auto ends = [&](const char* suf) { return word.size() >= strlen(suf) && word.compare(word.size() - strlen(suf), strlen(suf), suf) == 0; };
    if (!ends("ing")) return {};
    std::string stem;
    static const QRegularExpression doubled(QStringLiteral("([bcdgklmnprstvxz])\\1ing$|cking$"));
    if (isKnown(word.substr(0, word.size() - 3))) stem = word.substr(0, word.size() - 3);
    else if (isKnown(word.substr(0, word.size() - 3) + "e")) stem = word.substr(0, word.size() - 3) + "e";
    else if (doubled.match(qs(word)).hasMatch() && isKnown(word.substr(0, word.size() - 4))) stem = word.substr(0, word.size() - 4);
    else return {};
    const Result r = lookup(stem, stress, hasStress, ctx);
    if (!r.ok) return {};
    const std::string ps = ingSuffix(r.ps);
    if (ps.empty()) return {};
    return {ps, true};
}

Lexicon::Result Lexicon::getWord(const std::string& word, double stress, bool hasStress, const Context& ctx) const {
    if (Result r = specialCase(word, stress, hasStress, ctx); r.ok) return r;
    if (isKnown(word)) return lookup(word, stress, hasStress, ctx);
    if (word.size() > 2 && word.compare(word.size() - 2, 2, "s'") == 0 && isKnown(word.substr(0, word.size() - 2) + "'s"))
        return lookup(word.substr(0, word.size() - 2) + "'s", stress, hasStress, ctx);
    if (word.size() > 1 && word.back() == '\'' && isKnown(word.substr(0, word.size() - 1)))
        return lookup(word.substr(0, word.size() - 1), stress, hasStress, ctx);
    if (Result r = stemS(word, stress, hasStress, ctx); r.ok) return r;
    if (Result r = stemEd(word, stress, hasStress, ctx); r.ok) return r;
    if (Result r = stemIng(word, hasStress ? stress : 0.5, true, ctx); r.ok) return r;
    return {};
}

Lexicon::Result Lexicon::number(std::string word, const std::string& currency) const {
    static const QRegularExpression suffixRe(QStringLiteral("[a-z']+$"));
    std::string suffix;
    if (auto m = suffixRe.match(qs(word)); m.hasMatch()) {
        suffix = ss(m.captured());
        word = word.substr(0, word.size() - suffix.size());
    }
    std::vector<std::string> parts;  // phonemes of each word said
    const Context none;
    auto say = [&](const std::string& words, bool dropAnd = true) {
        for (const QString& w : qs(words).split(QRegularExpression(QStringLiteral("[^a-z]+")), Qt::SkipEmptyParts)) {
            if (dropAnd && w == "and") continue;
            const Result r = lookup(ss(w), w == "point" ? -2 : 0, w == "point", none);
            if (r.ok) parts.push_back(r.ps);
        }
    };
    if (!word.empty() && word[0] == '-') {
        say("minus");
        word = word.substr(1);
    }
    std::string digits = word;
    digits.erase(std::remove(digits.begin(), digits.end(), ','), digits.end());
    if (digits.empty()) return {};
    static const std::map<std::string, std::pair<std::string, std::string>> currencies{
        {"$", {"dollar", "cent"}}, {"£", {"pound", "pence"}}, {"€", {"euro", "cent"}}};
    const size_t dots = size_t(std::count(digits.begin(), digits.end(), '.'));
    if (dots > 1) {
        // Version numbers, IP addresses: digit by digit between the dots.
        for (const QString& n : qs(digits).split('.', Qt::SkipEmptyParts)) {
            if (n.startsWith('0') || n.size() != 2) {
                for (QChar c : n) say(numberWords(c.digitValue()));
            } else {
                say(numberWords(n.toLongLong()));
            }
        }
    } else if (auto cur = currencies.find(currency); cur != currencies.end() && (dots == 0 || qs(digits).split('.')[1].size() < 3)) {
        const QStringList halves = qs(digits).split('.');
        long long whole = halves[0].isEmpty() ? 0 : halves[0].toLongLong();
        long long cents = halves.size() > 1 ? QString(halves[1] + "00").left(2).toLongLong() : 0;
        auto unit = [&](long long n, const std::string& name) {
            say(numberWords(n));
            if (std::llabs(n) == 1 || name == "pence") {
                if (Result r = lookup(name, 0, false, none); r.ok) parts.push_back(r.ps);
            } else if (Result r = stemS(name + "s", 0, false, none); r.ok) {
                parts.push_back(r.ps);
            } else if (Result r2 = lookup(name + "s", 0, false, none); r2.ok) {
                parts.push_back(r2.ps);
            }
        };
        if (whole || !cents) unit(whole, cur->second.first);
        if (whole && cents) say("and", false);
        if (cents) unit(cents, cur->second.second);
    } else if (dots == 0) {
        const long long n = qs(digits).toLongLong();
        static const std::set<std::string> ordinals{"st", "nd", "rd", "th"};
        if (ordinals.count(suffix)) say(ordinalWords(n));
        else if (parts.empty() && digits.size() == 4 && word.find(',') == std::string::npos) say(yearWords(n));
        else say(numberWords(n));
    } else {
        const QStringList halves = qs(digits).split('.');
        if (!halves[0].isEmpty()) say(numberWords(halves[0].toLongLong()));
        say("point");
        for (QChar c : halves[1]) say(numberWords(c.digitValue()));
    }
    if (parts.empty()) return {};
    std::string ps;
    for (const std::string& p : parts) ps += (ps.empty() ? "" : " ") + p;
    if (suffix == "s" || suffix == "'s") return {sSuffix(ps), true};
    if (suffix == "ed" || suffix == "'d") return {edSuffix(ps), true};
    if (suffix == "ing") return {ingSuffix(ps), true};
    return {ps, true};
}

std::string spellingToPhonemes(const std::string& word) {
    std::string w = lower(word);
    w.erase(std::remove_if(w.begin(), w.end(), [](char c) { return c < 'a' || c > 'z'; }), w.end());
    if (w.empty()) return {};
    // Longest patterns first; a silent final e lengthens the vowel before it.
    static const std::vector<std::pair<std::string, std::string>> rules{
        {"tion", "ʃən"}, {"sion", "ʒən"}, {"ture", "ʧəɹ"}, {"ough", "O"}, {"augh", "ɔ"}, {"eigh", "A"}, {"igh", "I"}, {"tch", "ʧ"},
        {"dge", "ʤ"},    {"que", "k"},    {"ch", "ʧ"},     {"sh", "ʃ"},   {"th", "θ"},   {"ph", "f"},   {"ck", "k"},   {"ng", "ŋ"},
        {"qu", "kw"},    {"wh", "w"},     {"wr", "ɹ"},     {"kn", "n"},   {"gh", "ɡ"},   {"ee", "i"},   {"ea", "i"},   {"oo", "u"},
        {"ou", "W"},     {"ow", "O"},     {"oi", "Y"},     {"oy", "Y"},   {"ai", "A"},   {"ay", "A"},   {"au", "ɔ"},   {"aw", "ɔ"},
        {"ie", "i"},     {"ei", "A"},     {"ue", "u"},     {"ew", "u"},   {"er", "əɹ"},  {"ir", "ɜɹ"},  {"ur", "ɜɹ"},  {"ar", "ɑɹ"},
        {"or", "ɔɹ"},    {"a", "æ"},      {"e", "ɛ"},      {"i", "ɪ"},    {"o", "ɑ"},    {"u", "ʌ"},    {"b", "b"},    {"d", "d"},
        {"f", "f"},      {"g", "ɡ"},      {"h", "h"},      {"j", "ʤ"},    {"k", "k"},    {"l", "l"},    {"m", "m"},    {"n", "n"},
        {"p", "p"},      {"r", "ɹ"},      {"s", "s"},      {"t", "t"},    {"v", "v"},    {"w", "w"},    {"x", "ks"},   {"z", "z"}};
    static const std::map<char, std::string> longVowel{{'a', "A"}, {'e', "i"}, {'i', "I"}, {'o', "O"}, {'u', "u"}};
    auto isV = [](char c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'; };
    // A magic e: vowel, one consonant, e at the end.
    const bool magic = w.size() >= 4 && w.back() == 'e' && !isV(w[w.size() - 2]) && isV(w[w.size() - 3]);
    const size_t end = (w.size() > 2 && w.back() == 'e' && !isV(w[w.size() - 2])) ? w.size() - 1 : w.size();
    QString ps;
    bool stressed = false;
    for (size_t i = 0; i < end;) {
        const char c = w[i];
        std::string out;
        size_t used = 1;
        if (c == 'c' && !(i + 1 < w.size() && (w[i + 1] == 'h' || w[i + 1] == 'k')))
            out = (i + 1 < w.size() && (w[i + 1] == 'e' || w[i + 1] == 'i' || w[i + 1] == 'y')) ? "s" : "k";
        else if (c == 'y') out = i == 0 ? "j" : (i + 1 >= end ? "i" : "ɪ");
        else if (magic && i == w.size() - 3 && longVowel.count(c)) out = longVowel.at(c);
        else
            for (const auto& [g, p] : rules)
                if (w.compare(i, g.size(), g) == 0 && i + g.size() <= end + (g.back() == 'e' ? 1 : 0)) {
                    out = p, used = g.size();
                    break;
                }
        const QString o = qs(out);
        // The first vowel takes the stress.
        if (!stressed && std::any_of(o.begin(), o.end(), isVowel)) {
            ps += kPrimary;
            stressed = true;
        }
        ps += o;
        i += used;
    }
    return ss(ps);
}

Lexicon::Result Lexicon::compound(const std::string& word, const Context& ctx, int depth) const {
    const std::string w = lower(word);
    if (depth < 2 && w.size() >= 6 && isAlpha(w)) {
        // The longest known first part with a known (or itself compound) rest.
        for (size_t cut = w.size() - 3; cut >= 3; --cut) {
            const std::string head = w.substr(0, cut), tail = w.substr(cut);
            const Result a = getWord(head, 0, false, ctx);
            if (!a.ok || !isKnown(head)) continue;
            Result b = isKnown(tail) ? getWord(tail, 0, false, ctx) : compound(tail, ctx, depth + 1);
            if (!b.ok) continue;
            return {a.ps + applyStress(b.ps, -1), true};
        }
    }
    if (depth > 0) return {};
    const std::string guess = spellingToPhonemes(w);
    if (guess.empty()) return {};
    return {guess, true};
}

Lexicon::Result Lexicon::word(const std::string& token, const std::string& currency, const Context& ctx) const {
    std::string w = token;
    // Curly apostrophes as straight ones.
    for (const char* curly : {"\xE2\x80\x98", "\xE2\x80\x99"}) {
        for (size_t at; (at = w.find(curly)) != std::string::npos;) w.replace(at, 3, "'");
    }
    const bool hasStress = !isLower(w);
    const double stress = isUpper(w) ? 2 : 0.5;
    if (Result r = getWord(w, stress, hasStress, ctx); r.ok) return r;
    // Numbers, with suffixes: 2026, 3rd, 1990s, 12.5, -4.
    const QString q = qs(w);
    if (std::any_of(q.begin(), q.end(), [](QChar c) { return c.isDigit(); })) {
        static const QRegularExpression numRe(QStringLiteral("^-?[0-9][0-9,]*(\\.[0-9]+)*(st|nd|rd|th|s|'s|ed|'d|ing)?$|^-?\\.[0-9]+$"));
        if (numRe.match(q).hasMatch()) return number(w, currency);
    }
    if (!lexiconChars(w)) return {};
    if (!isLower(w) && (isUpper(w) || q.mid(1) == q.mid(1).toLower())) {
        if (Result r = getWord(lower(w), stress, hasStress, ctx); r.ok) return r;
    }
    // Short acronyms are spelt out; other words are split into known words or sounded out.
    if (isUpper(w) && q.size() <= 4 && isAlpha(w)) return nnp(w);
    return compound(w, ctx);
}

std::string Lexicon::phonemize(const std::string& text) const {
    // Tokens: words (letters, digits, apostrophes, inner dots and commas), punctuation, whitespace.
    struct Token {
        QString text, space;
        bool punct = false;
        std::string currency;
        std::string ps;
    };
    std::vector<Token> tokens;
    const QString t = qs(text);
    QString pendingCurrency;
    for (int i = 0; i < t.size();) {
        const QChar c = t[i];
        if (c.isSpace()) {
            if (!tokens.empty()) tokens.back().space = " ";
            ++i;
            continue;
        }
        if (c == '$' || c == QChar(0x00A3) || c == QChar(0x20AC)) {  // $ £ €
            pendingCurrency = c;
            ++i;
            continue;
        }
        if (c.isLetterOrNumber() || c == '\'' || c == QChar(0x2019) || (c == '-' && i + 1 < t.size() && t[i + 1].isDigit() && (i == 0 || t[i - 1].isSpace()))) {
            int j = i + 1;
            while (j < t.size()) {
                const QChar d = t[j];
                const bool inner = (d == '.' || d == ',') && j + 1 < t.size() && t[j + 1].isLetterOrNumber() && t[j - 1].isLetterOrNumber() &&
                                   (t[j + 1].isDigit() || d == '.');
                if (d.isLetterOrNumber() || d == '\'' || d == QChar(0x2019) || inner) ++j;
                else break;
            }
            // A trailing dot after single letters is part of initials (U.S.), and of abbreviations the
            // dictionary knows (Dr., etc.).
            QString word = t.mid(i, j - i);
            if (j < t.size() && t[j] == '.' && ((word.contains('.') && word.size() <= 6) || golds_.count(ss(word + '.')) || silvers_.count(ss(word + '.'))))
                word += '.', ++j;
            Token tok;
            tok.text = word;
            tok.currency = ss(pendingCurrency);
            pendingCurrency.clear();
            tokens.push_back(tok);
            i = j;
            continue;
        }
        if (c == '-' || c == QChar(0x2013) || c == QChar(0x2014)) {
            // A dash between spaces is a pause; inside a word it joins (said as two words).
            const bool spaced = (i == 0 || t[i - 1].isSpace()) || (i + 1 < t.size() && t[i + 1].isSpace()) || c == QChar(0x2014);
            if (spaced) {
                Token tok;
                tok.text = QStringLiteral("—");
                tok.punct = true;
                tokens.push_back(tok);
            } else if (!tokens.empty()) {
                tokens.back().space = " ";
            }
            ++i;
            continue;
        }
        if (kPuncts.contains(c) || c == '(' || c == ')') {
            Token tok;
            tok.text = c;
            // Straight double quotes as curly ones, opening after a space or at the start.
            if (c == '"') tok.text = (i == 0 || t[i - 1].isSpace() || t[i - 1] == '(') ? QStringLiteral("“") : QStringLiteral("”");
            tok.punct = true;
            tokens.push_back(tok);
            ++i;
            continue;
        }
        static const std::set<QChar> symbols{'%', '&', '+', '@'};
        if (symbols.count(c)) {
            Token tok;
            tok.text = c;
            tokens.push_back(tok);
            if (!tokens.empty() && tokens.size() > 1 && tokens[tokens.size() - 2].space.isEmpty()) tokens[tokens.size() - 2].space = " ";
            ++i;
            continue;
        }
        ++i;  // anything else is not said
    }
    // From the end, so each word knows what follows it.
    Context ctx;
    for (auto it = tokens.rbegin(); it != tokens.rend(); ++it) {
        Token& tok = *it;
        if (tok.punct) {
            tok.ps = ss(tok.text);
        } else {
            Result r = word(ss(tok.text), tok.currency, ctx);
            if (!r.ok) {
                // Unknown: letter by letter, if it is letters at all.
                QString letters = tok.text;
                letters.remove(QRegularExpression(QStringLiteral("[^A-Za-z]")));
                if (!letters.isEmpty()) r = nnp(ss(letters));
            }
            tok.ps = r.ok ? r.ps : std::string();
        }
        // What the next word to the left sees: a vowel, a consonant, or a pause.
        const QString ps = qs(tok.ps);
        for (QChar c : ps) {
            if (kNonQuotePuncts.contains(c)) {
                ctx.futureVowel = -1;
                break;
            }
            if (isVowel(c)) {
                ctx.futureVowel = 1;
                break;
            }
            if (kConsonants.contains(c)) {
                ctx.futureVowel = 0;
                break;
            }
        }
        ctx.futureTo = tok.text == "to" || tok.text == "To";
    }
    std::string out;
    for (const Token& tok : tokens) {
        if (tok.ps.empty()) continue;
        out += tok.ps;
        out += ss(tok.space);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}  // namespace montage
