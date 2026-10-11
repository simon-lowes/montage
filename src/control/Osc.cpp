#include "Osc.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace montage::osc {

namespace {

constexpr int kMaxDepth = 8;  // bundles inside bundles

uint32_t readU32(const std::string& p, size_t at) {
    return uint32_t(uint8_t(p[at])) << 24 | uint32_t(uint8_t(p[at + 1])) << 16 | uint32_t(uint8_t(p[at + 2])) << 8 | uint32_t(uint8_t(p[at + 3]));
}

uint64_t readU64(const std::string& p, size_t at) { return uint64_t(readU32(p, at)) << 32 | readU32(p, at + 4); }

void writeU32(std::string& out, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out += char((v >> shift) & 0xFF);
}

void writeU64(std::string& out, uint64_t v) {
    writeU32(out, uint32_t(v >> 32));
    writeU32(out, uint32_t(v));
}

void pad(std::string& out) {
    while (out.size() % 4) out += '\0';
}

// A string at `at` in [at, end): null-terminated and padded to four bytes; `at` moves past it.
bool readString(const std::string& p, size_t& at, size_t end, std::string& s) {
    const size_t zero = p.find('\0', at);
    if (zero == std::string::npos || zero >= end) return false;
    s.assign(p, at, zero - at);
    at = (zero + 4) & ~size_t(3);
    return at <= end;
}

bool fail(std::string* error, const char* what) {
    if (error) *error = what;
    return false;
}

bool parseMessage(const std::string& p, size_t at, size_t end, std::vector<Message>& out, std::string* error) {
    Message m;
    if (!readString(p, at, end, m.address) || m.address.empty() || m.address[0] != '/') return fail(error, "an OSC message starts with an address (\"/...\")");
    std::string tags;
    if (at == end) {
        out.push_back(std::move(m));  // (old senders leave the type tags out when there are no arguments)
        return true;
    }
    if (!readString(p, at, end, tags) || tags.empty() || tags[0] != ',') return fail(error, "an OSC message's type tags start with ','");
    for (size_t k = 1; k < tags.size(); ++k) {
        Value v;
        v.type = tags[k];
        switch (v.type) {
            case 'i':
                if (end - at < 4) return fail(error, "an OSC int32 runs past the packet");
                v.i = int32_t(readU32(p, at));
                at += 4;
                break;
            case 'h':
                if (end - at < 8) return fail(error, "an OSC int64 runs past the packet");
                v.i = int64_t(readU64(p, at));
                at += 8;
                break;
            case 'f': {
                if (end - at < 4) return fail(error, "an OSC float32 runs past the packet");
                const uint32_t bits = readU32(p, at);
                float f;
                std::memcpy(&f, &bits, 4);
                v.f = f;
                at += 4;
                break;
            }
            case 'd': {
                if (end - at < 8) return fail(error, "an OSC double runs past the packet");
                const uint64_t bits = readU64(p, at);
                double d;
                std::memcpy(&d, &bits, 8);
                v.f = d;
                at += 8;
                break;
            }
            case 's':
            case 'S':
                if (!readString(p, at, end, v.s)) return fail(error, "an OSC string runs past the packet");
                break;
            case 'b': {
                if (end - at < 4) return fail(error, "an OSC blob runs past the packet");
                const uint32_t n = readU32(p, at);
                at += 4;
                if (n > end - at) return fail(error, "an OSC blob runs past the packet");
                v.s.assign(p, at, n);
                at = (at + n + 3) & ~size_t(3);
                if (at > end) return fail(error, "an OSC blob runs past the packet");
                break;
            }
            case 't':  // a time tag as an argument: kept as a number
                if (end - at < 8) return fail(error, "an OSC time tag runs past the packet");
                v.i = int64_t(readU64(p, at));
                at += 8;
                break;
            case 'c':
            case 'r':
            case 'm':
                if (end - at < 4) return fail(error, "an OSC argument runs past the packet");
                v.i = int32_t(readU32(p, at));
                at += 4;
                break;
            case 'T':
            case 'F':
            case 'N':
            case 'I':
                break;
            case '[':
            case ']':
                continue;  // (arrays are read flat)
            default: return fail(error, "an OSC message has an unknown type tag");
        }
        m.args.push_back(std::move(v));
    }
    out.push_back(std::move(m));
    return true;
}

bool parseElement(const std::string& p, size_t at, size_t end, std::vector<Message>& out, std::string* error, int depth) {
    if (end - at >= 8 && p.compare(at, 8, std::string("#bundle\0", 8)) == 0) {
        if (depth >= kMaxDepth) return fail(error, "OSC bundles nested too deeply");
        if (end - at < 16) return fail(error, "an OSC bundle has no time tag");
        at += 16;  // "#bundle\0" and the time tag (not waited for)
        while (at < end) {
            if (end - at < 4) return fail(error, "an OSC bundle element runs past the packet");
            const uint32_t n = readU32(p, at);
            at += 4;
            if (n == 0 || n % 4 || n > end - at) return fail(error, "an OSC bundle element has a wrong size");
            if (!parseElement(p, at, at + n, out, error, depth + 1)) return false;
            at += n;
        }
        return true;
    }
    return parseMessage(p, at, end, out, error);
}

}  // namespace

double Value::number(bool* ok) const {
    if (ok) *ok = true;
    switch (type) {
        case 'i':
        case 'h': return double(i);
        case 'f':
        case 'd': return f;
        case 'T': return 1;
        case 'F': return 0;
        case 's':
        case 'S': {
            char* rest = nullptr;
            const double d = std::strtod(s.c_str(), &rest);
            if (!s.empty() && rest && *rest == '\0' && std::isfinite(d)) return d;
            break;
        }
        default: break;
    }
    if (ok) *ok = false;
    return 0;
}

std::string Value::text() const {
    switch (type) {
        case 's':
        case 'S': return s;
        case 'i':
        case 'h': return std::to_string(i);
        case 'f':
        case 'd': {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.10g", f);
            return buf;
        }
        case 'T': return "true";
        case 'F': return "false";
        default: return {};
    }
}

bool Value::truthy() const {
    if (type == 's' || type == 'S') return s == "1" || s == "true" || s == "on" || s == "yes";
    bool ok = false;
    const double d = number(&ok);
    return ok && d != 0;
}

Value int32(int32_t v) {
    Value x;
    x.type = 'i';
    x.i = v;
    return x;
}

Value float32(float v) {
    Value x;
    x.type = 'f';
    x.f = v;
    return x;
}

Value string(std::string v) {
    Value x;
    x.type = 's';
    x.s = std::move(v);
    return x;
}

Value boolean(bool v) {
    Value x;
    x.type = v ? 'T' : 'F';
    return x;
}

bool parse(const std::string& packet, std::vector<Message>& out, std::string* error) {
    std::vector<Message> found;
    if (packet.empty() || packet.size() % 4) return fail(error, "an OSC packet is a whole number of four-byte words");
    if (!parseElement(packet, 0, packet.size(), found, error, 0)) return false;
    out.insert(out.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
    return true;
}

std::string encode(const Message& m) {
    std::string out = m.address;
    out += '\0';
    pad(out);
    std::string tags = ",";
    for (const Value& v : m.args) tags += v.type;
    out += tags;
    out += '\0';
    pad(out);
    for (const Value& v : m.args) {
        switch (v.type) {
            case 'i':
            case 'c':
            case 'r':
            case 'm': writeU32(out, uint32_t(int32_t(v.i))); break;
            case 'h':
            case 't': writeU64(out, uint64_t(v.i)); break;
            case 'f': {
                const float f = float(v.f);
                uint32_t bits;
                std::memcpy(&bits, &f, 4);
                writeU32(out, bits);
                break;
            }
            case 'd': {
                uint64_t bits;
                std::memcpy(&bits, &v.f, 8);
                writeU64(out, bits);
                break;
            }
            case 's':
            case 'S':
                out += v.s;
                out += '\0';
                pad(out);
                break;
            case 'b':
                writeU32(out, uint32_t(v.s.size()));
                out += v.s;
                pad(out);
                break;
            default: break;
        }
    }
    return out;
}

}  // namespace montage::osc
