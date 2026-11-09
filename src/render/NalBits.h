// Montage — reading and writing the bits of H.264 and HEVC NAL units: emulation prevention taken out and put back,
// fixed-length and Exp-Golomb fields, and bits copied from one unit into another (render/HrdCheck, render/LongGopCopy).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace montage::nal {

// A NAL unit's payload without its emulation prevention bytes (00 00 03 → 00 00), from byte `from` (the NAL header's
// bytes are kept as they are when `from` is 0).
inline std::vector<uint8_t> unescape(const uint8_t* d, size_t n, size_t from = 0) {
    std::vector<uint8_t> out;
    out.reserve(n);
    int zeros = 0;
    for (size_t i = from; i < n; ++i) {
        if (zeros >= 2 && d[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = d[i] == 0 ? zeros + 1 : 0;
        out.push_back(d[i]);
    }
    return out;
}

// The NAL unit of `header` (its one or two bytes) and RBSP `rbsp`, emulation prevention put back.
inline std::vector<uint8_t> escape(const uint8_t* header, size_t headerBytes, const std::vector<uint8_t>& rbsp) {
    std::vector<uint8_t> out(header, header + headerBytes);
    out.reserve(headerBytes + rbsp.size() + rbsp.size() / 64 + 2);
    int zeros = 0;
    for (const uint8_t b : rbsp) {
        if (zeros >= 2 && b <= 3) {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return out;
}

struct Reader {
    const uint8_t* d = nullptr;
    size_t n = 0;    // bytes
    size_t pos = 0;  // bits
    bool ok = true;
    Reader() = default;
    Reader(const uint8_t* data, size_t bytes, size_t bit = 0) : d(data), n(bytes), pos(bit) {}
    explicit Reader(const std::vector<uint8_t>& v, size_t bit = 0) : d(v.data()), n(v.size()), pos(bit) {}
    size_t bitsLeft() const { return pos < n * 8 ? n * 8 - pos : 0; }
    int bit() {
        if (pos >= n * 8) {
            ok = false;
            return 0;
        }
        const int v = (d[pos / 8] >> (7 - pos % 8)) & 1;
        ++pos;
        return v;
    }
    uint64_t u(int bits) {
        uint64_t v = 0;
        for (int k = 0; k < bits; ++k) v = v << 1 | uint64_t(bit());
        return v;
    }
    bool flag() { return bit() != 0; }
    uint64_t ue() {
        int zeros = 0;
        while (ok && bit() == 0) {
            if (++zeros > 40) {
                ok = false;
                return 0;
            }
        }
        if (!ok) return 0;
        return ((uint64_t(1) << zeros) - 1) + u(zeros);
    }
    int64_t se() {
        const uint64_t k = ue();
        return k & 1 ? int64_t((k + 1) / 2) : -int64_t(k / 2);
    }
    void skip(size_t bits) {
        pos += bits;
        if (pos > n * 8) ok = false;
    }
    bool byteAligned() const { return pos % 8 == 0; }
    // Whether RBSP data is left before the stop bit (more_rbsp_data()).
    bool moreData() const {
        if (pos >= n * 8) return false;
        size_t last = n * 8;  // the stop bit: the last 1
        while (last > 0) {
            --last;
            if ((d[last / 8] >> (7 - last % 8)) & 1) break;
        }
        return pos < last;
    }
};

struct Writer {
    std::vector<uint8_t> out;
    size_t bits = 0;
    void bit(int v) {
        if (bits % 8 == 0) out.push_back(0);
        if (v) out.back() |= uint8_t(0x80 >> (bits % 8));
        ++bits;
    }
    void u(uint64_t v, int n) {
        for (int k = n - 1; k >= 0; --k) bit(int((v >> k) & 1));
    }
    void flag(bool v) { bit(v ? 1 : 0); }
    void ue(uint64_t v) {
        const uint64_t x = v + 1;
        int len = 0;
        while ((x >> len) > 1) ++len;
        u(0, len);
        u(x, len + 1);
    }
    void se(int64_t v) { ue(v > 0 ? uint64_t(2 * v - 1) : uint64_t(-2 * v)); }
    // Bits [from, to) of `src`.
    void copy(const uint8_t* src, size_t from, size_t to) {
        for (size_t p = from; p < to; ++p) bit((src[p / 8] >> (7 - p % 8)) & 1);
    }
    void copy(const std::vector<uint8_t>& src, size_t from, size_t to) { copy(src.data(), from, to); }
    bool aligned() const { return bits % 8 == 0; }
    // rbsp_trailing_bits(): a stop bit and zeros to the byte.
    void trailing() {
        bit(1);
        while (bits % 8) bit(0);
    }
    // An SEI payload's end: when not on a byte, a one and zeros to it.
    void payloadAlign() {
        if (bits % 8 == 0) return;
        bit(1);
        while (bits % 8) bit(0);
    }
};

// The position of an RBSP's stop bit (the last 1), in bits; 0 when there is none.
inline size_t stopBit(const std::vector<uint8_t>& rbsp) {
    size_t last = rbsp.size() * 8;
    while (last > 0) {
        --last;
        if ((rbsp[last / 8] >> (7 - last % 8)) & 1) return last;
    }
    return 0;
}

}  // namespace montage::nal
