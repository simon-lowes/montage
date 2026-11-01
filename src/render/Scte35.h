// Montage — SCTE 35 cue messages (splice_info_section) for ad breaks in streaming packages: splice_insert and
// time_signal with segmentation and avail descriptors, as ANSI/SCTE 35 2023r1 lays them out, written and read back
// (CRC checked). Times are in 90 kHz ticks. Encrypted sections are refused.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace montage::scte35 {

struct Segmentation {
    uint32_t eventId = 0;
    bool cancel = false;
    bool deliveryNotRestricted = true;
    bool webDeliveryAllowed = true, noRegionalBlackout = true, archiveAllowed = true;
    int deviceRestrictions = 3;
    int64_t duration = -1;  // ticks, -1 = none
    int upidType = 0;
    std::vector<uint8_t> upid;
    int typeId = 0;  // 0x34 provider placement opportunity start, 0x35 its end, ...
    int segmentNum = 0, segmentsExpected = 0;
    bool subSegments = false;  // sub_segment_num and sub_segments_expected present (only for the types that allow them)
    int subSegmentNum = 0, subSegmentsExpected = 0;
};

struct Cue {
    enum Command { Null = 0x00, Insert = 0x05, TimeSignal = 0x06 } command = Insert;
    int sapType = 3;
    int64_t ptsAdjustment = 0;
    int cwIndex = 0xff;
    int tier = 0xfff;
    // splice_insert
    uint32_t eventId = 0;
    bool eventCancel = false;
    bool outOfNetwork = true;
    bool immediate = false;
    bool eventIdCompliance = true;
    int64_t ptsTime = -1;  // splice_time (also time_signal's); -1 = not specified
    int64_t breakDuration = -1;  // ticks, -1 = none
    bool autoReturn = true;
    int uniqueProgramId = 0, availNum = 0, availsExpected = 0;
    // descriptors
    std::vector<uint32_t> avails;  // avail_descriptor provider_avail_id
    std::vector<Segmentation> segmentations;
};

std::vector<uint8_t> encode(const Cue& cue);
bool decode(const std::vector<uint8_t>& bytes, Cue& cue, std::string* error = nullptr);

std::string toHex(const std::vector<uint8_t>& bytes);  // "0xFC30..."
std::vector<uint8_t> fromHex(const std::string& hex);  // with or without 0x; empty when not hex
std::string toBase64(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> fromBase64(const std::string& text);

}  // namespace montage::scte35
