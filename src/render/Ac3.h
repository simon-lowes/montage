// Montage — AC-3 (ATSC A/52) and E-AC-3 (ETSI TS 102 366 Annex E) sound through FFmpeg's own encoders, with the
// metadata a broadcast chain reads: the dialogue level (dialnorm) from Montage's loudness measurement, and the stereo
// downmix levels and production information (mixing level, room type) from the sequence.
//
// FFmpeg's E-AC-3 encoder stops at 5.1. Above it, a programme is carried as Annex E describes (E.2.8.2): independent
// substream 0 holds a 5.1 version (the side and back surrounds folded into the surrounds) for 5.1 decoders, and a
// dependent substream holds the four surrounds, which replace and extend them (its channel map: Ls, Rs, Lrs/Rrs). The
// dependent substream comes from a second FFmpeg encoder whose frames are rewritten into the dependent syntax
// (eac3MakeDependent): the fields only independent substreams carry are taken out and the channel map put in, which
// means finding where each audio block starts, so each block's exponents are decoded and its bit allocation worked
// out (A/52 clause 7.2) to know how long its mantissas are.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVDictionary;
struct AVFrame;
struct AVPacket;

namespace montage {

struct Ac3Metadata {
    int dialnorm = -24;                      // dB, -31 to -1: the programme's loudness (LKFS), rounded
    double centreMixDb = -3, surroundMixDb = -3;  // the stereo downmix's levels for the centre and the surrounds
    std::string preferredDownmix;            // "" (not stated), "loro" or "ltrt"
    int mixingLevel = 0;                     // the mixing room's peak level, dB SPL, 80 to 111 (0: not stated)
    std::string roomType;                    // "" (not stated), "large" or "small" (stated with a mixing level only)
};

// The dialnorm for a loudness in LKFS (rounded, held to -31..-1).
int ac3Dialnorm(double lkfs);

// A DASH AudioChannelConfiguration element for AC-3 or E-AC-3 with this many channels (1, 2, 6 or 8; L R C LFE Ls Rs
// [Lrs Rrs]): DASH-IF and DVB-DASH ask for the scheme ETSI TS 102 366 defines, its 16-bit channel mask in hex
// ("F801" for 5.1), not MPEG's CICP numbers.
std::string ac3DashChannelConfiguration(int channels);

// Options for FFmpeg's "ac3" or "eac3" encoder carrying this metadata (set into `opts` for avcodec_open2), with
// per-frame metadata on so the dialogue level can be set after the encoder opens (ac3SetDialnorm). `channels` and
// `centre` / `surround` say which mix levels the layout has.
void ac3EncoderOptions(const Ac3Metadata& m, bool eac3, bool centre, bool surround, AVDictionary** opts);
// Sets the dialogue level of an open ac3 or eac3 encoder (from the next frame).
void ac3SetDialnorm(AVCodecContext* encoder, int dialnorm);

// What a syncframe's header says: one entry per substream in `data` (an AC-3 frame, or an E-AC-3 access unit: an
// independent substream and the dependent ones after it).
struct Ac3FrameInfo {
    bool eac3 = false;
    int bsid = 0, acmod = 0, sampleRate = 0, bytes = 0;
    bool lfe = false;
    int streamType = 0, substream = 0;  // E-AC-3: 0 independent, 1 dependent
    int chanmap = -1;                   // a dependent substream's custom channel map (MSB = Left), -1 none
    int dialnorm = 0;                   // dB (-31 to -1)
    // The downmix levels as stated, in dB (-99: none; NaN: not stated). AC-3: cmixlev and surmixlev (or with the
    // alternate syntax, its Lo/Ro levels); E-AC-3: the Lo/Ro levels. And the Lt/Rt ones where stated.
    double centreMixDb, surroundMixDb, ltrtCentreMixDb, ltrtSurroundMixDb;
    int preferredDownmix = -1;          // dmixmod: 0 not indicated, 1 Lt/Rt, 2 Lo/Ro (-1 not stated)
    int mixingLevel = -1, roomType = -1;  // dB SPL (80-111) and roomtyp (0, 1 large, 2 small), -1 not stated
    bool crcOk = false;                 // the frame's last CRC checks
    Ac3FrameInfo();
};
std::vector<Ac3FrameInfo> parseAc3Frames(const uint8_t* data, size_t size);

// Rewrites an E-AC-3 syncframe that FFmpeg's encoder coded as independent substream 0 (six blocks, no coupling,
// spectral extension or adaptive hybrid transform, the encoder's other choices off) as a dependent substream with
// this custom channel map, its CRC made again. False with the reason when the frame is not one it can rewrite.
bool eac3MakeDependent(std::vector<uint8_t>& frame, uint16_t chanmap, std::string* error = nullptr);

// E-AC-3 above 5.1: 7.1 (FFmpeg's channel order: L R C LFE Lb Rb Ls Rs) as an independent 5.1 substream and a
// dependent one with the four surrounds, from two of FFmpeg's encoders, each access unit both substreams.
class Eac3Extended {
public:
    Eac3Extended() = default;
    Eac3Extended(const Eac3Extended&) = delete;
    Eac3Extended& operator=(const Eac3Extended&) = delete;
    ~Eac3Extended();
    // `bitrate` in all (the independent substream takes five eighths, as its 5.1 is most of the programme).
    bool open(int sampleRate, int64_t bitrate, const Ac3Metadata& m, std::string* error = nullptr);
    // Planar float 7.1 frames of 1536 samples (the last may be shorter); nullptr flushes.
    int send(const AVFrame* frame);
    // As avcodec_receive_packet: AVERROR(EAGAIN) until both substreams of a frame are coded, AVERROR_EOF once flushed.
    int receive(AVPacket* pkt);
    void setDialnorm(int dialnorm);
    // A context describing the stream (eac3, 7.1, the sample rate and format, the frame size, bit rate and
    // priming) for the muxer's parameters; not opened. Owned by the caller once taken.
    AVCodecContext* describe() const;
    int frameSize() const { return 1536; }
    std::string error;  // why the last send or receive failed

private:
    AVCodecContext* main_ = nullptr;  // 5.1 (side): L R C LFE, the surrounds with the backs folded in
    AVCodecContext* dep_ = nullptr;   // quad (side): Ls Rs Lrs Rrs
    AVFrame* a_ = nullptr;
    AVFrame* b_ = nullptr;
    std::deque<AVPacket*> mainOut_, depOut_;
    bool flushed_ = false;
    int64_t bitrate_ = 0;
};

}  // namespace montage
