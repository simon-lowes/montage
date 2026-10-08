#include "Zip.h"

#include <array>
#include <cstring>
#include <fstream>
#include <iterator>

namespace montage {

uint32_t crc32(const void* data, size_t size, uint32_t crc) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    const auto* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

namespace {

// Canonical Huffman decoding, a table per code: symbols by code length and code.
struct Huffman {
    uint16_t count[16] = {};
    std::vector<uint16_t> symbol;
    // False for an over-subscribed set of lengths (an incomplete one is allowed, as zlib does for one-code trees).
    bool build(const uint8_t* lengths, int n) {
        std::memset(count, 0, sizeof count);
        for (int i = 0; i < n; ++i) ++count[lengths[i]];
        count[0] = 0;
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false;
        }
        uint16_t offs[16] = {};
        for (int len = 1; len < 15; ++len) offs[len + 1] = uint16_t(offs[len] + count[len]);
        symbol.assign(size_t(n), 0);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbol[offs[lengths[i]]++] = uint16_t(i);
        return true;
    }
};

struct Inflater {
    const uint8_t* in;
    size_t size, pos = 0;
    uint32_t bitBuf = 0;
    int bitCount = 0;
    std::string& out;
    bool error = false;

    Inflater(const uint8_t* d, size_t n, std::string& o) : in(d), size(n), out(o) {}

    int bits(int need) {
        uint32_t v = bitBuf;
        while (bitCount < need) {
            if (pos >= size) {
                error = true;
                return 0;
            }
            v |= uint32_t(in[pos++]) << bitCount;
            bitCount += 8;
        }
        bitBuf = v >> need;
        bitCount -= need;
        return int(v & ((1u << need) - 1));
    }
    int decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= bits(1);
            if (error) return -1;
            const int c = h.count[len];
            if (code - c < first) return h.symbol[size_t(index + (code - first))];
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        error = true;
        return -1;
    }
    bool stored() {
        bitBuf = 0;
        bitCount = 0;
        if (pos + 4 > size) return false;
        const unsigned len = in[pos] | (in[pos + 1] << 8), nlen = in[pos + 2] | (in[pos + 3] << 8);
        pos += 4;
        if (len != (~nlen & 0xFFFF) || pos + len > size) return false;
        out.append(reinterpret_cast<const char*>(in + pos), len);
        pos += len;
        return true;
    }
    bool codes(const Huffman& lencode, const Huffman& distcode) {
        static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const uint16_t dbase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                           193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int sym = decode(lencode);
            if (sym < 0) return false;
            if (sym < 256) {
                out.push_back(char(sym));
            } else if (sym == 256) {
                return true;
            } else {
                sym -= 257;
                if (sym >= 29) return false;
                const size_t len = lbase[sym] + size_t(bits(lext[sym]));
                const int d = decode(distcode);
                if (d < 0 || d >= 30) return false;
                const size_t dist = dbase[d] + size_t(bits(dext[d]));
                if (error || dist > out.size()) return false;
                const size_t from = out.size() - dist;
                for (size_t k = 0; k < len; ++k) out.push_back(out[from + k]);  // may overlap what it writes
            }
        }
    }
    bool fixed() {
        static Huffman lencode, distcode;
        static const bool ready = [] {
            uint8_t l[288];
            int i = 0;
            for (; i < 144; ++i) l[i] = 8;
            for (; i < 256; ++i) l[i] = 9;
            for (; i < 280; ++i) l[i] = 7;
            for (; i < 288; ++i) l[i] = 8;
            lencode.build(l, 288);
            uint8_t d[30];
            std::memset(d, 5, sizeof d);
            distcode.build(d, 30);
            return true;
        }();
        (void)ready;
        return codes(lencode, distcode);
    }
    bool dynamic() {
        static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        if (error || nlen > 286 || ndist > 30) return false;
        uint8_t lengths[320] = {};
        for (int i = 0; i < ncode; ++i) lengths[order[i]] = uint8_t(bits(3));
        Huffman lencode, distcode;
        if (error || !lencode.build(lengths, 19)) return false;
        int index = 0;
        while (index < nlen + ndist) {
            int sym = decode(lencode);
            if (sym < 0) return false;
            if (sym < 16) {
                lengths[index++] = uint8_t(sym);
                continue;
            }
            int len = 0, repeat;
            if (sym == 16) {
                if (index == 0) return false;
                len = lengths[index - 1];
                repeat = 3 + bits(2);
            } else if (sym == 17) {
                repeat = 3 + bits(3);
            } else {
                repeat = 11 + bits(7);
            }
            if (error || index + repeat > nlen + ndist) return false;
            while (repeat--) lengths[index++] = uint8_t(len);
        }
        if (lengths[256] == 0) return false;  // no end-of-block code
        if (!lencode.build(lengths, nlen) || !distcode.build(lengths + nlen, ndist)) return false;
        return codes(lencode, distcode);
    }
};

uint16_t le16(const std::string& b, size_t at) { return uint16_t(uint8_t(b[at]) | (uint8_t(b[at + 1]) << 8)); }
uint32_t le32(const std::string& b, size_t at) { return uint32_t(le16(b, at)) | (uint32_t(le16(b, at + 2)) << 16); }
uint64_t le64(const std::string& b, size_t at) { return uint64_t(le32(b, at)) | (uint64_t(le32(b, at + 4)) << 32); }

}  // namespace

bool inflateRaw(const uint8_t* data, size_t size, std::string& out, size_t expected) {
    if (expected) out.reserve(out.size() + expected);
    Inflater f(data, size, out);
    int last;
    do {
        last = f.bits(1);
        const int type = f.bits(2);
        if (f.error) return false;
        bool ok = false;
        if (type == 0) ok = f.stored();
        else if (type == 1) ok = f.fixed();
        else if (type == 2) ok = f.dynamic();
        if (!ok || f.error) return false;
    } while (!last);
    return true;
}

bool ZipReader::openFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    return open(std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()));
}

bool ZipReader::open(std::string bytes) {
    bytes_ = std::move(bytes);
    entries_.clear();
    const std::string& b = bytes_;
    if (b.size() < 22) return false;
    // The end of central directory record, searched for back from the end (it may have a comment).
    size_t eocd = std::string::npos;
    for (size_t i = b.size() - 22 + 1; i-- > 0 && b.size() - i <= 22 + 65535;)
        if (le32(b, i) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos) return false;
    uint64_t count = le16(b, eocd + 10), cdOffset = le32(b, eocd + 16);
    if (cdOffset == 0xFFFFFFFFu || count == 0xFFFF) {
        // Zip64: the locator just before points at the Zip64 end record.
        if (eocd < 20 || le32(b, eocd - 20) != 0x07064b50) return false;
        const uint64_t at = le64(b, eocd - 20 + 8);
        if (at + 56 > b.size() || le32(b, size_t(at)) != 0x06064b50) return false;
        count = le64(b, size_t(at) + 32);
        cdOffset = le64(b, size_t(at) + 48);
    }
    size_t p = size_t(cdOffset);
    for (uint64_t n = 0; n < count; ++n) {
        if (p + 46 > b.size() || le32(b, p) != 0x02014b50) return false;
        Entry e;
        e.method = le16(b, p + 10);
        e.crc = le32(b, p + 16);
        e.compressedSize = le32(b, p + 20);
        e.size = le32(b, p + 24);
        const size_t nameLen = le16(b, p + 28), extraLen = le16(b, p + 30), commentLen = le16(b, p + 32);
        e.headerOffset = le32(b, p + 42);
        if (p + 46 + nameLen + extraLen > b.size()) return false;
        e.name = b.substr(p + 46, nameLen);
        // Zip64 sizes and offset, in that order, for each field that is all ones.
        for (size_t x = p + 46 + nameLen; x + 4 <= p + 46 + nameLen + extraLen;) {
            const uint16_t id = le16(b, x), len = le16(b, x + 2);
            if (id == 0x0001) {
                size_t q = x + 4;
                if (e.size == 0xFFFFFFFFu && q + 8 <= x + 4 + len) e.size = le64(b, q), q += 8;
                if (e.compressedSize == 0xFFFFFFFFu && q + 8 <= x + 4 + len) e.compressedSize = le64(b, q), q += 8;
                if (e.headerOffset == 0xFFFFFFFFu && q + 8 <= x + 4 + len) e.headerOffset = le64(b, q);
            }
            x += 4 + size_t(len);
        }
        entries_.push_back(std::move(e));
        p += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

const ZipReader::Entry* ZipReader::find(const std::string& name) const {
    for (const Entry& e : entries_)
        if (e.name == name) return &e;
    return nullptr;
}

bool ZipReader::read(const std::string& name, std::string& out, std::string* error) const {
    const Entry* e = find(name);
    if (!e) {
        if (error) *error = "No " + name + " in the archive";
        return false;
    }
    return read(*e, out, error);
}

bool ZipReader::read(const Entry& e, std::string& out, std::string* error) const {
    out.clear();
    const std::string& b = bytes_;
    const size_t h = size_t(e.headerOffset);
    if (h + 30 > b.size() || le32(b, h) != 0x04034b50) {
        if (error) *error = e.name + ": bad local header";
        return false;
    }
    const size_t data = h + 30 + le16(b, h + 26) + le16(b, h + 28);
    if (data + e.compressedSize > b.size()) {
        if (error) *error = e.name + ": truncated";
        return false;
    }
    const auto* src = reinterpret_cast<const uint8_t*>(b.data() + data);
    if (e.method == 0) {
        out.assign(reinterpret_cast<const char*>(src), size_t(e.compressedSize));
    } else if (e.method == 8) {
        if (!inflateRaw(src, size_t(e.compressedSize), out, size_t(e.size))) {
            if (error) *error = e.name + ": corrupt compressed data";
            return false;
        }
    } else {
        if (error) *error = e.name + ": unsupported compression";
        return false;
    }
    if (out.size() != e.size || crc32(out.data(), out.size()) != e.crc) {
        if (error) *error = e.name + ": checksum mismatch";
        out.clear();
        return false;
    }
    return true;
}

}  // namespace montage
