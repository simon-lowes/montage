// Montage — reading Media Composer bins (.avb) into the model AvidBin.h describes.
//
// This follows pyavb 1.4.0 (https://github.com/markreidvfx/pyavb), Mark Reid's reader and writer for Avid bins, under
// the MIT licence (Copyright (c) 2022 Mark Reid; the notice is in THIRD_PARTY_NOTICES.md and LICENSES/MIT-pyavb.txt).
// The layout of the file header and object table and of each class read here (what each field is, in what order, and
// the tagged extension fields after them) is pyavb's, rewritten in C++; the classes pyavb reads but the model does not
// need (effect keyframes FXPS, title pictures GRFX, colour correction, trackers, the remaining picture and sound
// descriptor fields) are left unread.
//
// A bin starts with its byte order (06 00 little-endian, 00 06 big-endian), "Domain" and a header naming how many
// objects follow and which is the bin; then each object: a four-letter class, its size and its bytes. An object is a
// run of sections, one for each class it derives from (a SourceClip is a Component, then a Clip, then a SourceClip),
// each opened by 02 and a version byte and followed by extension fields (01, a number, then a typed value); the last
// ends with 03. References are object numbers, 0 for none. Every read here is checked against the object's bytes; an
// object that runs out, has a field this reader does not know, or refers past the table is not read, and is kept as
// its class and the reason. Objects are read once each (shared when referred to twice), and a reference back to an
// object still being read is cut.
#include "AvidBin.h"

#include <QFile>
#include <QString>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

namespace montage {

namespace {

// Mac Roman's upper half (as Python's mac_roman codec maps it), for the bin's text.
const char16_t kMacRoman[128] = {
    0x00c4, 0x00c5, 0x00c7, 0x00c9, 0x00d1, 0x00d6, 0x00dc, 0x00e1, 0x00e0, 0x00e2, 0x00e4, 0x00e3, 0x00e5, 0x00e7,
    0x00e9, 0x00e8, 0x00ea, 0x00eb, 0x00ed, 0x00ec, 0x00ee, 0x00ef, 0x00f1, 0x00f3, 0x00f2, 0x00f4, 0x00f6, 0x00f5,
    0x00fa, 0x00f9, 0x00fb, 0x00fc, 0x2020, 0x00b0, 0x00a2, 0x00a3, 0x00a7, 0x2022, 0x00b6, 0x00df, 0x00ae, 0x00a9,
    0x2122, 0x00b4, 0x00a8, 0x2260, 0x00c6, 0x00d8, 0x221e, 0x00b1, 0x2264, 0x2265, 0x00a5, 0x00b5, 0x2202, 0x2211,
    0x220f, 0x03c0, 0x222b, 0x00aa, 0x00ba, 0x03a9, 0x00e6, 0x00f8, 0x00bf, 0x00a1, 0x00ac, 0x221a, 0x0192, 0x2248,
    0x2206, 0x00ab, 0x00bb, 0x2026, 0x00a0, 0x00c0, 0x00c3, 0x00d5, 0x0152, 0x0153, 0x2013, 0x2014, 0x201c, 0x201d,
    0x2018, 0x2019, 0x00f7, 0x25ca, 0x00ff, 0x0178, 0x2044, 0x20ac, 0x2039, 0x203a, 0xfb01, 0xfb02, 0x2021, 0x00b7,
    0x201a, 0x201e, 0x2030, 0x00c2, 0x00ca, 0x00c1, 0x00cb, 0x00c8, 0x00cd, 0x00ce, 0x00cf, 0x00cc, 0x00d3, 0x00d4,
    0xf8ff, 0x00d2, 0x00da, 0x00db, 0x00d9, 0x0131, 0x02c6, 0x02dc, 0x00af, 0x02d8, 0x02d9, 0x02da, 0x00b8, 0x02dd,
    0x02db, 0x02c7,
};

std::string macRomanToUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char ch : s) {
        const uint8_t b = uint8_t(ch);
        const uint32_t c = b < 0x80 ? b : kMacRoman[b - 0x80];
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xc0 | (c >> 6));
            out += char(0x80 | (c & 0x3f));
        } else {
            out += char(0xe0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3f));
            out += char(0x80 | (c & 0x3f));
        }
    }
    return out;
}

// The bytes of one object, read in the file's byte order. A read past the end, or a tag that is not the one expected,
// stops the cursor: every later read gives 0, and ok() says so.
class Cursor {
public:
    Cursor(const std::string& data, size_t pos, size_t size, bool le) : d_(data), pos_(pos), end_(pos + size), le_(le) {}

    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }
    void fail(const std::string& why) {
        if (ok_) error_ = why;
        ok_ = false;
    }
    size_t left() const { return ok_ ? end_ - pos_ : 0; }
    size_t pos() const { return pos_; }

    const unsigned char* take(size_t n) {
        if (!ok_ || n > end_ - pos_) {
            fail("the object ends early");
            return nullptr;
        }
        const unsigned char* p = reinterpret_cast<const unsigned char*>(d_.data()) + pos_;
        pos_ += n;
        return p;
    }
    void skip(size_t n) { take(n); }

    uint64_t uint(size_t n, bool le) {
        const unsigned char* p = take(n);
        if (!p) return 0;
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) v |= uint64_t(p[le ? i : n - 1 - i]) << (8 * i);
        return v;
    }
    uint8_t u8() { return uint8_t(uint(1, true)); }
    int8_t s8() { return int8_t(u8()); }
    bool boolean() { return u8() == 0x01; }
    uint16_t u16() { return uint16_t(uint(2, le_)); }
    int16_t s16() { return int16_t(u16()); }
    uint32_t u32() { return uint32_t(uint(4, le_)); }
    int32_t s32() { return int32_t(u32()); }
    uint64_t u64() { return uint(8, le_); }
    int64_t s64() { return int64_t(u64()); }
    uint32_t u32le() { return uint32_t(uint(4, true)); }
    uint32_t u32be() { return uint32_t(uint(4, false)); }
    // pyavb reads and writes doubles little-endian whichever the file's byte order.
    double dbl() {
        const uint64_t bits = uint(8, true);
        double v = 0;
        std::memcpy(&v, &bits, 8);
        return v;
    }
    // A rate as a mantissa and a power of ten (pyavb's exp10-encoded float).
    double exp10() {
        const int32_t mantissa = s32();
        const int16_t exponent = s16();
        return double(mantissa) * std::pow(10.0, double(exponent));
    }
    // A class ID: stored reversed in little-endian files.
    std::string fourcc() {
        const unsigned char* p = take(4);
        if (!p) return {};
        std::string s(reinterpret_cast<const char*>(p), 4);
        if (le_) std::swap(s[0], s[3]), std::swap(s[1], s[2]);
        return s;
    }
    std::string bytes(size_t n) {
        const unsigned char* p = take(n);
        return p ? std::string(reinterpret_cast<const char*>(p), n) : std::string();
    }
    // A string: a 16-bit size (65535 for none) and the bytes, Mac Roman unless `utf8`, null bytes trimmed at both ends.
    std::string string(bool utf8 = false) {
        const uint16_t n = u16();
        if (n == 0xffff) return {};
        std::string s = bytes(n);
        const size_t first = s.find_first_not_of('\0');
        if (first == std::string::npos) return {};
        s = s.substr(first, s.find_last_not_of('\0') - first + 1);
        return utf8 ? s : macRomanToUtf8(s);
    }
    void tag(uint8_t want) {
        const uint8_t got = u8();
        if (ok_ && got != want) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "expected tag 0x%02x, found 0x%02x", want, got);
            fail(buf);
        }
    }
    // pyavb's iter_ext: the next extension field's number, or -1 when the next byte does not open one.
    int ext() {
        if (!ok_ || pos_ >= end_ || uint8_t(d_[pos_]) != 0x01) return -1;
        ++pos_;
        return ok_ && pos_ < end_ ? u8() : (fail("the object ends early"), -1);
    }
    void unknownExt(int tag) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "unknown extension field 0x%02x", tag);
        fail(buf);
    }
    // A count of items each at least `itemSize` bytes long, refused if the object cannot hold that many.
    size_t count(int64_t n, size_t itemSize) {
        if (n < 0 || (itemSize && uint64_t(n) > left() / itemSize)) {
            fail("a count larger than the object");
            return 0;
        }
        return size_t(n);
    }

    // A UUID field by field (its first three in the file's byte order), kept with them little-endian as AAF does.
    void uuidFields(unsigned char* out) {
        const uint32_t d1 = u32();
        const uint16_t d2 = u16(), d3 = u16();
        for (int i = 0; i < 4; ++i) out[i] = uint8_t(d1 >> (8 * i));
        out[4] = uint8_t(d2), out[5] = uint8_t(d2 >> 8), out[6] = uint8_t(d3), out[7] = uint8_t(d3 >> 8);
    }
    // A UUID stored whole (pyavb's read_raw_uuid), written out as text.
    std::string rawUuid() {
        unsigned char b[16] = {};
        uuidFields(b);
        if (const unsigned char* p = take(8)) std::memcpy(b + 8, p, 8);
        char buf[40];
        std::snprintf(buf, sizeof buf, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                      unsigned(b[0] | b[1] << 8 | b[2] << 16 | uint32_t(b[3]) << 24), unsigned(b[4] | b[5] << 8),
                      unsigned(b[6] | b[7] << 8), b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        return buf;
    }
    // A mob ID (pyavb's read_mob_id): the label, length and instance number, then the material number as a tagged UUID.
    AvidMobId mobId() {
        AvidMobId id{};
        tag(65);
        if (s32() != 12 && ok_) fail("a mob ID's label is not 12 bytes");
        if (const unsigned char* p = take(12)) std::memcpy(id.data(), p, 12);
        for (int i = 12; i < 16; ++i) {
            tag(68);
            id[size_t(i)] = u8();
        }
        tag(72);
        const uint32_t d1 = u32();
        tag(70);
        const uint16_t d2 = u16();
        tag(70);
        const uint16_t d3 = u16();
        tag(65);
        if (s32() != 8 && ok_) fail("a mob ID's material number is not 16 bytes");
        for (int i = 0; i < 4; ++i) id[size_t(16 + i)] = uint8_t(d1 >> (8 * i));
        id[20] = uint8_t(d2), id[21] = uint8_t(d2 >> 8), id[22] = uint8_t(d3), id[23] = uint8_t(d3 >> 8);
        if (const unsigned char* p = take(8)) std::memcpy(id.data() + 24, p, 8);
        return id;
    }

private:
    const std::string& d_;
    size_t pos_, end_;
    bool le_;
    bool ok_ = true;
    std::string error_;
};

// The names of the effect parameters the importer looks for (from pyavb's table of parameter UUIDs).
const std::map<std::string, std::string>& parameterNames() {
    static const std::map<std::string, std::string> names = {
        {"72559a80-24d7-11d3-8a50-0050040ef7d2", "SpeedRatio"},
        {"8d56827c-847e-11d5-935a-50f857c10000", "PARAM_SPEED_MAP_U"},
        {"8d56827d-847e-11d5-935a-50f857c10000", "PARAM_SPEED_OFFSET_MAP_U"},
        {"8d568283-847e-11d5-935a-50f857c10000", "PARAM_PULLDOWN_PHASE_U"},
        {"f7ffed29-fc8e-43ed-943a-4b57b5c157ee", "AvidMotionPulldown"},
        {"e4962320-2267-11d3-8a4c-0050040ef7d2", "Level"},
        {"e4962322-2267-11d3-8a4c-0050040ef7d2", "Pan"},
        {"922d458d-8f22-11d4-a03c-0004ac969f50", "PanVol_IsTrimGainEffect"},
        {"8134fc48-3bd6-4904-9520-7a6360cc7127", "Curve Type"},
        {"0adba400-0bb6-43f3-a435-adb44045bab3", "Opacity"},
        {"8af7a5dc-367b-4919-afc7-5ca00a5bd558", "MC AVX2 EFFECT VERSION"},
    };
    return names;
}

std::string mediaKindName(int16_t id) {
    switch (id) {
    case 0: return {};
    case 1: return "picture";
    case 2: return "sound";
    case 3: return "timecode";
    case 4: return "edgecode";
    case 5: return "attribute";
    case 6: return "effectdata";
    case 7: return "DescriptiveMetadata";
    case 16: return "DataEssenceTrack";
    default: return "unknown" + std::to_string(id);
    }
}

std::string usageName(int32_t code) {
    switch (code) {
    case 0: return {};
    case 1: return "precompute";
    case 2: return "subclip";
    case 3: return "effect";
    case 4: return "group";
    case 5: return "groupoofter";
    case 6: return "motion";
    case 7: return "mastermob";
    case 9: return "precompute_file";
    default: return "unknown";
    }
}

const std::set<std::string> kClips = {"SCLP", "FILL", "TCCP", "ECCP", "TRKR", "CTRL", "PRCL"};
const std::set<std::string> kTrackEffects = {"TKFX", "PVOL", "ASPI", "EQMB"};
const std::set<std::string> kTimeWarps = {"MASK", "STRB", "SPED", "REPT"};
const std::set<std::string> kTrackGroups = {"TRKG", "TKFX", "PVOL", "ASPI", "EQMB", "MASK", "STRB",
                                            "SPED", "REPT", "RSET", "TNFX", "SLCT", "CMPO"};
const std::set<std::string> kDescriptors = {"MDES", "MDTP", "MDFM", "MDNG", "MDFL", "MULD", "WAVE", "AIFC", "PCMA",
                                            "MPGA", "DIDD", "CDCI", "MPGI", "JPED", "RGBA", "DATD", "ANCD"};
const std::set<std::string> kFileDescriptors = {"MDFL", "MULD", "WAVE", "AIFC", "PCMA", "MPGA", "DIDD",
                                                "CDCI", "MPGI", "JPED", "RGBA", "DATD", "ANCD"};
const std::set<std::string> kPictureDescriptors = {"DIDD", "CDCI", "MPGI", "JPED", "RGBA"};

constexpr int kMaxDepth = 128;
constexpr size_t kMaxCopies = 4'000'000;           // attributes and locators copied in all
constexpr int64_t kMaxLength = int64_t(1) << 40;  // edit units

size_t countAvidAttributes(const AvidAttributes& list) {
    size_t n = list.size();
    for (const AvidAttribute& a : list) n += countAvidAttributes(a.children);
    return n;
}

class BinReader {
public:
    struct Chunk {
        std::string classId;
        size_t pos = 0;
        uint32_t size = 0;
    };

    BinReader(const std::string& data, bool le, std::vector<Chunk> chunks, AvidBin& bin)
        : d_(data), le_(le), chunks_(std::move(chunks)), bin_(bin) {}

    bool readBin(uint32_t root, std::string* error);

private:
    const std::string& d_;
    bool le_;
    std::vector<Chunk> chunks_;  // object n is chunks_[n - 1]
    AvidBin& bin_;
    std::set<uint32_t> active_;  // objects being read, to cut reference loops
    std::set<std::string> issueSet_;
    std::map<uint32_t, std::shared_ptr<const AvidComponent>> components_;
    std::map<uint32_t, std::shared_ptr<const AvidDescriptor>> descriptors_;
    struct AttributeList {
        AvidAttributes attributes;
        std::vector<AvidLocator> locators;
        size_t nodes = 0;  // attributes and locators in it, nested ones too
    };
    std::map<uint32_t, AttributeList> attributes_;
    // Attribute lists are copied where they are used, so the copies are counted: a small file naming one large list
    // many times (or a list naming the next twice, and so on) cannot grow without bound.
    size_t copied_ = 0;
    bool spend(uint32_t index, size_t nodes) {
        if (nodes > kMaxCopies - copied_) {
            issue(index, "too many attributes to copy");
            return false;
        }
        copied_ += nodes;
        return true;
    }

    const Chunk& chunk(uint32_t index) const { return chunks_[index - 1]; }
    Cursor open(uint32_t index) const { return Cursor(d_, chunk(index).pos, chunk(index).size, le_); }
    // A reference: an object number in the table, or 0 for none (one past the table stops the cursor, as in pyavb).
    uint32_t ref(Cursor& c) const {
        const uint32_t index = c.u32();
        if (c.ok() && index > chunks_.size()) c.fail("a reference past the object table");
        return c.ok() ? index : 0;
    }
    void issue(uint32_t index, const std::string& why) {
        const std::string text = std::to_string(index) + " " + chunk(index).classId + ": " + why;
        if (issueSet_.insert(text).second) bin_.issues.push_back(text);
    }
    // Whether `index` may be read now; if not, why.
    std::string blocked(uint32_t index, int depth) const {
        if (active_.count(index)) return "it refers back to itself";
        if (depth > kMaxDepth) return "nested too deep";
        return {};
    }

    const AttributeList& attributes(uint32_t index, int depth);
    std::optional<AvidLocator> marker(uint32_t index, int depth);
    std::optional<AvidMediaLocator> mediaLocator(uint32_t index);
    std::vector<AvidParameter> parameters(uint32_t index, int depth);
    std::shared_ptr<const AvidComponent> component(uint32_t index, int depth);
    std::shared_ptr<const AvidDescriptor> descriptor(uint32_t index, int depth);
    void readComponent(Cursor& c, AvidComponent& out, int depth, AvidMob* mob);
    void readTrackGroup(Cursor& c, AvidComponent& out, int depth);
    void readDescriptor(Cursor& c, AvidDescriptor& out, int depth);
    std::optional<AvidMob> mob(uint32_t index);
    void readView(uint32_t index);
};

const BinReader::AttributeList& BinReader::attributes(uint32_t index, int depth) {
    static const AttributeList none;
    if (index == 0) return none;
    if (const auto it = attributes_.find(index); it != attributes_.end()) return it->second;
    if (chunk(index).classId != "ATTR") {
        issue(index, "an attribute list of another class");
        return none;
    }
    if (const std::string why = blocked(index, depth); !why.empty()) {
        issue(index, why);
        return none;
    }
    active_.insert(index);
    AttributeList list;
    Cursor c = open(index);
    c.tag(0x02);
    c.tag(0x01);
    const size_t count = c.count(c.u32(), 6);
    for (size_t i = 0; i < count && c.ok(); ++i) {
        AvidAttribute a;
        const uint32_t type = c.u32();
        a.name = c.string();
        if (type == 1) {
            a.kind = AvidAttribute::Kind::Int;
            a.intValue = c.s32();
        } else if (type == 2) {
            a.kind = AvidAttribute::Kind::String;
            a.text = c.string();
        } else if (type == 3) {
            a.kind = AvidAttribute::Kind::Object;
            if (const uint32_t r = ref(c)) {
                a.objectClass = chunk(r).classId;
                if (a.objectClass == "ATTR") {
                    const AttributeList& sub = attributes(r, depth + 1);
                    if (spend(r, sub.nodes)) {
                        a.children = sub.attributes;
                        list.nodes += sub.nodes;
                    }
                } else if (a.objectClass == "MCBR") {
                    // A bin reference (pyavb's BinRef): its UID, then its name, also as UTF-8 in an extension field.
                    Cursor b = open(r);
                    b.tag(0x02);
                    b.tag(0x01);
                    b.s32();
                    b.s32();
                    a.binName = b.string();
                    for (int t = b.ext(); t >= 0 && b.ok(); t = b.ext()) {
                        if (t != 0x01) {
                            b.unknownExt(t);
                            break;
                        }
                        b.tag(76);
                        if (std::string u = b.string(true); !u.empty()) a.binName = u;
                    }
                    b.tag(0x03);
                    if (!b.ok()) issue(r, b.error()), a.binName.clear();
                } else if (a.objectClass == "TMCS" && a.name == "_TMP_CRM") {
                    // The component's locators: a list of time crumbs.
                    if (const std::string why = blocked(r, depth + 1); !why.empty()) {
                        issue(r, why);
                    } else {
                        Cursor m = open(r);
                        m.tag(0x02);
                        m.tag(0x01);
                        const size_t n = m.count(m.s16(), 4);
                        std::vector<uint32_t> refs;
                        for (size_t k = 0; k < n && m.ok(); ++k) refs.push_back(ref(m));
                        m.tag(0x03);
                        if (!m.ok()) {
                            issue(r, m.error());
                        } else {
                            active_.insert(r);
                            for (const uint32_t mr : refs)
                                if (mr)
                                    if (auto loc = marker(mr, depth + 2)) {
                                        list.nodes += 1 + countAvidAttributes(loc->attributes);
                                        list.locators.push_back(std::move(*loc));
                                    }
                            active_.erase(r);
                        }
                    }
                }
            }
        } else if (type == 4) {
            a.kind = AvidAttribute::Kind::Bytes;
            a.text = c.bytes(c.count(c.u32(), 1));
        } else {
            c.fail("an attribute of unknown type " + std::to_string(type));
        }
        if (c.ok()) {
            list.attributes.push_back(std::move(a));
            ++list.nodes;
        }
    }
    c.tag(0x03);
    active_.erase(index);
    if (!c.ok()) {
        issue(index, c.error());
        list = {};
    }
    return attributes_[index] = std::move(list);
}

std::optional<AvidLocator> BinReader::marker(uint32_t index, int depth) {
    if (chunk(index).classId != "TMBC") {
        issue(index, "a locator of another class");
        return std::nullopt;
    }
    if (const std::string why = blocked(index, depth); !why.empty()) {
        issue(index, why);
        return std::nullopt;
    }
    AvidLocator loc;
    Cursor c = open(index);
    // MobRef
    c.tag(0x02);
    c.tag(0x01);
    c.u32();
    c.u32();
    loc.position = c.s32();
    for (int t = c.ext(); t >= 0 && c.ok(); t = c.ext()) {
        if (t == 0x01) loc.mobId = c.mobId();
        else c.unknownExt(t);
    }
    // Marker
    c.tag(0x02);
    c.tag(0x03);
    loc.offset = c.s32();
    const uint32_t attrs = ref(c);
    if (c.s16() != 1 && c.ok()) c.fail("a locator colour of unknown version");
    for (uint16_t& v : loc.rgb) v = c.u16();
    for (int t = c.ext(); t >= 0 && c.ok(); t = c.ext()) {
        if (t == 0x01) {
            c.tag(66);
            c.boolean();
        } else {
            c.unknownExt(t);
        }
    }
    c.tag(0x03);
    if (!c.ok()) {
        issue(index, c.error());
        return std::nullopt;
    }
    active_.insert(index);
    if (const AttributeList& list = attributes(attrs, depth + 1); spend(attrs ? attrs : index, list.nodes)) loc.attributes = list.attributes;
    active_.erase(index);
    const auto text = [&](const char* name) {
        const AvidAttribute* a = findAvidAttribute(loc.attributes, name);
        return a && a->kind == AvidAttribute::Kind::String ? a->text : std::string();
    };
    loc.comment = text("_ATN_CRM_COM");
    loc.colorName = text("_ATN_CRM_COLOR");
    loc.user = text("_ATN_CRM_USER");
    return loc;
}

std::optional<AvidMediaLocator> BinReader::mediaLocator(uint32_t index) {
    if (index == 0) return std::nullopt;
    AvidMediaLocator loc;
    loc.classId = chunk(index).classId;
    Cursor c = open(index);
    if (loc.classId == "FILE" || loc.classId == "WINF") {
        c.tag(0x02);
        c.tag(0x02);
        loc.path = c.string();
        for (int t = c.ext(); t >= 0 && c.ok(); t = c.ext()) {
            c.tag(76);
            if (t == 0x01) loc.pathPosix = c.string();
            else if (t == 0x02) loc.pathUtf8 = c.string(true);
            else if (t == 0x03) loc.path2Utf8 = c.string(true);
            else c.unknownExt(t);
        }
        c.tag(0x03);
    } else if (loc.classId == "URLL") {
        c.tag(0x03);
    } else if (loc.classId == "MSML") {
        c.tag(0x02);
        c.tag(0x02);
        c.u32();
        c.u32();
        loc.lastKnownVolume = c.string();
        for (int t = c.ext(); t >= 0 && c.ok(); t = c.ext()) {
            if (t == 0x01) {
                c.tag(71);
                loc.domainType = c.s32();
            } else if (t == 0x02) {
                loc.mobId = c.mobId();
            } else if (t == 0x03) {
                c.tag(76);
                loc.lastKnownVolumeUtf8 = c.string(true);
            } else {
                c.unknownExt(t);
            }
        }
        c.tag(0x03);
    } else {
        issue(index, "a locator of a class this reader does not know");
        return loc;
    }
    if (!c.ok()) {
        issue(index, c.error());
        AvidMediaLocator bare;
        bare.classId = loc.classId;
        return bare;
    }
    return loc;
}

std::vector<AvidParameter> BinReader::parameters(uint32_t index, int depth) {
    std::vector<AvidParameter> out;
    if (index == 0) return out;
    if (chunk(index).classId != "PRLS") {
        issue(index, "a parameter list of another class");
        return out;
    }
    Cursor c = open(index);
    c.tag(0x02);
    c.tag(0x01);
    const size_t n = c.count(c.s32(), 4);
    std::vector<uint32_t> refs;
    for (size_t i = 0; i < n && c.ok(); ++i) refs.push_back(ref(c));
    c.tag(0x03);
    if (!c.ok()) {
        issue(index, c.error());
        return out;
    }
    for (const uint32_t r : refs) {
        if (!r) continue;
        if (chunk(r).classId != "PRIT") {
            issue(r, "a parameter of another class");
            continue;
        }
        AvidParameter p;
        Cursor q = open(r);
        q.tag(0x02);
        q.tag(0x02);
        p.uuid = q.rawUuid();
        p.valueType = q.s16();
        if (p.valueType == 1) p.intValue = q.s32();
        else if (p.valueType == 2) p.doubleValue = q.dbl();
        else if (p.valueType == 4) ref(q);
        else q.fail("a parameter value of unknown type");
        const std::string name = q.string();
        p.enabled = q.boolean();
        const uint32_t control = ref(q);
        for (int t = q.ext(); t >= 0 && q.ok(); t = q.ext()) {
            if (t == 0x01) {
                q.tag(66);
                q.boolean();
            } else {
                q.unknownExt(t);
            }
        }
        q.tag(0x03);
        if (!q.ok()) {
            issue(r, q.error());
            continue;
        }
        const auto known = parameterNames().find(p.uuid);
        p.name = known != parameterNames().end() ? known->second : !name.empty() ? name : p.uuid;
        if (control) {
            if (const auto curve = component(control, depth + 1); curve && curve->readable && curve->classId == "PRCL") {
                p.interpKind = curve->interpKind;
                p.points = curve->points;
            }
        }
        out.push_back(std::move(p));
    }
    return out;
}

void BinReader::readTrackGroup(Cursor& c, AvidComponent& out, int depth) {
    out.isTrackGroup = true;
    c.tag(0x02);
    c.tag(0x08);
    out.mcMode = c.u8();
    out.length = c.s32();
    out.numScalars = c.s32();
    const size_t n = c.count(c.s32(), 2);
    struct Refs {
        uint32_t attributes = 0, component = 0, fillerProxy = 0;
    };
    std::vector<Refs> refs;  // (grown as read: a count alone allocates nothing)
    for (size_t i = 0; i < n && c.ok(); ++i) {
        refs.emplace_back();
        AvidTrack& t = out.tracks.emplace_back();
        const uint16_t flags = c.u16();
        if (flags & 0xfc00) c.fail("a track with unknown flags");
        if (flags & 0x0001) t.index = c.s16();
        if (flags & 0x0002) refs[i].attributes = ref(c);
        if (flags & 0x0200) ref(c);  // session attributes
        if (flags & 0x0004) refs[i].component = ref(c);
        if (flags & 0x0008) refs[i].fillerProxy = ref(c);
        if (flags & 0x0010) ref(c);  // bob data
        if (flags & 0x0020) c.s16();  // control code
        if (flags & 0x0040) c.s16();  // control sub code
        if (flags & 0x0080) t.startPos = c.s32();
        if (flags & 0x0100) c.boolean();  // read only
    }
    for (int t = c.ext(); t >= 0 && c.ok(); t = c.ext()) {
        if (t != 0x01) {
            c.unknownExt(t);
            break;
        }
        for (size_t i = 0; i < n && c.ok(); ++i) {
            c.tag(69);
            out.tracks[i].lockNumber = c.s16();
        }
    }
    if (!c.ok()) return;
    for (size_t i = 0; i < n; ++i) {
        AvidTrack& t = out.tracks[i];
        if (const AttributeList& list = attributes(refs[i].attributes, depth + 1); spend(refs[i].attributes, list.nodes)) t.attributes = list.attributes;
        t.component = component(refs[i].component, depth + 1);
        t.fillerProxy = component(refs[i].fillerProxy, depth + 1);
    }
}

void BinReader::readComponent(Cursor& c, AvidComponent& out, int depth, AvidMob* mob) {
    const std::string& k = out.classId;
    if (k != "SEQU" && !kClips.count(k) && !kTrackGroups.count(k)) {
        c.fail("not a component class this reader knows");
        return;
    }
    // Component
    c.tag(0x02);
    c.tag(0x03);
    ref(c);  // left bob
    ref(c);  // right bob
    out.mediaKindId = c.s16();
    out.mediaKind = mediaKindName(out.mediaKindId);
    out.editRate = c.exp10();
    out.name = c.string();
    out.effectId = c.string();
    const uint32_t attrs = ref(c);
    ref(c);  // session attributes
    ref(c);  // precomputed
    uint32_t paramList = 0;
    for (int t = c.ext(); t >= 0 && c.ok(); t = c.ext()) {
        if (t == 0x01) {
            c.tag(72);
            paramList = ref(c);
        } else {
            c.unknownExt(t);
        }
    }

    if (k == "SEQU") {
        c.tag(0x02);
        c.tag(0x03);
        const size_t n = c.count(c.u32(), 4);
        std::vector<uint32_t> refs;
        for (size_t i = 0; i < n && c.ok(); ++i) refs.push_back(ref(c));
        c.tag(0x03);
        if (!c.ok()) return;
        for (const uint32_t r : refs) {
            auto item = component(r, depth + 1);
            if (!item) continue;
            // (saturating: a sequence naming the same long one many times stays in range)
            const int64_t add = item->classId == "TNFX" ? -item->length : item->length;
            out.length = std::clamp(out.length + std::clamp(add, -kMaxLength, kMaxLength), -kMaxLength, kMaxLength);
            out.components.push_back(std::move(item));
        }
    } else if (kClips.count(k)) {
        c.tag(0x02);
        c.tag(0x01);
        out.length = c.u32();
        if (k == "SCLP") {
            c.tag(0x02);
            c.tag(0x03);
            c.u32();
            c.u32();
            out.trackId = c.s16();
            out.startTime = c.s32();
            for (int t = c.ext(); t >= 0 && c.ok(); t = c.ext()) {
                if (t == 0x01) out.mobId = c.mobId(), out.hasMobId = true;
                else c.unknownExt(t);
            }
            c.tag(0x03);
        } else if (k == "FILL") {
            c.tag(0x02);
            c.tag(0x01);
            c.tag(0x03);
        } else if (k == "TCCP") {
            c.tag(0x02);
            c.tag(0x01);
            out.timecodeFlags = c.u32();
            out.fps = c.u16();
            c.skip(6);
            out.timecodeStart = c.u32();
            c.tag(0x03);
        } else if (k == "ECCP") {
            c.tag(0x02);
            c.tag(0x01);
            out.edgeHeader = c.bytes(8);
            out.filmKind = c.u8();
            out.codeFormat = c.u8();
            out.basePerf = c.u16();
            c.u32();
            out.edgeStart = c.s32();
            c.tag(0x03);
        } else if (k == "TRKR") {
            c.tag(0x02);
            c.tag(0x01);
            out.relativeScope = c.s16();
            out.relativeTrack = c.s16();
            c.tag(0x03);
        } else if (k == "CTRL") {
            c.tag(0x02);
            c.tag(0x03);
            out.interpKind = c.s32();
            const size_t n = c.count(c.s32(), 23);
            for (size_t i = 0; i < n && c.ok(); ++i) {
                AvidControlPoint p;
                p.offsetNum = c.s32();
                p.offsetDen = c.s32();
                p.timeScale = c.s32();
                if (!c.boolean() && c.ok()) c.fail("a control point without a value");
                p.valueNum = c.s32();
                p.valueDen = c.s32();
                p.value = p.valueDen ? double(p.valueNum) / p.valueDen : 0.0;
                const size_t pp = c.count(c.s16(), 10);
                for (size_t j = 0; j < pp && c.ok(); ++j) c.skip(10);
                out.points.push_back(p);
            }
            c.tag(0x03);
        } else if (k == "PRCL") {
            c.tag(0x02);
            c.tag(0x01);
            out.interpKind = c.s32();
            const int16_t valueType = c.s16();
            if (valueType != 1 && valueType != 2 && valueType != 4) c.fail("a curve of unknown value type");
            const size_t n = c.count(c.s32(), 18);
            for (size_t i = 0; i < n && c.ok(); ++i) {
                AvidControlPoint p;
                p.offsetNum = c.s32();
                p.offsetDen = c.s32();
                p.timeScale = c.s32();
                if (valueType == 1) p.value = c.s32();
                else if (valueType == 2) p.value = c.dbl();
                else ref(c);
                const size_t pp = c.count(c.s16(), 8);
                for (size_t j = 0; j < pp && c.ok(); ++j) {
                    c.s16();
                    const int16_t type = c.s16();
                    if (type == 2) c.dbl();
                    else if (type == 1) c.s32();
                    else c.fail("a curve point property of unknown type");
                }
                out.points.push_back(p);
            }
            for (int t = c.ext(); t >= 0 && c.ok(); t = c.ext()) {
                if (t == 0x01 || t == 0x02) {
                    c.tag(71);
                    c.s32();
                } else {
                    c.unknownExt(t);
                }
            }
            c.tag(0x03);
        }
    } else {
        readTrackGroup(c, out, depth);
        if (kTrackEffects.count(k)) {
            c.tag(0x02);
            c.tag(0x06);
            out.leftLength = c.s32();
            out.rightLength = c.s32();
            c.s16();      // info version
            c.s32();      // current key frame
            c.s32();      // smooth
            c.s16();      // colour item
            c.s16();      // quality
            c.s8();       // reversed
            c.boolean();  // aspect on
            ref(c);       // key frames
            c.boolean();  // force software
            c.boolean();  // never hardware
            for (int t = c.ext(); t >= 0 && c.ok(); t = c.ext()) {
                if (t == 0x02) {
                    c.tag(72);
                    ref(c);  // tracker
                } else {
                    c.unknownExt(t);
                }
            }
            if (k == "TKFX") {
                c.tag(0x03);
            } else if (k == "PVOL") {
                c.tag(0x02);
                c.tag(0x05);
                out.level = c.s32();
                out.pan = c.s32();
                c.boolean();  // suppress validation
                out.levelSet = c.boolean();
                out.panSet = c.boolean();
                for (int t = c.ext(); t >= 0 && c.ok(); t = c.ext()) {
                    if (t == 0x01 || t == 0x02) {
                        c.tag(71);
                        c.s32();
                    } else {
                        c.unknownExt(t);
                    }
                }
                c.tag(0x03);
            }
            // ASPI and EQMB: their plug-in and band settings are not read.
        } else if (kTimeWarps.count(k)) {
            c.tag(0x02);
            c.tag(0x02);
            out.phaseOffset = c.s32();
            if (k == "MASK") {
                c.tag(0x02);
                c.tag(0x01);
                out.isDouble = c.boolean();
                out.maskBits = c.u32();
            } else if (k == "STRB") {
                c.tag(0x02);
                c.tag(0x01);
                out.strobe = c.s32();
            } else if (k == "SPED") {
                c.tag(0x02);
                c.tag(0x03);
                out.speedNum = c.s32();
                out.speedDen = c.s32();
                for (int t = c.ext(); t >= 0 && c.ok(); t = c.ext()) {
                    if (t == 0x01) {
                        c.tag(75);
                        out.offsetAdjust = c.dbl();
                    } else if (t == 0x02) {
                        c.tag(72);
                        ref(c);  // source parameter list
                    } else if (t == 0x03) {
                        c.tag(66);
                        c.boolean();  // new source calculation
                    } else {
                        c.unknownExt(t);
                    }
                }
            } else {
                c.tag(0x02);
                c.tag(0x01);
            }
            c.tag(0x03);
        } else if (k == "RSET") {
            c.tag(0x02);
            c.tag(0x01);
            for (int t = c.ext(); t >= 0 && c.ok(); t = c.ext()) {
                if (t == 0x01) {
                    c.tag(71);
                    out.repSetType = c.s32();
                } else {
                    c.unknownExt(t);
                }
            }
            c.tag(0x03);
        } else if (k == "TNFX") {
            c.tag(0x02);
            c.tag(0x01);
            out.cutPoint = c.s32();
            c.tag(0x02);
            c.tag(0x05);
            out.leftLength = c.s32();
            out.rightLength = c.s32();
            c.s16();
            c.s32();
            c.s32();
            c.s16();
            c.s16();
            c.s8();
            c.boolean();
            ref(c);  // key frames (FXPS)
            c.boolean();
            c.boolean();
            for (int t = c.ext(); t >= 0 && c.ok(); t = c.ext()) {
                if (t == 0x01) {
                    c.tag(72);
                    ref(c);  // tracker
                } else {
                    c.unknownExt(t);
                }
            }
            c.tag(0x03);
        } else if (k == "SLCT") {
            c.tag(0x02);
            c.tag(0x01);
            out.isGanged = c.boolean();
            out.selected = c.u16();
            if (c.ok() && out.selected >= out.tracks.size()) c.fail("a selector choosing a track it does not have");
            c.tag(0x03);
        } else if (k == "CMPO" && mob) {
            c.tag(0x02);
            c.tag(0x02);
            c.u32();
            c.u32();
            mob->lastModified = c.u32();
            mob->mobTypeId = c.u8();
            mob->usageCode = c.s32();
            const uint32_t desc = ref(c);
            for (int t = c.ext(); t >= 0 && c.ok(); t = c.ext()) {
                if (t == 0x01) {
                    c.tag(71);
                    mob->creationTime = c.u32();
                } else if (t == 0x02) {
                    mob->mobId = c.mobId();
                    mob->urn = avidMobIdUrn(mob->mobId);
                } else {
                    c.unknownExt(t);
                }
            }
            c.tag(0x03);
            if (c.ok() && desc) mob->descriptor = descriptor(desc, depth + 1);
        }
    }
    if (!c.ok()) return;
    if (const AttributeList& list = attributes(attrs, depth + 1); spend(attrs, list.nodes)) {
        out.attributes = list.attributes;
        out.locators = list.locators;
    }
    out.parameters = parameters(paramList, depth + 1);
}

std::shared_ptr<const AvidComponent> BinReader::component(uint32_t index, int depth) {
    if (index == 0) return nullptr;
    if (const auto it = components_.find(index); it != components_.end()) return it->second;
    auto out = std::make_shared<AvidComponent>();
    out->classId = chunk(index).classId;
    if (const std::string why = blocked(index, depth); !why.empty()) {
        out->readable = false;
        out->error = why;
        issue(index, why);
        return out;
    }
    active_.insert(index);
    Cursor c = open(index);
    readComponent(c, *out, depth, nullptr);
    active_.erase(index);
    if (!c.ok()) {
        issue(index, c.error());
        auto bare = std::make_shared<AvidComponent>();
        bare->classId = out->classId;
        // (what was read before the failure: its kind, rate and length, so what follows it keeps its place)
        bare->mediaKindId = out->mediaKindId;
        bare->mediaKind = out->mediaKind;
        bare->editRate = out->editRate;
        bare->length = out->length;
        bare->readable = false;
        bare->error = c.error();
        out = bare;
    }
    components_[index] = out;
    return out;
}

void BinReader::readDescriptor(Cursor& c, AvidDescriptor& out, int depth) {
    const std::string& k = out.classId;
    if (!kDescriptors.count(k)) {
        c.fail("not a descriptor class this reader knows");
        return;
    }
    // MediaDescriptor
    c.tag(0x02);
    c.tag(0x03);
    out.mobKind = c.u8();
    const uint32_t locator = ref(c);
    out.intermediate = c.boolean();
    const uint32_t physical = ref(c);
    uint32_t attrs = 0;
    for (int t = c.ext(); t >= 0 && c.ok(); t = c.ext()) {
        if (t == 0x01) {
            c.tag(65);
            if (c.s32() != 16 && c.ok()) c.fail("a descriptor UUID that is not 16 bytes");
            out.uuid = c.rawUuid();
        } else if (t == 0x02) {
            c.tag(65);
            c.skip(c.count(c.s32(), 1));  // UTF-16 text
        } else if (t == 0x03) {
            c.tag(72);
            attrs = ref(c);
        } else {
            c.unknownExt(t);
        }
    }
    if (k == "MDES") {
        c.tag(0x03);
    } else if (k == "MDTP") {
        c.tag(0x02);
        c.tag(0x02);
        out.cframe = c.s16();
        c.tag(0x03);
    } else if (k == "MDFM" || k == "MDNG") {
        c.tag(0x02);
        c.tag(0x01);
        c.tag(0x03);
    } else if (kFileDescriptors.count(k)) {
        out.isFile = true;
        c.tag(0x02);
        c.tag(0x03);
        out.editRate = c.exp10();
        out.length = c.s32();
        c.s16();  // is OMFI
        c.s32();  // data offset
        if (k == "MDFL") {
            c.tag(0x03);
        } else if (k == "MULD") {
            c.tag(0x02);
            c.tag(0x01);
            const size_t n = c.count(c.s32(), 4);
            std::vector<uint32_t> refs;
            for (size_t i = 0; i < n && c.ok(); ++i) refs.push_back(ref(c));
            c.tag(0x03);
            if (c.ok())
                for (const uint32_t r : refs)
                    if (auto d = descriptor(r, depth + 1)) out.children.push_back(std::move(d));
        } else if (k == "PCMA" || k == "MPGA") {
            // The rest of the sound descriptor (reference level, peak envelope, ...) is not read.
            c.tag(0x02);
            c.tag(0x01);
            out.channels = c.u16();
            out.bitsPerSample = c.u16();
            out.sampleRate = c.exp10();
        } else if (kPictureDescriptors.count(k)) {
            // The digital image descriptor's first fields; its boxes, colour and the CDCI, RGBA, MPGI and JPED parts are
            // not read.
            c.tag(0x02);
            c.tag(0x02);
            out.storedHeight = c.s32();
            out.storedWidth = c.s32();
            c.skip(16);  // sampled size and offset
            out.displayHeight = c.s32();
            out.displayWidth = c.s32();
            c.skip(8);   // display offset
            c.s16();     // frame layout
            out.aspectNum = c.s32();
            out.aspectDen = c.s32();
            const int32_t lineMap = c.s32();
            if (lineMap > 0) c.skip(size_t(lineMap / 4) * 4);
            c.s32();      // alpha transparency
            c.boolean();  // uniformness
            c.s32();      // image size
            ref(c);       // next descriptor
            out.compression = c.fourcc();
            out.resolutionId = c.s32();
        }
        // WAVE, AIFC, DATD and ANCD: their summaries are not read.
    }
    if (!c.ok()) return;
    out.locator = mediaLocator(locator);
    out.physicalMedia = descriptor(physical, depth + 1);
    out.attributes = attributes(attrs, depth + 1).attributes;
}

std::shared_ptr<const AvidDescriptor> BinReader::descriptor(uint32_t index, int depth) {
    if (index == 0) return nullptr;
    if (const auto it = descriptors_.find(index); it != descriptors_.end()) return it->second;
    auto out = std::make_shared<AvidDescriptor>();
    out->classId = chunk(index).classId;
    if (const std::string why = blocked(index, depth); !why.empty()) {
        out->readable = false;
        out->error = why;
        issue(index, why);
        return out;
    }
    active_.insert(index);
    Cursor c = open(index);
    readDescriptor(c, *out, depth);
    active_.erase(index);
    if (!c.ok()) {
        issue(index, c.error());
        auto bare = std::make_shared<AvidDescriptor>();
        bare->classId = out->classId;
        bare->readable = false;
        bare->error = c.error();
        out = bare;
    }
    descriptors_[index] = out;
    return out;
}

std::optional<AvidMob> BinReader::mob(uint32_t index) {
    if (chunk(index).classId != "CMPO") {
        issue(index, "a bin item that is not a mob");
        return std::nullopt;
    }
    AvidMob m;
    AvidComponent c;
    c.classId = "CMPO";
    active_.insert(index);
    Cursor cur = open(index);
    readComponent(cur, c, 1, &m);
    active_.erase(index);
    if (!cur.ok()) {
        issue(index, cur.error());
        return std::nullopt;
    }
    m.name = c.name;
    m.length = c.length;
    m.editRate = c.editRate;
    m.attributes = std::move(c.attributes);
    m.locators = std::move(c.locators);
    m.tracks = std::move(c.tracks);
    m.mobType = m.mobTypeId == 1 ? "CompositionMob" : m.mobTypeId == 2 ? "MasterMob" : m.mobTypeId == 3 ? "SourceMob" : "";
    m.usage = usageName(m.usageCode);
    return m;
}

void BinReader::readView(uint32_t index) {
    if (index == 0) return;
    if (chunk(index).classId != "BVst") {
        issue(index, "a bin view of another class");
        return;
    }
    Cursor c = open(index);
    // Setting
    c.tag(0x02);
    c.tag(0x06);
    bin_.viewName = c.string();
    bin_.viewKind = c.string();
    c.s16();
    c.s16();
    ref(c);  // its attributes
    // BinViewSetting
    c.tag(0x02);
    c.tag(10);
    const size_t n = c.count(c.u16(), 7);
    for (size_t i = 0; i < n && c.ok(); ++i) {
        AvidBinColumn col;
        col.title = c.string();
        col.format = c.s16();
        col.type = c.s16();
        col.hidden = c.boolean();
        bin_.columns.push_back(col);
    }
    for (int t = c.ext(); t >= 0 && c.ok(); t = c.ext()) {
        if (t != 0x01) {
            c.unknownExt(t);
            break;
        }
        c.tag(69);
        const size_t m = c.count(c.s16(), 16);
        for (size_t i = 0; i < m && c.ok(); ++i) {
            c.tag(69);
            const int16_t id = c.s16();
            c.tag(71);
            const int32_t size = c.s32();
            c.tag(76);
            c.s32();
            const std::string text = c.bytes(c.count(size, 1));
            bin_.formatDescriptors.emplace_back(id, text);
        }
    }
    c.tag(0x03);
    if (!c.ok()) {
        issue(index, c.error());
        bin_.columns.clear();
        bin_.formatDescriptors.clear();
    }
}

bool BinReader::readBin(uint32_t root, std::string* error) {
    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const std::string cls = chunk(root).classId;
    if (cls != "ABIN" && cls != "BINF") return fail("the bin's root object is a " + cls + ", not a bin");
    Cursor c = open(root);
    c.tag(0x02);
    const uint8_t version = c.u8();
    if (c.ok() && version != 0x0e && version != 0x0f) c.fail("a bin of unknown version");
    bin_.largeBin = version == 0x0f;
    const uint32_t view = ref(c);
    bin_.uid = c.u64();
    const size_t n = c.count(bin_.largeBin ? int64_t(c.u32()) : int64_t(c.u16()), 13);
    std::vector<uint32_t> mobRefs;
    for (size_t i = 0; i < n && c.ok(); ++i) {
        AvidBinItem item;
        mobRefs.push_back(ref(c));
        item.x = c.s16();
        item.y = c.s16();
        item.keyframe = c.s32();
        item.userPlaced = c.boolean();
        bin_.items.push_back(item);
    }
    bin_.displayMask = c.s32();
    bin_.displayMode = c.s16();
    bin_.sifted = c.boolean();
    for (int i = 0; i < 6; ++i) {
        c.s16();
        c.string();
        c.string();
    }
    const size_t sorts = c.count(c.s16(), 3);
    for (size_t i = 0; i < sorts && c.ok(); ++i) {
        const uint8_t direction = c.u8();
        bin_.sortColumns.emplace_back(direction, c.string());
    }
    bin_.macFont = c.s16();
    bin_.macFontSize = c.s16();
    bin_.macImageScale = c.s16();
    if (c.s16() != 1 && c.ok()) c.fail("a rectangle of unknown version");
    for (int16_t& v : bin_.homeRect) v = c.s16();
    for (auto* colour : {&bin_.backgroundColor, &bin_.foregroundColor}) {
        if (c.s16() != 1 && c.ok()) c.fail("a colour of unknown version");
        for (uint16_t& v : *colour) v = c.u16();
    }
    bin_.qlImageScale = c.s16();
    const uint32_t attrs = ref(c);
    bin_.wasIconic = c.boolean();
    if (cls == "ABIN") {
        c.tag(0x03);
    } else {
        c.tag(0x02);
        c.tag(0x01);
        c.s32();
        c.tag(0x03);
    }
    if (!c.ok()) return fail("the bin object cannot be read: " + c.error());

    readView(view);
    bin_.attributes = attributes(attrs, 1).attributes;
    std::map<uint32_t, int> mobIndex;
    for (size_t i = 0; i < mobRefs.size(); ++i) {
        const uint32_t r = mobRefs[i];
        if (!r) continue;
        if (const auto it = mobIndex.find(r); it != mobIndex.end()) {
            bin_.items[i].mob = it->second;
            continue;
        }
        if (auto m = mob(r)) {
            mobIndex[r] = int(bin_.mobs.size());
            bin_.items[i].mob = int(bin_.mobs.size());
            bin_.mobs.push_back(std::move(*m));
        } else {
            mobIndex[r] = -1;
        }
    }
    return true;
}

}  // namespace

std::string avidMobIdUrn(const AvidMobId& id) {
    // As pyavb (and pyaaf2) write it: the material number's UUID fields first, unless the number is Media Composer's
    // half-swapped one (its last eight bytes a SMPTE label), which is written with those bytes first.
    const uint8_t* m = id.data() + 16;
    const unsigned d1 = unsigned(m[0] | m[1] << 8 | m[2] << 16 | uint32_t(m[3]) << 24);
    const unsigned d2 = unsigned(m[4] | m[5] << 8), d3 = unsigned(m[6] | m[7] << 8);
    const uint8_t* d4 = m + 8;
    char buf[96];
    int n = std::snprintf(buf, sizeof buf, "urn:smpte:umid:%02x%02x%02x%02x.%02x%02x%02x%02x.%02x%02x%02x%02x.%02x%02x%02x%02x.",
                          id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7], id[8], id[9], id[10], id[11], id[12], id[13],
                          id[14], id[15]);
    const bool halfSwapped = id[11] == 0x00 && d4[0] == 0x06 && d4[1] == 0x0e && d4[2] == 0x2b && d4[3] == 0x34 &&
                             d4[4] == 0x7f && d4[5] == 0x7f;
    if (halfSwapped)
        std::snprintf(buf + n, sizeof buf - size_t(n), "%02x%02x%02x%02x.%02x%02x%02x%02x.%08x.%04x%04x", d4[0], d4[1], d4[2],
                      d4[3], d4[4], d4[5], d4[6], d4[7], d1, d2, d3);
    else
        std::snprintf(buf + n, sizeof buf - size_t(n), "%08x.%04x%04x.%02x%02x%02x%02x.%02x%02x%02x%02x", d1, d2, d3, d4[0],
                      d4[1], d4[2], d4[3], d4[4], d4[5], d4[6], d4[7]);
    return buf;
}

const AvidAttribute* findAvidAttribute(const AvidAttributes& list, const std::string& name) {
    for (auto it = list.rbegin(); it != list.rend(); ++it)
        if (it->name == name) return &*it;
    return nullptr;
}

const AvidMob* AvidBin::findMob(const AvidMobId& id) const {
    for (const AvidMob& m : mobs)
        if (m.mobId == id) return &m;
    return nullptr;
}

bool readAvidBinData(const std::string& data, AvidBin& out, std::string* error) {
    out = AvidBin{};
    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (data.size() < 8) return fail("not a Media Composer bin");
    bool le;
    if (data[0] == 0x06 && data[1] == 0x00) le = true;
    else if (data[0] == 0x00 && data[1] == 0x06) le = false;
    else return fail("not a Media Composer bin");
    if (data.compare(2, 6, "Domain") != 0) return fail("not a Media Composer bin");
    out.bigEndian = !le;

    Cursor c(data, 8, data.size() - 8, le);
    if (c.fourcc() != "OBJD" || c.string() != "AObjDoc" || c.u8() != 0x04) return fail("not a Media Composer bin");
    out.lastSave = c.string();
    const uint32_t count = c.u32();
    const uint32_t root = c.u32();
    const uint32_t order = c.u32();
    if (c.ok() && order != 0x49494949 && order != 0x4d4d4d4d) return fail("not a Media Composer bin");
    c.u32();  // the last save, as seconds
    c.skip(4);
    if (c.fourcc() != "ATob" || c.fourcc() != "ATve") return fail("not a Media Composer bin");
    out.creatorVersion = c.string();
    c.skip(16);
    if (!c.ok()) return fail("the bin's header is cut short");

    // The object table: each object's class and size, then its bytes.
    if (count > c.left() / 8) return fail("the bin is cut short: its header names more objects than it holds");
    std::vector<BinReader::Chunk> chunks;
    chunks.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        BinReader::Chunk ch;
        ch.classId = c.fourcc();
        ch.size = c.u32();
        ch.pos = c.pos();
        c.skip(ch.size);
        if (!c.ok()) return fail("the bin is cut short at object " + std::to_string(i + 1) + " of " + std::to_string(count));
        chunks.push_back(std::move(ch));
    }
    if (root == 0 || root > count) return fail("the bin's root object is not in its object table");
    BinReader reader(data, le, std::move(chunks), out);
    if (!reader.readBin(root, error)) {
        out = AvidBin{};
        return false;
    }
    return true;
}

bool readAvidBin(const std::string& path, AvidBin& out, std::string* error) {
    QFile f(QString::fromStdString(path));  // (UTF-8 paths on Windows too)
    if (!f.open(QIODevice::ReadOnly)) {
        out = AvidBin{};
        if (error) *error = "cannot open " + path;
        return false;
    }
    const QByteArray data = f.readAll();
    return readAvidBinData(data.toStdString(), out, error);
}

}  // namespace montage
