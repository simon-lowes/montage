#include "Usd.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <locale>
#include <memory>
#include <new>
#include <sstream>
#include <unordered_map>

#include "core/Zip.h"

namespace montage::usd {

namespace {

constexpr int kMaxDepth = 256;                       // prims within prims, values within values
constexpr size_t kMaxElements = size_t(1) << 26;     // the most elements an array may hold
constexpr size_t kMaxUnpacked = size_t(1) << 30;     // the most bytes a compressed section may unpack to
constexpr qint64 kMaxFile = qint64(1) << 30;
constexpr size_t kMaxPrims = size_t(1) << 18;  // prims a stage composes

}  // namespace

// ---- Values ----------------------------------------------------------------------------------------------------------

size_t Value::size() const {
    if (kind == Kind::Number) return numbers.size() / size_t(std::max(1, width));
    if (kind == Kind::Text) return texts.size();
    return 0;
}

double Value::number(size_t i, double fallback) const {
    return kind == Kind::Number && i < numbers.size() ? numbers[i] : fallback;
}

std::string Value::text(size_t i) const { return kind == Kind::Text && i < texts.size() ? texts[i] : std::string(); }

namespace {

// Whether values of this type are interpolated between time samples (USD interpolates floating-point types only).
bool interpolated(const std::string& type) {
    static const char* const kinds[] = {"float", "double", "half", "point", "normal", "vector", "color", "texCoord", "matrix", "quat", "frame", "timecode"};
    return std::any_of(std::begin(kinds), std::end(kinds), [&](const char* k) { return type.rfind(k, 0) == 0; });
}

std::array<double, 4> slerp4(std::array<double, 4> a, std::array<double, 4> b, double t) {
    double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0) {
        d = -d;
        for (double& v : b) v = -v;
    }
    std::array<double, 4> r;
    if (d > 0.9995) {
        for (int k = 0; k < 4; ++k) r[size_t(k)] = a[size_t(k)] + (b[size_t(k)] - a[size_t(k)]) * t;
    } else {
        const double th = std::acos(std::clamp(d, -1.0, 1.0)), s = std::sin(th);
        for (int k = 0; k < 4; ++k) r[size_t(k)] = (a[size_t(k)] * std::sin((1 - t) * th) + b[size_t(k)] * std::sin(t * th)) / s;
    }
    const double l = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
    for (double& v : r) v = l > 0 ? v / l : 0;
    return r;
}

}  // namespace

Value Attribute::at(double t) const {
    if (samples.empty()) return value;
    if (t <= samples.front().first) return samples.front().second;
    if (t >= samples.back().first) return samples.back().second;
    const auto hi = std::upper_bound(samples.begin(), samples.end(), t, [](double v, const auto& s) { return v < s.first; });
    const auto lo = hi - 1;
    const Value &a = lo->second, &b = hi->second;
    if (!interpolated(typeName) || a.kind != Value::Kind::Number || b.kind != Value::Kind::Number || a.numbers.size() != b.numbers.size() ||
        hi->first <= lo->first)
        return a;
    const double u = (t - lo->first) / (hi->first - lo->first);
    Value r = a;
    if (typeName.rfind("quat", 0) == 0 && a.width == 4) {
        for (size_t i = 0; i + 3 < a.numbers.size(); i += 4) {
            const std::array<double, 4> q = slerp4({a.numbers[i], a.numbers[i + 1], a.numbers[i + 2], a.numbers[i + 3]},
                                                   {b.numbers[i], b.numbers[i + 1], b.numbers[i + 2], b.numbers[i + 3]}, u);
            std::copy(q.begin(), q.end(), r.numbers.begin() + long(i));
        }
        return r;
    }
    for (size_t i = 0; i < r.numbers.size(); ++i) r.numbers[i] = a.numbers[i] + (b.numbers[i] - a.numbers[i]) * u;
    return r;
}

std::string Attribute::metaText(const std::string& key, const std::string& fallback) const {
    const auto it = meta.find(key);
    return it != meta.end() && it->second.kind == Value::Kind::Text && !it->second.texts.empty() ? it->second.texts[0] : fallback;
}

Prim* Prim::child(const std::string& n) {
    for (Prim& c : children)
        if (c.name == n) return &c;
    return nullptr;
}

const Prim* Prim::child(const std::string& n) const {
    for (const Prim& c : children)
        if (c.name == n) return &c;
    return nullptr;
}

Prim& Prim::childOrNew(const std::string& n) {
    if (Prim* c = child(n)) return *c;
    children.emplace_back();
    children.back().name = n;
    children.back().specifier = Specifier::Over;
    return children.back();
}

const Attribute* Prim::attribute(const std::string& n) const {
    const auto it = attributes.find(n);
    return it != attributes.end() ? &it->second : nullptr;
}

const std::vector<std::string>* Prim::relationship(const std::string& n) const {
    const auto it = relationships.find(n);
    return it != relationships.end() ? &it->second : nullptr;
}

double Layer::timeCodesPerSecondOr24() const {
    if (timeCodesPerSecond > 0) return timeCodesPerSecond;
    if (framesPerSecond > 0) return framesPerSecond;
    return 24;
}

// ---- Numbers ---------------------------------------------------------------------------------------------------------

namespace {

// The decimal number at s[i...] (moving i past it): exact for up to 15 significant digits and a power of ten within
// 22 (Clinger's fast path); longer ones go through the C++ library in the classic locale.
bool readDecimal(const std::string& s, size_t& i, double& out) {
    const size_t start = i;
    size_t p = i;
    bool neg = false;
    if (p < s.size() && (s[p] == '-' || s[p] == '+')) neg = s[p++] == '-';
    auto word = [&](const char* w) {
        const size_t n = std::strlen(w);
        return s.compare(p, n, w) == 0 && (p + n >= s.size() || !(std::isalnum(uint8_t(s[p + n])) || s[p + n] == '_'));
    };
    if (word("inf")) {
        out = neg ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
        i = p + 3;
        return true;
    }
    if (word("nan")) {
        out = std::numeric_limits<double>::quiet_NaN();
        i = p + 3;
        return true;
    }
    uint64_t mant = 0;
    int digits = 0, exp10 = 0;
    bool any = false;
    while (p < s.size() && std::isdigit(uint8_t(s[p]))) {
        any = true;
        if (digits < 19) {
            mant = mant * 10 + uint64_t(s[p] - '0');
            if (mant) ++digits;
        } else {
            ++exp10;
        }
        ++p;
    }
    if (p < s.size() && s[p] == '.') {
        ++p;
        while (p < s.size() && std::isdigit(uint8_t(s[p]))) {
            any = true;
            if (digits < 19) {
                mant = mant * 10 + uint64_t(s[p] - '0');
                if (mant) ++digits;
                --exp10;
            }
            ++p;
        }
    }
    if (!any) return false;
    if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
        size_t q = p + 1;
        bool eneg = false;
        if (q < s.size() && (s[q] == '-' || s[q] == '+')) eneg = s[q++] == '-';
        if (q < s.size() && std::isdigit(uint8_t(s[q]))) {
            int e = 0;
            while (q < s.size() && std::isdigit(uint8_t(s[q]))) e = std::min(100000, e * 10 + (s[q++] - '0'));
            exp10 += eneg ? -e : e;
            p = q;
        }
    }
    i = p;
    static const double pow10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                                   1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    if (mant == 0) {
        out = neg ? -0.0 : 0.0;
        return true;
    }
    if (digits <= 15 && exp10 >= -22 && exp10 <= 22) {
        const double m = double(mant);
        out = exp10 >= 0 ? m * pow10[exp10] : m / pow10[-exp10];
        if (neg) out = -out;
        return true;
    }
    std::istringstream in(s.substr(start, p - start));
    in.imbue(std::locale::classic());
    in >> out;
    return !in.fail();
}

float halfToFloat(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000) << 16;
    int exp = (h >> 10) & 0x1f;
    uint32_t mant = h & 0x3ff, bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {  // subnormal: normalised
            exp = 1;
            while (!(mant & 0x400)) mant <<= 1, --exp;
            mant &= 0x3ff;
            bits = sign | uint32_t(exp + 112) << 23 | mant << 13;
        }
    } else if (exp == 31) {
        bits = sign | 0x7f800000u | mant << 13;
    } else {
        bits = sign | uint32_t(exp + 112) << 23 | mant << 13;
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// Components a value of a USD type has (vectors, matrices, quaternions), 1 for the rest.
int typeWidth(const std::string& type) {
    std::string t = type;
    if (t.size() > 2 && t.compare(t.size() - 2, 2, "[]") == 0) t.resize(t.size() - 2);
    if (t.rfind("matrix2", 0) == 0) return 4;
    if (t.rfind("matrix3", 0) == 0) return 9;
    if (t.rfind("matrix4", 0) == 0 || t == "frame4d") return 16;
    if (t.rfind("quat", 0) == 0) return 4;
    if (!t.empty() && (t.back() == 'f' || t.back() == 'd' || t.back() == 'h' || t.back() == 'i') && t.size() >= 2 && std::isdigit(uint8_t(t[t.size() - 2])))
        return t[t.size() - 2] - '0';
    return 1;
}

}  // namespace

// ---- USDA ------------------------------------------------------------------------------------------------------------

namespace {

class UsdaParser {
public:
    UsdaParser(const std::string& s, Layer& layer) : s_(s), layer_(layer) {}

    bool parse(std::string* error) {
        if (s_.compare(0, 5, "#usda") != 0) return fail("Not a USDA file (it does not start with #usda)", error);
        while (i_ < s_.size() && s_[i_] != '\n') ++i_;
        skip();
        if (peek('(') && !layerMeta()) return fail(err_, error);
        while (ok_) {
            skip();
            if (i_ >= s_.size()) break;
            if (peekWord() == "reorder") {  // (reorder rootPrims = [...])
                word(), word();
                Value v;
                if (!expect('=') || !value(v)) break;
                continue;
            }
            if (!primStatement(layer_.root, 0)) break;
        }
        if (!ok_) return fail(err_, error);
        return true;
    }

private:
    const std::string& s_;
    Layer& layer_;
    size_t i_ = 0;
    bool ok_ = true;
    std::string err_;

    bool fail(const std::string& why, std::string* error) {
        if (error) *error = why;
        return false;
    }
    bool bad(const std::string& why) {
        if (ok_) {
            const int line = 1 + int(std::count(s_.begin(), s_.begin() + long(std::min(i_, s_.size())), '\n'));
            err_ = "USDA line " + std::to_string(line) + ": " + why;
        }
        ok_ = false;
        return false;
    }
    void skip() {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i_;
            } else if (c == '#') {
                while (i_ < s_.size() && s_[i_] != '\n') ++i_;
            } else {
                break;
            }
        }
    }
    bool peek(char c) {
        skip();
        return i_ < s_.size() && s_[i_] == c;
    }
    bool eat(char c) {
        if (!peek(c)) return false;
        ++i_;
        return true;
    }
    bool expect(char c) { return eat(c) || bad(std::string("expected '") + c + "'"); }
    static bool identChar(char c) { return std::isalnum(uint8_t(c)) || c == '_' || c == ':' || c == '.'; }
    std::string peekWord() {
        skip();
        size_t j = i_;
        if (j < s_.size() && (std::isalpha(uint8_t(s_[j])) || s_[j] == '_'))
            while (j < s_.size() && identChar(s_[j])) ++j;
        return s_.substr(i_, j - i_);
    }
    std::string word() {
        std::string w = peekWord();
        i_ += w.size();
        return w;
    }
    bool eatWord(const char* w) {
        if (peekWord() != w) return false;
        i_ += std::strlen(w);
        return true;
    }

    bool string(std::string& out) {
        skip();
        if (i_ >= s_.size() || (s_[i_] != '"' && s_[i_] != '\'')) return bad("expected a string");
        const char q = s_[i_];
        const bool triple = s_.compare(i_, 3, std::string(3, q)) == 0;
        i_ += triple ? 3 : 1;
        out.clear();
        while (true) {
            if (i_ >= s_.size()) return bad("a string does not end");
            const char c = s_[i_];
            if (triple ? s_.compare(i_, 3, std::string(3, q)) == 0 : c == q) {
                i_ += triple ? 3 : 1;
                return true;
            }
            if (!triple && c == '\n') return bad("a string does not end on its line");
            if (c == '\\' && i_ + 1 < s_.size()) {
                const char e = s_[i_ + 1];
                i_ += 2;
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '0': out += '\0'; break;
                    case 'x':
                        if (i_ + 1 < s_.size() && std::isxdigit(uint8_t(s_[i_])) && std::isxdigit(uint8_t(s_[i_ + 1]))) {
                            out += char(std::stoi(s_.substr(i_, 2), nullptr, 16));
                            i_ += 2;
                        }
                        break;
                    default: out += e;
                }
                continue;
            }
            out += c;
            ++i_;
        }
    }
    bool asset(std::string& out) {
        skip();
        if (s_.compare(i_, 3, "@@@") == 0) {
            size_t e = i_ + 3;
            out.clear();
            while (true) {  // ("\@@@" stands for @@@ inside)
                if (e >= s_.size()) return bad("an asset path does not end");
                if (s_.compare(e, 4, "\\@@@") == 0) {
                    out += "@@@", e += 4;
                } else if (s_.compare(e, 3, "@@@") == 0) {
                    break;
                } else {
                    out += s_[e++];
                }
            }
            i_ = e + 3;
            return true;
        }
        if (i_ >= s_.size() || s_[i_] != '@') return bad("expected an asset path");
        const size_t e = s_.find('@', i_ + 1);
        if (e == std::string::npos || s_.find('\n', i_) < e) return bad("an asset path does not end");
        out = s_.substr(i_ + 1, e - i_ - 1);
        i_ = e + 1;
        return true;
    }
    bool path(std::string& out) {
        skip();
        if (i_ >= s_.size() || s_[i_] != '<') return bad("expected a path");
        const size_t e = s_.find('>', i_ + 1);
        if (e == std::string::npos || s_.find('\n', i_) < e) return bad("a path does not end");
        out = s_.substr(i_ + 1, e - i_ - 1);
        i_ = e + 1;
        return true;
    }

    // One element: a number, string, asset, path, a (nested) tuple of numbers, None, true or false. `count` gets
    // the numbers it held.
    bool element(Value& v, int depth, size_t& count) {
        if (depth > kMaxDepth) return bad("values nest too deeply");
        skip();
        if (i_ >= s_.size()) return bad("a value is missing");
        const char c = s_[i_];
        if (c == '(') {
            ++i_;
            if (eat(')')) return true;
            do {
                if (peek(')')) break;  // (a trailing comma)
                if (!element(v, depth + 1, count)) return false;
            } while (eat(','));
            return expect(')');
        }
        if (c == '"' || c == '\'') {
            std::string t;
            if (!string(t)) return false;
            v.kind = Value::Kind::Text, v.texts.push_back(t);
            return true;
        }
        if (c == '@') {
            std::string t;
            if (!asset(t)) return false;
            v.kind = Value::Kind::Text, v.asset = true, v.texts.push_back(t);
            if (peek('<')) {  // (a reference's prim, read by arcs())
                std::string p;
                path(p);
            }
            return true;
        }
        if (c == '<') {
            std::string t;
            if (!path(t)) return false;
            v.kind = Value::Kind::Text, v.texts.push_back(t);
            return true;
        }
        if (c == '{') return skipBraces();
        const std::string w = peekWord();
        if (w == "None") {
            i_ += 4;
            v.kind = Value::Kind::Block;
            return true;
        }
        if (w == "true" || w == "false") {
            i_ += w.size();
            v.kind = Value::Kind::Number, v.numbers.push_back(w == "true" ? 1 : 0), ++count;
            return true;
        }
        double d;
        if (readDecimal(s_, i_, d)) {
            v.kind = Value::Kind::Number, v.numbers.push_back(d), ++count;
            return true;
        }
        if (!w.empty()) {  // (a bare token, as unregistered metadata may hold)
            i_ += w.size();
            v.kind = Value::Kind::Text, v.texts.push_back(w);
            return true;
        }
        return bad("expected a value");
    }
    // A value: one element, or an array of them.
    bool value(Value& v) {
        v = Value{};
        if (peekWord() == "edit") {  // (an array edit: not read)
            word();
            if (!peek('[')) return bad("expected an array edit");
            int depth = 0;
            do {
                if (s_[i_] == '[') ++depth;
                if (s_[i_] == ']') --depth;
                ++i_;
            } while (i_ < s_.size() && depth > 0);
            return depth == 0 || bad("an array edit does not end");
        }
        if (eat('[')) {
            v.array = true;
            if (eat(']')) return true;
            do {
                if (peek(']')) break;
                size_t n = 0;
                if (!element(v, 1, n)) return false;
                if (n > 0 && v.numbers.size() == n) v.width = int(n);
                if (v.numbers.size() > kMaxElements * 16 || v.texts.size() > kMaxElements) return bad("an array is too long");
            } while (eat(','));
            return expect(']');
        }
        size_t n = 0;
        if (!element(v, 0, n)) return false;
        if (n > 1) v.width = int(n);
        return true;
    }
    bool skipBraces() {
        if (!expect('{')) return false;
        int depth = 1;
        while (i_ < s_.size() && depth > 0) {
            const char c = s_[i_];
            if (c == '"' || c == '\'') {
                std::string t;
                if (!string(t)) return false;
                continue;
            }
            if (c == '#') {
                skip();
                continue;
            }
            if (c == '{') ++depth;
            if (c == '}') --depth;
            ++i_;
        }
        return depth == 0 || bad("a dictionary does not end");
    }
    bool skipParens() {
        if (!expect('(')) return false;
        int depth = 1;
        while (i_ < s_.size() && depth > 0) {
            const char c = s_[i_];
            if (c == '"' || c == '\'') {
                std::string t;
                if (!string(t)) return false;
                continue;
            }
            if (c == '@') {
                std::string t;
                if (!asset(t)) return false;
                continue;
            }
            if (c == '#') {
                skip();
                continue;
            }
            if (c == '(') ++depth;
            if (c == ')') --depth;
            ++i_;
        }
        return depth == 0 || bad("parentheses do not close");
    }

    // A layer offset's "(offset = 10; scale = 2)" after an arc or sublayer.
    bool layerOffset(double& offset, double& scale) {
        if (!eat('(')) return true;
        while (ok_ && !eat(')')) {
            const std::string k = word();
            if (k.empty()) {
                if (eat(';') || eat(',')) continue;
                return bad("expected an offset or scale");
            }
            if (!expect('=')) return false;
            Value v;
            if (peek('{')) {
                if (!skipBraces()) return false;
            } else if (!value(v)) {
                return false;
            }
            if (k == "offset") offset = v.number(0, 0);
            if (k == "scale") scale = v.number(0, 1);
            eat(';'), eat(',');
        }
        return ok_;
    }
    // References or payloads: None, one arc, or a list of them.
    bool arcs(std::vector<Arc>& out) {
        auto one = [&]() {
            Arc a;
            skip();
            if (peek('@') && !asset(a.asset)) return false;
            if (peek('<') && !path(a.prim)) return false;
            if (!layerOffset(a.offset, a.scale)) return false;
            out.push_back(a);
            return true;
        };
        if (eatWord("None")) return true;
        if (eat('[')) {
            if (eat(']')) return true;
            do {
                if (peek(']')) break;
                if (!one()) return false;
            } while (eat(','));
            return expect(']');
        }
        return one();
    }

    // Metadata in parentheses, each entry handed to `entry(key, listOp)` to read its value.
    bool meta(const std::function<bool(const std::string& key, const std::string& op)>& entry) {
        if (!expect('(')) return false;
        while (ok_) {
            if (eat(')')) return true;
            if (eat(';')) continue;
            if (peek('"') || peek('\'')) {  // a bare doc string
                std::string d;
                if (!string(d)) return false;
                continue;
            }
            std::string op, key = word();
            if (key == "prepend" || key == "append" || key == "add" || key == "delete" || key == "reorder") op = key, key = word();
            if (key.empty()) return bad("expected a metadata name");
            if (!expect('=')) return false;
            if (!entry(key, op)) return false;
        }
        return false;
    }
    // A metadata value skipped (any form).
    bool skipValue() {
        if (peek('{')) return skipBraces();
        if (peek('@')) {  // (a single arc)
            std::vector<Arc> a;
            return arcs(a);
        }
        Value v;
        return value(v);
    }

    bool layerMeta() {
        return meta([&](const std::string& key, const std::string&) {
            Value v;
            if (key == "subLayers") {
                if (!expect('[')) return false;
                if (eat(']')) return true;
                do {
                    if (peek(']')) break;
                    std::string a;
                    double offset = 0, scale = 1;
                    if (!asset(a) || !layerOffset(offset, scale)) return false;
                    layer_.subLayers.push_back(a);
                    layer_.subLayerOffsets.emplace_back(offset, scale);
                } while (eat(','));
                return expect(']');
            }
            if (peek('{')) return skipBraces();
            if (!value(v)) return false;
            if (key == "upAxis") layer_.upAxis = v.text();
            else if (key == "defaultPrim") layer_.defaultPrim = v.text();
            else if (key == "metersPerUnit") layer_.metersPerUnit = v.number(0, 0.01);
            else if (key == "timeCodesPerSecond") layer_.timeCodesPerSecond = v.number(0, 0);
            else if (key == "framesPerSecond") layer_.framesPerSecond = v.number(0, 0);
            else if (key == "startTimeCode") layer_.startTimeCode = v.number(0, 0), layer_.hasTimeRange = true;
            else if (key == "endTimeCode") layer_.endTimeCode = v.number(0, 0), layer_.hasTimeRange = true;
            return true;
        });
    }

    bool primMeta(Prim& p) {
        return meta([&](const std::string& key, const std::string& op) {
            if (key == "references" || key == "payload") {
                std::vector<Arc> a;
                if (!arcs(a)) return false;
                if (op != "delete") {
                    std::vector<Arc>& to = key == "payload" ? p.payloads : p.references;
                    to.insert(to.end(), a.begin(), a.end());
                }
                return true;
            }
            if (key == "variants") {
                if (!expect('{')) return false;
                while (ok_ && !eat('}')) {
                    word();  // (its type: string)
                    std::string set, sel;
                    if (peek('"') || peek('\'')) {
                        if (!string(set)) return false;
                    } else {
                        set = word();
                    }
                    if (!expect('=') || !string(sel)) return false;
                    p.variantSelection[set] = sel;
                    eat(';'), eat(',');
                }
                return ok_;
            }
            if (peek('{') || peek('@')) return skipValue();
            Value v;
            if (!value(v)) return false;
            if (key == "kind") p.kind = v.text();
            else if (key == "active") p.active = v.number(0, 1) != 0;
            else if (key == "apiSchemas" && op != "delete") p.apiSchemas.insert(p.apiSchemas.end(), v.texts.begin(), v.texts.end());
            return true;
        });
    }

    bool propertyMeta(Attribute& a) {
        return meta([&](const std::string& key, const std::string&) {
            if (peek('{') || peek('@')) return skipValue();
            Value v;
            if (!value(v)) return false;
            a.meta[key] = v;
            return true;
        });
    }

    // def/over/class [Type] "name" [(meta)] { body }
    bool primStatement(Prim& parent, int depth) {
        if (depth > kMaxDepth) return bad("prims nest too deeply");
        const std::string spec = word();
        if (spec != "def" && spec != "over" && spec != "class") return bad("expected def, over or class");
        std::string type;
        if (!peek('"') && !peek('\'')) type = word();
        std::string name;
        if (!string(name)) return false;
        parent.children.emplace_back();
        Prim& p = parent.children.back();
        p.name = name, p.typeName = type;
        p.specifier = spec == "def" ? Prim::Specifier::Def : spec == "over" ? Prim::Specifier::Over : Prim::Specifier::Class;
        if (peek('(') && !primMeta(p)) return false;
        if (!expect('{')) return false;
        return body(p, depth) && expect('}');
    }

    bool body(Prim& p, int depth) {
        if (depth > kMaxDepth) return bad("prims nest too deeply");  // (variants within variants come here directly)
        while (ok_) {
            skip();
            if (i_ >= s_.size()) return bad("a prim does not end");
            if (peek('}')) return true;
            const std::string w = peekWord();
            if (w == "def" || w == "over" || w == "class") {
                if (!primStatement(p, depth + 1)) return false;
            } else if (w == "variantSet") {
                word();
                std::string set;
                if (!string(set) || !expect('=') || !expect('{')) return false;
                while (ok_ && !eat('}')) {
                    std::string variant;
                    if (!string(variant)) return false;
                    Prim& v = p.variantSets[set][variant];
                    if (peek('(') && !primMeta(v)) return false;
                    if (!expect('{') || !body(v, depth + 1) || !expect('}')) return false;
                }
            } else if (w == "reorder") {
                word();
                word();
                if (!expect('=')) return false;
                Value v;
                if (!value(v)) return false;
            } else if (w.empty()) {
                if (eat(';')) continue;
                return bad("expected a prim or a property");
            } else {
                if (!property(p)) return false;
            }
        }
        return false;
    }

    bool property(Prim& p) {
        std::string op;
        std::string w = word();
        if (w == "prepend" || w == "append" || w == "add" || w == "delete") op = w, w = word();
        bool uniform = false;
        if (w == "custom") w = word();
        if (w == "uniform" || w == "varying" || w == "config") uniform = w == "uniform", w = word();
        if (w == "rel") {
            const std::string name = word();
            if (name.empty()) return bad("expected a relationship's name");
            std::vector<std::string> targets;
            if (eat('=')) {
                Value v;
                if (!value(v)) return false;
                targets = v.texts;
            }
            Attribute ignored;
            if (peek('(') && !propertyMeta(ignored)) return false;
            if (op != "delete") {
                std::vector<std::string>& to = p.relationships[name];
                if (op.empty()) to = targets;
                else to.insert(op == "prepend" ? to.begin() : to.end(), targets.begin(), targets.end());
            }
            return true;
        }
        std::string type = w;
        if (type.empty()) return bad("expected a property");
        if (peek('[')) {
            ++i_;
            if (!expect(']')) return false;
            type += "[]";
        }
        std::string name = word();
        if (name.empty()) return bad("expected a property's name");
        std::string suffix;
        for (const char* s : {".connect", ".timeSamples", ".spline"})
            if (name.size() > std::strlen(s) && name.compare(name.size() - std::strlen(s), std::strlen(s), s) == 0) {
                suffix = s;
                name.resize(name.size() - std::strlen(s));
            }
        Attribute scratch;
        Attribute& a = op == "delete" ? scratch : p.attributes[name];
        if (a.typeName.empty() || suffix.empty()) a.typeName = type;
        a.uniform = a.uniform || uniform;
        if (eat('=')) {
            if (suffix == ".timeSamples") {
                if (!timeSamples(a)) return false;
            } else if (suffix == ".spline") {
                if (!skipBraces()) return false;
            } else {
                Value v;
                if (!value(v)) return false;
                if (suffix == ".connect") {
                    a.connections = v.texts;
                } else {
                    shape(v, type);
                    a.value = v;
                }
            }
        }
        if (peek('(') && !propertyMeta(a)) return false;
        return true;
    }

    bool timeSamples(Attribute& a) {
        if (!expect('{')) return false;
        while (ok_ && !eat('}')) {
            skip();
            double t;
            if (!readDecimal(s_, i_, t)) return bad("expected a time");
            if (!expect(':')) return false;
            Value v;
            if (!value(v)) return false;
            shape(v, a.typeName);
            a.samples.emplace_back(t, v);
            eat(',');
        }
        std::stable_sort(a.samples.begin(), a.samples.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        return ok_;
    }

    // A value fitted to its type: its width, quaternions turned from USDA's (w, x, y, z) to x, y, z, w, and whether it
    // names assets.
    static void shape(Value& v, const std::string& type) {
        const int w = typeWidth(type);
        if (v.kind == Value::Kind::Number && w > 1 && v.numbers.size() % size_t(w) == 0) v.width = w;
        if (type.rfind("quat", 0) == 0 && v.width == 4)
            for (size_t i = 0; i + 3 < v.numbers.size(); i += 4) std::rotate(v.numbers.begin() + long(i), v.numbers.begin() + long(i) + 1, v.numbers.begin() + long(i) + 4);
        if (type.rfind("asset", 0) == 0) v.asset = v.kind == Value::Kind::Text;
        if (singlePrecision(type))
            for (double& d : v.numbers) d = double(float(d));
    }
    // Types held in 32 (or 16) bits: their values read as USD keeps them.
    static bool singlePrecision(const std::string& type) {
        std::string t = type;
        if (t.size() > 2 && t.compare(t.size() - 2, 2, "[]") == 0) t.resize(t.size() - 2);
        return t.rfind("float", 0) == 0 || t.rfind("half", 0) == 0 || (!t.empty() && (t.back() == 'f' || t.back() == 'h'));
    }
};

}  // namespace

bool parseUsda(const std::string& text, Layer& out, std::string* error) {
    out = Layer{};
    return UsdaParser(text, out).parse(error);
}

// ---- USDC ("crate") --------------------------------------------------------------------------------------------------

namespace {

// LZ4's block format, decompressed into `out` (at most `cap` bytes): false if it is malformed.
bool lz4Block(const uint8_t* src, size_t n, uint8_t* dst, size_t cap, size_t& written) {
    size_t ip = 0, op = 0;
    while (ip < n) {
        const uint8_t token = src[ip++];
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t c;
            do {
                if (ip >= n) return false;
                c = src[ip++];
                lit += c;
            } while (c == 255);
        }
        if (lit > n - ip || lit > cap - op) return false;
        std::memcpy(dst + op, src + ip, lit);
        ip += lit, op += lit;
        if (ip >= n) break;  // (the last sequence has literals only)
        if (n - ip < 2) return false;
        const size_t offset = size_t(src[ip]) | size_t(src[ip + 1]) << 8;
        ip += 2;
        if (offset == 0 || offset > op) return false;
        size_t len = token & 15;
        if (len == 15) {
            uint8_t c;
            do {
                if (ip >= n) return false;
                c = src[ip++];
                len += c;
            } while (c == 255);
        }
        len += 4;
        if (len > cap - op) return false;
        for (size_t k = 0; k < len; ++k) dst[op + k] = dst[op - offset + k];
        op += len;
    }
    written = op;
    return true;
}

// USD's framing of LZ4 (TfFastCompression): a count of chunks, 0 for one block, else each chunk's size before it.
bool fastDecompress(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t cap) {
    if (n < 1 || cap > kMaxUnpacked) return false;
    cap = std::min(cap, n * 255 + 1024);  // (LZ4 makes at most about 255 bytes of each: no more is set aside)
    out.resize(cap);
    const int chunks = int(int8_t(src[0]));
    size_t written = 0;
    if (chunks == 0) {
        if (!lz4Block(src + 1, n - 1, out.data(), cap, written)) return false;
    } else {
        if (chunks < 0) return false;
        size_t pos = 1;
        for (int c = 0; c < chunks; ++c) {
            if (n - pos < 4) return false;
            int32_t size;
            std::memcpy(&size, src + pos, 4);
            pos += 4;
            if (size < 0 || size_t(size) > n - pos) return false;
            size_t w = 0;
            if (!lz4Block(src + pos, size_t(size), out.data() + written, cap - written, w)) return false;
            written += w, pos += size_t(size);
        }
    }
    out.resize(written);
    return true;
}

// USD's integer coding: the commonest difference, then 2 bits a value saying how each difference is stored (the
// common one, or in 1, 2 or 4 bytes for 32-bit integers; 2, 4 or 8 for 64-bit), then those differences.
template <class Int>
bool decodeInts(const std::vector<uint8_t>& in, size_t count, std::vector<Int>& out) {
    using SInt = std::make_signed_t<Int>;
    using Small = std::conditional_t<sizeof(Int) == 4, int8_t, int16_t>;
    using Medium = std::conditional_t<sizeof(Int) == 4, int16_t, int32_t>;
    const size_t codeBytes = (count * 2 + 7) / 8;
    if (in.size() < sizeof(SInt) + codeBytes) return false;
    SInt common;
    std::memcpy(&common, in.data(), sizeof(SInt));
    const uint8_t* codes = in.data() + sizeof(SInt);
    size_t vp = sizeof(SInt) + codeBytes;
    out.resize(count);
    SInt prev = 0;
    for (size_t i = 0; i < count; ++i) {
        const int code = (codes[i / 4] >> (2 * (i % 4))) & 3;
        auto read = [&](auto zero) -> bool {
            using T = decltype(zero);
            if (in.size() - vp < sizeof(T)) return false;
            T v;
            std::memcpy(&v, in.data() + vp, sizeof(T));
            vp += sizeof(T);
            prev = SInt(prev + SInt(v));
            return true;
        };
        if (code == 0) prev = SInt(prev + common);
        else if (code == 1 && !read(Small{})) return false;
        else if (code == 2 && !read(Medium{})) return false;
        else if (code == 3 && !read(SInt{})) return false;
        out[i] = Int(prev);
    }
    return true;
}

struct Reader {
    const std::string& b;
    size_t pos = 0;
    bool ok = true;
    template <class T>
    T get() {
        T v{};
        if (!ok || b.size() < sizeof(T) || pos > b.size() - sizeof(T)) {
            ok = false;
            return v;
        }
        std::memcpy(&v, b.data() + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    const uint8_t* take(size_t n) {
        if (!ok || n > b.size() || pos > b.size() - n) {
            ok = false;
            return nullptr;
        }
        const uint8_t* p = reinterpret_cast<const uint8_t*>(b.data()) + pos;
        pos += n;
        return p;
    }
    void seek(int64_t p) {
        if (p < 0 || uint64_t(p) > b.size()) ok = false;
        else pos = size_t(p);
    }
};

// Crate type ids (SdfCrate's CrateDataType).
enum CrateType : int {
    tBool = 1, tUChar, tInt, tUInt, tInt64, tUInt64, tHalf, tFloat, tDouble, tString, tToken, tAssetPath,
    tMatrix2d, tMatrix3d, tMatrix4d, tQuatd, tQuatf, tQuath,
    tVec2d, tVec2f, tVec2h, tVec2i, tVec3d, tVec3f, tVec3h, tVec3i, tVec4d, tVec4f, tVec4h, tVec4i,
    tDictionary, tTokenListOp, tStringListOp, tPathListOp, tReferenceListOp, tIntListOp, tInt64ListOp, tUIntListOp,
    tUInt64ListOp, tPathVector, tTokenVector, tSpecifier, tPermission, tVariability, tVariantSelectionMap,
    tTimeSamples, tPayload, tDoubleVector, tLayerOffsetVector, tStringVector, tValueBlock, tValue,
    tUnregisteredValue, tUnregisteredValueListOp, tPayloadListOp, tTimeCode
};

// How a type's elements are stored: their components and each one's kind.
enum class Comp { None, U8, I32, U32, I64, U64, F16, F32, F64, StringIdx, TokenIdx };
std::pair<int, Comp> layout(int type) {
    switch (type) {
        case tBool: case tUChar: return {1, Comp::U8};
        case tInt: return {1, Comp::I32};
        case tUInt: return {1, Comp::U32};
        case tInt64: return {1, Comp::I64};
        case tUInt64: return {1, Comp::U64};
        case tHalf: return {1, Comp::F16};
        case tFloat: return {1, Comp::F32};
        case tDouble: case tTimeCode: return {1, Comp::F64};
        case tString: return {1, Comp::StringIdx};
        case tToken: case tAssetPath: return {1, Comp::TokenIdx};
        case tMatrix2d: return {4, Comp::F64};
        case tMatrix3d: return {9, Comp::F64};
        case tMatrix4d: return {16, Comp::F64};
        case tQuatd: return {4, Comp::F64};
        case tQuatf: return {4, Comp::F32};
        case tQuath: return {4, Comp::F16};
        case tVec2d: return {2, Comp::F64};
        case tVec2f: return {2, Comp::F32};
        case tVec2h: return {2, Comp::F16};
        case tVec2i: return {2, Comp::I32};
        case tVec3d: return {3, Comp::F64};
        case tVec3f: return {3, Comp::F32};
        case tVec3h: return {3, Comp::F16};
        case tVec3i: return {3, Comp::I32};
        case tVec4d: return {4, Comp::F64};
        case tVec4f: return {4, Comp::F32};
        case tVec4h: return {4, Comp::F16};
        case tVec4i: return {4, Comp::I32};
        default: return {0, Comp::None};
    }
}
size_t compSize(Comp c) {
    switch (c) {
        case Comp::U8: return 1;
        case Comp::F16: return 2;
        case Comp::I32: case Comp::U32: case Comp::F32: case Comp::StringIdx: case Comp::TokenIdx: return 4;
        case Comp::I64: case Comp::U64: case Comp::F64: return 8;
        default: return 0;
    }
}

class CrateReader {
public:
    CrateReader(const std::string& bytes, Layer& layer) : b_(bytes), layer_(layer) {}

    bool read(std::string* error) {
        if (!header() || !sections()) return fail(error);
        return build() || fail(error);
    }

private:
    const std::string& b_;
    Layer& layer_;
    std::string err_;
    int major_ = 0, minor_ = 0, patch_ = 0;
    std::vector<std::string> tokens_;
    std::vector<uint32_t> strings_;
    std::vector<uint32_t> fieldTokens_;
    std::vector<uint64_t> fieldReps_;
    std::vector<uint32_t> fieldSets_;
    // Paths: each one's parent (-1 the root), its last element and whether that is a property.
    struct PathNode {
        int parent = -1;
        std::string element;
        bool property = false;
        bool set = false;
    };
    std::vector<PathNode> paths_;
    struct Spec {
        uint32_t path = 0, fieldSet = 0, type = 0;
    };
    std::vector<Spec> specs_;
    struct Section {
        int64_t start = 0, size = 0;
    };
    std::map<std::string, Section> toc_;

    bool fail(std::string* error) {
        if (error) *error = "USDC: " + (err_.empty() ? std::string("the file is damaged") : err_);
        return false;
    }
    bool bad(const std::string& why) {
        if (err_.empty()) err_ = why;
        return false;
    }
    // Values can be shared (one array named by many fields, one long token by many elements), so a small file can
    // name a great deal: what it unpacks to is counted against this.
    size_t budget_ = size_t(1) << 31;  // bytes
    bool spend(size_t bytes) {
        if (bytes > budget_) return bad("it holds more than Montage reads");
        budget_ -= bytes;
        return true;
    }
    bool keepText(std::vector<std::string>& texts, std::string t) {
        if (!spend(t.size() + sizeof(std::string))) return false;
        texts.push_back(std::move(t));
        return true;
    }
    bool atLeast(int ma, int mi) const { return major_ > ma || (major_ == ma && minor_ >= mi); }

    bool header() {
        if (b_.size() < 88 || b_.compare(0, 8, "PXR-USDC") != 0) return bad("not a USDC file");
        major_ = uint8_t(b_[8]), minor_ = uint8_t(b_[9]), patch_ = uint8_t(b_[10]);
        if (major_ != 0 || minor_ < 4) return bad("crate version " + std::to_string(major_) + "." + std::to_string(minor_) + " is not read (0.4 and later are)");
        Reader r{b_, 16};
        const int64_t toc = r.get<int64_t>();
        r.seek(toc);
        const uint64_t n = r.get<uint64_t>();
        if (!r.ok || n > 64) return bad("its table of contents is damaged");
        for (uint64_t k = 0; k < n; ++k) {
            const uint8_t* name = r.take(16);
            Section s;
            s.start = r.get<int64_t>(), s.size = r.get<int64_t>();
            if (!r.ok || s.start < 0 || s.size < 0 || uint64_t(s.start) > b_.size() || uint64_t(s.size) > b_.size() - uint64_t(s.start))
                return bad("a section lies outside the file");
            toc_[std::string(reinterpret_cast<const char*>(name), strnlen(reinterpret_cast<const char*>(name), 16))] = s;
        }
        return true;
    }

    template <class Int>
    bool compressedInts(Reader& r, size_t count, std::vector<Int>& out) {
        if (count > kMaxElements) return bad("too many values");
        if (!spend(count * sizeof(Int) * 2 + 16)) return false;  // (what it unpacks to, and the values)
        const uint64_t size = r.get<uint64_t>();
        if (!r.ok || size > b_.size() - r.pos) return bad("compressed integers lie outside the file");
        const uint8_t* p = r.take(size_t(size));
        std::vector<uint8_t> raw;
        const size_t cap = sizeof(Int) + (count * 2 + 7) / 8 + count * sizeof(Int);
        if (count == 0) {
            out.clear();
            return true;
        }
        if (!fastDecompress(p, size_t(size), raw, cap) || !decodeInts(raw, count, out)) return bad("compressed integers are damaged");
        return true;
    }

    bool sections() {
        for (const char* need : {"TOKENS", "STRINGS", "FIELDS", "FIELDSETS", "PATHS", "SPECS"})
            if (!toc_.count(need)) return bad(std::string("its ") + need + " section is missing");
        {  // TOKENS
            Reader r{b_, size_t(toc_["TOKENS"].start)};
            const uint64_t n = r.get<uint64_t>(), raw = r.get<uint64_t>(), packed = r.get<uint64_t>();
            if (!r.ok || n > kMaxElements || raw > kMaxUnpacked || packed > b_.size() - r.pos) return bad("its tokens are damaged");
            if (!spend(size_t(raw) * 2 + size_t(n) * sizeof(std::string))) return false;
            std::vector<uint8_t> text;
            if (!fastDecompress(r.take(size_t(packed)), size_t(packed), text, size_t(raw))) return bad("its tokens are damaged");
            tokens_.reserve(size_t(n));
            size_t start = 0;
            for (size_t i = 0; i < text.size() && tokens_.size() < n; ++i)
                if (text[i] == 0) {
                    tokens_.emplace_back(reinterpret_cast<const char*>(text.data()) + start, i - start);
                    start = i + 1;
                }
            if (tokens_.size() != n) return bad("its tokens are damaged");
        }
        {  // STRINGS
            Reader r{b_, size_t(toc_["STRINGS"].start)};
            const uint64_t n = r.get<uint64_t>();
            if (!r.ok || n > kMaxElements || n * 4 > b_.size() - r.pos) return bad("its strings are damaged");
            if (!spend(size_t(n) * 4)) return false;
            strings_.resize(size_t(n));
            for (uint32_t& s : strings_) s = r.get<uint32_t>();
        }
        {  // FIELDS
            Reader r{b_, size_t(toc_["FIELDS"].start)};
            const uint64_t n = r.get<uint64_t>();
            if (!r.ok || n > kMaxElements || !compressedInts(r, size_t(n), fieldTokens_)) return bad("its fields are damaged");
            const uint64_t packed = r.get<uint64_t>();
            if (!r.ok || packed > b_.size() - r.pos) return bad("its fields are damaged");
            if (!spend(size_t(n) * 16)) return false;
            std::vector<uint8_t> raw;
            if (!fastDecompress(r.take(size_t(packed)), size_t(packed), raw, size_t(n) * 8) || raw.size() != size_t(n) * 8) return bad("its fields are damaged");
            fieldReps_.resize(size_t(n));
            std::memcpy(fieldReps_.data(), raw.data(), raw.size());
        }
        {  // FIELDSETS
            Reader r{b_, size_t(toc_["FIELDSETS"].start)};
            const uint64_t n = r.get<uint64_t>();
            if (!r.ok || !compressedInts(r, size_t(n), fieldSets_)) return bad("its field sets are damaged");
        }
        {  // PATHS
            Reader r{b_, size_t(toc_["PATHS"].start)};
            const uint64_t numPaths = r.get<uint64_t>(), encoded = r.get<uint64_t>();
            std::vector<uint32_t> index;
            std::vector<int32_t> element, jump;
            if (!r.ok || numPaths > kMaxElements || encoded > kMaxElements || !compressedInts(r, size_t(encoded), index) ||
                !compressedInts(r, size_t(encoded), element) || !compressedInts(r, size_t(encoded), jump))
                return bad("its paths are damaged");
            if (!spend(size_t(numPaths) * sizeof(PathNode))) return false;
            paths_.assign(size_t(numPaths), PathNode{});
            // The tree in depth-first order: each entry's jump says whether a child follows (> 0 or -1) and where its
            // next sibling is (>= 0: that many entries on; 0: the next).
            struct Todo {
                size_t at;
                int parent;
            };
            std::vector<Todo> todo{{0, -2}};
            size_t visited = 0;
            while (!todo.empty()) {
                Todo t = todo.back();
                todo.pop_back();
                int parent = t.parent;
                size_t cur = t.at;
                while (true) {
                    if (cur >= index.size() || ++visited > index.size() * 2) return bad("its paths are damaged");
                    const size_t here = cur++;
                    const uint32_t pi = index[here];
                    if (pi >= paths_.size()) return bad("its paths are damaged");
                    if (parent == -2) {  // the root
                        paths_[pi].set = true;
                        parent = int(pi);
                    } else {
                        const int32_t e = element[here];
                        const uint32_t tok = uint32_t(e < 0 ? -int64_t(e) : e);
                        if (tok >= tokens_.size()) return bad("its paths are damaged");
                        paths_[pi] = {parent, tokens_[tok], e < 0, true};
                    }
                    const bool child = jump[here] > 0 || jump[here] == -1, sibling = jump[here] >= 0;
                    if (child) {
                        if (sibling) todo.push_back({here + size_t(jump[here]), parent});
                        parent = int(pi);
                    }
                    if (!child && !sibling) break;
                }
            }
        }
        {  // SPECS
            Reader r{b_, size_t(toc_["SPECS"].start)};
            const uint64_t n = r.get<uint64_t>();
            std::vector<uint32_t> path, set, type;
            if (!r.ok || !compressedInts(r, size_t(n), path) || !compressedInts(r, size_t(n), set) || !compressedInts(r, size_t(n), type))
                return bad("its specs are damaged");
            if (!spend(size_t(n) * sizeof(Spec))) return false;
            specs_.resize(size_t(n));
            for (size_t i = 0; i < specs_.size(); ++i) specs_[i] = {path[i], set[i], type[i]};
        }
        return true;
    }

    std::string token(uint64_t i) const { return i < tokens_.size() ? tokens_[size_t(i)] : std::string(); }
    std::string string(uint64_t i) const { return i < strings_.size() ? token(strings_[size_t(i)]) : std::string(); }
    std::string pathText(uint64_t i, int depth = 0) const {
        if (i >= paths_.size() || depth > kMaxDepth || !paths_[size_t(i)].set) return {};
        const PathNode& p = paths_[size_t(i)];
        if (p.parent < 0) return "/";
        const std::string parent = pathText(uint64_t(p.parent), depth + 1);
        if (p.property) return parent + "." + p.element;
        if (!p.element.empty() && p.element[0] == '{') return parent + p.element;
        return parent == "/" ? "/" + p.element : (!parent.empty() && parent.back() == '}' ? parent + p.element : parent + "/" + p.element);
    }

    // One element's components from the file (raw).
    bool components(Reader& r, Comp c, size_t n, Value& v) {
        const size_t sz = compSize(c);
        if (n > kMaxElements * 16 || n * sz > b_.size() - std::min(b_.size(), r.pos)) return bad("a value lies outside the file");
        if (c == Comp::StringIdx || c == Comp::TokenIdx) {
            v.kind = Value::Kind::Text;
            for (size_t k = 0; k < n; ++k) {
                const uint32_t idx = r.get<uint32_t>();
                if (!keepText(v.texts, c == Comp::StringIdx ? string(idx) : token(idx))) return false;
            }
            return r.ok;
        }
        v.kind = Value::Kind::Number;
        if (!spend(n * sizeof(double))) return false;
        v.numbers.reserve(v.numbers.size() + n);
        for (size_t k = 0; k < n; ++k) {
            double d = 0;
            switch (c) {
                case Comp::U8: d = r.get<uint8_t>(); break;
                case Comp::I32: d = r.get<int32_t>(); break;
                case Comp::U32: d = r.get<uint32_t>(); break;
                case Comp::I64: d = double(r.get<int64_t>()); break;
                case Comp::U64: d = double(r.get<uint64_t>()); break;
                case Comp::F16: d = halfToFloat(r.get<uint16_t>()); break;
                case Comp::F32: d = r.get<float>(); break;
                case Comp::F64: d = r.get<double>(); break;
                default: break;
            }
            v.numbers.push_back(d);
        }
        return r.ok;
    }

    // A value's ValueRep unpacked (not list ops, references, dictionaries or time samples, which have their own).
    bool unpack(uint64_t rep, Value& v) {
        v = Value{};
        const bool array = rep >> 63 & 1, inlined = rep >> 62 & 1, compressed = rep >> 61 & 1;
        const int type = int(rep >> 48 & 0xff);
        const uint64_t payload = rep & ((uint64_t(1) << 48) - 1);
        const auto [width, comp] = layout(type);
        v.asset = type == tAssetPath;
        if (type == tValueBlock) {
            v.kind = Value::Kind::Block;
            return true;
        }
        if (type == tTokenVector || type == tStringVector || type == tPathVector || type == tDoubleVector || type == tLayerOffsetVector) {
            Reader r{b_};
            r.seek(int64_t(payload));
            const uint64_t n = r.get<uint64_t>();
            if (!r.ok || n > kMaxElements) return bad("a list is damaged");
            v.array = true;
            if (type == tDoubleVector || type == tLayerOffsetVector) {
                v.width = type == tLayerOffsetVector ? 2 : 1;
                return components(r, Comp::F64, size_t(n) * size_t(v.width), v);
            }
            v.kind = Value::Kind::Text;
            for (uint64_t k = 0; k < n && r.ok; ++k) {
                const uint32_t idx = r.get<uint32_t>();
                if (!keepText(v.texts, type == tTokenVector ? token(idx) : type == tStringVector ? string(idx) : pathText(idx))) return false;
            }
            return r.ok;
        }
        if (type == tTokenListOp || type == tStringListOp || type == tPathListOp) {
            v.kind = Value::Kind::Text;
            Reader r{b_};
            r.seek(int64_t(payload));
            return listOp(r, [&](Reader& rr, bool keep) {
                const uint32_t idx = rr.get<uint32_t>();
                if (keep && !keepText(v.texts, type == tTokenListOp ? token(idx) : type == tStringListOp ? string(idx) : pathText(idx))) return false;
                return rr.ok;
            });
        }
        if (type == tSpecifier || type == tVariability || type == tPermission) {
            v.kind = Value::Kind::Number, v.numbers.push_back(double(payload & 0xffffffff));
            return true;
        }
        if (comp == Comp::None) return true;  // (a type not read: no value)
        v.width = width;
        if (inlined) {
            const uint32_t d = uint32_t(payload);
            if (comp == Comp::StringIdx || comp == Comp::TokenIdx) {
                v.kind = Value::Kind::Text, v.texts.push_back(comp == Comp::StringIdx ? string(d) : token(d));
                return true;
            }
            v.kind = Value::Kind::Number;
            if (width == 1) {
                double x = 0;
                switch (comp) {
                    case Comp::U8: x = d & 0xff; break;
                    case Comp::I32: x = int32_t(d); break;
                    case Comp::U32: x = d; break;
                    case Comp::I64: x = double(int64_t(payload << 16) >> 16); break;
                    case Comp::U64: x = double(payload); break;
                    case Comp::F16: x = halfToFloat(uint16_t(d)); break;
                    case Comp::F32: case Comp::F64: {  // (a double inlined is stored as a float)
                        float f;
                        std::memcpy(&f, &d, 4);
                        x = f;
                        break;
                    }
                    default: break;
                }
                v.numbers.push_back(x);
                return true;
            }
            if (type == tVec2h) {
                v.numbers = {halfToFloat(uint16_t(d)), halfToFloat(uint16_t(d >> 16))};
                return true;
            }
            int8_t c[4];
            std::memcpy(c, &d, 4);
            if (type == tMatrix2d || type == tMatrix3d || type == tMatrix4d) {  // (diagonal)
                const int n = type == tMatrix2d ? 2 : type == tMatrix3d ? 3 : 4;
                v.numbers.assign(size_t(n * n), 0);
                for (int k = 0; k < n; ++k) v.numbers[size_t(k * n + k)] = c[k];
                return true;
            }
            for (int k = 0; k < width && k < 4; ++k) v.numbers.push_back(c[k]);
            return true;
        }
        Reader r{b_};
        r.seek(int64_t(payload));
        if (!array) return components(r, comp, size_t(width), v) || bad("a value lies outside the file");
        v.array = true;
        if (payload == 0) {
            v.kind = comp == Comp::StringIdx || comp == Comp::TokenIdx ? Value::Kind::Text : Value::Kind::Number;
            return true;
        }
        if (!atLeast(0, 5)) r.get<uint32_t>();  // (a shape, not used)
        const uint64_t n = atLeast(0, 7) ? r.get<uint64_t>() : r.get<uint32_t>();
        if (!r.ok || n > kMaxElements) return bad("an array is damaged");
        if (compressed && n >= 16 && width == 1) {
            v.kind = Value::Kind::Number;
            if (!spend(size_t(n) * sizeof(double))) return false;
            v.numbers.resize(size_t(n));
            if (comp == Comp::I32 || comp == Comp::U32) {
                std::vector<int32_t> ints;
                if (!compressedInts(r, size_t(n), ints)) return false;
                for (size_t k = 0; k < ints.size(); ++k) v.numbers[k] = comp == Comp::I32 ? double(ints[k]) : double(uint32_t(ints[k]));
                return true;
            }
            if (comp == Comp::I64 || comp == Comp::U64) {
                std::vector<int64_t> ints;
                if (!compressedInts(r, size_t(n), ints)) return false;
                for (size_t k = 0; k < ints.size(); ++k) v.numbers[k] = comp == Comp::I64 ? double(ints[k]) : double(uint64_t(ints[k]));
                return true;
            }
            if (comp == Comp::F16 || comp == Comp::F32 || comp == Comp::F64) {
                const char code = char(r.get<int8_t>());
                if (code == 'i') {
                    std::vector<int32_t> ints;
                    if (!compressedInts(r, size_t(n), ints)) return false;
                    for (size_t k = 0; k < ints.size(); ++k) v.numbers[k] = ints[k];
                    return true;
                }
                if (code == 't') {
                    const uint32_t lutSize = r.get<uint32_t>();
                    if (!r.ok || lutSize > n) return bad("an array is damaged");
                    Value lut;
                    if (!components(r, comp, lutSize, lut)) return false;
                    std::vector<uint32_t> idx;
                    if (!compressedInts(r, size_t(n), idx)) return false;
                    for (size_t k = 0; k < idx.size(); ++k) {
                        if (idx[k] >= lutSize) return bad("an array is damaged");
                        v.numbers[k] = lut.numbers[idx[k]];
                    }
                    return true;
                }
                return bad("an array's compression is not known");
            }
        }
        return components(r, comp, size_t(n) * size_t(width), v);
    }

    // A list op's items (explicit, added, prepended and appended kept; deleted and ordered ones read past), each read
    // by `item`.
    bool listOp(Reader& r, const std::function<bool(Reader&, bool keep)>& item) {
        const uint8_t h = r.get<uint8_t>();
        // Its lists in the order they are written: explicit 2, added 4, prepended 32, appended 64, deleted 8,
        // ordered 16.
        for (const int bit : {2, 4, 32, 64, 8, 16}) {
            if (!(h & bit)) continue;
            const uint64_t n = r.get<uint64_t>();
            if (!r.ok || n > kMaxElements) return bad("a list is damaged");
            for (uint64_t k = 0; k < n; ++k)
                if (!item(r, bit != 8 && bit != 16)) return bad("a list is damaged");
        }
        return r.ok;
    }

    bool skipDictionary(Reader& r, int depth = 0) {
        if (depth > 8) return bad("dictionaries nest too deeply");
        const uint64_t n = r.get<uint64_t>();
        if (!r.ok || n > kMaxElements) return bad("a dictionary is damaged");
        for (uint64_t k = 0; k < n && r.ok; ++k) {
            r.get<uint32_t>();
            const size_t at = r.pos;
            const int64_t off = r.get<int64_t>();
            r.seek(int64_t(at) + off);
            r.get<uint64_t>();
        }
        return r.ok;
    }

    bool arc(Reader& r, bool reference, std::vector<Arc>& out, bool keep) {
        Arc a;
        a.asset = string(r.get<uint32_t>());
        a.prim = pathText(r.get<uint32_t>());
        if (reference || atLeast(0, 8)) a.offset = r.get<double>(), a.scale = r.get<double>();
        if (reference && !skipDictionary(r)) return false;
        if (a.prim == "/") a.prim.clear();
        if (keep) out.push_back(a);
        return r.ok;
    }
    bool arcs(uint64_t rep, std::vector<Arc>& out) {
        const int type = int(rep >> 48 & 0xff);
        Reader r{b_};
        r.seek(int64_t(rep & ((uint64_t(1) << 48) - 1)));
        if (type == tPayload) return arc(r, false, out, true);
        if (type != tReferenceListOp && type != tPayloadListOp) return true;
        const uint8_t h = r.get<uint8_t>();
        for (const int bit : {2, 4, 32, 64, 8, 16}) {
            if (!(h & bit)) continue;
            const uint64_t n = r.get<uint64_t>();
            if (!r.ok || n > kMaxElements) return bad("a reference list is damaged");
            for (uint64_t k = 0; k < n; ++k)
                if (!arc(r, type == tReferenceListOp, out, bit != 8 && bit != 16)) return bad("a reference list is damaged");
        }
        return r.ok;
    }

    bool timeSamples(uint64_t rep, Attribute& a) {
        Reader r{b_};
        r.seek(int64_t(rep & ((uint64_t(1) << 48) - 1)));
        const size_t at = r.pos;
        const int64_t off = r.get<int64_t>();
        r.seek(int64_t(at) + off);
        const uint64_t timesRep = r.get<uint64_t>();
        const size_t values = r.pos;
        const int64_t off2 = r.get<int64_t>();
        r.seek(int64_t(values) + off2);
        const uint64_t n = r.get<uint64_t>();
        if (!r.ok || n > kMaxElements) return bad("time samples are damaged");
        Value times;
        if (!unpack(timesRep, times) || times.numbers.size() != n) return bad("time samples are damaged");
        std::vector<uint64_t> reps(static_cast<size_t>(n));
        for (uint64_t& v : reps) v = r.get<uint64_t>();
        if (!r.ok) return bad("time samples are damaged");
        for (size_t k = 0; k < reps.size(); ++k) {
            Value v;
            if (!unpack(reps[k], v)) return false;
            a.samples.emplace_back(times.numbers[k], v);
        }
        std::stable_sort(a.samples.begin(), a.samples.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        return true;
    }

    // The specs into prims: every prim and variant made first (as nodes by path), then their fields filled in, then
    // the tree put together in the order each prim lists its children.
    bool build() {
        struct Node {
            Prim prim;
            std::vector<int> kids;
            std::string set, variant;  // a variant's set and name
            std::vector<std::string> order;
        };
        std::vector<Node> nodes(1);
        std::vector<int> nodeOf(paths_.size(), -1);
        std::function<int(uint32_t, int)> node = [&](uint32_t p, int depth) -> int {
            if (p >= paths_.size() || depth > kMaxDepth || !paths_[p].set || paths_[p].property) return -1;
            if (nodeOf[p] >= 0) return nodeOf[p];
            if (paths_[p].parent < 0) return nodeOf[p] = 0;
            const int parent = node(uint32_t(paths_[p].parent), depth + 1);
            if (parent < 0) return -1;
            Node n;
            const std::string& e = paths_[p].element;
            if (!e.empty() && e[0] == '{') {
                const size_t eq = e.find('=');
                if (eq == std::string::npos || e.back() != '}') return -1;
                n.set = e.substr(1, eq - 1), n.variant = e.substr(eq + 1, e.size() - eq - 2);
            } else {
                n.prim.name = e;
            }
            nodes.push_back(std::move(n));
            const int id = int(nodes.size()) - 1;
            nodes[size_t(parent)].kids.push_back(id);
            return nodeOf[p] = id;
        };
        auto fields = [&](const Spec& s, const std::function<bool(const std::string&, uint64_t)>& f) {
            for (size_t k = s.fieldSet; k < fieldSets_.size() && fieldSets_[k] != 0xffffffffu; ++k) {
                if (k - s.fieldSet >= 4096) return bad("a spec has too many fields");
                const uint32_t fi = fieldSets_[k];
                if (fi >= fieldTokens_.size()) return bad("a field is missing");
                if (!f(token(fieldTokens_[fi]), fieldReps_[fi])) return false;
            }
            return true;
        };
        for (const Spec& s : specs_) {
            if (s.path >= paths_.size()) return bad("a spec's path is missing");
            if (s.type == 7) {  // the pseudo-root: the layer's metadata
                const bool good = fields(s, [&](const std::string& name, uint64_t rep) {
                    Value v;
                    if (name == "primChildren") {
                        if (!unpack(rep, v)) return false;
                        nodes[0].order = v.texts;
                        return true;
                    }
                    if (!unpack(rep, v)) return false;
                    if (name == "upAxis") layer_.upAxis = v.text();
                    else if (name == "defaultPrim") layer_.defaultPrim = v.text();
                    else if (name == "metersPerUnit") layer_.metersPerUnit = v.number(0, 0.01);
                    else if (name == "timeCodesPerSecond") layer_.timeCodesPerSecond = v.number();
                    else if (name == "framesPerSecond") layer_.framesPerSecond = v.number();
                    else if (name == "startTimeCode") layer_.startTimeCode = v.number(), layer_.hasTimeRange = true;
                    else if (name == "endTimeCode") layer_.endTimeCode = v.number(), layer_.hasTimeRange = true;
                    else if (name == "subLayers") layer_.subLayers = v.texts;
                    else if (name == "subLayerOffsets")
                        for (size_t k = 0; k + 1 < v.numbers.size(); k += 2) layer_.subLayerOffsets.emplace_back(v.numbers[k], v.numbers[k + 1]);
                    return true;
                });
                if (!good) return false;
                continue;
            }
            if (s.type == 6 || s.type == 10) {  // a prim, or a variant
                const int id = node(s.path, 0);
                if (id < 0) continue;
                Prim& p = nodes[size_t(id)].prim;
                const bool good = fields(s, [&](const std::string& name, uint64_t rep) {
                    if (name == "references" || name == "payload") return arcs(rep, name == "payload" ? p.payloads : p.references);
                    if (name == "variantSelection") {
                        Reader r{b_};
                        r.seek(int64_t(rep & ((uint64_t(1) << 48) - 1)));
                        const uint64_t n = r.get<uint64_t>();
                        if (!r.ok || n > kMaxElements) return bad("variant selections are damaged");
                        for (uint64_t k = 0; k < n && r.ok; ++k) {
                            const std::string set = string(r.get<uint32_t>());
                            p.variantSelection[set] = string(r.get<uint32_t>());
                        }
                        return r.ok;
                    }
                    const int type = int(rep >> 48 & 0xff);
                    if (type == tDictionary || type == tUnregisteredValue || type == tUnregisteredValueListOp) return true;
                    Value v;
                    if (!unpack(rep, v)) return false;
                    if (name == "specifier") {
                        const int sp = int(v.number());
                        p.specifier = sp == 0 ? Prim::Specifier::Def : sp == 1 ? Prim::Specifier::Over : Prim::Specifier::Class;
                    } else if (name == "typeName") {
                        p.typeName = v.text();
                    } else if (name == "primChildren") {
                        nodes[size_t(id)].order = v.texts;
                    } else if (name == "apiSchemas") {
                        p.apiSchemas = v.texts;
                    } else if (name == "kind") {
                        p.kind = v.text();
                    } else if (name == "active") {
                        p.active = v.number(0, 1) != 0;
                    }
                    return true;
                });
                if (!good) return false;
                continue;
            }
            if (s.type == 1 || s.type == 8) {  // an attribute, or a relationship
                const PathNode& pn = paths_[s.path];
                if (!pn.property || pn.parent < 0) continue;
                const int id = node(uint32_t(pn.parent), 0);
                if (id < 0) continue;
                Prim& p = nodes[size_t(id)].prim;
                if (s.type == 8) {
                    std::vector<std::string>& targets = p.relationships[pn.element];
                    const bool good = fields(s, [&](const std::string& name, uint64_t rep) {
                        if (name != "targetPaths" && name != "targetChildren") return true;
                        Value v;
                        if (!unpack(rep, v)) return false;
                        if (name == "targetPaths" || targets.empty()) targets = v.texts;
                        return true;
                    });
                    if (!good) return false;
                    continue;
                }
                Attribute& a = p.attributes[pn.element];
                const bool good = fields(s, [&](const std::string& name, uint64_t rep) {
                    const int type = int(rep >> 48 & 0xff);
                    if (name == "timeSamples") return type == tTimeSamples ? timeSamples(rep, a) : true;
                    if (type == tDictionary || type == tUnregisteredValue || type == tUnregisteredValueListOp) return true;
                    Value v;
                    if (!unpack(rep, v)) return false;
                    if (name == "typeName") a.typeName = v.text();
                    else if (name == "default") a.value = v;
                    else if (name == "variability") a.uniform = v.number() == 1;
                    else if (name == "connectionPaths") a.connections = v.texts;
                    else if (name != "custom" && name != "targetPaths" && name != "connectionChildren") a.meta[name] = v;
                    return true;
                });
                if (!good) return false;
            }
        }
        // Quaternions and assets as their attribute's type says (crate keeps x, y, z, w already).
        for (Node& n : nodes)
            for (auto& [name, a] : n.prim.attributes) {
                if (a.typeName.rfind("asset", 0) == 0) {
                    a.value.asset = a.value.kind == Value::Kind::Text;
                    for (auto& s : a.samples) s.second.asset = s.second.kind == Value::Kind::Text;
                }
            }
        // The tree, children in the order their parent lists them (the rest after).
        std::function<Prim(int, int)> assemble = [&](int id, int depth) -> Prim {
            Node& n = nodes[size_t(id)];
            Prim p = std::move(n.prim);
            // (Ranked once each, by a lookup: a parent may have very many children.)
            std::unordered_map<std::string, size_t> place;
            for (size_t k = 0; k < n.order.size(); ++k) place.emplace(n.order[k], k);
            std::vector<std::pair<size_t, int>> kids;
            for (const int k : n.kids) {
                const auto it = place.find(nodes[size_t(k)].prim.name);
                kids.push_back({it == place.end() ? n.order.size() : it->second, k});
            }
            std::stable_sort(kids.begin(), kids.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            if (depth > kMaxDepth) return p;
            for (const auto& [rank, k] : kids) {
                Node& kid = nodes[size_t(k)];
                if (!kid.set.empty()) {
                    const std::string set = kid.set, variant = kid.variant;
                    p.variantSets[set][variant] = assemble(k, depth + 1);
                } else {
                    p.children.push_back(assemble(k, depth + 1));
                }
            }
            return p;
        };
        layer_.root = assemble(0, 0);
        return true;
    }
};

}  // namespace

bool parseUsdc(const std::string& bytes, Layer& out, std::string* error) {
    out = Layer{};
    return CrateReader(bytes, out).read(error);
}

bool parseLayer(const std::string& bytes, Layer& out, std::string* error) {
    if (bytes.compare(0, 8, "PXR-USDC") == 0) return parseUsdc(bytes, out, error);
    if (bytes.compare(0, 5, "#usda") == 0) return parseUsda(bytes, out, error);
    if (error) *error = "Not a USD layer (USDA or USDC)";
    out = Layer{};
    return false;
}

bool isUsdPath(const std::string& path) {
    const QString s = QFileInfo(QString::fromStdString(path)).suffix().toLower();
    return s == "usd" || s == "usda" || s == "usdc" || s == "usdz";
}

// ---- Stages ----------------------------------------------------------------------------------------------------------

namespace {

// "pkg.usdz[inner/file]" split into the package and the file in it; false for a plain file.
bool splitPackaged(const std::string& id, std::string& package, std::string& inner) {
    if (id.empty() || id.back() != ']') return false;
    const size_t open = id.rfind('[');
    if (open == std::string::npos || open == 0) return false;
    package = id.substr(0, open), inner = id.substr(open + 1, id.size() - open - 2);
    return true;
}

std::string cleanInner(const std::string& p) {
    std::vector<std::string> parts;
    std::string cur;
    auto flush = [&] {
        if (cur == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!cur.empty() && cur != ".") {
            parts.push_back(cur);
        }
        cur.clear();
    };
    for (const char c : p) {
        if (c == '/' || c == '\\') flush();
        else cur += c;
    }
    flush();
    std::string out;
    for (const std::string& s : parts) out += (out.empty() ? "" : "/") + s;
    return out;
}

std::string defaultRead(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::u8path(path);
    if (!std::filesystem::is_regular_file(p, ec) || std::filesystem::file_size(p, ec) > uintmax_t(kMaxFile) || ec) return {};
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray b = f.read(kMaxFile + 1);
    return b.size() > kMaxFile ? std::string() : b.toStdString();
}

class Composer {
public:
    Composer(Stage& stage, FileReader read) : stage_(stage), read_(read ? read : defaultRead) {}

    // A layer's bytes by its resolved name (a file, or a file in a package).
    bool bytes(const std::string& id, std::string& out) {
        std::string package, inner;
        if (splitPackaged(id, package, inner)) {
            const ZipReader* z = zip(package);
            return z && z->read(inner, out);
        }
        out = read_(id);
        if (std::find(stage_.files.begin(), stage_.files.end(), id) == stage_.files.end()) stage_.files.push_back(id);
        return !out.empty();
    }

    // The flattened layer `id` names (a .usdz: the first layer in it). Null (with a note) when it cannot be read or
    // composes into itself. Variants are chosen only in the root layer (`variants`), so a selection made where a
    // layer is referenced wins over the layer's own.
    std::shared_ptr<const Layer> layer(std::string id, int depth, bool variants = false) {
        if (QFileInfo(QString::fromStdString(id)).suffix().toLower() == "usdz") {
            const ZipReader* z = zip(id);
            if (!z || z->entries().empty()) return note("Cannot read the package " + id);
            id += "[" + z->entries().front().name + "]";
        }
        if (const auto it = done_.find(id); it != done_.end() && !variants) return it->second;
        if (depth > 16 || std::find(open_.begin(), open_.end(), id) != open_.end()) return note("A layer composes into itself: " + id);
        std::string b, err;
        if (!bytes(id, b)) return note("Cannot read " + id);
        auto l = std::make_shared<Layer>();
        if (!parseLayer(b, *l, &err)) return note(id + ": " + err);
        open_.push_back(id);
        resolveAssets(l->root, id, 0);
        // Sublayers are weaker than the layer, and each than the one before it.
        for (size_t k = 0; k < l->subLayers.size(); ++k) {
            const auto sub = layer(resolve(l->subLayers[k], id), depth + 1);
            if (!sub) continue;
            Prim root = sub->root;
            if (k < l->subLayerOffsets.size()) shiftTimes(root, l->subLayerOffsets[k].first, l->subLayerOffsets[k].second, 0);
            merge(l->root, root, 0);
            if (l->timeCodesPerSecond <= 0) l->timeCodesPerSecond = sub->timeCodesPerSecond;
            if (l->framesPerSecond <= 0) l->framesPerSecond = sub->framesPerSecond;
            if (!l->hasTimeRange && sub->hasTimeRange)
                l->startTimeCode = sub->startTimeCode, l->endTimeCode = sub->endTimeCode, l->hasTimeRange = true;
            if (l->defaultPrim.empty()) l->defaultPrim = sub->defaultPrim;
        }
        l->subLayers.clear(), l->subLayerOffsets.clear();
        for (Prim& c : l->root.children) flatten(c, "/" + c.name, *l, id, depth, 0, variants);
        open_.pop_back();
        if (!variants) done_[id] = l;
        return l;
    }

    // An asset path resolved where it was written: in the package beside the layer that wrote it (if the package
    // holds it), else as a file beside the layer (or the package).
    std::string resolve(const std::string& asset, const std::string& anchor) {
        if (asset.empty()) return {};
        std::string a = asset;
        std::replace(a.begin(), a.end(), '\\', '/');
        std::string package, inner;
        if (splitPackaged(anchor, package, inner)) {
            const size_t slash = inner.rfind('/');
            const std::string within = cleanInner((slash == std::string::npos ? std::string() : inner.substr(0, slash + 1)) + a);
            if (const ZipReader* z = zip(package); z && z->find(within)) return package + "[" + within + "]";
            return fileBeside(a, package);
        }
        return fileBeside(a, anchor);
    }

    std::vector<std::string> notes;
    std::map<std::string, std::shared_ptr<const ZipReader>> packages;

private:
    Stage& stage_;
    FileReader read_;
    std::map<std::string, std::shared_ptr<const Layer>> done_;
    std::vector<std::string> open_;

    std::shared_ptr<const Layer> note(const std::string& n) {
        if (std::find(notes.begin(), notes.end(), n) == notes.end()) notes.push_back(n);
        return nullptr;
    }

    const ZipReader* zip(const std::string& package) {
        if (const auto it = packages.find(package); it != packages.end()) return it->second.get();
        auto z = std::make_shared<ZipReader>();
        std::string b = read_(package);
        if (std::find(stage_.files.begin(), stage_.files.end(), package) == stage_.files.end()) stage_.files.push_back(package);
        if (b.empty() || !z->open(std::move(b))) z.reset();
        packages[package] = z;
        return z.get();
    }

    static std::string fileBeside(const std::string& asset, const std::string& anchor) {
        if (QDir::isAbsolutePath(QString::fromStdString(asset))) return QDir::cleanPath(QString::fromStdString(asset)).toStdString();
        const QString dir = QFileInfo(QString::fromStdString(anchor)).path();
        return QDir::cleanPath(dir + "/" + QString::fromStdString(asset)).toStdString();
    }

    // Every asset path in the prims (values, samples, references and payloads) resolved against the layer. A
    // reference within the layer made inside a variant names the layer, as the variant may be chosen elsewhere.
    void resolveAssets(Prim& p, const std::string& anchor, int depth, bool inVariant = false) {
        if (depth > kMaxDepth) return;
        for (auto& [n, a] : p.attributes) {
            auto fix = [&](Value& v) {
                if (v.asset)
                    for (std::string& t : v.texts) t = resolve(t, anchor);
            };
            fix(a.value);
            for (auto& s : a.samples) fix(s.second);
        }
        for (std::vector<Arc>* arcs : {&p.references, &p.payloads})
            for (Arc& a : *arcs) a.asset = a.asset.empty() && inVariant ? anchor : resolve(a.asset, anchor);
        for (Prim& c : p.children) resolveAssets(c, anchor, depth + 1, inVariant);
        for (auto& [set, variants] : p.variantSets)
            for (auto& [name, v] : variants) resolveAssets(v, anchor, depth + 1, true);
    }

    static void shiftTimes(Prim& p, double offset, double scale, int depth) {
        if ((offset == 0 && scale == 1) || depth > kMaxDepth) return;
        for (auto& [n, a] : p.attributes) {
            for (auto& s : a.samples) s.first = s.first * scale + offset;
            if (scale < 0) std::reverse(a.samples.begin(), a.samples.end());  // (kept in time order)
        }
        for (Prim& c : p.children) shiftTimes(c, offset, scale, depth + 1);
        for (auto& [set, variants] : p.variantSets)
            for (auto& [name, v] : variants) shiftTimes(v, offset, scale, depth + 1);
    }

    // Paths inside a referenced prim's tree moved from where it was to where it is now.
    static void remap(Prim& p, const std::string& from, const std::string& to, int depth) {
        if (from.empty() || from == to || depth > kMaxDepth) return;
        auto fix = [&](std::string& s) {
            if (s.compare(0, from.size(), from) == 0 && (s.size() == from.size() || s[from.size()] == '/' || s[from.size()] == '.' || s[from.size()] == '{'))
                s = to + s.substr(from.size());
        };
        for (auto& [n, a] : p.attributes)
            for (std::string& c : a.connections) fix(c);
        for (auto& [n, targets] : p.relationships)
            for (std::string& t : targets) fix(t);
        for (Prim& c : p.children) remap(c, from, to, depth + 1);
        for (auto& [set, variants] : p.variantSets)
            for (auto& [name, v] : variants) remap(v, from, to, depth + 1);
    }

    // `weak`'s opinions added under `strong`'s: whatever the stronger already says stands.
    static void merge(Prim& strong, const Prim& weak, int depth) {
        if (depth > kMaxDepth) return;
        if (strong.typeName.empty()) strong.typeName = weak.typeName;
        if (weak.specifier == Prim::Specifier::Def) strong.specifier = Prim::Specifier::Def;
        if (strong.kind.empty()) strong.kind = weak.kind;
        if (!weak.active) strong.active = strong.active && weak.active;
        for (const std::string& s : weak.apiSchemas)
            if (std::find(strong.apiSchemas.begin(), strong.apiSchemas.end(), s) == strong.apiSchemas.end()) strong.apiSchemas.push_back(s);
        for (const auto& [n, a] : weak.attributes) {
            auto it = strong.attributes.find(n);
            if (it == strong.attributes.end()) {
                strong.attributes[n] = a;
                continue;
            }
            Attribute& s = it->second;
            if (s.typeName.empty()) s.typeName = a.typeName;
            if (s.value.kind == Value::Kind::None && s.samples.empty()) s.value = a.value, s.samples = a.samples;
            if (s.connections.empty()) s.connections = a.connections;
            for (const auto& [k, v] : a.meta) s.meta.emplace(k, v);
            s.uniform = s.uniform || a.uniform;
        }
        for (const auto& [n, r] : weak.relationships) strong.relationships.emplace(n, r);
        for (const auto& [k, v] : weak.variantSelection) strong.variantSelection.emplace(k, v);
        for (const auto& [set, variants] : weak.variantSets)
            for (const auto& [name, v] : variants) {
                auto& to = strong.variantSets[set];
                if (const auto it = to.find(name); it != to.end()) merge(it->second, v, depth + 1);
                else to.emplace(name, v);
            }
        strong.references.insert(strong.references.end(), weak.references.begin(), weak.references.end());
        strong.payloads.insert(strong.payloads.end(), weak.payloads.begin(), weak.payloads.end());
        for (const Prim& c : weak.children) {
            if (Prim* s = strong.child(c.name)) merge(*s, c, depth + 1);
            else strong.children.push_back(c);
        }
    }

    static const Prim* find(const Prim& root, const std::string& path) {
        const Prim* p = &root;
        std::string part;
        for (size_t i = 1; i <= path.size() && p; ++i) {
            if (i == path.size() || path[i] == '/') {
                if (!part.empty()) p = p->child(part);
                part.clear();
            } else {
                part += path[i];
            }
        }
        return p;
    }

    // A prim's variants, references and payloads composed into it (each weaker than what it says itself, variants
    // stronger than references), until none is left; then its children.
    size_t flattened_ = 0;  // prims composed (references within references can multiply them)
    // Bytes of values copied in by references and payloads (one large array referenced many times would otherwise
    // be copied each time).
    size_t copied_ = 0;
    static size_t primBytes(const Prim& p, int depth) {
        auto valueBytes = [](const Value& v) {
            size_t n = 64 + v.numbers.size() * sizeof(double);
            for (const std::string& t : v.texts) n += sizeof(std::string) + t.size();
            return n;
        };
        size_t n = 256;
        if (depth > kMaxDepth) return n;
        for (const auto& [name, a] : p.attributes) {
            n += 128 + name.size() + valueBytes(a.value);
            for (const auto& s : a.samples) n += valueBytes(s.second);
        }
        for (const Prim& c : p.children) n += primBytes(c, depth + 1);
        for (const auto& [set, variants] : p.variantSets)
            for (const auto& [name, v] : variants) n += primBytes(v, depth + 1);
        return n;
    }
    bool mayCopy(const Prim& target) {
        const size_t n = primBytes(target, 0);
        if (n > (size_t(1) << 31) - copied_) {
            if (copied_ != SIZE_MAX) note("The scene's references copy in more than 2 GB: the rest are not composed");
            copied_ = SIZE_MAX;
            return false;
        }
        copied_ += n;
        return true;
    }
    void flatten(Prim& p, const std::string& path, const Layer& owner, const std::string& ownerId, int layerDepth, int depth, bool variants) {
        if (depth > kMaxDepth) return;
        if (++flattened_ > kMaxPrims) {
            if (flattened_ == kMaxPrims + 1) note("The scene composes to more than 262,144 prims: the rest are not composed");
            return;
        }
        for (int round = 0; round < 8; ++round) {
            bool changed = false;
            const auto selections = variants ? p.variantSelection : std::map<std::string, std::string>{};
            for (const auto& [set, sel] : selections) {
                auto s = p.variantSets.find(set);
                if (s == p.variantSets.end()) continue;
                auto v = s->second.find(sel);
                if (v == s->second.end()) continue;
                Prim chosen = std::move(v->second);
                p.variantSets.erase(s);
                chosen.name = p.name;
                merge(p, chosen, 0);
                changed = true;
            }
            std::vector<Arc> arcs = p.references;
            arcs.insert(arcs.end(), p.payloads.begin(), p.payloads.end());
            p.references.clear(), p.payloads.clear();
            for (const Arc& a : arcs) {
                Prim target;
                std::string from;
                if (a.asset.empty() && a.prim.empty()) continue;  // (an empty arc)
                if (a.asset.empty() || a.asset == ownerId) {
                    const Prim* t = a.prim.empty() ? nullptr : find(owner.root, a.prim);
                    if (!t || a.prim == path || path.compare(0, a.prim.size() + 1, a.prim + "/") == 0) {
                        note("A reference to " + (a.prim.empty() ? std::string("nothing") : a.prim) + " is not found or holds itself");
                        continue;
                    }
                    if (!mayCopy(*t)) continue;
                    target = *t;
                    from = a.prim;
                } else {
                    const auto l = layer(a.asset, layerDepth + 1);
                    if (!l) continue;
                    std::string name = a.prim;
                    if (name.empty()) name = !l->defaultPrim.empty() ? "/" + l->defaultPrim : (!l->root.children.empty() ? "/" + l->root.children.front().name : "");
                    const Prim* t = name.empty() ? nullptr : find(l->root, name);
                    if (!t) {
                        note(a.asset + " has no prim " + (name.empty() ? std::string("to reference") : name));
                        continue;
                    }
                    if (!mayCopy(*t)) continue;
                    target = *t;
                    from = name;
                }
                target.name = p.name;
                remap(target, from, path, 0);
                shiftTimes(target, a.offset, a.scale, 0);
                merge(p, target, 0);
                changed = true;
            }
            if (!changed) break;
        }
        for (Prim& c : p.children) flatten(c, path + "/" + c.name, owner, ownerId, layerDepth, depth + 1, variants);
    }
};

}  // namespace

const Prim* Stage::prim(const std::string& path) const {
    const Prim* p = &layer.root;
    std::string part;
    for (size_t i = 1; i <= path.size() && p; ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!part.empty()) p = p->child(part);
            part.clear();
        } else {
            part += path[i];
        }
    }
    return p;
}

bool Stage::readAsset(const std::string& resolved, std::string& bytes) const {
    std::string package, inner;
    if (splitPackaged(resolved, package, inner)) {
        const auto it = packages_.find(package);
        return it != packages_.end() && it->second && it->second->read(inner, bytes);
    }
    bytes = (reader_ ? reader_ : defaultRead)(resolved);
    return !bytes.empty();
}

bool openStage(const std::string& path, Stage& out, std::string* error, FileReader readFile) {
    out = Stage{};
    out.path = path;
    out.reader_ = readFile;
    Composer c(out, readFile);
    std::shared_ptr<const Layer> l;
    try {
        l = c.layer(path, 0, true);
    } catch (const std::bad_alloc&) {  // (whatever the limits above let through, a load does not end the program)
        c.notes.insert(c.notes.begin(), path + " needs more memory than there is");
        l = nullptr;
    }
    out.notes = c.notes;
    out.packages_ = c.packages;
    if (!l) {
        if (error) *error = c.notes.empty() ? "Cannot read " + path : c.notes.front();
        return false;
    }
    out.layer = *l;
    return true;
}

// ---- Writing ---------------------------------------------------------------------------------------------------------

std::string formatNumber(double v, bool single) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
    for (int digits = 6; digits <= 17; ++digits) {
        std::ostringstream s;
        s.imbue(std::locale::classic());
        s.precision(digits);
        s << v;
        const std::string t = s.str();
        size_t i = 0;
        double back = 0;
        if (readDecimal(t, i, back) && (single ? float(back) == float(v) : back == v)) return t;
    }
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s.precision(17);
    s << v;
    return s.str();
}

std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += c;
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out + "\"";
}

}  // namespace montage::usd
