// Montage — Digital Cinema Packages (DaVinci Resolve Studio's DCP export; Premiere, Final Cut and Avid send films to
// DCP-o-matic or easyDCP): a sequence as the SMPTE DCP a cinema server plays and a festival asks for.
//
// The picture is rendered at 2K in the chosen container (Flat 1998 x 1080, Scope 2048 x 858 or the full 2048 x 1080),
// the sequence's frame fitted inside it on black; turned from the sequence's colour space into DCI X'Y'Z' (CIE XYZ of
// the display light with its white kept, scaled for 48 cd/m^2 at 52.37 and encoded with a 2.6 power, 12 bits); and
// compressed to JPEG 2000 codestreams in the DCI 2K profile (OpenJPEG's cinema mode, which keeps every frame under
// the 250 Mbit/s limit), several frames at a time. The sound is the sequence's mix at 48 kHz in 24 bits: 5.1, or
// 7.1 DS from a 7.1 sequence, a stereo mix on the left and right channels. Both go into SMPTE track files
// (render/DcpMxf.h), described by a Composition Playlist (ST 429-7, with the ST 429-16 metadata the SMPTE Bv2.1
// profile asks for), a Packing List with each file's SHA-1 (ST 429-8), an Asset Map and a Volume Index (ST 429-9), in
// a folder named by the Digital Cinema Naming Convention. 23.976 and 29.97 sequences play at 24 and 30, the sound
// following, as cinema servers need whole frame rates; 50 and 59.94/60 ones at 25 and 30 from every other frame.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

struct DcpSettings {
    std::string title;                // what the film is called (the folder and the CPL take their name from it)
    std::string kind = "feature";     // feature, short, trailer, teaser, advertisement, test, transitional, rating, psa
    std::string container = "flat";   // "flat" (1998 x 1080), "scope" (2048 x 858), "full" (2048 x 1080)
    std::string language = "en";      // the audio's (RFC 5646)
    std::string territory = "XX";     // where it will be shown (ISO 3166; XX: international)
    std::string issuer = "Montage";   // who made the package
    std::string studio, facility;     // short codes for the name (ISDCF: e.g. "DI", "MTG"); left out when empty
    int fps = 0;                      // 24, 25, 30 or 48; 0: the sequence's, to the nearest
    bool inOut = false;               // only In to Out
    int threads = 0;                  // JPEG 2000 encoders at once (0: one per processor core)
};

struct DcpResult {
    std::string folder;        // the DCP
    std::string name;          // its Digital Cinema Naming Convention name (the folder's and the CPL title)
    std::string cpl;           // the Composition Playlist's file
    int64_t frames = 0;
    int fps = 24;
    int width = 0, height = 0;  // the picture container
    int channels = 6;
    bool cinemaProfile = true;  // encoded in the DCI profile (false: FFmpeg without OpenJPEG; most servers will refuse it)
};

// The container sizes, the one that suits a sequence's shape (Scope from 2:1, else Flat), and the DCP rate for it.
bool dcpContainer(const std::string& container, int& width, int& height);
std::string defaultDcpContainer(const Sequence& s);
int dcpFrameRate(const Sequence& s, int requested);
// How many sequence frames make a DCP frame at `rate`: 2 for 50 and 60 (59.94) at 25 and 30, else 1.
int dcpFrameStep(const Sequence& s, int rate);
// "Title_FTR_F_EN-XX_XX_51_2K_DI_20261009_MTG_SMPTE_OV": title in camel case (14 characters at most), content kind,
// aspect, language and subtitles, territory, audio, resolution, studio, date, facility, standard, package type (the
// ISDCF Digital Cinema Naming Convention; studio and facility only when given).
std::string dcpName(const DcpSettings& settings, int channels, const std::string& date);

// Writes the DCP into a new folder named dcpName() under `parent`. `progress` (0..1) may return false to stop (the
// half-written folder is removed).
bool exportDcp(const Project& p, const Sequence& s, const DcpSettings& settings, const std::string& parent, DcpResult* result = nullptr,
               const std::function<bool(double)>& progress = {}, std::string* error = nullptr);

// Checks a DCP as a server or festival would: the asset map and volume index, every file present at its size, the
// packing list's hashes, each composition's assets in the packing list with durations within them, the picture
// track files readable as JPEG 2000 X'Y'Z' at a DCI size with as many frames as the composition says and within
// 250 Mbit/s, the sound 24-bit 48 kHz. An empty list means it passed.
// `progress` (0..1) may return false to stop (the list then ends with "Stopped").
std::vector<std::string> verifyDcp(const std::string& folder, const std::function<bool(double)>& progress = {});

// Frame `index` of a DCP picture track file decoded to 12-bit X'Y'Z' (three per pixel), for checks and tests.
bool readDcpFrame(const std::string& mxf, int index, std::vector<uint16_t>& xyz, int& width, int& height, std::string* error = nullptr);

}  // namespace montage
