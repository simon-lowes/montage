#include "DcpMxf.h"
#include "DcpSecurity.h"
#include "Jpeg2000.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <map>
#include <random>

#ifdef _WIN32
#include <QString>
#endif

namespace montage::dcp {

namespace {

using Bytes = std::vector<uint8_t>;

void put8(Bytes& b, uint64_t v) { b.push_back(uint8_t(v)); }
void put16(Bytes& b, uint64_t v) {
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}
void put32(Bytes& b, uint64_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
}
void put64(Bytes& b, uint64_t v) {
    for (int s = 56; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
}
void putBytes(Bytes& b, const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
void putBytes(Bytes& b, const Bytes& v) { b.insert(b.end(), v.begin(), v.end()); }

std::array<uint8_t, 16> ul(const char* hex) {
    std::array<uint8_t, 16> u{};
    for (int i = 0; i < 16; ++i) {
        auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        u[size_t(i)] = uint8_t(nib(hex[2 * i]) << 4 | nib(hex[2 * i + 1]));
    }
    return u;
}
void putUl(Bytes& b, const char* hex) {
    const auto u = ul(hex);
    putBytes(b, u.data(), 16);
}
void putUuid(Bytes& b, const Uuid& u) { putBytes(b, u.data(), 16); }

// UTF-16 (big-endian, no terminator), as MXF strings are.
void putUtf16(Bytes& b, const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        uint32_t c = uint8_t(s[i]);
        int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (extra) c &= 0x3fu >> extra;
        ++i;
        for (int k = 0; k < extra && i < s.size(); ++k, ++i) c = c << 6 | (uint8_t(s[i]) & 0x3f);
        if (c >= 0x10000) {
            c -= 0x10000;
            put16(b, 0xd800 + (c >> 10));
            put16(b, 0xdc00 + (c & 0x3ff));
        } else {
            put16(b, c);
        }
    }
}
Bytes utf16(const std::string& s) {
    Bytes b;
    putUtf16(b, s);
    return b;
}

void putBer4(Bytes& b, size_t n) {
    assert(n < (size_t(1) << 24));  // larger values need the 8-byte form (putBer8)
    b.push_back(0x83);
    b.push_back(uint8_t(n >> 16));
    b.push_back(uint8_t(n >> 8));
    b.push_back(uint8_t(n));
}

void putBer8(Bytes& b, uint64_t n) {
    b.push_back(0x87);
    for (int i = 6; i >= 0; --i) b.push_back(uint8_t(n >> (8 * i)));
}

Bytes klv(const std::array<uint8_t, 16>& key, const Bytes& value) {
    Bytes b;
    putBytes(b, key.data(), 16);
    putBer4(b, value.size());
    putBytes(b, value);
    return b;
}

// A local set: two-byte tags and lengths.
struct Set {
    Bytes body;
    void item(uint16_t tag, const Bytes& v) {
        put16(body, tag);
        put16(body, v.size());
        putBytes(body, v);
    }
    void u8(uint16_t tag, uint64_t v) { item(tag, {uint8_t(v)}); }
    void u16(uint16_t tag, uint64_t v) {
        Bytes b;
        put16(b, v);
        item(tag, b);
    }
    void u32(uint16_t tag, uint64_t v) {
        Bytes b;
        put32(b, v);
        item(tag, b);
    }
    void i64(uint16_t tag, int64_t v) {
        Bytes b;
        put64(b, uint64_t(v));
        item(tag, b);
    }
    void rational(uint16_t tag, uint32_t num, uint32_t den) {
        Bytes b;
        put32(b, num);
        put32(b, den);
        item(tag, b);
    }
    void uuid(uint16_t tag, const Uuid& u) { item(tag, Bytes(u.begin(), u.end())); }
    void label(uint16_t tag, const char* hex) {
        Bytes b;
        putUl(b, hex);
        item(tag, b);
    }
    void refs(uint16_t tag, const std::vector<Uuid>& ids) {
        Bytes b;
        put32(b, ids.size());
        put32(b, 16);
        for (const Uuid& u : ids) putUuid(b, u);
        item(tag, b);
    }
    void text(uint16_t tag, const std::string& s) { item(tag, utf16(s)); }
};

// Set keys: 06.0e.2b.34.02.53.01.01.0d.01.01.01.01.01.<kind>.00 (byte 14)
Bytes setKlv(uint8_t kind, const Set& s) {
    auto key = ul("060e2b34025301010d01010101010000");
    key[14] = kind;
    return klv(key, s.body);
}

Bytes setKlv(const char* key, const Set& s) { return klv(ul(key), s.body); }

// A UMID (SMPTE 330) whose material number is a UUID: how asdcplib names packages, the file package by the asset id.
Bytes umid(const Uuid& material) {
    Bytes b;
    putUl(b, "060a2b340101010501010f2013000000");
    putUuid(b, material);
    return b;
}

struct PrimerEntry {
    uint16_t tag;
    const char* ul;
};
// The local tags these files use and the labels they stand for (static tags from SMPTE ST 377-1, dynamic ones
// numbered down from 0xffff as asdcplib numbers them).
const PrimerEntry kCommonPrimer[] = {
    {0x0201, "060e2b34010101020407010000000000"}, {0x0202, "060e2b34010101020702020101030000"},
    {0x1001, "060e2b34010101020601010406090000"}, {0x1101, "060e2b34010101020601010301000000"},
    {0x1102, "060e2b34010101020601010302000000"}, {0x1201, "060e2b34010101020702010301040000"},
    {0x1501, "060e2b34010101020702010301050000"}, {0x1502, "060e2b34010101020404010102060000"},
    {0x1503, "060e2b34010101010404010105000000"}, {0x1901, "060e2b34010101020601010405010000"},
    {0x1902, "060e2b34010101020601010405020000"}, {0x2701, "060e2b34010101020601010601000000"},
    {0x3001, "060e2b34010101010406010100000000"}, {0x3002, "060e2b34010101010406010200000000"},
    {0x3004, "060e2b34010101020601010401020000"}, {0x3006, "060e2b34010101050601010305000000"},
    {0x3b02, "060e2b34010101020702011002040000"}, {0x3b03, "060e2b34010101020601010402010000"},
    {0x3b05, "060e2b34010101020301020105000000"}, {0x3b06, "060e2b34010101020601010406040000"},
    {0x3b07, "060e2b34010101020301020104000000"}, {0x3b08, "060e2b34010101040601010401080000"},
    {0x3b09, "060e2b34010101050102020300000000"}, {0x3b0a, "060e2b34010101050102021002010000"},
    {0x3b0b, "060e2b34010101050102021002020000"}, {0x3c01, "060e2b34010101020520070102010000"},
    {0x3c02, "060e2b34010101020520070103010000"}, {0x3c03, "060e2b34010101020520070104000000"},
    {0x3c04, "060e2b34010101020520070105010000"}, {0x3c05, "060e2b34010101020520070107000000"},
    {0x3c06, "060e2b34010101020702011002030000"}, {0x3c07, "060e2b3401010102052007010a000000"},
    {0x3c08, "060e2b34010101020520070106010000"}, {0x3c09, "060e2b34010101020520070101000000"},
    {0x3c0a, "060e2b34010101010101150200000000"}, {0x3f05, "060e2b34010101040406020100000000"},
    {0x3f06, "060e2b34010101040103040500000000"}, {0x3f07, "060e2b34010101040103040400000000"},
    {0x3f08, "060e2b34010101040404040101000000"}, {0x3f0a, "060e2b34010101050404040205000000"},
    {0x3f0b, "060e2b34010101050530040600000000"}, {0x3f0c, "060e2b340101010507020103010a0000"},
    {0x3f0d, "060e2b34010101050702020101020000"}, {0x3f0e, "060e2b34010101050404040107000000"},
    {0x4401, "060e2b34010101010101151000000000"}, {0x4402, "060e2b34010101010103030201000000"},
    {0x4403, "060e2b34010101020601010406050000"}, {0x4404, "060e2b34010101020702011002050000"},
    {0x4405, "060e2b34010101020702011001030000"}, {0x4701, "060e2b34010101020601010402030000"},
    {0x4801, "060e2b34010101020107010100000000"}, {0x4802, "060e2b34010101020107010201000000"},
    {0x4803, "060e2b34010101020601010402040000"}, {0x4804, "060e2b34010101020104010300000000"},
    {0x4b01, "060e2b34010101020530040500000000"}, {0x4b02, "060e2b34010101020702010301030000"},
    {0xffff, "060e2b34010101090601010406100000"},
};
const PrimerEntry kPicturePrimer[] = {
    {0x3201, "060e2b34010101020401060100000000"}, {0x3202, "060e2b34010101010401050201000000"},
    {0x3203, "060e2b34010101010401050202000000"}, {0x320c, "060e2b34010101010401030104000000"},
    {0x320e, "060e2b34010101010401010101000000"}, {0x3401, "060e2b34010101020401050306000000"},
    {0x3406, "060e2b3401010105040105030b000000"}, {0x3407, "060e2b3401010105040105030c000000"},
    {0x3f09, "060e2b34010101050404040106000000"}, {0xfff2, "060e2b340101010a040106030d000000"},
    {0xfff3, "060e2b340101010a040106030c000000"}, {0xfff4, "060e2b340101010a040106030b000000"},
    {0xfff5, "060e2b340101010a040106030a000000"}, {0xfff6, "060e2b340101010a0401060309000000"},
    {0xfff7, "060e2b340101010a0401060308000000"}, {0xfff8, "060e2b340101010a0401060307000000"},
    {0xfff9, "060e2b340101010a0401060306000000"}, {0xfffa, "060e2b340101010a0401060305000000"},
    {0xfffb, "060e2b340101010a0401060304000000"}, {0xfffc, "060e2b340101010a0401060303000000"},
    {0xfffd, "060e2b340101010a0401060302000000"}, {0xfffe, "060e2b340101010a0401060301000000"},
};
const PrimerEntry kSoundPrimer[] = {
    {0x3d01, "060e2b34010101040402030304000000"}, {0x3d02, "060e2b34010101040402030104000000"},
    {0x3d03, "060e2b34010101050402030101010000"}, {0x3d07, "060e2b34010101050402010104000000"},
    {0x3d09, "060e2b34010101050402030305000000"}, {0x3d0a, "060e2b34010101050402030201000000"},
    {0x3d32, "060e2b34010101070402010105000000"}, {0xfff8, "060e2b340101010e0103070106000000"},
    {0xfff9, "060e2b340101010e0103040a00000000"}, {0xfffa, "060e2b340101010d0301010203150000"},
    {0xfffb, "060e2b340101010e0103070103000000"}, {0xfffc, "060e2b340101010e0103070102000000"},
    {0xfffd, "060e2b340101010e0103070105000000"}, {0xfffe, "060e2b340101010e0103070101000000"},
};

// The descriptive metadata of an encrypted file: its cryptographic framework and context (ST 429-6), and the
// timed text descriptor's items (ST 429-5).
const PrimerEntry kCryptoPrimer[] = {
    {0x0602, "060e2b34010101020530040401000000"}, {0x6101, "060e2b340101010506010104020c0000"},
    {0xff86, "060e2b340101010906010104020d0000"}, {0xff85, "060e2b34010101090101151100000000"},
    {0xff84, "060e2b34010101090601010202000000"}, {0xff83, "060e2b34010101090209030101000000"},
    {0xff82, "060e2b34010101090209030201000000"}, {0xff81, "060e2b34010101090209030102000000"},
};
const PrimerEntry kTimedTextPrimer[] = {
    {0x3e01, "060e2b34010101030403030200000000"}, {0xfffe, "060e2b340101010c0101151200000000"},
    {0xfffd, "060e2b340101010c0409050000000000"}, {0xfffc, "060e2b34010101080102010501000000"},
    {0xfffb, "060e2b340101010e0301010202160000"}, {0xfffa, "060e2b340101010c0101151300000000"},
    {0xfff9, "060e2b34010101070409020100000000"},
};

Bytes primerOf(const std::vector<PrimerEntry>& entries) {
    Bytes v;
    put32(v, entries.size());
    put32(v, 18);
    for (const PrimerEntry& e : entries) {
        put16(v, e.tag);
        putUl(v, e.ul);
    }
    return klv(ul("060e2b34020501010d01020101050100"), v);
}

template <size_t N, size_t M>
Bytes primer(const PrimerEntry (&common)[N], const PrimerEntry (&extra)[M], bool encrypted = false) {
    std::vector<PrimerEntry> entries(std::begin(common), std::end(common));
    entries.insert(entries.end(), std::begin(extra), std::end(extra));
    if (encrypted) entries.insert(entries.end(), std::begin(kCryptoPrimer), std::end(kCryptoPrimer));
    return primerOf(entries);
}

// IMF (AS-02): the colour items, J2CLayout and mastering display the picture descriptors add, and the ST 2067-2
// multichannel label items (numbered as asdcplib numbers them in its IMF files).
const PrimerEntry kImfPicturePrimer[] = {
    {0x320d, "060e2b34010101020401030205000000"}, {0x3210, "060e2b34010101020401020101010200"},
    {0x3219, "060e2b34010101090401020101060100"}, {0x321a, "060e2b34010101020401020101030100"},
    {0x3405, "060e2b34010101050401040401000000"}, {0xfff1, "060e2b340101010e040106030e000000"},
    {0xfff0, "060e2b340101010e0420040101010000"}, {0xffef, "060e2b340101010e0420040101020000"},
    {0xffee, "060e2b340101010e0420040101030000"}, {0xffed, "060e2b340101010e0420040101040000"},
    {0xffec, "060e2b340101010e040106030f000000"},  // J2KExtendedCapabilities (HTJ2K)
};
const PrimerEntry kImfSoundPrimer[] = {
    {0x3d01, "060e2b34010101040402030304000000"}, {0x3d02, "060e2b34010101040402030104000000"},
    {0x3d03, "060e2b34010101050402030101010000"}, {0x3d07, "060e2b34010101050402010104000000"},
    {0x3d09, "060e2b34010101050402030305000000"}, {0x3d0a, "060e2b34010101050402030201000000"},
    {0x3d32, "060e2b34010101070402010105000000"}, {0xfff4, "060e2b340101010e0103070106000000"},
    {0xfff5, "060e2b340101010e0103040a00000000"}, {0xfff6, "060e2b340101010e0302010221000000"},
    {0xfff7, "060e2b340101010e0302010220000000"}, {0xfff8, "060e2b340101010e0105110000000000"},
    {0xfff9, "060e2b340101010e0105100000000000"}, {0xfffa, "060e2b340101010d0301010203150000"},
    {0xfffb, "060e2b340101010e0103070103000000"}, {0xfffc, "060e2b340101010e0103070102000000"},
    {0xfffd, "060e2b340101010e0103070105000000"}, {0xfffe, "060e2b340101010e0103070101000000"},
};

// IMF Application #5 (ST 2065-5): the picture items an ACES descriptor uses (no JPEG 2000 sub-descriptor), its display
// size and offsets, and the ACES picture and target frame sub-descriptors' items (numbered as asdcplib numbers them).
const PrimerEntry kAcesPrimer[] = {
    {0x3201, "060e2b34010101020401060100000000"}, {0x3202, "060e2b34010101010401050201000000"},
    {0x3203, "060e2b34010101010401050202000000"}, {0x3208, "060e2b3401010101040105010b000000"},
    {0x3209, "060e2b3401010101040105010c000000"}, {0x320a, "060e2b3401010101040105010d000000"},
    {0x320b, "060e2b3401010101040105010e000000"}, {0x320c, "060e2b34010101010401030104000000"},
    {0x320d, "060e2b34010101020401030205000000"}, {0x320e, "060e2b34010101010401010101000000"},
    {0x3210, "060e2b34010101020401020101010200"}, {0x3219, "060e2b34010101090401020101060100"},
    {0x3401, "060e2b34010101020401050306000000"}, {0xfffe, "060e2b340101010e0401060a01000000"},
    {0xfffd, "060e2b340101010e0401060a02000000"}, {0xfffc, "060e2b340101010e0401060a03000000"},
    {0xfffb, "060e2b340101010e0401060a04000000"}, {0xfffa, "060e2b340101010e0401060a05000000"},
    {0xfff9, "060e2b340101010e0401060901000000"}, {0xfff8, "060e2b340101010e0401060902000000"},
    {0xfff7, "060e2b340101010e0401060903000000"}, {0xfff6, "060e2b340101010e0401060904000000"},
    {0xfff5, "060e2b340101010e0401060905000000"}, {0xfff4, "060e2b340101010e0401060906000000"},
    {0xfff3, "060e2b340101010e0401060907000000"}, {0xfff2, "060e2b340101010e0401060908000000"},
    {0xfff1, "060e2b340101010e0401060909000000"},
};

constexpr const char* kOpAtom = "060e2b34040101020d01020110000000";
constexpr const char* kOp1a = "060e2b34040101010d01020101010100";
constexpr const char* kImfJ2kContainer = "060e2b340401010d0d010301020c0600";  // JPEG 2000, progressive frames (P1)
constexpr const char* kImfWaveContainer = "060e2b34040101010d01030102060200";  // Broadcast Wave, clip wrapped
constexpr const char* kImfSoundElement = "060e2b34010201010d01030116010201";   // a wave clip
constexpr const char* kGenericContainer = "060e2b34040101030d010301027f0100";  // MXF-GC, multiple mappings
constexpr const char* kJ2kContainer = "060e2b34040101070d010301020c0100";      // JPEG 2000, frame wrapped
constexpr const char* kWaveContainer = "060e2b34040101010d01030102060100";     // Broadcast Wave, frame wrapped
constexpr const char* kTimecodeDef = "060e2b34040101010103020101000000";
constexpr const char* kPictureDef = "060e2b34040101010103020201000000";
constexpr const char* kSoundDef = "060e2b34040101010103020202000000";
constexpr const char* kPictureElement = "060e2b34010201010d01030115010801";
constexpr const char* kSoundElement = "060e2b34010201010d01030116010101";
constexpr const char* kDataDef = "060e2b34040101010103020203000000";
constexpr const char* kTimedTextContainer = "060e2b340401010a0d01030102130101";  // ST 429-5, clip wrapped
constexpr const char* kTimedTextElement = "060e2b34010201010d01030117010b01";
constexpr const char* kAcesElement = "060e2b34010201010d01030115011201";  // an ACES frame (track number 15011201)
constexpr const char* kIabElement = "060e2b34010201050e09060100000001";        // ST 429-18 (not a registered UL)
constexpr const char* kIabDescriptor = "060e2b34025301050e09060300000000";
constexpr const char* kIabSubDescriptor = "060e2b34025301050e09060600000000";
constexpr const char* kStreamElement = "060e2b340101010c0d01050901000000";       // a generic stream's data
constexpr const char* kStreamPartition = "060e2b34020501010d01020101031100";
constexpr const char* kEncryptedContainer = "060e2b34040101070d010301020b0100";
constexpr const char* kEncryptedElement = "060e2b34020401010d010301027e0100";     // an encrypted triplet
constexpr const char* kCryptoScheme = "060e2b34040101070d01040102010100";         // the DM scheme of the context
constexpr const char* kDescriptiveDef = "060e2b34040101010103020110000000";
constexpr const char* kCryptoFramework = "060e2b34025301010d01040102010000";
constexpr const char* kCryptoContext = "060e2b34025301010d01040102020000";
constexpr const char* kAes = "060e2b34040101070209020101000000";
constexpr const char* kHmacSha1 = "060e2b34040101070209020201000000";
const uint8_t kCheckValue[16] = {'C', 'H', 'U', 'K', 'C', 'H', 'U', 'K', 'C', 'H', 'U', 'K', 'C', 'H', 'U', 'K'};
constexpr uint32_t kIndexSid = 129, kBodySid = 1;
constexpr uint64_t kHeaderSize = 16384;  // the header partition is padded to this, so it can be rewritten in place
constexpr size_t kEntriesPerSegment = 5000;  // index entries per index table segment (a local item holds 64 KiB)
const Uuid kProductUid = {0x5c, 0x1f, 0x7a, 0x3e, 0x92, 0x0d, 0x4b, 0x6a, 0xb1, 0x47, 0x2e, 0x8c, 0x60, 0xd5, 0x13, 0xa9};

std::string nowStamp() {
    const std::time_t t = std::time(nullptr);
    std::tm u{};
#ifdef _WIN32
    gmtime_s(&u, &t);
#else
    gmtime_r(&t, &u);
#endif
    Bytes b;
    put16(b, u.tm_year + 1900);
    put8(b, u.tm_mon + 1);
    put8(b, u.tm_mday);
    put8(b, u.tm_hour);
    put8(b, u.tm_min);
    put8(b, u.tm_sec);
    put8(b, 0);
    return std::string(b.begin(), b.end());
}
Bytes stamp(const std::string& s) { return Bytes(s.begin(), s.end()); }

Bytes productVersion() {
    Bytes b;
    for (int v : {0, 2, 0, 0, 1}) put16(b, v);  // major, minor, patch, build, release (1: released)
    return b;
}

// Instance identities, by role.
enum Id : size_t {
    kPreface, kIdentification, kGeneration, kStorage, kContainerData, kMaterial, kMpTcTrack, kMpTcSeq, kMpTc, kMpTrack,
    kMpSeq, kMpClip, kSource, kFpTcTrack, kFpTcSeq, kFpTc, kFpTrack, kFpSeq, kFpClip, kDescriptor, kSub, kMaterialUmid,
    kSoundfieldLink, kChannelSet0, kChannelLink0 = kChannelSet0 + 16, kStaticTrack = kChannelLink0 + 16, kDmSeq, kDmSegment,
    kFramework, kContext, kContextId, kStereoSub
};

// What an encrypted file's header says about it.
struct Crypto {
    bool on = false;
    Uuid context{}, key{};
    const char* wrapping = nullptr;  // the plain essence container
};

// The metadata both kinds share: everything but the essence descriptor and its sub-descriptors.
Bytes packages(const std::array<Uuid, 64>& ids, const Uuid& asset, int fps, int64_t duration, const std::string& created,
               const char* container, const char* dataDef, const std::string& trackName, uint32_t trackNumber,
               const std::string& packageName, const Crypto& crypto = {}) {
    Bytes out;
    {
        Set s;
        s.uuid(0x3c0a, ids[kPreface]);
        s.item(0x3b02, stamp(created));
        s.u16(0x3b05, 0x0102);
        s.u32(0x3b07, 1);
        s.uuid(0x3b08, ids[kSource]);
        s.refs(0x3b06, {ids[kIdentification]});
        s.uuid(0x3b03, ids[kStorage]);
        s.label(0x3b09, kOpAtom);
        Bytes ecs;
        put32(ecs, 2);
        put32(ecs, 16);
        putUl(ecs, kGenericContainer);
        putUl(ecs, crypto.on ? kEncryptedContainer : container);
        s.item(0x3b0a, ecs);
        Bytes schemes;
        put32(schemes, crypto.on ? 1 : 0);
        put32(schemes, 16);
        if (crypto.on) putUl(schemes, kCryptoScheme);
        s.item(0x3b0b, schemes);
        putBytes(out, setKlv(0x2f, s));
    }
    {
        Set s;
        s.uuid(0x3c0a, ids[kIdentification]);
        s.uuid(0x3c09, ids[kGeneration]);
        s.text(0x3c01, "Montage");
        s.text(0x3c02, "Montage");
        s.item(0x3c03, productVersion());
        s.text(0x3c04, "0.2.0");
        s.uuid(0x3c05, kProductUid);
        s.item(0x3c06, stamp(created));
        s.item(0x3c07, productVersion());
#ifdef _WIN32
        s.text(0x3c08, "win32");
#else
        s.text(0x3c08, "unix");
#endif
        putBytes(out, setKlv(0x30, s));
    }
    {
        Set s;
        s.uuid(0x3c0a, ids[kStorage]);
        s.refs(0x1901, {ids[kSource], ids[kMaterial]});
        s.refs(0x1902, {ids[kContainerData]});
        putBytes(out, setKlv(0x18, s));
    }
    {
        Set s;
        s.uuid(0x3c0a, ids[kContainerData]);
        s.item(0x2701, umid(asset));
        s.u32(0x3f06, kIndexSid);
        s.u32(0x3f07, kBodySid);
        putBytes(out, setKlv(0x23, s));
    }
    auto track = [&](Id self, Id seq, uint32_t trackId, uint32_t number, const std::string& name) {
        Set s;
        s.uuid(0x3c0a, ids[self]);
        s.u32(0x4801, trackId);
        s.u32(0x4804, number);
        s.text(0x4802, name);
        s.uuid(0x4803, ids[seq]);
        s.rational(0x4b01, uint32_t(fps), 1);
        s.i64(0x4b02, 0);
        putBytes(out, setKlv(0x3b, s));
    };
    auto sequence = [&](Id self, Id component, const char* def) {
        Set s;
        s.uuid(0x3c0a, ids[self]);
        s.label(0x0201, def);
        s.i64(0x0202, duration);
        s.refs(0x1001, {ids[component]});
        putBytes(out, setKlv(0x0f, s));
    };
    auto timecode = [&](Id self) {
        Set s;
        s.uuid(0x3c0a, ids[self]);
        s.label(0x0201, kTimecodeDef);
        s.i64(0x0202, duration);
        s.u16(0x1502, uint32_t(fps));
        s.i64(0x1501, 0);
        s.u8(0x1503, 0);
        putBytes(out, setKlv(0x14, s));
    };
    auto clip = [&](Id self, const Bytes& sourcePackage, uint32_t sourceTrack) {
        Set s;
        s.uuid(0x3c0a, ids[self]);
        s.label(0x0201, dataDef);
        s.i64(0x0202, duration);
        s.i64(0x1201, 0);
        s.item(0x1101, sourcePackage);
        s.u32(0x1102, sourceTrack);
        putBytes(out, setKlv(0x11, s));
    };
    {
        Set s;
        s.uuid(0x3c0a, ids[kMaterial]);
        s.item(0x4401, umid(ids[kMaterialUmid]));
        s.text(0x4402, "Material Package");
        s.item(0x4405, stamp(created));
        s.item(0x4404, stamp(created));
        s.refs(0x4403, {ids[kMpTcTrack], ids[kMpTrack]});
        putBytes(out, setKlv(0x36, s));
    }
    track(kMpTcTrack, kMpTcSeq, 1, 0, "Timecode Track");
    sequence(kMpTcSeq, kMpTc, kTimecodeDef);
    timecode(kMpTc);
    track(kMpTrack, kMpSeq, 2, 0, trackName);
    sequence(kMpSeq, kMpClip, dataDef);
    clip(kMpClip, umid(asset), 2);
    {
        Set s;
        s.uuid(0x3c0a, ids[kSource]);
        s.item(0x4401, umid(asset));
        s.text(0x4402, packageName);
        s.item(0x4405, stamp(created));
        s.item(0x4404, stamp(created));
        std::vector<Uuid> tracks = {ids[kFpTcTrack], ids[kFpTrack]};
        if (crypto.on) tracks.push_back(ids[kStaticTrack]);
        s.refs(0x4403, tracks);
        s.uuid(0x4701, ids[kDescriptor]);
        putBytes(out, setKlv(0x37, s));
    }
    track(kFpTcTrack, kFpTcSeq, 1, 0, "Timecode Track");
    sequence(kFpTcSeq, kFpTc, kTimecodeDef);
    timecode(kFpTc);
    track(kFpTrack, kFpSeq, 2, trackNumber, trackName);
    sequence(kFpSeq, kFpClip, dataDef);
    clip(kFpClip, Bytes(32, 0), 0);  // the end of the chain
    if (crypto.on) {
        // A static descriptive track whose one segment holds the cryptographic framework and its context.
        {
            Set s;
            s.uuid(0x3c0a, ids[kStaticTrack]);
            s.u32(0x4801, 3);
            s.u32(0x4804, 0);
            s.text(0x4802, "Descriptive Track");
            s.uuid(0x4803, ids[kDmSeq]);
            putBytes(out, setKlv(0x3a, s));
        }
        {
            Set s;
            s.uuid(0x3c0a, ids[kDmSeq]);
            s.label(0x0201, kDescriptiveDef);
            s.refs(0x1001, {ids[kDmSegment]});
            putBytes(out, setKlv(0x0f, s));
        }
        {
            Set s;
            s.uuid(0x3c0a, ids[kDmSegment]);
            s.label(0x0201, kDescriptiveDef);
            s.text(0x0602, "AS-DCP KLV Encryption");
            s.uuid(0x6101, ids[kFramework]);
            putBytes(out, setKlv(0x41, s));
        }
        {
            Set s;
            s.uuid(0x3c0a, ids[kFramework]);
            s.uuid(0xff86, ids[kContext]);
            putBytes(out, setKlv(kCryptoFramework, s));
        }
        {
            Set s;
            s.uuid(0x3c0a, ids[kContext]);
            s.uuid(0xff85, crypto.context);
            s.label(0xff84, crypto.wrapping);
            s.label(0xff83, kAes);
            s.label(0xff82, kHmacSha1);
            s.uuid(0xff81, crypto.key);
            putBytes(out, setKlv(kCryptoContext, s));
        }
    }
    return out;
}

void indexBase(Set& s, EditRate rate, int64_t start, int64_t duration, uint32_t editUnitBytes);

// The same for an IMF (AS-02, OP1a) file: one track in each package, no timecode, a rational edit rate.
Bytes packagesImf(const std::array<Uuid, 64>& ids, const Uuid& asset, EditRate rate, int64_t duration, const std::string& created,
                  const char* container, const char* dataDef, const std::string& trackName, uint32_t trackNumber,
                  const std::string& packageName, const char* conformsTo = nullptr, uint16_t conformsTag = 0) {
    Bytes out;
    {
        Set s;
        s.uuid(0x3c0a, ids[kPreface]);
        s.item(0x3b02, stamp(created));
        s.u16(0x3b05, 0x0103);
        if (conformsTo) {  // ConformsToSpecifications (a dynamic tag)
            Bytes specs;
            put32(specs, 1);
            put32(specs, 16);
            putUl(specs, conformsTo);
            s.item(conformsTag, specs);
        }
        s.u32(0x3b07, 1);
        s.uuid(0x3b08, ids[kSource]);
        s.refs(0x3b06, {ids[kIdentification]});
        s.uuid(0x3b03, ids[kStorage]);
        s.label(0x3b09, kOp1a);
        Bytes ecs;
        put32(ecs, 2);
        put32(ecs, 16);
        putUl(ecs, kGenericContainer);
        putUl(ecs, container);
        s.item(0x3b0a, ecs);
        Bytes none;
        put32(none, 0);
        put32(none, 16);
        s.item(0x3b0b, none);
        putBytes(out, setKlv(0x2f, s));
    }
    {
        Set s;
        s.uuid(0x3c0a, ids[kIdentification]);
        s.uuid(0x3c09, ids[kGeneration]);
        s.text(0x3c01, "Montage");
        s.text(0x3c02, "Montage");
        s.item(0x3c03, productVersion());
        s.text(0x3c04, "0.2.0");
        s.uuid(0x3c05, kProductUid);
        s.item(0x3c06, stamp(created));
        s.item(0x3c07, productVersion());
#ifdef _WIN32
        s.text(0x3c08, "win32");
#else
        s.text(0x3c08, "unix");
#endif
        putBytes(out, setKlv(0x30, s));
    }
    {
        Set s;
        s.uuid(0x3c0a, ids[kStorage]);
        s.refs(0x1901, {ids[kSource], ids[kMaterial]});
        s.refs(0x1902, {ids[kContainerData]});
        putBytes(out, setKlv(0x18, s));
    }
    {
        Set s;
        s.uuid(0x3c0a, ids[kContainerData]);
        s.item(0x2701, umid(asset));
        s.u32(0x3f06, kIndexSid);
        s.u32(0x3f07, kBodySid);
        putBytes(out, setKlv(0x23, s));
    }
    auto track = [&](Id self, Id seq, uint32_t number) {
        Set s;
        s.uuid(0x3c0a, ids[self]);
        s.u32(0x4801, 1);
        s.u32(0x4804, number);
        s.text(0x4802, trackName);
        s.uuid(0x4803, ids[seq]);
        s.rational(0x4b01, rate.num, rate.den);
        s.i64(0x4b02, 0);
        putBytes(out, setKlv(0x3b, s));
    };
    auto sequence = [&](Id self, Id component) {
        Set s;
        s.uuid(0x3c0a, ids[self]);
        s.label(0x0201, dataDef);
        s.i64(0x0202, duration);
        s.refs(0x1001, {ids[component]});
        putBytes(out, setKlv(0x0f, s));
    };
    auto clip = [&](Id self, const Bytes& sourcePackage, uint32_t sourceTrack) {
        Set s;
        s.uuid(0x3c0a, ids[self]);
        s.label(0x0201, dataDef);
        s.i64(0x0202, duration);
        s.i64(0x1201, 0);
        s.item(0x1101, sourcePackage);
        s.u32(0x1102, sourceTrack);
        putBytes(out, setKlv(0x11, s));
    };
    {
        Set s;
        s.uuid(0x3c0a, ids[kMaterial]);
        s.item(0x4401, umid(ids[kMaterialUmid]));
        s.text(0x4402, "Material Package");
        s.item(0x4405, stamp(created));
        s.item(0x4404, stamp(created));
        s.refs(0x4403, {ids[kMpTrack]});
        putBytes(out, setKlv(0x36, s));
    }
    track(kMpTrack, kMpSeq, 0);
    sequence(kMpSeq, kMpClip);
    clip(kMpClip, umid(asset), 1);
    {
        Set s;
        s.uuid(0x3c0a, ids[kSource]);
        s.item(0x4401, umid(asset));
        s.text(0x4402, packageName);
        s.item(0x4405, stamp(created));
        s.item(0x4404, stamp(created));
        s.refs(0x4403, {ids[kFpTrack]});
        s.uuid(0x4701, ids[kDescriptor]);
        putBytes(out, setKlv(0x37, s));
    }
    track(kFpTrack, kFpSeq, trackNumber);
    sequence(kFpSeq, kFpClip);
    clip(kFpClip, Bytes(32, 0), 0);
    return out;
}

// Frame offsets as index table segments (5000 entries to a segment), every frame a random access point.
Bytes vbrIndex(const std::vector<uint64_t>& offsets, EditRate rate) {
    Bytes out;
    const int64_t n = int64_t(offsets.size());
    for (int64_t first = 0; first < n || (n == 0 && first == 0); first += int64_t(kEntriesPerSegment)) {
        const int64_t count = std::min<int64_t>(int64_t(kEntriesPerSegment), n - first);
        Set s;
        indexBase(s, rate, first, count, 0);
        Bytes delta;
        put32(delta, 1);
        put32(delta, 6);
        put8(delta, 0);
        put8(delta, 0);
        put32(delta, 0);
        s.item(0x3f09, delta);
        Bytes entries;
        put32(entries, uint64_t(count));
        put32(entries, 11);
        for (int64_t k = first; k < first + count; ++k) {
            put8(entries, 0);
            put8(entries, 0);
            put8(entries, 0x80);
            put64(entries, offsets[size_t(k)]);
        }
        s.item(0x3f0a, entries);
        putBytes(out, klv(ul("060e2b34025301010d01020101100100"), s.body));
        if (n == 0) break;
    }
    return out;
}

// An index table segment's common items.
void indexBase(Set& s, EditRate rate, int64_t start, int64_t duration, uint32_t editUnitBytes) {
    s.uuid(0x3c0a, newUuid());
    s.rational(0x3f0b, rate.num, rate.den);
    s.i64(0x3f0c, start);
    s.i64(0x3f0d, duration);
    s.u32(0x3f05, editUnitBytes);
    s.u32(0x3f06, kIndexSid);
    s.u32(0x3f07, kBodySid);
    s.u8(0x3f08, 0);
    s.u8(0x3f0e, 0);
}

uint32_t readBe(const uint8_t* p, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) v = v << 8 | p[i];
    return v;
}

}  // namespace

// ---- Identities ---------------------------------------------------------------------------------------------------

Uuid newUuid() {
    static thread_local std::mt19937_64 rng(std::random_device{}() ^ uint64_t(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    Uuid u;
    for (size_t i = 0; i < 16; i += 8) {
        const uint64_t r = rng();
        std::memcpy(u.data() + i, &r, 8);
    }
    u[6] = uint8_t((u[6] & 0x0f) | 0x40);
    u[8] = uint8_t((u[8] & 0x3f) | 0x80);
    return u;
}

std::string uuidString(const Uuid& u) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s += '-';
        s += hex[u[i] >> 4];
        s += hex[u[i] & 15];
    }
    return s;
}

bool parseUuid(const std::string& text, Uuid& out) {
    std::string h;
    for (char c : text)
        if (std::isxdigit(static_cast<unsigned char>(c))) h += c;
        else if (c != '-') return false;
    if (h.size() != 32) return false;
    out = ul(h.c_str());
    return true;
}

bool parseJ2kHeader(const uint8_t* d, size_t n, J2kHeader& out) {
    if (n < 4 || d[0] != 0xff || d[1] != 0x4f) return false;  // SOC
    size_t i = 2;
    bool siz = false, cod = false, qcd = false;
    while (i + 4 <= n) {
        if (d[i] != 0xff) return false;
        const uint8_t marker = d[i + 1];
        if (marker == 0x90 || marker == 0x93 || marker == 0xd9) break;  // SOT, SOD, EOC: the main header is over
        const uint32_t len = readBe(d + i + 2, 2);
        if (len < 2 || i + 2 + len > n) return false;
        const uint8_t* body = d + i + 4;
        const size_t bodyLen = len - 2;
        if (marker == 0x51 && bodyLen >= 36) {
            out.rsiz = uint16_t(readBe(body, 2));
            out.xsiz = readBe(body + 2, 4), out.ysiz = readBe(body + 6, 4);
            out.xosiz = readBe(body + 10, 4), out.yosiz = readBe(body + 14, 4);
            out.xtsiz = readBe(body + 18, 4), out.ytsiz = readBe(body + 22, 4);
            out.xtosiz = readBe(body + 26, 4), out.ytosiz = readBe(body + 30, 4);
            out.csiz = uint16_t(readBe(body + 34, 2));
            if (bodyLen < 36 + size_t(out.csiz) * 3) return false;
            out.components.clear();
            for (int c = 0; c < out.csiz; ++c) out.components.push_back({body[36 + c * 3], body[37 + c * 3], body[38 + c * 3]});
            siz = true;
        } else if (marker == 0x52) {
            out.cod.assign(body, body + bodyLen);
            cod = true;
        } else if (marker == 0x5c) {
            out.qcd.assign(body, body + bodyLen);
            qcd = true;
        } else if (marker == 0x50 && bodyLen >= 4) {
            out.pcap = readBe(body, 4);
            out.ccap.clear();
            for (size_t k = 4; k + 2 <= bodyLen; k += 2) out.ccap.push_back(uint16_t(readBe(body + k, 2)));
        }
        i += 2 + len;
    }
    return siz && cod && qcd;
}

// ---- Encryption ---------------------------------------------------------------------------------------------------

namespace {

// SHA-1's compression of one 64-byte block (no padding: the FIPS 186-2 generator's G function).
void sha1Block(uint32_t h[5], const uint8_t* p) {
    auto rol = [](uint32_t v, int n) { return v << n | v >> (32 - n); };
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = readBe(p + 4 * i, 4);
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) f = (b & c) | (~b & d), k = 0x5a827999;
        else if (i < 40) f = b ^ c ^ d, k = 0x6ed9eba1;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdc;
        else f = b ^ c ^ d, k = 0xca62c1d6;
        const uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
}

// SHA-1 of a message (FIPS 180-4), without OpenSSL.
void sha1Plain(const uint8_t* data, size_t size, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    size_t at = 0;
    for (; at + 64 <= size; at += 64) sha1Block(h, data + at);
    uint8_t tail[128] = {};
    const size_t left = size - at;
    if (left) std::memcpy(tail, data + at, left);
    tail[left] = 0x80;
    const size_t blocks = left + 9 <= 64 ? 1 : 2;
    const uint64_t bits = uint64_t(size) * 8;
    for (int k = 0; k < 8; ++k) tail[blocks * 64 - 1 - size_t(k)] = uint8_t(bits >> (8 * k));
    for (size_t b = 0; b < blocks; ++b) sha1Block(h, tail + 64 * b);
    for (int i = 0; i < 5; ++i)
        for (int k = 0; k < 4; ++k) out[4 * i + k] = uint8_t(h[i] >> (24 - 8 * k));
}

// The 56 bytes after an encrypted triplet's value: the track file, the element's sequence number and its MIC (over
// the encrypted value and the 36 bytes before the MIC).
Bytes integrityPack(const Uuid& asset, uint64_t sequence, const uint8_t* esv, size_t esvSize, const std::array<uint8_t, 16>& mic) {
    Bytes b;
    putBer4(b, 16);
    putUuid(b, asset);
    putBer4(b, 8);
    put64(b, sequence);
    putBer4(b, 20);
    Bytes data(esv, esv + esvSize);
    putBytes(data, b);
    uint8_t mac[20];
    hmacSha1(mic.data(), mic.size(), data.data(), data.size(), mac);
    putBytes(b, mac, 20);
    return b;
}

}  // namespace

std::array<uint8_t, 16> micKey(const std::array<uint8_t, 16>& contentKey) {
    // ST 430-6 7.10: x1 of the FIPS 186-2 (change notice 1) general purpose generator seeded with the content key.
    uint8_t xkey[64] = {};
    std::memcpy(xkey, contentKey.data(), 16);
    auto g = [&](uint8_t out[20]) {
        uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
        sha1Block(h, xkey);
        for (int i = 0; i < 5; ++i)
            for (int k = 0; k < 4; ++k) out[4 * i + k] = uint8_t(h[i] >> (24 - 8 * k));
    };
    uint8_t x0[20], x1[20];
    g(x0);
    unsigned carry = 1;  // XKEY = (1 + XKEY + x0) mod 2^160
    for (int i = 19; i >= 0; --i) {
        const unsigned sum = unsigned(xkey[i]) + x0[i] + carry;
        xkey[i] = uint8_t(sum);
        carry = sum >> 8;
    }
    std::memset(xkey + 20, 0, 44);
    g(x1);
    std::array<uint8_t, 16> out{};
    std::memcpy(out.data(), x1, 16);
    return out;
}

// ---- Track files --------------------------------------------------------------------------------------------------

TrackFileWriter::~TrackFileWriter() {
    if (f_) std::fclose(f_);
}

bool TrackFileWriter::openImfFile(const std::string& path, const Uuid& asset, EditRate rate, std::string* error) {
    imf_ = true;
    if (!openFile(path, asset, int(std::lround(rate.value())), error)) return false;
    rate_ = rate;
    return true;
}

bool TrackFileWriter::writeRaw(const uint8_t* data, size_t size, std::string* error) {
    if (!f_) return false;
    if (size && std::fwrite(data, 1, size, f_) != size) {
        if (error) *error = "Cannot write " + path_ + " (is the disk full?)";
        return false;
    }
    essenceBytes_ += size;
    return true;
}

const Uuid& TrackFileWriter::descriptorId() const { return ids_[kDescriptor]; }

void TrackFileWriter::encryptWith(const TrackEncryption& e) {
    encrypted_ = true;
    crypto_ = e;
    micKey_ = micKey(e.key);
}

std::array<uint8_t, 16> TrackFileWriter::containerLabel() const { return encrypted_ ? ul(kEncryptedContainer) : essenceContainer(); }

bool TrackFileWriter::openFile(const std::string& path, const Uuid& asset, int fps, std::string* error) {
    path_ = path;
    asset_ = asset;
    fps_ = fps;
    rate_ = {uint32_t(fps), 1};
    for (Uuid& u : ids_) u = newUuid();
    created_ = nowStamp();
    elements_ = 0;
    streams_.clear();
    if (encrypted_ && imf_) {
        if (error) *error = "IMF track files are not encrypted here";
        return false;
    }
#ifdef _WIN32
    f_ = _wfopen(QString::fromStdString(path).toStdWString().c_str(), L"wb+");  // the path is UTF-8
#else
    f_ = std::fopen(path.c_str(), "wb+");
#endif
    if (!f_) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    // Room for the header (written properly on closing), then the body partition the essence follows.
    Bytes zeros(kHeaderSize, 0);
    Bytes body = partitionPack(ul("060e2b34020501010d01020101030400").data(), kHeaderSize, 0, 0, 0, 0, 0, kBodySid);
    if (std::fwrite(zeros.data(), 1, zeros.size(), f_) != zeros.size() || std::fwrite(body.data(), 1, body.size(), f_) != body.size()) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    return true;
}

std::vector<uint8_t> TrackFileWriter::partitionPack(const uint8_t key[16], uint64_t thisPartition, uint64_t previous, uint64_t footer,
                                                   uint64_t headerBytes, uint64_t indexBytes, uint32_t indexSid, uint32_t bodySid) const {
    Bytes v;
    put16(v, 1);              // major
    put16(v, imf_ ? 3 : 2);   // minor
    put32(v, 1);              // KAG
    put64(v, thisPartition);
    put64(v, previous);
    put64(v, footer);
    put64(v, headerBytes);
    put64(v, indexBytes);
    put32(v, indexSid);
    put64(v, 0);  // body offset
    put32(v, bodySid);
    putUl(v, imf_ ? kOp1a : kOpAtom);
    put32(v, 2);
    put32(v, 16);
    putUl(v, kGenericContainer);
    const auto ec = containerLabel();
    putBytes(v, ec.data(), 16);
    std::array<uint8_t, 16> k;
    std::memcpy(k.data(), key, 16);
    return klv(k, v);
}

bool TrackFileWriter::writeElement(const uint8_t key[16], const uint8_t* data, size_t size, std::string* error, bool index) {
    if (!f_) return false;
    ++elements_;
    if (encrypted_) {
        // The triplet: the context, the plaintext offset (none: the codestream's header is encrypted too, as
        // asdcplib does by default), the plain element's key and length, then the encrypted value (IV, the check value
        // and the essence in one CBC chain, the last block padded 00 01 02 ...) and the integrity pack.
        const size_t whole = size - size % 16;
        Bytes plain(kCheckValue, kCheckValue + 16);
        plain.insert(plain.end(), data, data + size);
        for (uint8_t i = 0; plain.size() < 16 + whole + 16; ++i) plain.push_back(i);
        Bytes esv(16 + plain.size());
        if (!randomBytes(esv.data(), 16) || !aesCbcEncrypt(crypto_.key.data(), esv.data(), plain.data(), plain.size(), esv.data() + 16)) {
            if (error) *error = "Cannot encrypt " + path_ + " (this build has no OpenSSL)";
            return false;
        }
        const Bytes pack = integrityPack(asset_, elements_, esv.data(), esv.size(), micKey_);
        const bool large = esv.size() >= (size_t(1) << 24) - 200;
        Bytes head;
        auto ber = [&](Bytes& b, uint64_t n) { large ? putBer8(b, n) : putBer4(b, size_t(n)); };
        putUl(head, kEncryptedElement);
        const uint64_t length = 20 + 12 + 20 + 12 + (large ? 8 : 4) + esv.size() + pack.size();
        ber(head, length);
        putBer4(head, 16);
        putUuid(head, ids_[kContextId]);
        putBer4(head, 8);
        put64(head, 0);
        putBer4(head, 16);
        putBytes(head, key, 16);
        putBer4(head, 8);
        put64(head, size);
        ber(head, esv.size());
        if (std::fwrite(head.data(), 1, head.size(), f_) != head.size() || std::fwrite(esv.data(), 1, esv.size(), f_) != esv.size() ||
            std::fwrite(pack.data(), 1, pack.size(), f_) != pack.size()) {
            if (error) *error = "Cannot write " + path_ + " (is the disk full?)";
            return false;
        }
        if (index) offsets_.push_back(essenceBytes_);
        essenceBytes_ += head.size() + esv.size() + pack.size();
        return true;
    }
    Bytes kl(key, key + 16);
    // A frame of 16 MiB or more (lossless UHD and 4K can be) needs the 8-byte length.
    if (size < (size_t(1) << 24)) putBer4(kl, size);
    else putBer8(kl, size);
    if (std::fwrite(kl.data(), 1, kl.size(), f_) != kl.size() || (size && std::fwrite(data, 1, size, f_) != size)) {
        if (error) *error = "Cannot write " + path_ + " (is the disk full?)";
        return false;
    }
    if (index) offsets_.push_back(essenceBytes_);
    essenceBytes_ += kl.size() + size;
    return true;
}

bool TrackFileWriter::startStream(uint32_t sid, std::string* error) {
    const uint64_t at = kHeaderSize + 140 + essenceBytes_;
    const uint64_t previous = streams_.empty() ? kHeaderSize : streams_.back().second;
    const Bytes pack = partitionPack(ul(kStreamPartition).data(), at, previous, 0, 0, 0, 0, sid);
    if (!writeRaw(pack.data(), pack.size(), error)) return false;
    streams_.emplace_back(sid, at);
    return true;
}

bool TrackFileWriter::close(std::string* error) {
    if (!f_) return false;
    auto fail = [&] {
        if (error) *error = "Cannot finish " + path_;
        std::fclose(f_);
        f_ = nullptr;
        return false;
    };
    const uint64_t bodyStart = kHeaderSize;
    if (imf_) {
        // AS-02: the index in a partition of its own after the essence, then an empty footer; the body partition
        // pack learns where the footer is.
        if (clipLengthAt_ >= 0) {
            Bytes len;
            put8(len, 0x87);
            const uint64_t n = essenceBytes_ - clipStart_;
            for (int k = 6; k >= 0; --k) put8(len, (n >> (8 * k)) & 0xff);
            if (std::fseek(f_, long(clipLengthAt_), SEEK_SET) != 0 || std::fwrite(len.data(), 1, len.size(), f_) != len.size()) return fail();
        }
        const uint64_t indexAt = bodyStart + 140 + essenceBytes_;
        const Bytes index = indexSegments();
        const uint64_t footerAt = indexAt + 140 + index.size();
        // (After any generic stream partitions: a timed text document's resources.)
        const uint64_t previous = streams_.empty() ? bodyStart : streams_.back().second;
        Bytes tail = partitionPack(ul("060e2b34020501010d01020101030400").data(), indexAt, previous, footerAt, 0, index.size(), kIndexSid, 0);
        putBytes(tail, index);
        putBytes(tail, partitionPack(ul("060e2b34020501010d01020101040400").data(), footerAt, indexAt, footerAt, 0, 0, 0, 0));
        std::vector<std::pair<uint32_t, uint64_t>> partitions = {{0, 0}, {kBodySid, bodyStart}};
        partitions.insert(partitions.end(), streams_.begin(), streams_.end());
        partitions.emplace_back(0, indexAt);
        partitions.emplace_back(0, footerAt);
        Bytes rip;
        for (auto [sid, at] : partitions) {
            put32(rip, sid);
            put64(rip, at);
        }
        put32(rip, 16 + 4 + rip.size() + 4);
        putBytes(tail, klv(ul("060e2b34020501010d01020101110100"), rip));
        if (std::fseek(f_, 0, SEEK_END) != 0 || std::fwrite(tail.data(), 1, tail.size(), f_) != tail.size()) return fail();
        Bytes body = partitionPack(ul("060e2b34020501010d01020101030400").data(), bodyStart, 0, footerAt, 0, 0, 0, kBodySid);
        if (std::fseek(f_, long(bodyStart), SEEK_SET) != 0 || std::fwrite(body.data(), 1, body.size(), f_) != body.size()) return fail();
        Bytes head = partitionPack(ul("060e2b34020501010d01020101020400").data(), 0, 0, footerAt, kHeaderSize - 140, 0, 0, 0);
        putBytes(head, headerMetadata(duration()));
        if (head.size() + 20 > kHeaderSize) return fail();
        Bytes fill(kHeaderSize - head.size() - 20, 0);
        putBytes(head, klv(ul("060e2b34010101020301021001000000"), fill));
        if (std::fseek(f_, 0, SEEK_SET) != 0 || std::fwrite(head.data(), 1, head.size(), f_) != head.size()) return fail();
        const bool ok = std::fclose(f_) == 0;
        f_ = nullptr;
        if (!ok && error) *error = "Cannot finish " + path_;
        return ok;
    }
    const uint64_t footerAt = bodyStart + 20 + 120 + essenceBytes_;
    const Bytes index = indexSegments();
    const uint64_t previous = streams_.empty() ? bodyStart : streams_.back().second;
    Bytes tail = partitionPack(ul("060e2b34020501010d01020101040400").data(), footerAt, previous, footerAt, 0, index.size(), kIndexSid, 0);
    putBytes(tail, index);
    // Random index pack: where each partition is.
    std::vector<std::pair<uint32_t, uint64_t>> partitions = {{0, 0}, {kBodySid, bodyStart}};
    partitions.insert(partitions.end(), streams_.begin(), streams_.end());
    partitions.emplace_back(0, footerAt);
    Bytes rip;
    for (auto [sid, at] : partitions) {
        put32(rip, sid);
        put64(rip, at);
    }
    put32(rip, 16 + 4 + rip.size() + 4);
    putBytes(tail, klv(ul("060e2b34020501010d01020101110100"), rip));
    if (std::fseek(f_, 0, SEEK_END) != 0 || std::fwrite(tail.data(), 1, tail.size(), f_) != tail.size()) return fail();
    // The header, now the duration and the footer are known.
    Bytes head = partitionPack(ul("060e2b34020501010d01020101020400").data(), 0, 0, footerAt, kHeaderSize - 140, 0, 0, 0);
    putBytes(head, headerMetadata(duration()));
    if (head.size() + 20 > kHeaderSize) return fail();
    Bytes fill(kHeaderSize - head.size() - 20, 0);
    putBytes(head, klv(ul("060e2b34010101020301021001000000"), fill));
    if (std::fseek(f_, 0, SEEK_SET) != 0 || std::fwrite(head.data(), 1, head.size(), f_) != head.size()) return fail();
    const bool ok = std::fclose(f_) == 0;
    f_ = nullptr;
    if (!ok && error) *error = "Cannot finish " + path_;
    return ok;
}

// ---- Picture ------------------------------------------------------------------------------------------------------

bool PictureMxfWriter::header(const uint8_t* codestream, size_t size, std::string* error) {
    if (!haveHeader_) {
        if (!parseJ2kHeader(codestream, size, j2k_)) {
            if (error) *error = "Not a JPEG 2000 codestream";
            return false;
        }
        haveHeader_ = true;
    }
    return true;
}

bool PictureMxfWriter::write(const uint8_t* codestream, size_t size, std::string* error) {
    if (stereo_) {
        if (error) *error = "A stereoscopic track file takes both eyes at once";
        return false;
    }
    return header(codestream, size, error) && writeElement(ul(kPictureElement).data(), codestream, size, error);
}

bool PictureMxfWriter::writeStereo(const uint8_t* left, size_t leftSize, const uint8_t* right, size_t rightSize, std::string* error) {
    if (!stereo_) {
        if (error) *error = "Not a stereoscopic track file";
        return false;
    }
    // The right eye straight after the left, the index pointing at the left (ST 429-10).
    return header(left, leftSize, error) && writeElement(ul(kPictureElement).data(), left, leftSize, error) &&
           writeElement(ul(kPictureElement).data(), right, rightSize, error, false);
}

std::array<uint8_t, 16> PictureMxfWriter::essenceContainer() const { return ul(kJ2kContainer); }

std::vector<uint8_t> PictureMxfWriter::headerMetadata(int64_t duration) const {
    Bytes out = primer(kCommonPrimer, kPicturePrimer, encrypted_);
    putBytes(out, packages(ids_, asset_, fps_, duration, created_, kJ2kContainer, kPictureDef, "Picture Track", 0x15010801,
                           stereo_ ? "File Package: SMPTE 429-10 frame wrapping of stereoscopic JPEG 2000 codestreams"
                                   : "File Package: SMPTE 429-4 frame wrapping of JPEG 2000 codestreams",
                           {encrypted_, ids_[kContextId], crypto_.keyId, kJ2kContainer}));
    const uint32_t w = j2k_.xsiz - j2k_.xosiz, h = j2k_.ysiz - j2k_.yosiz;
    // The DCI profile the codestream declares (Rsiz 3: 2K, 4: 4K).
    const char* coding = j2k_.rsiz == 4 ? "060e2b34040101090401020203010104" : "060e2b34040101090401020203010103";
    {
        Set s;
        s.uuid(0x3c0a, ids_[kDescriptor]);
        if (stereo_) s.refs(0xffff, {ids_[kSub], ids_[kStereoSub]});
        else s.refs(0xffff, {ids_[kSub]});
        s.u32(0x3006, 2);
        s.rational(0x3001, uint32_t(stereo_ ? 2 * fps_ : fps_), 1);  // stereoscopic: the codestreams' rate, two to a frame
        s.i64(0x3002, duration);
        s.label(0x3004, kJ2kContainer);
        s.u8(0x320c, 0);  // full frame
        s.u32(0x3203, w);
        s.u32(0x3202, h);
        s.rational(0x320e, w, h);
        s.label(0x3201, coding);
        s.u32(0x3406, 4095);
        s.u32(0x3407, 0);
        s.item(0x3401, Bytes(16, 0));
        putBytes(out, setKlv(0x29, s));
    }
    {
        Set s;
        s.uuid(0x3c0a, ids_[kSub]);
        s.u16(0xfffe, j2k_.rsiz);
        s.u32(0xfffd, j2k_.xsiz);
        s.u32(0xfffc, j2k_.ysiz);
        s.u32(0xfffb, j2k_.xosiz);
        s.u32(0xfffa, j2k_.yosiz);
        s.u32(0xfff9, j2k_.xtsiz);
        s.u32(0xfff8, j2k_.ytsiz);
        s.u32(0xfff7, j2k_.xtosiz);
        s.u32(0xfff6, j2k_.ytosiz);
        s.u16(0xfff5, j2k_.csiz);
        Bytes sizing;
        put32(sizing, j2k_.components.size());
        put32(sizing, 3);
        for (const auto& c : j2k_.components) putBytes(sizing, c.data(), 3);
        s.item(0xfff4, sizing);
        s.item(0xfff3, j2k_.cod);
        s.item(0xfff2, j2k_.qcd);
        putBytes(out, setKlv(0x5a, s));
    }
    if (stereo_) {
        Set s;
        s.uuid(0x3c0a, ids_[kStereoSub]);
        putBytes(out, setKlv(0x63, s));
    }
    return out;
}

std::vector<uint8_t> PictureMxfWriter::indexSegments() const {
    Bytes out;
    const int64_t n = frames();
    for (int64_t first = 0; first < n || (n == 0 && first == 0); first += int64_t(kEntriesPerSegment)) {
        const int64_t count = std::min<int64_t>(int64_t(kEntriesPerSegment), n - first);
        Set s;
        indexBase(s, rate_, first, count, 0);
        Bytes delta;
        put32(delta, 1);
        put32(delta, 6);
        put8(delta, 0);   // PosTableIndex
        put8(delta, 0);   // Slice
        put32(delta, 0);  // ElementDelta
        s.item(0x3f09, delta);
        Bytes entries;
        put32(entries, uint64_t(count));
        put32(entries, 11);
        for (int64_t k = first; k < first + count; ++k) {
            put8(entries, 0);     // temporal offset
            put8(entries, 0);     // key frame offset
            put8(entries, 0x80);  // a random access point
            put64(entries, offsets_[size_t(k)]);
        }
        s.item(0x3f0a, entries);
        putBytes(out, klv(ul("060e2b34025301010d01020101100100"), s.body));
        if (n == 0) break;
    }
    return out;
}

// ---- Sound --------------------------------------------------------------------------------------------------------

namespace {

// ST 430-12's edit rate codes and the zero bits that end a packet at each, so the packets fill the edit unit.
bool fskRate(int fps, int& code, int& packets, int& padding) {
    static const int rates[] = {24, 25, 30, 48, 50, 60, 96, 100, 120};
    for (int k = 0; k < 9; ++k)
        if (rates[k] == fps) {
            code = k;
            packets = k < 3 ? 4 : k < 6 ? 2 : 1;
            padding = k % 3 == 0 ? 25 : k % 3 == 1 ? 20 : 0;
            return true;
        }
    return false;
}

// The CRC (polynomial 0x1021, starting at 0, most significant bit first) a bit at a time.
void crcBit(uint16_t& crc, bool bit) {
    const bool top = ((crc >> 15) & 1) != bit;
    crc = uint16_t(crc << 1);
    if (top) crc ^= 0x1021;
}

}  // namespace

bool FskSync::start(int fps, const Uuid& asset) {
    if (!fskRate(fps, code_, packets_, padding_)) return false;
    samples_ = 48000 / fps;
    asset_ = asset;
    subIndex_ = 0;
    lastBit_ = polarity_ = false;
    return true;
}

void FskSync::frame(int64_t index, int32_t* out, size_t stride) {
    std::vector<bool> bits;
    bits.reserve(size_t(samples_ / 4));
    uint16_t crc = 0;
    bool crcOn = false;
    auto put = [&](uint32_t v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            const bool b = (v >> i) & 1;
            if (crcOn) crcBit(crc, b);
            bits.push_back(b);
        }
    };
    for (int k = 0; k < packets_; ++k) {
        put(0x4d56, 16);
        crc = 0;
        crcOn = true;
        put(uint32_t(code_), 4);
        put(0, 2);
        put(uint32_t(subIndex_), 2);
        for (int b = 0; b < 4; ++b) put(asset_[size_t(subIndex_ * 4 + b)], 8);
        put(uint32_t(index) & 0xffffff, 24);
        crcOn = false;
        put(crc, 16);
        put(0, 4);
        put(0, padding_);
        subIndex_ = (subIndex_ + 1) % 4;
    }
    // Each bit four samples: a 0 a half-cycle of 6 kHz, a 1 a whole cycle of 12 kHz; the polarity turns over after each 0.
    constexpr double kScale = 8388608.0;
    static const int32_t zero[4] = {int32_t(0.03827 * kScale), int32_t(0.09239 * kScale), int32_t(0.09239 * kScale), int32_t(0.03827 * kScale)};
    static const int32_t one[4] = {int32_t(0.07071 * kScale), int32_t(0.07071 * kScale), int32_t(-0.07071 * kScale), int32_t(-0.07071 * kScale)};
    size_t at = 0;
    for (bool b : bits) {
        if (!lastBit_) polarity_ = !polarity_;
        for (int i = 0; i < 4; ++i, ++at) {
            const int32_t v = b ? one[i] : zero[i];
            out[at * stride] = polarity_ ? v : -v;
        }
        lastBit_ = b;
    }
    for (; at < size_t(samples_); ++at) out[at * stride] = 0;
}

std::vector<FskPacket> readFskSync(const int32_t* samples, size_t count, size_t stride) {
    // A 1 changes sign halfway through its four samples; a 0 does not.
    std::vector<uint8_t> bits(count / 4);
    for (size_t k = 0; k < bits.size(); ++k) bits[k] = (samples[(4 * k) * stride] > 0) != (samples[(4 * k + 2) * stride] > 0);
    std::vector<FskPacket> out;
    auto value = [&](size_t at, int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = v << 1 | bits[at + size_t(i)];
        return v;
    };
    for (size_t at = 0; at + 16 + 80 <= bits.size();) {
        if (value(at, 16) != 0x4d56) {
            ++at;
            continue;
        }
        FskPacket p;
        size_t b = at + 16;
        uint16_t crc = 0;
        for (size_t i = 0; i < 64; ++i) crcBit(crc, bits[b + i]);
        p.rateCode = int(value(b, 4));
        p.subIndex = int(value(b + 6, 2));
        for (int i = 0; i < 4; ++i) p.uuid[size_t(i)] = uint8_t(value(b + 8 + size_t(8 * i), 8));
        p.editUnit = value(b + 40, 24);
        p.crc = uint16_t(value(b + 64, 16));
        p.crcOk = p.crc == crc;
        out.push_back(p);
        at = b + 80;
    }
    return out;
}

bool SoundMxfWriter::openWithSync(const std::string& path, const Uuid& asset, int fps, int soundfield, const std::string& language,
                                  std::string* error) {
    if (soundfield != 6 && soundfield != 8) {
        if (error) *error = "DCP sound is 5.1 (6 channels) or 7.1 (8 channels)";
        return false;
    }
    if (!fsk_.start(fps, asset)) {
        if (error) *error = "The sync signal for immersive sound runs at 24, 25, 30, 48, 50, 60, 96, 100 or 120 frames a second";
        return false;
    }
    sync_ = true;
    soundfield_ = soundfield;
    channels_ = 16;
    language_ = language.empty() ? "en" : language;
    return openFile(path, asset, fps, error);
}

bool SoundMxfWriter::open(const std::string& path, const Uuid& asset, int fps, int channels, const std::string& language, std::string* error) {
    if (channels != 6 && channels != 8) {
        if (error) *error = "DCP sound is 5.1 (6 channels) or 7.1 (8 channels)";
        return false;
    }
    if (48000 % fps != 0) {
        if (error) *error = "That frame rate does not divide 48 kHz";
        return false;
    }
    channels_ = channels;
    soundfield_ = channels;
    sync_ = false;
    language_ = language.empty() ? "en" : language;
    return openFile(path, asset, fps, error);
}

bool SoundMxfWriter::write(const float* samples, std::string* error) {
    const size_t n = size_t(samplesPerFrame()) * size_t(channels_);
    buf_.resize(n * 3);
    for (size_t i = 0; i < n; ++i) {
        const double v = std::clamp(double(samples[i]), -1.0, 1.0) * 8388607.0;
        const int32_t s = int32_t(std::lround(v));
        buf_[i * 3] = uint8_t(s);
        buf_[i * 3 + 1] = uint8_t(s >> 8);
        buf_[i * 3 + 2] = uint8_t(s >> 16);
    }
    if (sync_) {
        fskBuf_.resize(size_t(samplesPerFrame()));
        fsk_.frame(frames(), fskBuf_.data());
        for (size_t i = 0; i < fskBuf_.size(); ++i) {
            uint8_t* p = &buf_[(i * size_t(channels_) + size_t(kSyncChannel)) * 3];
            p[0] = uint8_t(fskBuf_[i]);
            p[1] = uint8_t(fskBuf_[i] >> 8);
            p[2] = uint8_t(fskBuf_[i] >> 16);
        }
    }
    return writeElement(ul(kSoundElement).data(), buf_.data(), buf_.size(), error);
}

std::array<uint8_t, 16> SoundMxfWriter::essenceContainer() const { return ul(kWaveContainer); }

std::vector<uint8_t> SoundMxfWriter::headerMetadata(int64_t duration) const {
    Bytes out = primer(kCommonPrimer, kSoundPrimer, encrypted_);
    putBytes(out, packages(ids_, asset_, fps_, duration, created_, kWaveContainer, kSoundDef, "Sound Track", 0x16010101,
                           "File Package: SMPTE 382M frame wrapping of wave audio", {encrypted_, ids_[kContextId], crypto_.keyId, kWaveContainer}));
    struct Channel {
        const char* symbol;
        const char* name;
        const char* dictionary;
    };
    static const Channel five[] = {{"chL", "Left", "060e2b340401010d0302010100000000"},
                                   {"chR", "Right", "060e2b340401010d0302010200000000"},
                                   {"chC", "Center", "060e2b340401010d0302010300000000"},
                                   {"chLFE", "LFE", "060e2b340401010d0302010400000000"},
                                   {"chLs", "Left Surround", "060e2b340401010d0302010500000000"},
                                   {"chRs", "Right Surround", "060e2b340401010d0302010600000000"}};
    static const Channel seven[] = {{"chL", "Left", "060e2b340401010d0302010100000000"},
                                    {"chR", "Right", "060e2b340401010d0302010200000000"},
                                    {"chC", "Center", "060e2b340401010d0302010300000000"},
                                    {"chLFE", "LFE", "060e2b340401010d0302010400000000"},
                                    {"chLss", "Left Side Surround", "060e2b340401010d0302010700000000"},
                                    {"chRss", "Right Side Surround", "060e2b340401010d0302010800000000"},
                                    {"chLrs", "Left Rear Surround", "060e2b340401010d0302010900000000"},
                                    {"chRrs", "Right Rear Surround", "060e2b340401010d0302010a00000000"}};
    // (with sync, only the soundfield's channels are labelled, as other DCP tools do: the sync channel has none)
    const int field = soundfield();
    const Channel* labels = field == 8 ? seven : five;
    std::vector<Uuid> subs = {ids_[kSub]};
    for (int c = 0; c < field; ++c) subs.push_back(ids_[kChannelSet0 + size_t(c)]);
    {
        Set s;
        s.uuid(0x3c0a, ids_[kDescriptor]);
        s.refs(0xffff, subs);
        s.u32(0x3006, 2);
        s.rational(0x3001, uint32_t(fps_), 1);
        s.i64(0x3002, duration);
        s.label(0x3004, kWaveContainer);
        s.rational(0x3d03, 48000, 1);
        s.u8(0x3d02, 0);
        s.u32(0x3d07, uint32_t(channels_));
        s.u32(0x3d01, 24);
        s.u16(0x3d0a, uint32_t(3 * channels_));
        s.u32(0x3d09, uint32_t(48000 * 3 * channels_));
        // ST 429-2 channel configuration: MCA labels, or configuration 4 (the 16-channel layout) with sync
        s.label(0x3d32, sync_ ? "060e2b340401010b0402021003010400" : "060e2b340401010d0402021003020000");
        putBytes(out, setKlv(0x48, s));
    }
    const Bytes language(language_.begin(), language_.end());
    {
        Set s;
        s.uuid(0x3c0a, ids_[kSub]);
        s.label(0xfffe, field == 8 ? "060e2b340401010d0302020200000000" : "060e2b340401010d0302020100000000");
        s.uuid(0xfffd, ids_[kSoundfieldLink]);
        s.text(0xfffc, field == 8 ? "sg71" : "sg51");
        s.text(0xfffb, field == 8 ? "7.1DS" : "5.1");
        s.item(0xfffa, language);
        putBytes(out, setKlv(0x6c, s));
    }
    for (int c = 0; c < field; ++c) {
        Set s;
        s.uuid(0x3c0a, ids_[kChannelSet0 + size_t(c)]);
        s.label(0xfffe, labels[c].dictionary);
        s.uuid(0xfffd, ids_[kChannelLink0 + size_t(c)]);
        s.text(0xfffc, labels[c].symbol);
        s.text(0xfffb, labels[c].name);
        s.u32(0xfff9, uint32_t((sync_ ? syncChannel(field, c) : c) + 1));
        s.item(0xfffa, language);
        s.uuid(0xfff8, ids_[kSoundfieldLink]);
        putBytes(out, setKlv(0x6b, s));
    }
    return out;
}

std::vector<uint8_t> SoundMxfWriter::indexSegments() const {
    Set s;
    const size_t bytes = size_t(samplesPerFrame()) * size_t(channels_) * 3;
    // Each frame's KLV, or its encrypted triplet (68 bytes of context, the value, 56 of integrity pack).
    const size_t unit = encrypted_ ? 20 + 68 + (bytes - bytes % 16 + 48) + 56 : 20 + bytes;
    indexBase(s, rate_, 0, frames(), uint32_t(unit));
    Bytes entries;
    put32(entries, 0);
    put32(entries, 11);
    s.item(0x3f0a, entries);
    return klv(ul("060e2b34025301010d01020101100100"), s.body);
}

// ---- Immersive sound (IAB) ----------------------------------------------------------------------------------------

namespace {

// ST 429-18's Data Essence Coding item (its version byte 5, as the standard has it) and its sub-descriptor's items.
const PrimerEntry kIabPrimer[] = {
    {0x3e01, "060e2b34010101050403030200000000"}, {0xfffe, "060e2b34010101050e09050600000000"},
    {0xfffd, "060e2b34010101050e09050700000000"}, {0xfffc, "060e2b34010101050e09050800000000"},
    {0xfffb, "060e2b34010101050e09050a00000000"}, {0xfffa, "060e2b340101010e040203010f000000"},
};

}  // namespace

bool IabMxfWriter::open(const std::string& path, const Uuid& asset, int fps, int maxChannels, int maxObjects, std::string* error) {
    maxChannels_ = maxChannels;
    maxObjects_ = maxObjects;
    return openFile(path, asset, fps, error);
}

bool IabMxfWriter::write(const uint8_t* frame, size_t size, std::string* error) {
    if (size > maxFrameBytes(fps_)) {
        if (error) *error = "An immersive sound frame is larger than a DCP allows (" + std::to_string(maxFrameBytes(fps_)) + " bytes)";
        return false;
    }
    return writeElement(ul(kIabElement).data(), frame, size, error);
}

std::array<uint8_t, 16> IabMxfWriter::essenceContainer() const { return ul(kContainer); }

std::vector<uint8_t> IabMxfWriter::headerMetadata(int64_t duration) const {
    Bytes out = primer(kCommonPrimer, kIabPrimer, encrypted_);
    putBytes(out, packages(ids_, asset_, fps_, duration, created_, kContainer, kDataDef, "IAB Track", 1,
                           "File Package: SMPTE ST 429-18 frame wrapping of IA bitstreams",
                           {encrypted_, ids_[kContextId], crypto_.keyId, kContainer}));
    {
        Set s;
        s.uuid(0x3c0a, ids_[kDescriptor]);
        s.refs(0xffff, {ids_[kSub]});
        s.u32(0x3006, 2);
        s.rational(0x3001, uint32_t(fps_), 1);
        s.i64(0x3002, duration);
        s.label(0x3004, kContainer);
        s.label(0x3e01, kCoding);
        putBytes(out, setKlv(kIabDescriptor, s));
    }
    {
        Set s;
        s.uuid(0x3c0a, ids_[kSub]);
        s.u8(0xfffe, 1);  // the IAFrames' Version
        s.u16(0xfffd, uint32_t(maxChannels_));
        s.u16(0xfffc, uint32_t(maxObjects_));
        s.u32(0xfffb, 0);  // the first frame of action is the first edit unit
        s.rational(0xfffa, 48000, 1);
        putBytes(out, setKlv(kIabSubDescriptor, s));
    }
    return out;
}

std::vector<uint8_t> IabMxfWriter::indexSegments() const { return vbrIndex(offsets_, rate_); }

// ---- Timed text ---------------------------------------------------------------------------------------------------

bool TimedTextMxfWriter::write(const std::string& xml, const Uuid& resourceId, int64_t duration, const std::string& language,
                               const std::vector<TimedTextResource>& resources, std::string* error) {
    resourceId_ = resourceId;
    duration_ = duration;
    language_ = language;
    // The root element's namespace, as the descriptor repeats it.
    namespace_.clear();
    for (size_t at = xml.find('<'); at != std::string::npos; at = xml.find('<', at + 1)) {
        if (xml.compare(at, 2, "<?") == 0 || xml.compare(at, 2, "<!") == 0) continue;
        const size_t end = xml.find('>', at);
        const std::string tag = xml.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (const size_t ns = tag.find("xmlns=\""); ns != std::string::npos) namespace_ = tag.substr(ns + 7, tag.find('"', ns + 7) - ns - 7);
        break;
    }
    if (!writeElement(ul(kTimedTextElement).data(), reinterpret_cast<const uint8_t*>(xml.data()), xml.size(), error)) return false;
    // Each resource in a generic stream partition of its own, streams 10, 11, ... in the descriptor's order.
    uint32_t sid = 10;
    for (const TimedTextResource& r : resources) {
        resources_.emplace_back(r.id, r.mime);
        resourceSets_.push_back(newUuid());
        if (!startStream(sid++, error) || !writeElement(ul(kStreamElement).data(), r.data.data(), r.data.size(), error, false)) return false;
    }
    return true;
}

std::array<uint8_t, 16> TimedTextMxfWriter::essenceContainer() const { return ul(kTimedTextContainer); }

std::vector<uint8_t> TimedTextMxfWriter::headerMetadata(int64_t duration) const {
    std::vector<PrimerEntry> entries(std::begin(kCommonPrimer), std::end(kCommonPrimer));
    entries.insert(entries.end(), std::begin(kTimedTextPrimer), std::end(kTimedTextPrimer));
    if (encrypted_) entries.insert(entries.end(), std::begin(kCryptoPrimer), std::end(kCryptoPrimer));
    Bytes out = primerOf(entries);
    // The file package's track number is 0, as asdcplib writes it.
    putBytes(out, packages(ids_, asset_, fps_, duration, created_, kTimedTextContainer, kDataDef, "Timed Text Track", 0,
                           "File Package: SMPTE 429-5 clip wrapping of D-Cinema Timed Text data",
                           {encrypted_, ids_[kContextId], crypto_.keyId, kTimedTextContainer}));
    {
        Set s;
        s.uuid(0x3c0a, ids_[kDescriptor]);
        if (!resourceSets_.empty()) s.refs(0xffff, resourceSets_);
        s.u32(0x3006, 2);
        s.rational(0x3001, uint32_t(fps_), 1);
        s.i64(0x3002, duration);
        s.label(0x3004, kTimedTextContainer);
        s.item(0x3e01, Bytes(16, 0));
        s.uuid(0xfffe, resourceId_);
        s.text(0xfffd, "UTF-8");
        s.text(0xfffc, namespace_);
        s.text(0xfffb, language_);
        putBytes(out, setKlv(0x64, s));
    }
    for (size_t i = 0; i < resources_.size(); ++i) {
        Set s;
        s.uuid(0x3c0a, resourceSets_[i]);
        s.uuid(0xfffa, resources_[i].first);
        s.text(0xfff9, resources_[i].second);
        s.u32(0x3f07, uint32_t(10 + i));
        putBytes(out, setKlv(0x65, s));
    }
    return out;
}

std::vector<uint8_t> TimedTextMxfWriter::indexSegments() const { return vbrIndex(offsets_, rate_); }

// ---- Reading ------------------------------------------------------------------------------------------------------

namespace {

// Whether two keys are the same, their version bytes (7) aside.
bool sameKey(const uint8_t* a, const char* hex) {
    const auto b = ul(hex);
    for (int i = 0; i < 16; ++i)
        if (i != 7 && a[i] != b[size_t(i)]) return false;
    return true;
}

std::string hexOf(const uint8_t* p) {
    static const char* digits = "0123456789abcdef";
    std::string h;
    for (int i = 0; i < 16; ++i) h += digits[p[i] >> 4], h += digits[p[i] & 15];
    return h;
}

struct File {
    std::FILE* f = nullptr;
    uint64_t size = 0;
    ~File() {
        if (f) std::fclose(f);
    }
    bool seek(uint64_t at) {
        if (at > size) return false;
#ifdef _WIN32
        return _fseeki64(f, int64_t(at), SEEK_SET) == 0;
#else
        return fseeko(f, off_t(at), SEEK_SET) == 0;
#endif
    }
    uint64_t tell() {
#ifdef _WIN32
        return uint64_t(_ftelli64(f));
#else
        return uint64_t(ftello(f));
#endif
    }
    bool read(uint8_t* p, size_t n) { return std::fread(p, 1, n, f) == n; }
    // A key and its BER length: the value's size, and the bytes before it.
    bool kl(uint8_t key[16], uint64_t& length, uint64_t& header) {
        uint8_t b;
        if (!read(key, 16) || !read(&b, 1)) return false;
        header = 17;
        if (b < 0x80) {
            length = b;
            return fits(length);
        }
        const int n = b & 0x7f;
        if (n > 8) return false;
        uint8_t v[8];
        if (!read(v, size_t(n))) return false;
        length = 0;
        for (int i = 0; i < n; ++i) length = length << 8 | v[i];
        header += uint64_t(n);
        return fits(length);
    }
    // Whether `length` bytes from here are in the file (a damaged or hostile length is not read).
    bool fits(uint64_t length) {
        const uint64_t at = tell();
        return at <= size && length <= size - at;
    }
};

bool openRead(File& file, const std::string& path) {
#ifdef _WIN32
    file.f = _wfopen(QString::fromStdString(path).toStdWString().c_str(), L"rb");
#else
    file.f = std::fopen(path.c_str(), "rb");
#endif
    if (!file.f) return false;
    if (std::fseek(file.f, 0, SEEK_END) != 0) return false;
    file.size = file.tell();
    return std::fseek(file.f, 0, SEEK_SET) == 0;
}

// A BER length inside a value (the triplet's own items).
bool berAt(const Bytes& v, size_t& at, uint64_t& out) {
    if (at >= v.size()) return false;
    const uint8_t b = v[at++];
    if (b < 0x80) {
        out = b;
        return true;
    }
    const int n = b & 0x7f;
    if (n > 8 || at + size_t(n) > v.size()) return false;
    out = 0;
    for (int i = 0; i < n; ++i) out = out << 8 | v[at++];
    return true;
}

// A local set's items by tag.
std::map<uint16_t, Bytes> items(const Bytes& v) {
    std::map<uint16_t, Bytes> out;
    for (size_t i = 0; i + 4 <= v.size();) {
        const uint16_t tag = uint16_t(v[i] << 8 | v[i + 1]);
        const size_t n = size_t(v[i + 2] << 8 | v[i + 3]);
        if (i + 4 + n > v.size()) break;
        out[tag] = Bytes(v.begin() + ptrdiff_t(i + 4), v.begin() + ptrdiff_t(i + 4 + n));
        i += 4 + n;
    }
    return out;
}

uint64_t beOf(const Bytes& b) {
    uint64_t v = 0;
    for (uint8_t c : b) v = v << 8 | c;
    return v;
}

}  // namespace

bool TrackFileReader::open(const std::string& path, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    path_ = path;
    iab_ = false;
    File file;
    if (!openRead(file, path)) return fail("Cannot open " + path);
    fileSize_ = file.size;
    // The random index pack at the end: where each partition is. Every length below has been checked against the
    // file's size (File::kl), so a damaged or hostile file is refused, not read past.
    uint8_t tail[4];
    if (fileSize_ < 64 || !file.seek(fileSize_ - 4) || !file.read(tail, 4)) return fail(path + " is not an MXF file");
    const uint64_t ripSize = readBe(tail, 4);
    uint8_t key[16];
    uint64_t length = 0, header = 0;
    if (ripSize > fileSize_ || !file.seek(fileSize_ - ripSize) || !file.kl(key, length, header) || !sameKey(key, "060e2b34020501010d01020101110100"))
        return fail(path + " has no random index pack");
    if (length < 4) return fail(path + " has a damaged random index pack");
    Bytes rip(static_cast<size_t>(length));
    if (!file.read(rip.data(), rip.size())) return fail(path + " is cut short");
    rip_.clear();
    for (size_t i = 0; i + 12 <= rip.size() - 4; i += 12) rip_.emplace_back(readBe(rip.data() + i, 4), uint64_t(readBe(rip.data() + i + 4, 4)) << 32 | readBe(rip.data() + i + 8, 4));
    // The header partition and its metadata.
    if (!file.seek(0) || !file.kl(key, length, header) || length < 64 || length > (1 << 20)) return fail(path + " is not an MXF file");
    Bytes pack(static_cast<size_t>(length));
    if (!file.read(pack.data(), pack.size())) return fail(path + " is cut short");
    const uint64_t footer = beOf(Bytes(pack.begin() + 24, pack.begin() + 32));
    const uint64_t headerBytes = beOf(Bytes(pack.begin() + 32, pack.begin() + 40));
    uint64_t at = header + length;
    const uint64_t end = headerBytes > fileSize_ - std::min(at, fileSize_) ? fileSize_ : at + headerBytes;
    std::map<std::string, uint16_t> tags;  // the primer: label to local tag
    auto tag = [&](const char* label) {
        const auto it = tags.find(label);
        return it == tags.end() ? uint16_t(0) : it->second;
    };
    bool haveRate = false;
    resourceStreams_.clear();
    while (at < end && file.seek(at) && file.kl(key, length, header)) {
        Bytes v(static_cast<size_t>(length));
        if (!file.read(v.data(), v.size())) return fail(path + " is cut short");
        at += header + length;
        if (sameKey(key, "060e2b34020501010d01020101050100")) {
            for (size_t i = 8; i + 18 <= v.size(); i += 18) tags[hexOf(v.data() + i + 2)] = uint16_t(v[i] << 8 | v[i + 1]);
            continue;
        }
        const auto set = items(v);
        auto get = [&](uint16_t t) {
            const auto it = set.find(t);
            return it == set.end() ? Bytes() : it->second;
        };
        if (std::memcmp(key, ul(kCryptoContext).data(), 16) == 0) {
            encrypted_ = true;
            const Bytes context = get(tag("060e2b34010101090101151100000000")), id = get(tag("060e2b34010101090209030102000000"));
            const Bytes mic = get(tag("060e2b34010101090209030201000000"));
            if (context.size() == 16) std::memcpy(context_.data(), context.data(), 16);
            if (id.size() == 16) std::memcpy(keyId_.data(), id.data(), 16);
            hmac_ = mic.size() == 16 && sameKey(mic.data(), kHmacSha1);
            continue;
        }
        if (sameKey(key, kIabDescriptor)) {  // an immersive sound track file (ST 429-18)
            iab_ = true;
            containerDuration_ = int64_t(beOf(get(0x3002)));
            continue;
        }
        const auto prefix = ul("060e2b34025301010d01010101010000");
        if (std::memcmp(key, prefix.data(), 14) != 0) continue;
        switch (key[14]) {
        case 0x3b:  // a track: the first one's rate is the file's
            if (!haveRate) {
                const Bytes r = get(0x4b01);
                if (r.size() == 8) rate_ = {readBe(r.data(), 4), readBe(r.data() + 4, 4)}, haveRate = true;
            }
            break;
        case 0x37:  // the file package: the asset is its UMID's material number
            if (const Bytes u = get(0x4401); u.size() == 32) std::memcpy(asset_.data(), u.data() + 16, 16);
            break;
        case 0x63: stereo_ = true; break;
        case 0x64: timedText_ = true; [[fallthrough]];
        case 0x29:
        case 0x48:
            containerDuration_ = int64_t(beOf(get(0x3002)));
            break;
        case 0x65: {
            const Bytes id = get(tag("060e2b340101010c0101151300000000"));
            Uuid u{};
            if (id.size() == 16) std::memcpy(u.data(), id.data(), 16);
            resourceStreams_.emplace_back(u, uint32_t(beOf(get(0x3f07))));
            break;
        }
        default: break;
        }
    }
    // The essence starts after the body partition's pack.
    bodyStart_ = 0;
    for (const auto& [sid, offset] : rip_)
        if (sid == 1 && file.seek(offset) && file.kl(key, length, header)) bodyStart_ = offset + header + length;
    if (!bodyStart_) return fail(path + " has no body partition");
    // The index, in the footer.
    offsets_.clear();
    if (!file.seek(footer) || !file.kl(key, length, header)) return fail(path + " has no footer");
    at = footer + header + length;
    while (at + 20 < fileSize_ && file.seek(at) && file.kl(key, length, header) && sameKey(key, "060e2b34025301010d01020101100100")) {
        Bytes v(static_cast<size_t>(length));
        if (!file.read(v.data(), v.size())) return fail(path + " is cut short");
        at += header + length;
        const auto set = items(v);
        auto get = [&](uint16_t t) {
            const auto it = set.find(t);
            return it == set.end() ? Bytes() : it->second;
        };
        const uint64_t unit = beOf(get(0x3f05));
        if (unit > 0) {
            // As many edit units as the file can hold, whatever the duration says.
            const uint64_t n = std::min<uint64_t>(containerDuration_ > 0 ? uint64_t(containerDuration_) : beOf(get(0x3f0d)), fileSize_ / unit);
            for (uint64_t k = 0; k < n; ++k) offsets_.push_back(k * unit);
            continue;
        }
        const Bytes e = get(0x3f0a);
        if (e.size() < 8) continue;
        const uint64_t count = readBe(e.data(), 4), size = readBe(e.data() + 4, 4);
        if (size < 11 || count > (e.size() - 8) / size) return fail(path + " has a damaged index");
        for (uint64_t k = 0; k < count; ++k)
            offsets_.push_back(beOf(Bytes(e.begin() + ptrdiff_t(8 + k * size + 3), e.begin() + ptrdiff_t(8 + k * size + 11))));
    }
    return true;
}

std::vector<uint32_t> TrackFileReader::streams() const {
    std::vector<uint32_t> out;
    for (const auto& [sid, offset] : rip_)
        if (sid >= 2) out.push_back(sid);
    return out;
}

uint64_t TrackFileReader::largestEditUnit() const {
    // Up to the next partition after the body: a generic stream's or the footer.
    uint64_t end = fileSize_;
    for (const auto& [sid, offset] : rip_)
        if (offset > bodyStart_) end = std::min(end, offset);
    uint64_t largest = 0;
    for (size_t i = 0; i < offsets_.size(); ++i) {
        const uint64_t next = i + 1 < offsets_.size() ? offsets_[i + 1] : end - bodyStart_;
        largest = std::max(largest, next - offsets_[i]);
    }
    return largest;
}

bool TrackFileReader::element(uint64_t at, uint64_t sequence, std::vector<uint8_t>& out, const uint8_t* key, std::string* error, uint64_t* next) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    File file;
    uint8_t k[16];
    uint64_t length = 0, header = 0;
    if (!openRead(file, path_) || !file.seek(at) || !file.kl(k, length, header)) return fail(path_ + " is cut short");
    if (next) *next = at + header + length;
    Bytes v(static_cast<size_t>(length));
    if (!file.read(v.data(), v.size())) return fail(path_ + " is cut short");
    if (!sameKey(k, kEncryptedElement)) {
        out = std::move(v);
        return true;
    }
    if (!key) return fail(path_ + " is encrypted: its content key is needed");
    // The triplet's context, plaintext offset, source key and length, then the encrypted value and integrity pack.
    size_t i = 0;
    uint64_t n = 0, offset = 0, sourceLength = 0, esvLength = 0;
    if (!berAt(v, i, n) || n != 16 || i + 16 > v.size() || std::memcmp(v.data() + i, context_.data(), 16) != 0)
        return fail(path_ + ": an element names another cryptographic context");
    i += 16;
    if (!berAt(v, i, n) || n != 8 || i + 8 > v.size()) return fail(path_ + ": a damaged encrypted element");
    offset = beOf(Bytes(v.begin() + ptrdiff_t(i), v.begin() + ptrdiff_t(i + 8)));
    i += 8;
    if (!berAt(v, i, n) || n != 16) return fail(path_ + ": a damaged encrypted element");
    i += 16;
    if (!berAt(v, i, n) || n != 8 || i + 8 > v.size()) return fail(path_ + ": a damaged encrypted element");
    sourceLength = beOf(Bytes(v.begin() + ptrdiff_t(i), v.begin() + ptrdiff_t(i + 8)));
    i += 8;
    // Every length against the element's own size (so none can wrap around).
    if (!berAt(v, i, esvLength) || esvLength > v.size() - i || offset > sourceLength || sourceLength > v.size() || esvLength < 48 ||
        offset > esvLength - 48 || esvLength != offset + (sourceLength - offset) - (sourceLength - offset) % 16 + 48)
        return fail(path_ + ": a damaged encrypted element");
    const uint8_t* esv = v.data() + i;
    // The check value and the encrypted blocks are one CBC chain; the plaintext part sits between them.
    Bytes chained(esv + 16, esv + 32);
    chained.insert(chained.end(), esv + 32 + offset, esv + esvLength);
    Bytes plain(chained.size());
    if (!aesCbcDecrypt(key, esv, chained.data(), chained.size(), plain.data())) return fail("This build cannot decrypt (it has no OpenSSL)");
    if (std::memcmp(plain.data(), kCheckValue, 16) != 0) return fail(path_ + ": that is not its content key");
    // DCI asks for the integrity pack on every encrypted element: a file without one cannot show it is unaltered.
    if (!hmac_) return fail(path_ + " has no integrity check (MIC) on its encrypted essence");
    {
        const size_t pack = i + size_t(esvLength);
        if (pack + 56 > v.size()) return fail(path_ + ": an encrypted element has no integrity pack");
        size_t j = pack;
        if (!berAt(v, j, n) || n != 16 || std::memcmp(v.data() + j, asset_.data(), 16) != 0) return fail(path_ + ": an element from another track file");
        j += 16;
        if (!berAt(v, j, n) || n != 8) return fail(path_ + ": a damaged integrity pack");
        const uint64_t seq = beOf(Bytes(v.begin() + ptrdiff_t(j), v.begin() + ptrdiff_t(j + 8)));
        if (sequence && seq != sequence) return fail(path_ + ": an element out of its place");
        std::array<uint8_t, 16> k16;
        std::memcpy(k16.data(), key, 16);
        const Bytes expected = integrityPack(asset_, seq, esv, size_t(esvLength), micKey(k16));
        if (std::memcmp(expected.data(), v.data() + pack, 56) != 0) return fail(path_ + ": an element's check value does not match (damaged or altered)");
    }
    out.assign(esv + 32, esv + 32 + offset);
    out.insert(out.end(), plain.begin() + 16, plain.begin() + 16 + ptrdiff_t(sourceLength - offset));
    return true;
}

bool TrackFileReader::read(int64_t index, int eye, std::vector<uint8_t>& out, const uint8_t* key, std::string* error) {
    if (index < 0 || index >= editUnits() || (eye && !stereo_)) {
        if (error) *error = "No edit unit " + std::to_string(index) + (eye ? " (right eye)" : "") + " in " + path_;
        return false;
    }
    const uint64_t at = bodyStart_ + offsets_[size_t(index)];
    const uint64_t sequence = stereo_ ? uint64_t(2 * index + eye + 1) : uint64_t(index + 1);
    if (!eye) return element(at, sequence, out, key, error);
    uint64_t next = 0;
    std::vector<uint8_t> left;
    // The right eye's element follows the left's: find where that ends without decrypting it.
    if (!element(at, 0, left, nullptr, nullptr, &next) && !next) return element(at, sequence, out, key, error);
    return element(next, sequence, out, key, error);
}

bool TrackFileReader::readStream(uint32_t sid, std::vector<uint8_t>& out, const uint8_t* key, std::string* error) {
    uint64_t sequence = 0;
    for (const auto& [s, offset] : rip_) {
        if (s > 0) ++sequence;
        if (s != sid) continue;
        File file;
        uint8_t k[16];
        uint64_t length = 0, header = 0;
        if (!openRead(file, path_) || !file.seek(offset) || !file.kl(k, length, header)) break;
        return element(offset + header + length, sequence, out, key, error);
    }
    if (error) *error = "No stream " + std::to_string(sid) + " in " + path_;
    return false;
}

// ---- IMF picture ---------------------------------------------------------------------------------------------------

bool ImfPictureWriter::open(const std::string& path, const Uuid& asset, EditRate rate, int bits, const ImfColour& colour, uint32_t aspectNum,
                            uint32_t aspectDen, std::string* error) {
    bits_ = bits;
    colour_ = colour;
    aspectNum_ = aspectNum;
    aspectDen_ = aspectDen;
    return openImfFile(path, asset, rate, error);
}

bool ImfPictureWriter::write(const uint8_t* codestream, size_t size, std::string* error) {
    if (!haveHeader_) {
        if (!parseJ2kHeader(codestream, size, j2k_)) {
            if (error) *error = "Not a JPEG 2000 codestream";
            return false;
        }
        haveHeader_ = true;
    }
    return writeElement(ul(kPictureElement).data(), codestream, size, error);
}

const Uuid& ImfPictureWriter::subDescriptorId() const { return ids_[kSub]; }

std::array<uint8_t, 16> ImfPictureWriter::essenceContainer() const { return ul(kImfJ2kContainer); }

std::vector<uint8_t> ImfPictureWriter::headerMetadata(int64_t duration) const {
    std::vector<PrimerEntry> entries(std::begin(kCommonPrimer), std::end(kCommonPrimer));
    entries.insert(entries.end(), std::begin(kPicturePrimer), std::end(kPicturePrimer));
    entries.insert(entries.end(), std::begin(kImfPicturePrimer), std::end(kImfPicturePrimer));
    Bytes out = primerOf(entries);
    putBytes(out, packagesImf(ids_, asset_, rate_, duration, created_, kImfJ2kContainer, kPictureDef, "Image Track", 0x15010801,
                              "File Package: SMPTE ST 422 / ST 2067-5 frame wrapping of JPEG 2000 codestreams"));
    const uint32_t w = j2k_.xsiz - j2k_.xosiz, h = j2k_.ysiz - j2k_.yosiz;
    uint8_t coding[16];
    imfPictureCoding(j2k_.rsiz, coding);
    Bytes layout(16, 0);  // R, G, B and their bits
    layout[0] = 'R', layout[1] = uint8_t(bits_), layout[2] = 'G', layout[3] = uint8_t(bits_), layout[4] = 'B', layout[5] = uint8_t(bits_);
    {
        Set s;
        s.uuid(0x3c0a, ids_[kDescriptor]);
        s.refs(0xffff, {ids_[kSub]});
        s.u32(0x3006, 1);
        s.rational(0x3001, rate_.num, rate_.den);
        s.i64(0x3002, duration);
        s.label(0x3004, kImfJ2kContainer);
        s.u8(0x320c, 0);  // full frame
        s.u32(0x3203, w);
        s.u32(0x3202, h);
        s.rational(0x320e, aspectNum_, aspectDen_);
        s.label(0x3210, colour_.transfer.c_str());
        s.item(0x3201, Bytes(coding, coding + 16));
        if (!colour_.codingEquations.empty()) s.label(0x321a, colour_.codingEquations.c_str());
        s.label(0x3219, colour_.primaries.c_str());
        Bytes lines;
        put32(lines, 2);
        put32(lines, 4);
        put32(lines, 0);
        put32(lines, 0);
        s.item(0x320d, lines);
        if (colour_.hdr) {
            // Chromaticities in steps of 0.00002, luminance in steps of 0.0001 cd/m^2.
            auto xy = [](double v) { return uint64_t(std::clamp(std::lround(v / 0.00002), 0L, 50000L)); };
            Bytes primaries;
            for (int i = 0; i < 6; ++i) put16(primaries, xy(colour_.display[i]));
            s.item(0xfff0, primaries);
            Bytes white;
            put16(white, xy(colour_.display[6]));
            put16(white, xy(colour_.display[7]));
            s.item(0xffef, white);
            s.u32(0xffee, uint64_t(std::llround(colour_.maxLuminance * 10000)));
            s.u32(0xffed, uint64_t(std::llround(colour_.minLuminance * 10000)));
        }
        s.u32(0x3406, (1u << bits_) - 1);
        s.u32(0x3407, 0);
        s.u8(0x3405, 0);
        s.item(0x3401, layout);
        putBytes(out, setKlv(0x29, s));
    }
    {
        Set s;
        s.uuid(0x3c0a, ids_[kSub]);
        s.u16(0xfffe, j2k_.rsiz);
        s.u32(0xfffd, j2k_.xsiz);
        s.u32(0xfffc, j2k_.ysiz);
        s.u32(0xfffb, j2k_.xosiz);
        s.u32(0xfffa, j2k_.yosiz);
        s.u32(0xfff9, j2k_.xtsiz);
        s.u32(0xfff8, j2k_.ytsiz);
        s.u32(0xfff7, j2k_.xtosiz);
        s.u32(0xfff6, j2k_.ytosiz);
        s.u16(0xfff5, j2k_.csiz);
        Bytes sizing;
        put32(sizing, j2k_.components.size());
        put32(sizing, 3);
        for (const auto& c : j2k_.components) putBytes(sizing, c.data(), 3);
        s.item(0xfff4, sizing);
        s.item(0xfff3, j2k_.cod);
        s.item(0xfff2, j2k_.qcd);
        if (j2k_.pcap) {
            Bytes caps;  // Pcap, then the Ccap values as a batch
            put32(caps, j2k_.pcap);
            put32(caps, j2k_.ccap.size());
            put32(caps, 2);
            for (uint16_t c : j2k_.ccap) put16(caps, c);
            s.item(0xffec, caps);
        }
        s.item(0xfff1, layout);
        putBytes(out, setKlv(0x5a, s));
    }
    return out;
}

std::vector<uint8_t> ImfPictureWriter::indexSegments() const { return vbrIndex(offsets_, rate_); }

// ---- IMF Application #5 (ACES) ------------------------------------------------------------------------------------

bool ImfAcesWriter::open(const std::string& path, const Uuid& asset, EditRate rate, int width, int height, bool alpha, const AcesPicture& picture,
                         uint32_t aspectNum, uint32_t aspectDen, std::string* error) {
    width_ = width;
    height_ = height;
    alpha_ = alpha;
    picture_ = picture;
    aspectNum_ = aspectNum;
    aspectDen_ = aspectDen;
    constraints_ = newUuid();
    pictureSet_ = newUuid();
    targets_.clear();
    return openImfFile(path, asset, rate, error);
}

bool ImfAcesWriter::write(const uint8_t* exr, size_t size, std::string* error) {
    // (an OpenEXR file starts 76 2f 31 01)
    if (size < 8 || exr[0] != 0x76 || exr[1] != 0x2f || exr[2] != 0x31 || exr[3] != 0x01) {
        if (error) *error = "Not an OpenEXR frame";
        return false;
    }
    return writeElement(ul(kAcesElement).data(), exr, size, error);
}

bool ImfAcesWriter::writeTargetFrames(std::vector<AcesTargetFrame> targets, std::string* error) {
    // Each named by a UUID made from its bytes (version 5, under Montage's own name space).
    static const uint8_t space[16] = {0x8d, 0x3e, 0x51, 0x0b, 0x6f, 0x2a, 0x4c, 0x77, 0x9e, 0x14, 0xc2, 0x58, 0x31, 0xa6, 0x0f, 0xd4};
    uint32_t sid = 10;
    if (targets.size() > size_t(kMaxTargetFrames)) {
        if (error) *error = "More than " + std::to_string(kMaxTargetFrames) + " target frames";
        return false;
    }
    for (AcesTargetFrame& t : targets) {
        if (t.index < 0 || t.index >= frames()) {
            if (error) *error = "A target frame is not one of the track file's frames";
            return false;
        }
        // (named by the frame it is of as well as its bytes, so two frames that look the same are two resources)
        Bytes named(space, space + 16);
        put64(named, uint64_t(t.index));
        putBytes(named, t.data);
        uint8_t digest[20];
        sha1Plain(named.data(), named.size(), digest);
        std::copy(digest, digest + 16, t.resource.begin());
        t.resource[6] = uint8_t((t.resource[6] & 0x0f) | 0x50);
        t.resource[8] = uint8_t((t.resource[8] & 0x3f) | 0x80);
        t.stream = sid++;
        t.set = newUuid();
        if (!startStream(t.stream, error) || !writeElement(ul(kStreamElement).data(), t.data.data(), t.data.size(), error, false)) return false;
        t.data = {};
        targets_.push_back(std::move(t));
    }
    return true;
}

std::array<uint8_t, 16> ImfAcesWriter::essenceContainer() const { return ul(kContainer); }

std::vector<uint8_t> ImfAcesWriter::headerMetadata(int64_t duration) const {
    std::vector<PrimerEntry> entries(std::begin(kCommonPrimer), std::end(kCommonPrimer));
    entries.insert(entries.end(), std::begin(kAcesPrimer), std::end(kAcesPrimer));
    Bytes out = primerOf(entries);
    putBytes(out, packagesImf(ids_, asset_, rate_, duration, created_, kContainer, kPictureDef, "Image Track", 0x15011201,
                              "File Package: SMPTE ST 2065-5 frame wrapping of ACES image containers"));
    // B, G, R (A first when there is alpha), each a half float (253), the rest of the eight entries empty.
    Bytes layout(16, 0);
    const char* order = alpha_ ? "ABGR" : "BGR";
    for (int i = 0; order[i]; ++i) layout[size_t(2 * i)] = uint8_t(order[i]), layout[size_t(2 * i + 1)] = 253;
    std::vector<Uuid> subs = {constraints_, pictureSet_};
    for (const AcesTargetFrame& t : targets_) subs.push_back(t.set);
    {
        // ST 2065-5 leaves out the items that do not apply to ACES: no coding equations, component or alpha ranges,
        // alpha transparency, second field or image offsets.
        Set s;
        s.uuid(0x3c0a, ids_[kDescriptor]);
        s.refs(0xffff, subs);
        s.u32(0x3006, 1);
        s.rational(0x3001, rate_.num, rate_.den);
        s.i64(0x3002, duration);
        s.label(0x3004, kContainer);
        s.u8(0x320c, 0);  // full frame
        s.u32(0x3203, uint32_t(width_));
        s.u32(0x3202, uint32_t(height_));
        s.u32(0x3208, uint32_t(height_));
        s.u32(0x3209, uint32_t(width_));
        s.u32(0x320a, 0);
        s.u32(0x320b, 0);
        s.rational(0x320e, aspectNum_, aspectDen_);
        s.label(0x3210, kTransfer);
        s.label(0x3201, coding());
        s.label(0x3219, kPrimaries);
        Bytes lines;
        put32(lines, 2);
        put32(lines, 4);
        put32(lines, 0);
        put32(lines, 0);
        s.item(0x320d, lines);
        s.item(0x3401, layout);
        putBytes(out, setKlv(0x29, s));
    }
    {
        Set s;  // ContainerConstraintsSubDescriptor (ST 379-2): nothing but its id
        s.uuid(0x3c0a, constraints_);
        putBytes(out, setKlv(0x67, s));
    }
    {
        Set s;  // ACESPictureSubDescriptor
        s.uuid(0x3c0a, pictureSet_);
        if (!picture_.authoring.empty()) s.text(0xfffe, picture_.authoring);
        if (picture_.mastering) {
            // Chromaticities in steps of 0.00002, luminance in steps of 0.0001 cd/m^2.
            auto xy = [](double v) { return uint64_t(std::clamp(std::lround(v / 0.00002), 0L, 50000L)); };
            Bytes primaries;
            for (int i = 0; i < 6; ++i) put16(primaries, xy(picture_.display[i]));
            s.item(0xfffd, primaries);
            Bytes white;
            put16(white, xy(picture_.display[6]));
            put16(white, xy(picture_.display[7]));
            s.item(0xfffc, white);
            s.u32(0xfffb, uint64_t(std::llround(picture_.maxLuminance * 10000)));
            s.u32(0xfffa, uint64_t(std::llround(picture_.minLuminance * 10000)));
        }
        putBytes(out, setKlv(0x79, s));
    }
    for (const AcesTargetFrame& t : targets_) {
        Set s;  // TargetFrameSubDescriptor
        s.uuid(0x3c0a, t.set);
        s.uuid(0xfff9, t.resource);
        s.text(0xfff8, t.mediaType);
        s.i64(0xfff7, t.index);
        s.label(0xfff6, t.transfer.c_str());
        s.label(0xfff5, t.primaries.c_str());
        s.u32(0xfff4, t.maxRef);
        s.u32(0xfff3, t.minRef);
        s.u32(0xfff2, t.stream);
        s.uuid(0xfff1, pictureSet_);
        putBytes(out, setKlv(0x7a, s));
    }
    return out;
}

std::vector<uint8_t> ImfAcesWriter::indexSegments() const { return vbrIndex(offsets_, rate_); }

// ---- IMF sound -----------------------------------------------------------------------------------------------------

McaLabel imfSoundfield(int channels) {
    if (channels == 2) return {"sgST", "Standard Stereo", "060e2b340401010d0302022001000000"};
    if (channels == 8) return {"sg71", "7.1DS", "060e2b340401010d0302020200000000"};
    return {"sg51", "5.1", "060e2b340401010d0302020100000000"};
}

std::vector<McaLabel> imfChannels(int channels) {
    std::vector<McaLabel> c = {{"chL", "Left", "060e2b340401010d0302010100000000"}, {"chR", "Right", "060e2b340401010d0302010200000000"}};
    if (channels == 2) return c;
    c.push_back({"chC", "Center", "060e2b340401010d0302010300000000"});
    c.push_back({"chLFE", "LFE", "060e2b340401010d0302010400000000"});
    if (channels == 8) {
        c.push_back({"chLss", "Left Side Surround", "060e2b340401010d0302010700000000"});
        c.push_back({"chRss", "Right Side Surround", "060e2b340401010d0302010800000000"});
        c.push_back({"chLrs", "Left Rear Surround", "060e2b340401010d0302010900000000"});
        c.push_back({"chRrs", "Right Rear Surround", "060e2b340401010d0302010a00000000"});
    } else {
        c.push_back({"chLs", "Left Surround", "060e2b340401010d0302010500000000"});
        c.push_back({"chRs", "Right Surround", "060e2b340401010d0302010600000000"});
    }
    return c;
}

bool ImfSoundWriter::open(const std::string& path, const Uuid& asset, int channels, const std::string& language, const std::string& title,
                          const std::string& titleVersion, std::string* error, const std::string& contentKind) {
    if (channels != 2 && channels != 6 && channels != 8) {
        if (error) *error = "IMF sound here is stereo, 5.1 or 7.1";
        return false;
    }
    channels_ = channels;
    language_ = language.empty() ? "en" : language;
    title_ = title.empty() ? "Untitled" : title;
    version_ = titleVersion.empty() ? "1" : titleVersion;
    kind_ = contentKind.empty() ? "PRM" : contentKind;
    if (!openImfFile(path, asset, {48000, 1}, error)) return false;
    // The one KLV the sound is wrapped in: its length (an eight-byte BER) is written when the file is closed.
    Bytes kl;
    putUl(kl, kImfSoundElement);
    clipLengthAt_ = long(kHeaderSize + 140 + 16);
    kl.insert(kl.end(), 8, 0);
    kl[16] = 0x87;
    if (!writeRaw(kl.data(), kl.size(), error)) return false;
    clipStart_ = essenceBytes_;
    return true;
}

bool ImfSoundWriter::write(const float* samples, size_t frames, std::string* error) {
    const size_t n = frames * size_t(channels_);
    buf_.resize(n * 3);
    for (size_t i = 0; i < n; ++i) {
        const int32_t s = int32_t(std::lround(std::clamp(double(samples[i]), -1.0, 1.0) * 8388607.0));
        buf_[i * 3] = uint8_t(s);
        buf_[i * 3 + 1] = uint8_t(s >> 8);
        buf_[i * 3 + 2] = uint8_t(s >> 16);
    }
    if (!writeRaw(buf_.data(), buf_.size(), error)) return false;
    samples_ += int64_t(frames);
    return true;
}

bool ImfSoundWriter::writePcm(const int32_t* samples, size_t frames, std::string* error) {
    const size_t n = frames * size_t(channels_);
    buf_.resize(n * 3);
    for (size_t i = 0; i < n; ++i) {
        const int32_t s = std::clamp(samples[i], -8388607, 8388607);
        buf_[i * 3] = uint8_t(s);
        buf_[i * 3 + 1] = uint8_t(s >> 8);
        buf_[i * 3 + 2] = uint8_t(s >> 16);
    }
    if (!writeRaw(buf_.data(), buf_.size(), error)) return false;
    samples_ += int64_t(frames);
    return true;
}

const Uuid& ImfSoundWriter::soundfieldId() const { return ids_[kSub]; }
const Uuid& ImfSoundWriter::soundfieldLink() const { return ids_[kSoundfieldLink]; }
const Uuid& ImfSoundWriter::channelId(int c) const { return ids_[kChannelSet0 + size_t(c)]; }
const Uuid& ImfSoundWriter::channelLink(int c) const { return ids_[kChannelLink0 + size_t(c)]; }

std::array<uint8_t, 16> ImfSoundWriter::essenceContainer() const { return ul(kImfWaveContainer); }

std::vector<uint8_t> ImfSoundWriter::headerMetadata(int64_t duration) const {
    std::vector<PrimerEntry> entries(std::begin(kCommonPrimer), std::end(kCommonPrimer));
    entries.insert(entries.end(), std::begin(kImfSoundPrimer), std::end(kImfSoundPrimer));
    Bytes out = primerOf(entries);
    putBytes(out, packagesImf(ids_, asset_, rate_, duration, created_, kImfWaveContainer, kSoundDef, "Sound Track", 0x16010201,
                              "File Package: SMPTE 382M clip wrapping of wave audio"));
    const std::vector<McaLabel> labels = imfChannels(channels_);
    std::vector<Uuid> subs = {ids_[kSub]};
    for (int c = 0; c < channels_; ++c) subs.push_back(ids_[kChannelSet0 + size_t(c)]);
    {
        Set s;
        s.uuid(0x3c0a, ids_[kDescriptor]);
        s.refs(0xffff, subs);
        s.u32(0x3006, 1);
        s.rational(0x3001, 48000, 1);
        s.i64(0x3002, duration);
        s.label(0x3004, kImfWaveContainer);
        s.rational(0x3d03, 48000, 1);
        s.u8(0x3d02, 0);
        s.u32(0x3d07, uint32_t(channels_));
        s.u32(0x3d01, 24);
        s.u16(0x3d0a, uint32_t(3 * channels_));
        s.u32(0x3d09, uint32_t(48000 * 3 * channels_));
        s.label(0x3d32, "060e2b340401010d0402021004010000");  // ST 2067-2: multichannel labels
        putBytes(out, setKlv(0x48, s));
    }
    const Bytes language(language_.begin(), language_.end());
    const McaLabel field = imfSoundfield(channels_);
    {
        Set s;
        s.uuid(0x3c0a, ids_[kSub]);
        s.label(0xfffe, field.dictionary);
        s.uuid(0xfffd, ids_[kSoundfieldLink]);
        s.text(0xfffc, field.symbol);
        s.text(0xfffb, field.name);
        s.item(0xfffa, language);
        s.text(0xfff9, title_);
        s.text(0xfff8, version_);
        s.text(0xfff7, kind_);   // the programme, music and effects, or described
        s.text(0xfff6, "FCMP");  // its full mix
        putBytes(out, setKlv(0x6c, s));
    }
    for (int c = 0; c < channels_; ++c) {
        Set s;
        s.uuid(0x3c0a, ids_[kChannelSet0 + size_t(c)]);
        s.label(0xfffe, labels[size_t(c)].dictionary);
        s.uuid(0xfffd, ids_[kChannelLink0 + size_t(c)]);
        s.text(0xfffc, labels[size_t(c)].symbol);
        s.text(0xfffb, labels[size_t(c)].name);
        s.u32(0xfff5, uint32_t(c + 1));
        s.item(0xfffa, language);
        s.uuid(0xfff4, ids_[kSoundfieldLink]);
        putBytes(out, setKlv(0x6b, s));
    }
    return out;
}

std::vector<uint8_t> ImfSoundWriter::indexSegments() const {
    Set s;
    indexBase(s, rate_, 0, samples_, uint32_t(3 * channels_));
    Bytes entries;
    put32(entries, 0);
    put32(entries, 11);
    s.item(0x3f0a, entries);
    return klv(ul("060e2b34025301010d01020101100100"), s.body);
}

// ---- IMF immersive sound (IAB) ------------------------------------------------------------------------------------

namespace {

// The sound descriptor's items an IAB descriptor uses, the label's (numbered as asdcplib numbers them) and the
// preface's ConformsToSpecifications.
const PrimerEntry kImfIabPrimer[] = {
    {0x3d01, "060e2b34010101040402030304000000"}, {0x3d02, "060e2b34010101040402030104000000"},
    {0x3d03, "060e2b34010101050402030101010000"}, {0x3d06, "060e2b34010101020402040200000000"},
    {0x3d07, "060e2b34010101050402010104000000"}, {0xfffe, "060e2b340101010e0103070101000000"},
    {0xfffd, "060e2b340101010e0103070105000000"}, {0xfffc, "060e2b340101010e0103070102000000"},
    {0xfffb, "060e2b340101010e0103070103000000"}, {0xfffa, "060e2b340101010d0301010203150000"},
    {0xfff9, "060e2b340101010e0105100000000000"}, {0xfff8, "060e2b340101010e0105110000000000"},
    {0xfff7, "060e2b340101010e0302010220000000"}, {0xfff6, "060e2b340101010e0302010221000000"},
    {0xfff5, "060e2b340101010e0402010106000000"}, {0xfff4, "060e2b340101010e0402010107000000"},
    {0xfff3, "060e2b340101010e0102021002040000"},
};
constexpr const char* kImfIabElement = "060e2b34010201010d01030116010d01";  // an IA bitstream clip (track number 16010d01)

}  // namespace

bool ImfIabWriter::open(const std::string& path, const Uuid& asset, EditRate rate, const std::string& language, const std::string& title,
                        const std::string& titleVersion, std::string* error) {
    language_ = language.empty() ? "en" : language;
    title_ = title.empty() ? "Untitled" : title;
    version_ = titleVersion.empty() ? "1" : titleVersion;
    if (!openImfFile(path, asset, rate, error)) return false;
    // One KLV for the whole bitstream, its length written on closing; the index points at each frame inside it.
    Bytes kl;
    putUl(kl, kImfIabElement);
    clipLengthAt_ = long(kHeaderSize + 140 + 16);
    kl.insert(kl.end(), 8, 0);
    kl[16] = 0x87;
    if (!writeRaw(kl.data(), kl.size(), error)) return false;
    clipStart_ = essenceBytes_;
    return true;
}

bool ImfIabWriter::write(const uint8_t* frame, size_t size, std::string* error) {
    const uint64_t at = essenceBytes_;
    if (!writeRaw(frame, size, error)) return false;
    offsets_.push_back(at);
    return true;
}

const Uuid& ImfIabWriter::soundfieldId() const { return ids_[kSub]; }
const Uuid& ImfIabWriter::soundfieldLink() const { return ids_[kSoundfieldLink]; }

std::array<uint8_t, 16> ImfIabWriter::essenceContainer() const { return ul(kContainer); }

std::vector<uint8_t> ImfIabWriter::headerMetadata(int64_t duration) const {
    std::vector<PrimerEntry> entries(std::begin(kCommonPrimer), std::end(kCommonPrimer));
    entries.insert(entries.end(), std::begin(kImfIabPrimer), std::end(kImfIabPrimer));
    Bytes out = primerOf(entries);
    putBytes(out, packagesImf(ids_, asset_, rate_, duration, created_, kContainer, kSoundDef, "IA Bitstream", 0x16010d01,
                              "Clip wrapping of IA bitstreams as specified in SMPTE ST 2067-201", kLevel0, 0xfff3));
    {
        Set s;
        s.uuid(0x3c0a, ids_[kDescriptor]);
        s.refs(0xffff, {ids_[kSub]});
        s.u32(0x3006, 1);
        s.rational(0x3001, rate_.num, rate_.den);
        s.i64(0x3002, duration);
        s.label(0x3004, kContainer);
        s.rational(0x3d03, 48000, 1);
        s.u8(0x3d02, 0);
        s.u32(0x3d07, 0);
        s.u32(0x3d01, 24);
        s.label(0x3d06, kCoding);
        s.rational(0xfff5, rate_.num, rate_.den);
        s.u8(0xfff4, uint8_t(int8_t(kReferenceLevel)));
        putBytes(out, setKlv("060e2b34025301010d01010101017b00", s));
    }
    {
        const Bytes language(language_.begin(), language_.end());
        Set s;
        s.uuid(0x3c0a, ids_[kSub]);
        s.label(0xfffe, kSoundfield);
        s.uuid(0xfffd, ids_[kSoundfieldLink]);
        s.text(0xfffc, "IAB");
        s.text(0xfffb, "IAB");
        s.item(0xfffa, language);
        s.text(0xfff9, title_);
        s.text(0xfff8, version_);
        s.text(0xfff7, "PRM");
        s.text(0xfff6, "FCMP");
        putBytes(out, setKlv("060e2b34025301010d01010101017c00", s));
    }
    return out;
}

std::vector<uint8_t> ImfIabWriter::indexSegments() const { return vbrIndex(offsets_, rate_); }

// ---- IMF timed text -----------------------------------------------------------------------------------------------

bool ImfTimedTextWriter::write(const std::string& xml, int64_t duration, const std::string& language, std::string* error,
                               const std::vector<TimedTextResource>& resources) {
    resourceId_ = newUuid();
    duration_ = duration;
    language_ = language.empty() ? "en" : language;
    image_ = xml.find(kImageProfile) != std::string::npos;
    if (!writeElement(ul(kTimedTextElement).data(), reinterpret_cast<const uint8_t*>(xml.data()), xml.size(), error)) return false;
    // Each resource in a generic stream partition of its own, streams 10, 11, ... in the descriptor's order.
    uint32_t sid = 10;
    for (const TimedTextResource& r : resources) {
        resources_.emplace_back(r.id, r.mime);
        resourceSets_.push_back(newUuid());
        if (!startStream(sid++, error) || !writeElement(ul(kStreamElement).data(), r.data.data(), r.data.size(), error, false)) return false;
    }
    return true;
}

std::array<uint8_t, 16> ImfTimedTextWriter::essenceContainer() const { return ul(kTimedTextContainer); }

std::vector<uint8_t> ImfTimedTextWriter::headerMetadata(int64_t duration) const {
    std::vector<PrimerEntry> entries(std::begin(kCommonPrimer), std::end(kCommonPrimer));
    entries.insert(entries.end(), std::begin(kTimedTextPrimer), std::end(kTimedTextPrimer));
    Bytes out = primerOf(entries);
    putBytes(out, packagesImf(ids_, asset_, rate_, duration, created_, kTimedTextContainer, kDataDef, "Data Track", 0x17010b01,
                              "File Package: SMPTE 429-5 clip wrapping of IMF Timed Text data"));
    Set s;
    s.uuid(0x3c0a, ids_[kDescriptor]);
    if (!resourceSets_.empty()) s.refs(0xffff, resourceSets_);
    s.u32(0x3006, 1);
    s.rational(0x3001, rate_.num, rate_.den);
    s.i64(0x3002, duration);
    s.label(0x3004, kTimedTextContainer);
    s.item(0x3e01, Bytes(16, 0));
    s.uuid(0xfffe, resourceId_);
    s.text(0xfffd, "UTF-8");
    s.text(0xfffc, profile());
    s.text(0xfffb, language_);
    putBytes(out, setKlv(0x64, s));
    for (size_t i = 0; i < resources_.size(); ++i) {
        Set r;
        r.uuid(0x3c0a, resourceSets_[i]);
        r.uuid(0xfffa, resources_[i].first);
        r.text(0xfff9, resources_[i].second);
        r.u32(0x3f07, uint32_t(10 + i));
        putBytes(out, setKlv(0x65, r));
    }
    return out;
}

std::vector<uint8_t> ImfTimedTextWriter::indexSegments() const { return vbrIndex(offsets_, rate_); }

// ---- Reading IMF track files --------------------------------------------------------------------------------------

bool ImfTrackReader::open(const std::string& path, std::string* error, bool imfPictureOnly) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    path_ = path;
    kind_ = Kind::Unknown;
    asset_ = {};
    channels_ = width_ = height_ = 0;
    rate_ = {};
    op1a_ = rgba_ = j2k_ = aces_ = false;
    acesAuthoring_.clear();
    targets_.clear();
    elements_.clear();
    clipAt_ = clipSize_ = 0;
    resources_.clear();
    std::map<uint16_t, std::string> primer;  // local tags and the labels they stand for (hex, the version byte as 00)
    // An item of a set by its label, through the primer (its dynamic tags differ between writers).
    auto item = [&](const std::map<uint16_t, Bytes>& set, const char* hex) -> const Bytes* {
        std::string want = hex;
        want.replace(14, 2, "00");
        for (const auto& [tag, value] : set)
            if (auto p = primer.find(tag); p != primer.end() && p->second == want) return &value;
        return nullptr;
    };
    File file;
    if (!openRead(file, path)) return fail("Cannot open " + path);
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> streamData;  // generic streams' elements, by body stream
    std::vector<std::pair<Uuid, uint32_t>> streamOf;                // timed text resources' ids and streams
    uint32_t stream = 0;                                            // the generic stream partition read last
    // Every KLV in turn: the header's sets (the file package's id, the sound's channels) and the essence elements
    // (06.0e.2b.34.01.02.01.01.0d.01.03.01, byte 12 the item: 15 picture, 16 sound, 17 data).
    bool sound = false, text = false, iab = false;
    for (uint64_t at = 0; at + 17 <= file.size;) {
        uint8_t key[16];
        uint64_t length = 0, head = 0;
        if (!file.seek(at) || !file.kl(key, length, head)) break;
        const uint64_t value = at + head;
        static const uint8_t essence[12] = {0x06, 0x0e, 0x2b, 0x34, 0x01, 0x02, 0x01, 0x01, 0x0d, 0x01, 0x03, 0x01};
        static const uint8_t set[6] = {0x06, 0x0e, 0x2b, 0x34, 0x02, 0x53};
        if (std::memcmp(key, ul("060e2b34020501010d01020101050100").data(), 16) == 0 && length >= 8 && length < (1u << 20)) {
            Bytes v(static_cast<size_t>(length));
            if (!file.read(v.data(), v.size())) return fail(path + " is damaged");
            const uint64_t n = beOf(Bytes(v.begin(), v.begin() + 4)), each = beOf(Bytes(v.begin() + 4, v.begin() + 8));
            for (uint64_t i = 0; each == 18 && i < n && 8 + 18 * (i + 1) <= v.size(); ++i) {
                const uint8_t* e = v.data() + 8 + 18 * i;
                std::string label = hexOf(e + 2);
                label.replace(14, 2, "00");
                primer[uint16_t(e[0] << 8 | e[1])] = label;
            }
        } else if (std::memcmp(key, ul(kStreamPartition).data(), 16) == 0 && length >= 64) {
            // (a generic stream partition: its BodySID, 60 bytes into the pack)
            Bytes v(64);
            if (!file.read(v.data(), v.size())) return fail(path + " is damaged");
            stream = uint32_t(beOf(Bytes(v.begin() + 60, v.begin() + 64)));
        } else if (std::memcmp(key, ul(kStreamElement).data(), 13) == 0) {
            if (stream) streamData[stream] = {value, length};
        } else if (std::memcmp(key, set, 6) == 0 && length < (1u << 20) && (key[14] == 0x79 || key[14] == 0x7a)) {
            // ACES picture and target frame sub-descriptors (ST 2065-5).
            Bytes v(static_cast<size_t>(length));
            if (!file.read(v.data(), v.size())) return fail(path + " is damaged");
            const auto it = items(v);
            auto text = [](const Bytes& b) {
                std::string out;
                for (size_t i = 0; i + 1 < b.size(); i += 2)
                    if (const unsigned c = unsigned(b[i]) << 8 | b[i + 1]; c >= 0x20 && c < 0x7f) out += char(c);
                return out;
            };
            if (key[14] == 0x79) {
                if (const Bytes* a = item(it, "060e2b340101010e0401060a01000000")) acesAuthoring_ = text(*a);
            } else {
                TargetFrame t;
                const Bytes* id = item(it, "060e2b340101010e0401060901000000");
                const Bytes* type = item(it, "060e2b340101010e0401060902000000");
                const Bytes* index = item(it, "060e2b340101010e0401060903000000");
                const Bytes* sid = item(it, "060e2b340101010e0401060908000000");
                if (id && id->size() == 16 && index && index->size() == 8 && sid && sid->size() == 4 && targets_.size() < 10000) {
                    std::copy(id->begin(), id->end(), t.resource.begin());
                    t.index = int64_t(std::min<uint64_t>(beOf(*index), uint64_t(1) << 40));
                    t.stream = uint32_t(beOf(*sid));
                    if (type) t.mediaType = text(*type);
                    targets_.push_back(t);
                }
            }
        } else if (std::memcmp(key, set, 6) == 0 && length < (1u << 20) && key[14] == 0x65) {
            // A timed text resource: its id and stream.
            Bytes v(static_cast<size_t>(length));
            if (!file.read(v.data(), v.size())) return fail(path + " is damaged");
            const auto it = items(v);
            const auto id = it.find(0xfffa), sid = it.find(0x3f07);
            if (id != it.end() && id->second.size() == 16 && sid != it.end() && sid->second.size() == 4) {
                Uuid u;
                std::copy(id->second.begin(), id->second.end(), u.begin());
                streamOf.emplace_back(u, uint32_t(beOf(sid->second)));
            }
        } else if (std::memcmp(key, essence, 12) == 0) {
            // (past the header: what it said is all there is)
            if (imfPictureOnly && !(op1a_ && rgba_ && (j2k_ || aces_))) return fail(path + " is not an IMF picture track file");
            if (key[12] == 0x15) {
                // (a frame a few bytes long, by the million: a damaged or hostile file, not pictures)
                if (elements_.size() >= (size_t(1) << 24)) return fail(path + " has too many frames");
                elements_.emplace_back(value, length);
            } else if (key[12] == 0x16 || key[12] == 0x17) {
                if (clipSize_ || sound || text) return fail(path + " is not clip-wrapped");
                (key[12] == 0x16 ? sound : text) = true;
                clipAt_ = value;
                clipSize_ = length;
            }
        } else if (std::memcmp(key, set, 6) == 0 && length < (1u << 20) &&
                   (key[14] == 0x2f || key[14] == 0x37 || key[14] == 0x48 || key[14] == 0x29 || key[14] == 0x28 || key[14] == 0x64 || key[14] == 0x7b)) {
            Bytes v(static_cast<size_t>(length));
            if (!file.read(v.data(), v.size())) return fail(path + " is damaged");
            const auto it = items(v);
            if (key[14] == 0x2f) {
                // (the preface's operational pattern: OP1a is 06.0e.2b.34.04.01.01.xx.0d.01.02.01.01.01, then its qualifier)
                if (auto op = it.find(0x3b09); op != it.end() && op->second.size() == 16) {
                    const Bytes& u = op->second;
                    op1a_ = u[8] == 0x0d && u[9] == 0x01 && u[10] == 0x02 && u[11] == 0x01 && u[12] == 0x01 && u[13] == 0x01;
                }
            } else if (key[14] == 0x37) {
                // (the file package's UMID: the material number, a UUID, after its 16-byte label)
                if (auto u = it.find(0x4401); u != it.end() && u->second.size() == 32) std::copy(u->second.begin() + 16, u->second.end(), asset_.begin());
            } else {
                if (auto c = it.find(0x3d07); c != it.end() && key[14] == 0x48) channels_ = int(beOf(c->second));
                if (key[14] == 0x29) rgba_ = true;
                if (key[14] == 0x7b) iab = true;  // an IAB essence descriptor (ST 2067-201)
                if (key[14] == 0x29 || key[14] == 0x28) {
                    auto size = [&](uint16_t tag) {
                        const auto x = it.find(tag);
                        return x != it.end() && x->second.size() == 4 ? int(std::min<uint64_t>(beOf(x->second), 65536)) : 0;
                    };
                    width_ = size(0x3203), height_ = size(0x3202);
                }
                // Sound Montage reads: 24-bit PCM at 48 or 96 kHz.
                if (key[14] == 0x48) {
                    if (auto q = it.find(0x3d01); q != it.end() && q->second.size() == 4 && beOf(q->second) != 24)
                        return fail(path + " is not 24-bit sound");
                }
                // (the essence container: the JPEG 2000 mapping, 06.0e.2b.34.04.01.01.xx.0d.01.03.01.02.0c)
                // (or ACES, ...02.19, ST 2065-5)
                if (auto ec = it.find(0x3004); ec != it.end() && ec->second.size() == 16 && (key[14] == 0x29 || key[14] == 0x28)) {
                    const bool gc = ec->second[8] == 0x0d && ec->second[9] == 0x01 && ec->second[10] == 0x03 && ec->second[11] == 0x01 &&
                                    ec->second[12] == 0x02;
                    j2k_ = gc && ec->second[13] == 0x0c;
                    aces_ = gc && ec->second[13] == 0x19;
                }
                if (auto r = it.find(0x3001); r != it.end() && r->second.size() == 8)
                    rate_ = EditRate{uint32_t(beOf(Bytes(r->second.begin(), r->second.begin() + 4))), uint32_t(beOf(Bytes(r->second.begin() + 4, r->second.end())))};
            }
        }
        at = value + length;
    }
    // (an edit rate a frame count can be turned into a time with)
    if (!text && (rate_.num == 0 || rate_.den == 0 || rate_.value() < 1 || rate_.value() > 192000))
        return fail(path + " has no edit rate Montage reads");
    if (iab && sound && elements_.empty()) {
        // The bitstream's frames, one after another in the clip: each an (empty) preamble and an IAFrame, both
        // tagged and with a 4-byte length.
        const uint64_t end = clipAt_ + clipSize_;
        for (uint64_t at = clipAt_; at < end;) {
            uint8_t h[5];
            if (!file.seek(at) || !file.read(h, 5) || h[0] != 0x01) return fail(path + " has a damaged IA bitstream");
            uint64_t next = at + 5 + readBe(h + 1, 4);
            if (next + 5 > end || !file.seek(next) || !file.read(h, 5) || h[0] != 0x02) return fail(path + " has a damaged IA bitstream");
            next += 5 + readBe(h + 1, 4);
            if (next > end || elements_.size() >= (size_t(1) << 24)) return fail(path + " has a damaged IA bitstream");
            elements_.emplace_back(at, next - at);
            at = next;
        }
        kind_ = Kind::Iab;
    } else if (!elements_.empty() && !sound && !text) kind_ = Kind::Picture;
    else if (sound && elements_.empty() && channels_ > 0 && channels_ <= 64) kind_ = Kind::Sound;
    else if (text && elements_.empty()) kind_ = Kind::TimedText;
    else return fail(path + " is not an IMF track file Montage reads");
    for (const auto& [id, sid] : streamOf) {
        const auto d = streamData.find(sid);
        resources_.push_back({id, d == streamData.end() ? 0 : d->second.first, d == streamData.end() ? 0 : d->second.second, d != streamData.end()});
    }
    for (TargetFrame& t : targets_)
        if (const auto d = streamData.find(t.stream); d != streamData.end()) t.at = d->second.first, t.size = d->second.second, t.present = true;
    return true;
}

bool ImfTrackReader::targetFrame(size_t i, std::vector<uint8_t>& out, std::string* error) const {
    File file;
    if (i < targets_.size() && targets_[i].present && targets_[i].size <= (uint64_t(1) << 30) && openRead(file, path_) &&
        file.seek(targets_[i].at) && file.fits(targets_[i].size)) {
        out.resize(size_t(targets_[i].size));
        if (file.read(out.data(), out.size())) return true;
    }
    if (error) *error = "Cannot read target frame " + std::to_string(i) + " of " + path_;
    return false;
}

bool ImfTrackReader::resource(const Uuid& id, std::vector<uint8_t>& out, std::string* error) const {
    for (const Resource& r : resources_) {
        if (r.id != id) continue;
        File file;
        if (!r.present || r.size > (uint64_t(64) << 20) || !openRead(file, path_) || !file.seek(r.at) || !file.fits(r.size)) break;
        out.resize(size_t(r.size));
        if (!file.read(out.data(), out.size())) break;
        return true;
    }
    if (error) *error = "No resource " + uuidString(id) + " in " + path_;
    return false;
}

uint64_t ImfTrackReader::largestFrame() const {
    uint64_t most = 0;
    for (const auto& e : elements_) most = std::max(most, e.second);
    return most;
}

bool ImfTrackReader::frame(int64_t index, std::vector<uint8_t>& out, std::string* error) const {
    if (index < 0 || index >= frames()) {
        if (error) *error = "No such frame";
        return false;
    }
    File file;
    const auto [at, size] = elements_[size_t(index)];
    if (!openRead(file, path_) || !file.seek(at) || !file.fits(size)) {
        if (error) *error = "Cannot read " + path_;
        return false;
    }
    out.resize(size_t(size));
    if (!file.read(out.data(), out.size())) {
        if (error) *error = "Cannot read " + path_;
        return false;
    }
    return true;
}

bool ImfTrackReader::pcm(int64_t first, int64_t count, std::vector<int32_t>& out, std::string* error) const {
    const uint64_t frame = 3 * uint64_t(std::max(1, channels_));
    File file;
    if (kind_ != Kind::Sound || first < 0 || count < 0 || first > samples() || count > samples() - first || !openRead(file, path_) ||
        !file.seek(clipAt_ + uint64_t(first) * frame)) {
        if (error) *error = "Cannot read the sound of " + path_;
        return false;
    }
    Bytes b(size_t(uint64_t(count) * frame));
    if (!file.read(b.data(), b.size())) {
        if (error) *error = "Cannot read the sound of " + path_;
        return false;
    }
    out.resize(b.size() / 3);
    for (size_t i = 0; i < out.size(); ++i) {
        const uint32_t v = uint32_t(b[3 * i]) | uint32_t(b[3 * i + 1]) << 8 | uint32_t(b[3 * i + 2]) << 16;
        out[i] = int32_t(v << 8) >> 8;
    }
    return true;
}

bool ImfTrackReader::text(std::string& out, std::string* error) const {
    File file;
    if (kind_ != Kind::TimedText || clipSize_ > (uint64_t(64) << 20) || !openRead(file, path_) || !file.seek(clipAt_)) {
        if (error) *error = "Cannot read the timed text of " + path_;
        return false;
    }
    out.resize(size_t(clipSize_));
    if (!file.read(reinterpret_cast<uint8_t*>(out.data()), out.size())) {
        if (error) *error = "Cannot read the timed text of " + path_;
        return false;
    }
    return true;
}


}  // namespace montage::dcp
