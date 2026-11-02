#include "Scte35.h"

#include <QByteArray>

#include <cctype>

namespace montage::scte35 {
namespace {

class Writer {
public:
    void put(uint64_t v, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            if (n_ % 8 == 0) out.push_back(0);
            if ((v >> i) & 1) out.back() |= uint8_t(0x80 >> (n_ % 8));
            ++n_;
        }
    }
    void bytes(const std::vector<uint8_t>& b) {
        for (uint8_t c : b) put(c, 8);
    }
    std::vector<uint8_t> out;

private:
    size_t n_ = 0;
};

class Reader {
public:
    Reader(const uint8_t* d, size_t n) : d_(d), n_(n * 8) {}
    uint64_t get(int bits) {
        uint64_t v = 0;
        for (int i = 0; i < bits; ++i, ++pos_) v = (v << 1) | (pos_ < n_ ? (d_[pos_ / 8] >> (7 - pos_ % 8)) & 1 : 0);
        return v;
    }
    size_t byte() const { return pos_ / 8; }
    void seekByte(size_t b) { pos_ = b * 8; }
    bool overrun() const { return pos_ > n_; }

private:
    const uint8_t* d_;
    size_t n_, pos_ = 0;
};

// MPEG-2 sections' CRC (polynomial 0x04C11DB7, most significant bit first, starting from all ones, not inverted).
uint32_t crc32Mpeg(const uint8_t* d, size_t n) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= uint32_t(d[i]) << 24;
        for (int k = 0; k < 8; ++k) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u : crc << 1;
    }
    return crc;
}

constexpr uint32_t kCuei = 0x43554549;  // "CUEI"

// The segmentation types that may carry sub_segment_num and sub_segments_expected.
bool hasSubSegmentTypes(int type) {
    return type == 0x30 || type == 0x32 || type == 0x34 || type == 0x36 || type == 0x38 || type == 0x3a || type == 0x44 || type == 0x46;
}

void spliceTime(Writer& w, int64_t pts) {
    if (pts >= 0) {
        w.put(1, 1);
        w.put(0x3f, 6);
        w.put(uint64_t(pts) & 0x1ffffffffull, 33);
    } else {
        w.put(0, 1);
        w.put(0x7f, 7);
    }
}

int64_t readSpliceTime(Reader& r) {
    if (r.get(1)) {
        r.get(6);
        return int64_t(r.get(33));
    }
    r.get(7);
    return -1;
}

}  // namespace

std::vector<uint8_t> encode(const Cue& c) {
    Writer cmd;
    if (c.command == Cue::Insert) {
        cmd.put(c.eventId, 32);
        cmd.put(c.eventCancel, 1);
        cmd.put(0x7f, 7);
        if (!c.eventCancel) {
            cmd.put(c.outOfNetwork, 1);
            cmd.put(1, 1);  // program_splice_flag (component mode is deprecated)
            cmd.put(c.breakDuration >= 0, 1);
            cmd.put(c.immediate, 1);
            cmd.put(c.eventIdCompliance, 1);
            cmd.put(0x7, 3);
            if (!c.immediate) spliceTime(cmd, c.ptsTime);
            if (c.breakDuration >= 0) {
                cmd.put(c.autoReturn, 1);
                cmd.put(0x3f, 6);
                cmd.put(uint64_t(c.breakDuration) & 0x1ffffffffull, 33);
            }
            cmd.put(uint32_t(c.uniqueProgramId), 16);
            cmd.put(uint32_t(c.availNum), 8);
            cmd.put(uint32_t(c.availsExpected), 8);
        }
    } else if (c.command == Cue::TimeSignal) {
        spliceTime(cmd, c.ptsTime);
    }
    Writer desc;
    for (uint32_t a : c.avails) {
        desc.put(0x00, 8);
        desc.put(8, 8);
        desc.put(kCuei, 32);
        desc.put(a, 32);
    }
    for (const Segmentation& s : c.segmentations) {
        Writer d;
        d.put(kCuei, 32);
        d.put(s.eventId, 32);
        d.put(s.cancel, 1);
        d.put(1, 1);  // segmentation_event_id_compliance_indicator
        d.put(0x3f, 6);
        if (!s.cancel) {
            d.put(1, 1);  // program_segmentation_flag
            d.put(s.duration >= 0, 1);
            d.put(s.deliveryNotRestricted, 1);
            if (!s.deliveryNotRestricted) {
                d.put(s.webDeliveryAllowed, 1);
                d.put(s.noRegionalBlackout, 1);
                d.put(s.archiveAllowed, 1);
                d.put(uint32_t(s.deviceRestrictions) & 3, 2);
            } else {
                d.put(0x1f, 5);
            }
            if (s.duration >= 0) d.put(uint64_t(s.duration) & 0xffffffffffull, 40);
            d.put(uint32_t(s.upidType), 8);
            d.put(uint32_t(s.upid.size()), 8);
            d.bytes(s.upid);
            d.put(uint32_t(s.typeId), 8);
            d.put(uint32_t(s.segmentNum), 8);
            d.put(uint32_t(s.segmentsExpected), 8);
            if (s.subSegments && hasSubSegmentTypes(s.typeId)) {
                d.put(uint32_t(s.subSegmentNum), 8);
                d.put(uint32_t(s.subSegmentsExpected), 8);
            }
        }
        desc.put(0x02, 8);
        desc.put(uint32_t(d.out.size()), 8);
        desc.bytes(d.out);
    }
    Writer body;  // after section_length
    body.put(0, 8);  // protocol_version
    body.put(0, 1);  // encrypted_packet
    body.put(0, 6);  // encryption_algorithm
    body.put(uint64_t(c.ptsAdjustment) & 0x1ffffffffull, 33);
    body.put(uint32_t(c.cwIndex) & 0xff, 8);
    body.put(uint32_t(c.tier) & 0xfff, 12);
    body.put(uint32_t(cmd.out.size()), 12);
    body.put(uint32_t(c.command), 8);
    body.bytes(cmd.out);
    body.put(uint32_t(desc.out.size()), 16);
    body.bytes(desc.out);
    Writer w;
    w.put(0xfc, 8);
    w.put(0, 1);  // section_syntax_indicator
    w.put(0, 1);  // private_indicator
    w.put(uint32_t(c.sapType) & 3, 2);
    w.put(uint32_t(body.out.size() + 4), 12);
    w.bytes(body.out);
    const uint32_t crc = crc32Mpeg(w.out.data(), w.out.size());
    w.put(crc, 32);
    return w.out;
}

bool decode(const std::vector<uint8_t>& b, Cue& c, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    c = Cue{};
    if (b.size() < 17 || b[0] != 0xfc) return fail("not a splice_info_section");
    Reader r(b.data(), b.size());
    r.get(8);
    r.get(2);
    c.sapType = int(r.get(2));
    const size_t length = size_t(r.get(12));
    if (length + 3 != b.size()) return fail("its section_length does not match its size");
    if (crc32Mpeg(b.data(), b.size()) != 0) return fail("its CRC does not match");
    if (r.get(8) != 0) return fail("an unknown protocol_version");
    if (r.get(1)) return fail("it is encrypted");
    r.get(6);
    c.ptsAdjustment = int64_t(r.get(33));
    c.cwIndex = int(r.get(8));
    c.tier = int(r.get(12));
    const size_t cmdLength = size_t(r.get(12));
    const int type = int(r.get(8));
    const size_t cmdStart = r.byte();
    if (type == Cue::Insert) {
        c.command = Cue::Insert;
        c.eventId = uint32_t(r.get(32));
        c.eventCancel = r.get(1);
        r.get(7);
        if (!c.eventCancel) {
            c.outOfNetwork = r.get(1);
            const bool program = r.get(1);
            const bool duration = r.get(1);
            c.immediate = r.get(1);
            c.eventIdCompliance = r.get(1);
            r.get(3);
            if (!program) return fail("component splice mode (deprecated) is not read");
            if (!c.immediate) c.ptsTime = readSpliceTime(r);
            if (duration) {
                c.autoReturn = r.get(1);
                r.get(6);
                c.breakDuration = int64_t(r.get(33));
            }
            c.uniqueProgramId = int(r.get(16));
            c.availNum = int(r.get(8));
            c.availsExpected = int(r.get(8));
        }
    } else if (type == Cue::TimeSignal) {
        c.command = Cue::TimeSignal;
        c.ptsTime = readSpliceTime(r);
    } else if (type == Cue::Null) {
        c.command = Cue::Null;
    } else {
        return fail("a splice command other than splice_null, splice_insert or time_signal");
    }
    // (a splice_command_length of 0xFFF is the legacy "not given")
    if (cmdLength != 0xfff) {
        if (r.byte() - cmdStart != cmdLength) return fail("its splice_command_length does not match the command");
        r.seekByte(cmdStart + cmdLength);
    }
    const size_t loop = size_t(r.get(16));
    const size_t loopStart = r.byte();
    if (loopStart + loop + 4 > b.size()) return fail("its descriptors run past the end");
    while (r.byte() + 2 <= loopStart + loop) {
        const int tag = int(r.get(8));
        const size_t len = size_t(r.get(8));
        const size_t start = r.byte();
        if (start + len > loopStart + loop) return fail("a descriptor runs past the loop");
        const uint32_t id = len >= 4 ? uint32_t(r.get(32)) : 0;
        if (id == kCuei && tag == 0x00 && len >= 8) {
            c.avails.push_back(uint32_t(r.get(32)));
        } else if (id == kCuei && tag == 0x02 && len >= 9) {
            Segmentation s;
            s.eventId = uint32_t(r.get(32));
            s.cancel = r.get(1);
            r.get(7);
            if (!s.cancel) {
                const bool program = r.get(1);
                const bool duration = r.get(1);
                s.deliveryNotRestricted = r.get(1);
                if (!s.deliveryNotRestricted) {
                    s.webDeliveryAllowed = r.get(1);
                    s.noRegionalBlackout = r.get(1);
                    s.archiveAllowed = r.get(1);
                    s.deviceRestrictions = int(r.get(2));
                } else {
                    r.get(5);
                }
                if (!program) {
                    const int n = int(r.get(8));
                    for (int i = 0; i < n; ++i) r.get(48);
                }
                if (duration) s.duration = int64_t(r.get(40));
                s.upidType = int(r.get(8));
                const size_t upidLength = size_t(r.get(8));
                for (size_t i = 0; i < upidLength; ++i) s.upid.push_back(uint8_t(r.get(8)));
                s.typeId = int(r.get(8));
                s.segmentNum = int(r.get(8));
                s.segmentsExpected = int(r.get(8));
                if (hasSubSegmentTypes(s.typeId) && r.byte() + 2 <= start + len) {
                    s.subSegments = true;
                    s.subSegmentNum = int(r.get(8));
                    s.subSegmentsExpected = int(r.get(8));
                }
            }
            if (r.byte() > start + len) return fail("a segmentation descriptor runs past its length");
            c.segmentations.push_back(std::move(s));
        }
        r.seekByte(start + len);
    }
    if (r.overrun()) return fail("it ends early");
    return true;
}

std::string toHex(const std::vector<uint8_t>& bytes) {
    static const char* digits = "0123456789ABCDEF";
    std::string s = "0x";
    for (uint8_t c : bytes) s += {digits[c >> 4], digits[c & 15]};
    return s;
}

std::vector<uint8_t> fromHex(const std::string& hex) {
    std::string h;
    for (char c : hex)
        if (!std::isspace(static_cast<unsigned char>(c))) h += c;
    if (h.size() >= 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h = h.substr(2);
    if (h.size() % 2) return {};
    std::vector<uint8_t> out;
    auto value = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i < h.size(); i += 2) {
        const int a = value(h[i]), b = value(h[i + 1]);
        if (a < 0 || b < 0) return {};
        out.push_back(uint8_t(a * 16 + b));
    }
    return out;
}

std::string toBase64(const std::vector<uint8_t>& bytes) {
    return QByteArray(reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size())).toBase64().toStdString();
}

std::vector<uint8_t> fromBase64(const std::string& text) {
    const QByteArray b = QByteArray::fromBase64(QByteArray::fromStdString(text));
    return {b.begin(), b.end()};
}

}  // namespace montage::scte35
