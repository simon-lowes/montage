// Montage — immersive sound as an Immersive Audio Bitstream (SMPTE ST 2098-2), the open format cinemas play immersive
// sound from (a DCP's IAB track, ST 429-18, under the ISDCF's Application Profile 1, RDD 57) and IMF packages carry it
// in (ST 2067-201).
//
// Each frame of the bitstream holds a picture frame's worth of sound: a bed of speaker channels and the objects, each
// object at its place in the room for every few milliseconds (the pan sub-blocks), and each channel's samples as
// 24-bit PCM (IMF) or coded without loss (DLC: linear prediction and Rice codes, the decoder's integer maths in the
// standard's Annex B, which DCPs must use). The mix is the one an ADM master carries (render/Adm.h ImmersiveMix):
// the sequence's speakers become the bed channels a cinema has (left, centre, right, the side and rear surrounds, the
// top surrounds and LFE: 7.1 DS and 9.1 OH), a 5.1 surround split between the side and rear surround of its side at
// -3 dB, as a 7.1 cinema plays it, and any other speaker (those above at the front or the back) an object fixed at its
// place; each object track an object that moves as its panner does. The room is a unit cube: X from the left wall (0)
// to the right (1), Y from the screen (0) to the back wall (1), Z from the ear (0) to the ceiling (1).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/Model.h"

namespace montage::iab {

// IAFrame rate codes (ST 2098-2 Table 18): 24, 25, 30, 48, 50, 60, 96, 100, 120, 23.976, 29.97, 59.94.
int rateCode(uint32_t num, uint32_t den);  // -1: IAB has no such rate
void rateOf(int code, uint32_t& num, uint32_t& den);
// Samples at 48 kHz in a frame (`frameIndex`, a FrameSequence's, changes none of the rates ST 2098-2:2022 defines;
// 29.97 and 59.94 are reserved there).
int frameSamples(int code, int frameIndex = 0);
bool sequenced(int code);  // none of the defined rates carries a FrameSequence
int panSubBlocks(int code);
// Where pan sub-block `sb` starts, in samples into its frame.
int panSubBlockStart(int code, int frameIndex, int sb);
bool dlcAllowed(int code);  // DLC is defined for the whole-number rates only

// ChannelIDs (Table 20) of the cinema bed.
enum Channel : int { L = 0x0, C = 0x2, R = 0x4, Lss = 0x5, Ls = 0x6, Lrs = 0x7, Rrs = 0x8, Rss = 0x9, Rs = 0xA, Lts = 0xB, Rts = 0xC, LFE = 0xD };
const char* channelName(int id);  // "L", "C", ... ("" for others)

struct Point {
    double x = 0.5, y = 0, z = 0;
};
// From ADM's cartesian coordinates (X -1 left to 1 right, Y -1 behind to 1 in front, Z 0 to 1 overhead).
Point roomPoint(double admX, double admY, double admZ);

struct BedChannel {
    int id = L;
    uint32_t audio = 0;  // AudioDataID (0: silent)
};
struct Object {
    uint32_t metaId = 1;
    uint32_t audio = 0;
    std::vector<Point> pan;  // one for each pan sub-block
};
// One frame: the bed, the objects and each waveform's 24-bit samples by AudioDataID.
struct Frame {
    int rate = 0;        // rate code
    int frameIndex = 0;  // the FrameSequence's (29.97, 59.94)
    int maxRendered = 0;  // 0: the bed's channels and the objects
    bool hasBed = true;
    uint32_t bedMetaId = 0;
    std::vector<BedChannel> bed;
    std::vector<Object> objects;
    std::map<uint32_t, std::vector<int32_t>> audio;
    bool dlc = false;  // as read: some of its waveforms were DLC
    bool pcm = false;  // ... and some PCM
};

// One IABitstream frame (an empty preamble, then the IAFrame): FrameSequence (when sequenced), the waveforms, the bed,
// the objects; each waveform as PCM or DLC. False when the frame does not fit the format (a waveform of the wrong
// length, DLC at a fractional rate).
bool writeFrame(const Frame& f, bool dlc, std::vector<uint8_t>& out, std::string* error = nullptr);
// Reads one IABitstream frame, decoding PCM and DLC waveforms; `used` gets its bytes. Elements Montage does not use
// (bed remaps, zone definitions, nested beds and objects, authoring tool and user data) are skipped, and noted in
// `skipped` (their ElementIDs) when given.
bool readFrame(const uint8_t* data, size_t size, Frame& f, std::string* error = nullptr, size_t* used = nullptr,
               std::vector<uint32_t>* skipped = nullptr);

// DLC on its own: a frame's waveform (`count` = frameSamples(code) samples, 24-bit) coded from DLCSampleRate to the
// alignment (what follows DLCSize), and decoded back.
std::vector<uint8_t> encodeDlc(const int32_t* samples, int count, int rateCode);
bool decodeDlc(const uint8_t* data, size_t size, int rateCode, int count, std::vector<int32_t>& out, std::string* error = nullptr);

// How a sequence's speakers sit in an IAB bed: each bed channel with the sequence channels it takes (and their
// gains), and the speakers that become fixed objects.
struct BedMap {
    struct Feed {
        int channel = 0;
        float gain = 1;
    };
    struct BedOut {
        int id = 0;
        std::vector<Feed> from;
    };
    std::vector<BedOut> bed;  // by ChannelID
    struct FixedObject {
        int channel = 0;
        Point at;
    };
    std::vector<FixedObject> objects;
};
BedMap bedMap(const std::string& layout);

// A sequence's immersive mix as IAB frames, one at a time.
struct ProgramSettings {
    uint32_t rateNum = 24, rateDen = 1;  // the frames' rate (the composition's)
    int mixRate = 48000;     // samples a second along the sequence's timeline (render/Adm.h ImmersiveMix::open)
    int64_t firstSample = 0;  // where the first frame starts (at mixRate, from the sequence's start)
    int64_t frames = 0;
    int64_t heard = -1;      // samples heard, then silence (-1: all)
    bool dlc = false;
    bool cinema = false;     // Application Profile 1 (RDD 57): at most 118 objects, 128 rendered
};
class Program {
public:
    Program();
    ~Program();
    bool open(const Project& p, const Sequence& s, const ProgramSettings& settings, std::string* error = nullptr);
    // The next frame's bitstream; false at the end or on an error.
    bool next(std::vector<uint8_t>& frame, std::string* error = nullptr);
    int64_t written() const;
    int bedChannels() const;  // in the IAB bed
    int objects() const;      // fixed speakers and object tracks
    int maxRendered() const { return bedChannels() + objects(); }
    int rateCode() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace montage::iab
