// Montage — Digital Cinema Packages (DaVinci Resolve Studio's DCP export; Premiere, Final Cut and Avid send films to
// DCP-o-matic or easyDCP): a sequence as the SMPTE DCP a cinema server plays and a festival asks for.
//
// The picture is rendered at 2K or 4K in the chosen container (Flat 1998 x 1080, Scope 2048 x 858 or the full
// 2048 x 1080, each twice the size at 4K), both eyes of a stereoscopic sequence for 3D (ST 429-10), the sequence's
// frame fitted inside it on black; turned from the sequence's colour space into DCI X'Y'Z' (CIE XYZ of
// the display light with its white kept, scaled for 48 cd/m^2 at 52.37 and encoded with a 2.6 power, 12 bits); and
// compressed to JPEG 2000 codestreams in the DCI 2K or 4K profile (OpenJPEG's cinema mode, which keeps every frame under
// the 250 Mbit/s limit), several frames at a time. The sound is the sequence's mix at 48 kHz in 24 bits: 5.1, or
// 7.1 DS from a 7.1 sequence, a stereo mix on the left and right channels. Both go into SMPTE track files
// (render/DcpMxf.h), described by a Composition Playlist (ST 429-7, with the ST 429-16 metadata the SMPTE Bv2.1
// profile asks for), a Packing List with each file's SHA-1 (ST 429-8), an Asset Map and a Volume Index (ST 429-9), in
// a folder named by the Digital Cinema Naming Convention. 23.976 and 29.97 sequences play at 24 and 30, the sound
// following, as cinema servers need whole frame rates; 50 and 59.94/60 ones at 25 and 30 from every other frame.
//
// A caption track can go with it as subtitles shown on screen or closed captions for viewers' own devices: ST 428-7
// XML with an embedded font in an ST 429-5 track file (render/DcpMxf.h), or burned into the picture. The package can
// be encrypted (ST 429-6) for a release: each track file under a content key of its own, the composition and packing
// list signed by Montage's certificate chain, and the keys kept in a DKDM (a KDM to Montage itself) from which the KDMs
// for cinemas' servers are made (render/DcpSecurity.h). The Bv2.1 rules for subtitles and codestreams are kept, and
// verifyDcp checks them as libdcp's and ClairMeta's checks do.
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
    std::string resolution = "2k";    // "2k", or "4k" (twice the size each way: 3996 x 2160, 4096 x 1716, 4096 x 2160)
    std::string language = "en";      // the audio's (RFC 5646)
    std::string territory = "XX";     // where it will be shown (ISO 3166; XX: international)
    std::string issuer = "Montage";   // who made the package
    std::string studio, facility;     // short codes for the name (ISDCF: e.g. "DI", "MTG"); left out when empty
    int fps = 0;                      // 24, 25, 30 or 48; 0: the sequence's, to the nearest
    bool inOut = false;               // only In to Out
    int threads = 0;                  // JPEG 2000 encoders at once (0: one per processor core)
    bool stereo = false;              // stereoscopic 3D: both eyes of a stereoscopic sequence (2K only)
    Id subtitles = 0;                 // a caption track's id: its captions go with the picture (0: none)
    bool closedCaptions = false;      // as closed captions (for viewers' own devices) instead of subtitles on screen
    bool burnIn = false;              // the subtitles drawn into the picture instead of a track of their own
    std::string subtitleLanguage;     // RFC 5646; empty: the caption track's
    bool encrypt = false;             // encrypted track files that KDMs open (needs a build with OpenSSL)
    bool sign = true;                 // the composition and packing list signed (when the build has OpenSSL)
    std::string certificates;         // the signer's chain folder; empty: Montage's own (dcp::signerFolder)
    std::string dkdms;                // where an encrypted package's DKDM goes; empty: Montage's (dcp::dkdmFolder)
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
    bool stereo = false, encrypted = false, signedPackage = false;
    int subtitleCount = 0;      // subtitles (or closed captions) in the track, burned in or not
    std::string cplId;          // the composition's UUID (what KDMs name)
    std::string dkdm;           // an encrypted package's DKDM file
};

// The container sizes (at 2K or 4K), the one that suits a sequence's shape (Scope from 2:1, else Flat), and the DCP
// rate for it.
bool dcpContainer(const std::string& container, int& width, int& height, bool fourK = false);
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
// `progress` (0..1) may return false to stop (the list then ends with "Stopped"). Also: signatures, encrypted track
// files against the keys the composition names, every codestream against the Bv2.1 rules (checkDciCodestream), 3D
// pictures' two eyes, subtitles read back (their own id, the font, the language, closed captions' 3 lines of 32
// characters). `advice` gets what Bv2.1 recommends but servers play anyway (subtitles from 4 seconds in, at least 15
// frames each and 2 apart, lines of 52 characters at most).
std::vector<std::string> verifyDcp(const std::string& folder, const std::function<bool(double)>& progress = {},
                                   std::vector<std::string>* advice = nullptr);

// Frame `index` of a DCP picture track file decoded to 12-bit X'Y'Z' (three per pixel), for checks and tests: the
// right eye's with `eye` 1, decrypted with `key` (16 bytes) when the file is encrypted.
bool readDcpFrame(const std::string& mxf, int index, std::vector<uint16_t>& xyz, int& width, int& height, std::string* error = nullptr,
                  const uint8_t* key = nullptr, int eye = 0);

// The Bv2.1 rules for a DCI codestream (libdcp's checks): the 2K or 4K profile, one tile, 12-bit X'Y'Z', CPRL with
// one layer, 32 x 32 code-blocks and the profile's precincts, guard bits (1 at 2K, 2 at 4K), the tile parts, a TLM
// marker, and 4K's one POC marker. Empty when it keeps them.
std::vector<std::string> checkDciCodestream(const uint8_t* data, size_t size);

// KDMs for an encrypted DCP, made from its DKDM: one for each cinema server certificate (PEM text, or a file of it),
// the keys valid from `notBefore` to `notAfter` (seconds since the epoch), written into `folder`.
struct KdmRequest {
    std::string dkdm;                     // the DKDM file (exportDcp's DcpResult::dkdm)
    std::vector<std::string> recipients;  // the servers' leaf certificates
    int64_t notBefore = 0, notAfter = 0;
    int offsetMinutes = 0;                // the time zone the window is written in
    std::string folder;
    std::string annotation;
    std::string certificates;             // the signer's chain folder (as for exportDcp)
};
bool makeKdms(const KdmRequest& request, std::vector<std::string>* files = nullptr, std::string* error = nullptr);

}  // namespace montage
