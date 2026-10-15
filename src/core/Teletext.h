// Montage — teletext subtitles (World System Teletext, ETS 300 706) as broadcasters in the UK, Europe and Australia
// still take them inside HD files: a subtitle page (888 by convention) with each caption boxed in double-height rows
// near the bottom, the page erased and sent again at each change, sent as teletext packets in OP-47 subtitling
// distribution packets (Free TV Australia OP-47, SMPTE RDD 8): the ANC packets (DID 43h, SDID 02h) an ST 436 track of
// an MXF file carries. Read back the same way, so a file's subtitles can be checked or imported.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "Captions.h"
#include "Model.h"

namespace montage {

// One teletext packet as it goes on a VBI line, after the clock run-in and framing code: the magazine and row address
// (two Hamming 8/4 bytes) then 40 bytes; each byte as transmitted, first bit the least significant.
using TeletextPacket = std::array<uint8_t, 42>;

// The longest subtitle row: 40 columns less the colour, double height and two start and end box codes.
constexpr int kTeletextRowChars = 34;

// The packets each field carries (two a frame with `interlaced`, else one) for `frames` frames at `fps`, the captions
// timed in frames of that rate from frame 0: at each caption a header for `page` (100..899) with the erase and
// subtitle flags, then its rows (double height, boxed, at rows 16 to 22 from the bottom up, or from row 2 for a caption
// at the top; lines over kTeletextRowChars wrap, at most four), at its end an erasing header alone; every field ends
// with a dummy header (page 8FF in the page's magazine, as OP-47 asks) that also ends the page before it. At most five
// packets a field: more wait for the next. Characters outside the English set are folded to it (é to e).
std::vector<std::vector<TeletextPacket>> teletextSubtitleFields(const std::vector<Caption>& captions, Rational fps, int64_t frames,
                                                                int page = 888, bool interlaced = false);

// The user data words of an OP-47 subtitling distribution packet carrying up to five packets (more are refused): the
// identifier, length, format, line descriptors (SD lines from 21 back, in the field `field1` or the second), the 45
// bytes of each packet with its run-in and framing code, the footer with `sequence`, and the checksum.
std::vector<uint8_t> op47Sdp(const std::vector<TeletextPacket>& packets, uint16_t sequence, bool field1 = true);
// The packets in an OP-47 subtitling distribution packet's user data words; false if it is not one (or its checksum
// is wrong).
bool parseOp47Sdp(const uint8_t* data, size_t size, std::vector<TeletextPacket>& out);

// The subtitles on `page` from the packets each field carried (two fields a frame with `interlaced`): a caption for
// each time the page shows different boxed text, from the frame its header came in, timed in frames at `fps`; placed
// at the top when it starts in the first five rows, at the bottom when it reaches row 20, else in the middle. False
// if the page never came.
bool teletextSubtitles(const std::vector<std::vector<TeletextPacket>>& fields, Rational fps, int page, std::vector<Caption>& out,
                       bool interlaced = false, std::string* error = nullptr);

// Text as the English teletext character set has it, and back (the set's £, #, ½, ¼, ¾, ÷, arrows and dash included).
std::string teletextText(const std::string& utf8);
std::string teletextToUtf8(const std::string& teletext);

}  // namespace montage
