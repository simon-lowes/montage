// Montage — broadcast MXF by the book, through the EBU's bmx library (BSD): an MXF OP1a file FFmpeg wrote (XDCAM
// HD422 MPEG-2 or AVC-Intra, mono PCM tracks) rewrapped as broadcasters' ingest checks it:
//  - RDD 9, the MPEG-2 long-GOP layout XDCAM HD422 decks and servers write (content packages, system items, the index
//    every edit unit);
//  - AMWA AS-11 (UK DPP HD, X2 AVC-Intra, X8 MPEG-2 HD), with the core and UK DPP descriptive metadata frameworks and
//    the programme's parts as AS-11 segmentation;
//  - an ST 436 ancillary data track: CEA-708 captions (SMPTE 334 caption distribution packets, with the 608 bytes) and
//    OP-47 teletext subtitles (subtitling distribution packets), frame by frame.
// And read back: the frameworks, parts, essence and ancillary packets, for checks and tests.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../core/Captions.h"
#include "../core/Model.h"

namespace montage {

// One ancillary data packet: the line it goes on (SMPTE 274 numbering: 9 for captions, 12 and 575 for teletext in
// 1080i) and its DID, SDID and user data words, 8 bits each.
struct AncPacket {
    uint16_t line = 9;
    uint8_t did = 0, sdid = 0;
    std::vector<uint8_t> data;
    bool operator==(const AncPacket&) const = default;
};

// A programme part (AS-11 segmentation): from `start` (frames from the file's first) for `duration` frames.
struct MxfPart {
    int64_t start = 0, duration = 0;
    int number = 1, total = 1;
    bool operator==(const MxfPart&) const = default;
};

// How a file is rewrapped: `shim` names the delivery.
//  "rdd9": RDD 9 (MPEG-2 long GOP only), no AS-11 metadata;
//  "as11-dpp": AS-11 UK DPP HD (AVC-Intra 100 1080i25 in OP1a);
//  "as11-x2": AS-11 X2 (AVC-Intra HD in OP1a); "as11-x8": AS-11 X8 (MPEG-2 HD in RDD 9, the NABA's).
struct BroadcastMxfOptions {
    std::string shim = "rdd9";
    // Framework properties by the names AS-11 gives them (without the AS11 / UKDPP prefixes): core ones
    // (SeriesTitle, ProgrammeTitle, EpisodeTitleNumber, ShimName, ShimVersion, AudioTrackLayout, PrimaryAudioLanguage,
    // ClosedCaptionsPresent, ClosedCaptionsType, ClosedCaptionsLanguage) and UK DPP ones (ProductionNumber, Synopsis,
    // Originator, CopyrightYear, ... ContactEmail, ContactTelephoneNumber). Values as text: numbers, true / false,
    // timecodes ("10:00:00:00") for positions and durations, dates as 2024-05-31. Ones not given get defaults where
    // there are any (the shim's name and version, the clip's name as the titles, the DPP's flags); the DPP's
    // production number, synopsis, originator and contacts have none (missingAs11 names them).
    std::map<std::string, std::string> core, dpp;
    std::vector<MxfPart> parts;   // empty: one part, the whole programme
    std::string startTimecode;    // "10:00:00:00"; "" = the source file's
    std::string clipName;
    // Each frame's ancillary packets (frame 0 first); empty = no ST 436 track.
    std::vector<std::vector<AncPacket>> anc;
};

// The required properties `o` lacks (empty ones count as missing): the DPP's production number, synopsis,
// originator, contact e-mail and telephone for its shim (or when any DPP property is given).
std::vector<std::string> missingAs11(const BroadcastMxfOptions& o);

// Whether this build has bmx.
bool broadcastMxfAvailable();

// The UK DPP framework's properties, the core ones and the names AS-11 shims take ("as11-dpp" ...), for dialogs and
// the automation interface.
const std::vector<std::string>& as11CoreProperties();
const std::vector<std::string>& ukDppProperties();

// `in` (an MXF OP1a file of one picture track, MPEG-2 422P@HL long GOP or AVC-Intra, and mono or multichannel PCM)
// rewrapped into `out` as `o` says. False with `error` (and no file left at `out`) for a picture bmx cannot carry in
// the shim, metadata AS-11 refuses, or parts past the end.
bool rewrapBroadcastMxf(const std::string& in, const std::string& out, const BroadcastMxfOptions& o, std::string* error = nullptr);

struct BroadcastMxfInfo {
    std::string pictureEssence;      // bmx's name for it: "AVCI_100_1080i", "MPEG_2_Long_GOP_422P_HL_1080i"...
    bool separateFields = false;     // the picture is interlaced (frame layout separate fields)
    int soundTracks = 0;
    int64_t duration = 0;
    Rational editRate;
    std::string startTimecode;
    std::string as11Spec;            // "as11-x2" ... when the file names its AS-11 specification
    std::map<std::string, std::string> core, dpp;  // the frameworks' properties, as text
    std::vector<MxfPart> parts;
    std::vector<std::vector<AncPacket>> anc;       // each frame's ancillary packets
};
// A broadcast MXF file read with bmx: false with `error` if it does not open.
bool readBroadcastMxf(const std::string& path, BroadcastMxfInfo& info, std::string* error = nullptr);

// The captions in a file's ancillary data: from its caption distribution packets (CEA-708 service 1, else 608) and
// the teletext subtitles on `page` in its OP-47 packets, timed in its frames.
void ancCaptions(const BroadcastMxfInfo& info, int page, std::vector<Caption>& cea, std::vector<Caption>& teletext);

}  // namespace montage
