// Montage — Digital Cinema track files: the MXF files a DCP's picture and sound live in (SMPTE ST 429-4 JPEG 2000
// picture, ST 429-3 sound with ST 377-4 multichannel labels), as cinema servers and DCP checkers expect them.
//
// Each is an OP-Atom file: a closed, complete header partition holding the metadata (padded to 16 KiB, rewritten
// with the final duration when the file is closed), one body partition of frame-wrapped essence (one JPEG 2000
// codestream, or one picture frame's worth of 24-bit samples, per KLV), and a footer partition with the index table,
// then a random index pack. The layout and every label match what CineCert's asdcplib (the reference implementation
// every DCP tool and server is checked against) writes, so its tools read these files back.
//
// IMF track files (SMPTE ST 2067-5, the AS-02 layout) are OP1a: one track in each package and no timecode track,
// JPEG 2000 frame-wrapped as progressive frames with RGBA and JPEG 2000 sub-descriptors that carry the colour
// (primaries, transfer, coding equations, mastering display) and the component layout, or PCM clip-wrapped in a single
// KLV with ST 2067-2 multichannel labels (a soundfield group with title, version, content and element kinds); the
// index table sits in a partition of its own between the essence and the footer.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace montage::dcp {

using Uuid = std::array<uint8_t, 16>;
Uuid newUuid();                         // random (version 4)
std::string uuidString(const Uuid& u);  // lower case, 8-4-4-4-12
bool parseUuid(const std::string& text, Uuid& out);

// A JPEG 2000 codestream's main header, as the picture sub-descriptor repeats it.
struct J2kHeader {
    uint16_t rsiz = 0, csiz = 0;
    uint32_t xsiz = 0, ysiz = 0, xosiz = 0, yosiz = 0, xtsiz = 0, ytsiz = 0, xtosiz = 0, ytosiz = 0;
    std::vector<std::array<uint8_t, 3>> components;  // Ssiz, XRsiz, YRsiz
    std::vector<uint8_t> cod, qcd;                   // the COD and QCD marker segments' bodies
};
bool parseJ2kHeader(const uint8_t* data, size_t size, J2kHeader& out);

struct EditRate {
    uint32_t num = 24, den = 1;
    double value() const { return den ? double(num) / den : 0; }
};

class TrackFileWriter {
public:
    virtual ~TrackFileWriter();
    int64_t frames() const { return int64_t(offsets_.size()); }
    // The edit units in the file: frames, or for clip-wrapped sound its samples.
    virtual int64_t duration() const { return frames(); }
    // The essence descriptor's instance id (the composition playlist repeats the descriptor).
    const Uuid& descriptorId() const;
    const Uuid& asset() const { return asset_; }
    // Finishes the file: index, footer, random index pack, and the header again with the duration.
    bool close(std::string* error = nullptr);

protected:
    bool openFile(const std::string& path, const Uuid& asset, int fps, std::string* error);
    bool openImfFile(const std::string& path, const Uuid& asset, EditRate rate, std::string* error);
    bool writeRaw(const uint8_t* data, size_t size, std::string* error);
    bool writeElement(const uint8_t key[16], const uint8_t* data, size_t size, std::string* error);
    virtual std::vector<uint8_t> headerMetadata(int64_t duration) const = 0;  // primer and sets
    virtual std::vector<uint8_t> indexSegments() const = 0;
    virtual std::array<uint8_t, 16> essenceContainer() const = 0;
    std::vector<uint8_t> partitionPack(const uint8_t key[16], uint64_t thisPartition, uint64_t previous, uint64_t footer,
                                       uint64_t headerBytes, uint64_t indexBytes, uint32_t indexSid, uint32_t bodySid) const;

    std::FILE* f_ = nullptr;
    std::string path_;
    Uuid asset_{};
    int fps_ = 24;
    EditRate rate_;     // the essence's (IMF: rational; a DCP's is fps_)
    bool imf_ = false;  // the AS-02 layout
    long clipLengthAt_ = -1;   // where a clip-wrapped KLV's length is written (-1: frame wrapped)
    uint64_t clipStart_ = 0;   // the essence bytes before its value
    std::vector<uint64_t> offsets_;  // each frame's KLV, from the start of the essence
    uint64_t essenceBytes_ = 0;
    // Identities fixed when the file is opened, so the header written at the end matches the first one.
    std::array<Uuid, 48> ids_{};
    std::string created_;  // timestamp bytes
};

// SMPTE ST 429-4: one JPEG 2000 codestream per frame (2K or 4K DCI profile, XYZ 12-bit).
class PictureMxfWriter : public TrackFileWriter {
public:
    bool open(const std::string& path, const Uuid& asset, int fps, std::string* error = nullptr) { return openFile(path, asset, fps, error); }
    bool write(const uint8_t* codestream, size_t size, std::string* error = nullptr);

protected:
    std::vector<uint8_t> headerMetadata(int64_t duration) const override;
    std::vector<uint8_t> indexSegments() const override;
    std::array<uint8_t, 16> essenceContainer() const override;

private:
    J2kHeader j2k_;
    bool haveHeader_ = false;
};

// SMPTE ST 429-3: 24-bit PCM at 48 kHz, one picture frame of samples per KLV, with multichannel audio labels for
// 5.1 (L R C LFE Ls Rs) or 7.1 DS (L R C LFE Lss Rss Lrs Rrs).
class SoundMxfWriter : public TrackFileWriter {
public:
    bool open(const std::string& path, const Uuid& asset, int fps, int channels, const std::string& language,
              std::string* error = nullptr);
    // Exactly samplesPerFrame() interleaved sample frames of `channels()` channels, -1..1.
    bool write(const float* samples, std::string* error = nullptr);
    int channels() const { return channels_; }
    int samplesPerFrame() const { return 48000 / fps_; }

protected:
    std::vector<uint8_t> headerMetadata(int64_t duration) const override;
    std::vector<uint8_t> indexSegments() const override;
    std::array<uint8_t, 16> essenceContainer() const override;

private:
    int channels_ = 6;
    std::string language_ = "en";
    std::vector<uint8_t> buf_;
};

// ---- IMF ----------------------------------------------------------------------------------------------------------

// The picture's colour as the descriptor states it (SMPTE labels as hex, 32 digits).
struct ImfColour {
    std::string primaries, transfer;
    std::string codingEquations;  // empty: none (P3-D65)
    bool hdr = false;             // then the mastering display below is written
    double display[8] = {};       // red, green, blue and white x, y
    double maxLuminance = 0, minLuminance = 0;  // cd/m^2
};

// SMPTE ST 2067-5 / ST 422: one JPEG 2000 codestream per frame, full-range RGB of `bits` bits.
class ImfPictureWriter : public TrackFileWriter {
public:
    bool open(const std::string& path, const Uuid& asset, EditRate rate, int bits, const ImfColour& colour, uint32_t aspectNum,
              uint32_t aspectDen, std::string* error = nullptr);
    bool write(const uint8_t* codestream, size_t size, std::string* error = nullptr);
    // What the descriptor says, for the composition playlist's copy of it.
    const J2kHeader& j2k() const { return j2k_; }
    const Uuid& subDescriptorId() const;
    int bits() const { return bits_; }
    const ImfColour& colour() const { return colour_; }
    uint32_t aspectNum() const { return aspectNum_; }
    uint32_t aspectDen() const { return aspectDen_; }
    EditRate rate() const { return rate_; }

protected:
    std::vector<uint8_t> headerMetadata(int64_t duration) const override;
    std::vector<uint8_t> indexSegments() const override;
    std::array<uint8_t, 16> essenceContainer() const override;

private:
    J2kHeader j2k_;
    bool haveHeader_ = false;
    int bits_ = 12;
    ImfColour colour_;
    uint32_t aspectNum_ = 16, aspectDen_ = 9;
};

// The labels an IMF sound layout carries.
struct McaLabel {
    const char* symbol;
    const char* name;
    const char* dictionary;  // hex
};
McaLabel imfSoundfield(int channels);  // stereo (sgST), 5.1 (sg51) or 7.1 DS (sg71)
std::vector<McaLabel> imfChannels(int channels);

// SMPTE ST 382 clip-wrapped 24-bit PCM at 48 kHz with ST 2067-2 multichannel labels.
class ImfSoundWriter : public TrackFileWriter {
public:
    bool open(const std::string& path, const Uuid& asset, int channels, const std::string& language, const std::string& title,
              const std::string& titleVersion, std::string* error = nullptr);
    // `frames` interleaved sample frames of channels() channels, -1..1.
    bool write(const float* samples, size_t frames, std::string* error = nullptr);
    int channels() const { return channels_; }
    int64_t duration() const override { return samples_; }
    // The label sub-descriptors' identities: the soundfield group's instance and link, each channel's instance and link.
    const Uuid& soundfieldId() const;
    const Uuid& soundfieldLink() const;
    const Uuid& channelId(int c) const;
    const Uuid& channelLink(int c) const;
    const std::string& language() const { return language_; }
    const std::string& title() const { return title_; }
    const std::string& titleVersion() const { return version_; }

protected:
    std::vector<uint8_t> headerMetadata(int64_t duration) const override;
    std::vector<uint8_t> indexSegments() const override;
    std::array<uint8_t, 16> essenceContainer() const override;

private:
    int channels_ = 6;
    int64_t samples_ = 0;
    std::string language_ = "en", title_, version_;
    std::vector<uint8_t> buf_;
};

}  // namespace montage::dcp
