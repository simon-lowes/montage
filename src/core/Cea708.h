// Montage — CTA-708 (DTVCC) captions, the digital captions US broadcast and streaming deliveries carry beside the
// CEA-608 compatibility bytes: caption service 1 as pop-on windows (DefineWindow, SetWindowAttributes,
// SetPenAttributes and SetPenColor, the text, then Display and Hide Windows), each placed where its caption sits and in
// the track's colours, sent as DTVCC packets in A/53 cc_data (cc_type 3 then 2) with the 608 pairs; read back from
// that data; and the MacCaption MCC file (SMPTE 334 caption distribution packets a frame) that carries both.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Captions.h"
#include "Model.h"

namespace montage {

// One cc_data entry: marker bits, cc_valid and cc_type, then two bytes (A/53, CTA-708 §4.4).
using CcTriplet = std::array<uint8_t, 3>;

// The cc_data entries a frame at `fps` carries with 708 (A/53's 9600 bit/s: 600 a second; 20 at 29.97, 24 at 25,
// 10 at 59.94), at most 31.
int ccCountForRate(double fps);

// The caption data of each of `frames` video frames at `fps` (captions timed in frames of that rate, frame 0 first):
// CEA-608 channel 1 pairs on the 29.97 Hz line-21 clock (field 1, with field 2's nulls beside them), and with `cea708`
// the same captions as CTA-708 service 1, each loaded into a hidden window up to a second ahead (eight windows in
// turn; rows over 42 characters or more than 4 wrapped again), shown at its start (later only when its load cannot be
// sent in time) and hidden at its end, then padding up to ccCountForRate entries. Only 608: just its pairs.
std::vector<std::vector<CcTriplet>> captionCcData(const std::vector<Caption>& captions, Rational fps, int64_t frames,
                                                 bool cea608, bool cea708, const CaptionStyle& style = {});

// Captions from CTA-708 service 1 in cc_data entries and when each came (seconds, in order): what the windows shown
// say after each DTVCC packet (rows joined by '\n'; a window scrolls past its last row), each change a new caption
// placed by its window's anchor (text growing on screen stays one); timed in frames at `fps`. `painted` says whether
// text was written into windows on screen (roll-up, paint-on), which 608 reads better. False if no caption is found.
bool captionsFrom708(const std::vector<std::pair<double, CcTriplet>>& triplets, Rational fps, std::vector<Caption>& out,
                     std::string* error = nullptr, bool* painted = nullptr);

// MacCaption MCC (V1.0): a SMPTE 334 caption distribution packet for every frame, with the 608 and 708 data above,
// at the sequence's rate when MCC has it (24, 25, 30, 50, 60, and 29.97 and 59.94 drop frame), else at 29.97; V2.0 for
// 59.94, which V1.0 lacks.
std::string captionsToMcc(const std::vector<Caption>& captions, Rational fps, const CaptionStyle& style = {});
// Reads MCC: its 708 service 1 when it has pop-on captions, else 608 channel 1 (else the 708 there is).
bool parseMcc(const std::string& text, Rational fps, std::vector<Caption>& out, std::string* error = nullptr);

}  // namespace montage
