// Caption data of the tests' own for a feed with four caption languages: CTA-708 service 1 and CEA-608 channel 1
// carrying the same pop-on captions (as Montage's own captionCcData sends them), CTA-708 service 2 and CEA-608
// channel 3 (field 2) carrying roll-up lines as a live captioner types them. Each frame's cc_data entries, the 608
// pairs on the 29.97 Hz line-21 clock and the 708 packets in the room the frame has left.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "core/Cea708.h"

namespace montage::testcc {

// A roll-up line and the frame it is typed from.
struct Line {
    int64_t frame = 0;
    std::string text;
};

inline uint8_t oddParity(uint8_t b) {
    b &= 0x7f;
    int ones = 0;
    for (int i = 0; i < 7; ++i) ones += (b >> i) & 1;
    return uint8_t(b | ((ones % 2) ? 0 : 0x80));
}

inline std::vector<std::vector<CcTriplet>> fourLanguages(const std::vector<Caption>& popOn, const std::vector<Line>& service2,
                                                         const std::vector<Line>& channel3, Rational fps, int64_t frames) {
    std::vector<std::vector<CcTriplet>> out = captionCcData(popOn, fps, frames, true, true);
    // Channel 3's pairs (field 2, data channel 1: its control codes those of field 2), each control code sent twice.
    std::deque<std::pair<int64_t, uint16_t>> pairs;
    auto pair = [&](int64_t f, uint8_t a, uint8_t b) { pairs.push_back({f, uint16_t((oddParity(a) << 8) | oddParity(b))}); };
    auto control = [&](int64_t f, uint8_t a, uint8_t b) {
        pair(f, a, b);
        pair(f, a, b);
    };
    for (size_t i = 0; i < channel3.size(); ++i) {
        const int64_t f = channel3[i].frame;
        if (i == 0) {
            control(f, 0x15, 0x25);  // roll-up, two rows
            control(f, 0x14, 0x60);  // row 15 (a preamble)
        }
        control(f, 0x15, 0x2d);  // carriage return
        const std::string& t = channel3[i].text;
        for (size_t k = 0; k < t.size(); k += 2) pair(f, uint8_t(t[k]), k + 1 < t.size() ? uint8_t(t[k + 1]) : 0);
    }
    // Service 2's packets: a two-row window shown at once, then each line after a carriage return.
    std::deque<std::pair<int64_t, std::vector<uint8_t>>> packets;
    for (size_t i = 0; i < service2.size(); ++i) {
        std::vector<uint8_t> svc;
        if (i == 0) svc = {0x98, 0x38 /* visible */, 0x80 | 90, 50, (7 << 4) | 1, 31, 0x09};
        else svc = {0x0D};
        svc.insert(svc.end(), service2[i].text.begin(), service2[i].text.end());
        packets.push_back({service2[i].frame, svc});
    }
    int sequence = 0;
    for (int64_t f = 0; f < frames; ++f) {
        std::vector<CcTriplet>& cc = out[size_t(f)];
        for (CcTriplet& t : cc)
            if (t == CcTriplet{0xFD, 0x80, 0x80} && !pairs.empty() && pairs.front().first <= f) {
                t = {0xFD, uint8_t(pairs.front().second >> 8), uint8_t(pairs.front().second & 0xff)};
                pairs.pop_front();
            }
        while (!packets.empty() && packets.front().first <= f) {
            const std::vector<uint8_t>& svc = packets.front().second;
            std::vector<uint8_t> pk{0, uint8_t((2 << 5) | svc.size())};
            pk.insert(pk.end(), svc.begin(), svc.end());
            if (pk.size() % 2) pk.push_back(0);
            size_t room = 0;  // (the padding at the frame's end)
            while (room < cc.size() && !(cc[cc.size() - 1 - room][0] & 0x04)) ++room;
            if (room < pk.size() / 2) break;
            pk[0] = uint8_t(((sequence++ & 3) << 6) | (pk.size() / 2));
            const size_t at = cc.size() - room;
            for (size_t i = 0; i < pk.size(); i += 2) cc[at + i / 2] = {uint8_t(i ? 0xFE : 0xFF), pk[i], pk[i + 1]};
            packets.pop_front();
        }
    }
    return out;
}

}  // namespace montage::testcc
