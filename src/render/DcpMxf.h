// Montage — Digital Cinema track files: the MXF files a DCP's picture and sound live in (SMPTE ST 429-4 JPEG 2000
// picture, ST 429-3 sound with ST 377-4 multichannel labels), as cinema servers and DCP checkers expect them.
//
// Each is an OP-Atom file: a closed, complete header partition holding the metadata (padded to 16 KiB, rewritten
// with the final duration when the file is closed), one body partition of frame-wrapped essence (one JPEG 2000
// codestream, or one picture frame's worth of 24-bit samples, per KLV), and a footer partition with the index table,
// then a random index pack. The layout and every label match what CineCert's asdcplib (the reference implementation
// every DCP tool and server is checked against) writes, so its tools read these files back.
//
// Stereoscopic pictures (ST 429-10) put each frame's left and right codestreams one after the other, an edit unit
// to the pair. Subtitles and captions (ST 429-5) are one ST 428-7 XML document in the body with each font or image it
// loads in a generic stream partition of its own. Any of them can be encrypted (ST 429-6): each element becomes an
// encrypted triplet, AES-128-CBC under the track's content key with an HMAC-SHA1 check value (the MIC) under a key
// derived from it, and the header carries the cryptographic context that names the key.
//
// IMF track files (SMPTE ST 2067-5, the AS-02 layout) are OP1a: one track in each package and no timecode track,
// JPEG 2000 frame-wrapped as progressive frames with RGBA and JPEG 2000 sub-descriptors that carry the colour
// (primaries, transfer, coding equations, mastering display) and the component layout, or PCM clip-wrapped in a single
// KLV with ST 2067-2 multichannel labels (a soundfield group with title, version, content and element kinds); the
// index table sits in a partition of its own between the essence and the footer. Archival masters (SMPTE ST 2065-5, for
// ST 2067-50) frame-wrap one whole ACES image container file (ST 2065-4 OpenEXR) a frame, the RGBA descriptor naming ACES
// 2065-1, with an ACES picture sub-descriptor (the output transform it was made for, its mastering display) and the target
// frames (display-referred stills of chosen frames) in generic stream partitions, each described by a sub-descriptor.
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
    uint32_t pcap = 0;                               // the CAP marker's (HTJ2K): the parts it needs, 0 without one
    std::vector<uint16_t> ccap;                      // and their capabilities, one for each bit of pcap
};
bool parseJ2kHeader(const uint8_t* data, size_t size, J2kHeader& out);

struct EditRate {
    uint32_t num = 24, den = 1;
    double value() const { return den ? double(num) / den : 0; }
};

// A track file's content key (AES-128) and its id, as the composition and KDMs name it.
struct TrackEncryption {
    std::array<uint8_t, 16> key{};
    Uuid keyId{};
};

// The MIC key ST 429-6 derives from a content key (the FIPS 186-2 generator's second value).
std::array<uint8_t, 16> micKey(const std::array<uint8_t, 16>& contentKey);

class TrackFileWriter {
public:
    virtual ~TrackFileWriter();
    // Encrypts every essence element with the key (call before opening; DCP track files only).
    void encryptWith(const TrackEncryption& e);
    bool encrypted() const { return encrypted_; }
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
    // One essence element (an encrypted triplet when encrypting); `index`: an edit unit starts with it.
    bool writeElement(const uint8_t key[16], const uint8_t* data, size_t size, std::string* error, bool index = true);
    // A generic stream partition (body stream `sid`) after the essence, for one element of its own.
    bool startStream(uint32_t sid, std::string* error);
    virtual std::vector<uint8_t> headerMetadata(int64_t duration) const = 0;  // primer and sets
    virtual std::vector<uint8_t> indexSegments() const = 0;
    virtual std::array<uint8_t, 16> essenceContainer() const = 0;
    // The essence container the partitions and preface name: the encrypted one's label when encrypting.
    std::array<uint8_t, 16> containerLabel() const;
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
    bool encrypted_ = false;
    TrackEncryption crypto_;
    std::array<uint8_t, 16> micKey_{};
    uint64_t elements_ = 0;  // essence elements written (the MIC's sequence numbers count them)
    std::vector<std::pair<uint32_t, uint64_t>> streams_;  // generic stream partitions: body stream and file offset
};

// SMPTE ST 429-4: one JPEG 2000 codestream per frame (2K or 4K DCI profile, XYZ 12-bit); ST 429-10 when stereoscopic,
// the left eye's then the right eye's to each frame.
class PictureMxfWriter : public TrackFileWriter {
public:
    bool open(const std::string& path, const Uuid& asset, int fps, std::string* error = nullptr) { return openFile(path, asset, fps, error); }
    bool open(const std::string& path, const Uuid& asset, int fps, bool stereo, std::string* error) {
        stereo_ = stereo;
        return openFile(path, asset, fps, error);
    }
    bool write(const uint8_t* codestream, size_t size, std::string* error = nullptr);
    bool writeStereo(const uint8_t* left, size_t leftSize, const uint8_t* right, size_t rightSize, std::string* error = nullptr);
    bool stereo() const { return stereo_; }

protected:
    std::vector<uint8_t> headerMetadata(int64_t duration) const override;
    std::vector<uint8_t> indexSegments() const override;
    std::array<uint8_t, 16> essenceContainer() const override;

private:
    bool header(const uint8_t* codestream, size_t size, std::string* error);
    J2kHeader j2k_;
    bool haveHeader_ = false;
    bool stereo_ = false;
};

// SMPTE ST 429-5: a reel's subtitles or captions, the ST 428-7 XML with the fonts and images it loads.
struct TimedTextResource {
    Uuid id{};                 // as the XML loads it (LoadFont, Image)
    std::string mime;          // "application/x-font-opentype" or "image/png"
    std::vector<uint8_t> data;
};
class TimedTextMxfWriter : public TrackFileWriter {
public:
    bool open(const std::string& path, const Uuid& asset, int fps, std::string* error = nullptr) { return openFile(path, asset, fps, error); }
    // The XML (its Id `resourceId`, a different UUID from the asset's), `duration` edit units long, and its resources.
    bool write(const std::string& xml, const Uuid& resourceId, int64_t duration, const std::string& language,
               const std::vector<TimedTextResource>& resources, std::string* error = nullptr);
    int64_t duration() const override { return duration_; }

protected:
    std::vector<uint8_t> headerMetadata(int64_t duration) const override;
    std::vector<uint8_t> indexSegments() const override;
    std::array<uint8_t, 16> essenceContainer() const override;

private:
    Uuid resourceId_{};
    std::string namespace_, language_;
    int64_t duration_ = 0;
    std::vector<std::pair<Uuid, std::string>> resources_;  // id and type, in stream order
    std::vector<Uuid> resourceSets_;
};

// Reads DCP track files back as cinema servers do: the index, each edit unit's element (the right eye's after the
// left), the generic streams, decrypting with the content key and checking each MIC when given one.
class TrackFileReader {
public:
    bool open(const std::string& path, std::string* error = nullptr);
    int64_t editUnits() const { return int64_t(offsets_.size()); }
    int64_t containerDuration() const { return containerDuration_; }
    EditRate editRate() const { return rate_; }
    bool stereo() const { return stereo_; }
    bool encrypted() const { return encrypted_; }
    bool hmac() const { return hmac_; }
    const Uuid& asset() const { return asset_; }
    const Uuid& keyId() const { return keyId_; }
    bool timedText() const { return timedText_; }
    // The largest edit unit in bytes (both eyes when stereoscopic): what the bit rate limit is checked against.
    uint64_t largestEditUnit() const;
    // Edit unit `index`'s essence (`eye` 1: the right one), decrypted with `key` (16 bytes) when encrypted.
    bool read(int64_t index, int eye, std::vector<uint8_t>& out, const uint8_t* key = nullptr, std::string* error = nullptr);
    // A generic stream's element (timed text resources: streams 10, 11, ... in the descriptor's order).
    bool readStream(uint32_t sid, std::vector<uint8_t>& out, const uint8_t* key = nullptr, std::string* error = nullptr);
    std::vector<uint32_t> streams() const;
    // Timed text: each resource's id and stream, in order.
    const std::vector<std::pair<Uuid, uint32_t>>& resources() const { return resourceStreams_; }

private:
    bool element(uint64_t at, uint64_t sequence, std::vector<uint8_t>& out, const uint8_t* key, std::string* error, uint64_t* next = nullptr);
    std::string path_;
    std::vector<uint64_t> offsets_;
    std::vector<std::pair<uint32_t, uint64_t>> rip_;
    uint64_t bodyStart_ = 0, fileSize_ = 0;
    int64_t containerDuration_ = 0;
    EditRate rate_;
    bool stereo_ = false, encrypted_ = false, hmac_ = false, timedText_ = false;
    Uuid asset_{}, keyId_{}, context_{};
    std::vector<std::pair<Uuid, uint32_t>> resourceStreams_;
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

// What an ACES picture sub-descriptor says: the output transform the pictures were made through (ACESAuthoringInformation)
// and, when `mastering`, the display they were mastered on.
struct AcesPicture {
    std::string authoring;
    bool mastering = false;
    double display[8] = {};                     // red, green, blue and white x, y
    double maxLuminance = 0, minLuminance = 0;  // cd/m^2
};
// A target frame: a display-referred still of one frame of the track file (its index there), as `mediaType` (TIFF), its
// colour as SMPTE labels (hex, 32 digits), its code values' range.
struct AcesTargetFrame {
    std::vector<uint8_t> data;
    std::string mediaType = "image/tiff";
    int64_t index = 0;
    std::string transfer, primaries;
    uint32_t maxRef = 65535, minRef = 0;
    Uuid resource{};  // set when written: a name-based UUID of its bytes
    uint32_t stream = 0;
    Uuid set{};       // its sub-descriptor's instance id
};

// SMPTE ST 2065-5: one ACES image container (ST 2065-4 OpenEXR, media/Exr.h writeAcesContainer) a frame, frame-wrapped,
// for IMF Application #5 (ST 2067-50).
class ImfAcesWriter : public TrackFileWriter {
public:
    bool open(const std::string& path, const Uuid& asset, EditRate rate, int width, int height, bool alpha, const AcesPicture& picture,
              uint32_t aspectNum, uint32_t aspectDen, std::string* error = nullptr);
    bool write(const uint8_t* exr, size_t size, std::string* error = nullptr);
    // The target frames, each in a generic stream partition of its own (streams 10, 11, ...), after the last frame.
    bool writeTargetFrames(std::vector<AcesTargetFrame> targets, std::string* error = nullptr);
    int width() const { return width_; }
    int height() const { return height_; }
    bool alpha() const { return alpha_; }
    const AcesPicture& picture() const { return picture_; }
    uint32_t aspectNum() const { return aspectNum_; }
    uint32_t aspectDen() const { return aspectDen_; }
    EditRate rate() const { return rate_; }
    const Uuid& constraintsId() const { return constraints_; }
    const Uuid& pictureId() const { return pictureSet_; }
    // The target frames written (their data dropped).
    const std::vector<AcesTargetFrame>& targetFrames() const { return targets_; }
    static constexpr const char* kContainer = "060e2b340401010d0d01030102190100";  // MXF-GC frame-wrapped ACES pictures
    static constexpr const char* kPrimaries = "060e2b340401010d0401010103070000";  // ACES (AP0)
    static constexpr const char* kTransfer = "060e2b34040101060401010101060000";   // linear
    // The picture coding label: ACES without and with alpha.
    const char* coding() const { return alpha_ ? "060e2b340401010d0401020203040200" : "060e2b340401010d0401020203040100"; }

protected:
    std::vector<uint8_t> headerMetadata(int64_t duration) const override;
    std::vector<uint8_t> indexSegments() const override;
    std::array<uint8_t, 16> essenceContainer() const override;

private:
    int width_ = 0, height_ = 0;
    bool alpha_ = false;
    AcesPicture picture_;
    uint32_t aspectNum_ = 16, aspectDen_ = 9;
    Uuid constraints_{}, pictureSet_{};
    std::vector<AcesTargetFrame> targets_;
};

// The labels an IMF sound layout carries.
struct McaLabel {
    const char* symbol;
    const char* name;
    const char* dictionary;  // hex
};
McaLabel imfSoundfield(int channels);  // stereo (sgST), 5.1 (sg51) or 7.1 DS (sg71)
std::vector<McaLabel> imfChannels(int channels);

// SMPTE ST 382 clip-wrapped 24-bit PCM at 48 kHz with ST 2067-2 multichannel labels. `contentKind` is the MCA audio
// content kind (ST 377-41): "PRM" the programme (the main or a dubbed language), "ME" music and effects, "DV" described
// video.
class ImfSoundWriter : public TrackFileWriter {
public:
    bool open(const std::string& path, const Uuid& asset, int channels, const std::string& language, const std::string& title,
              const std::string& titleVersion, std::string* error = nullptr, const std::string& contentKind = "PRM");
    const std::string& contentKind() const { return kind_; }
    // `frames` interleaved sample frames of channels() channels, -1..1.
    bool write(const float* samples, size_t frames, std::string* error = nullptr);
    // The same as 24-bit integers.
    bool writePcm(const int32_t* samples, size_t frames, std::string* error = nullptr);
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
    std::string language_ = "en", title_, version_, kind_ = "PRM";
    std::vector<uint8_t> buf_;
};

// SMPTE ST 2067-2 timed text: one IMSC 1.1 document (the whole composition's, times from its start) clip-wrapped as
// ST 429-5 has it, in the AS-02 layout, its descriptor naming the IMSC 1.1 Text or Image profile and the RFC 5646
// language; an Image document's PNGs follow in generic stream partitions, each named by its resource id.
class ImfTimedTextWriter : public TrackFileWriter {
public:
    bool open(const std::string& path, const Uuid& asset, EditRate rate, std::string* error = nullptr) {
        return openImfFile(path, asset, rate, error);
    }
    // The document, `duration` edit units long, and the images it refers to as "urn:uuid:<id>".
    bool write(const std::string& xml, int64_t duration, const std::string& language, std::string* error = nullptr,
               const std::vector<TimedTextResource>& resources = {});
    int64_t duration() const override { return duration_; }
    EditRate rate() const { return rate_; }
    const Uuid& resourceId() const { return resourceId_; }
    const std::string& language() const { return language_; }
    const char* profile() const { return image_ ? kImageProfile : kProfile; }
    // The resources written: each one's id and type, and its sub-descriptor's instance id.
    const std::vector<std::pair<Uuid, std::string>>& resources() const { return resources_; }
    const std::vector<Uuid>& resourceSets() const { return resourceSets_; }
    static constexpr const char* kProfile = "http://www.w3.org/ns/ttml/profile/imsc1.1/text";
    static constexpr const char* kImageProfile = "http://www.w3.org/ns/ttml/profile/imsc1.1/image";

protected:
    std::vector<uint8_t> headerMetadata(int64_t duration) const override;
    std::vector<uint8_t> indexSegments() const override;
    std::array<uint8_t, 16> essenceContainer() const override;

private:
    Uuid resourceId_{};
    std::string language_ = "en";
    int64_t duration_ = 0;
    bool image_ = false;
    std::vector<std::pair<Uuid, std::string>> resources_;  // id and type, in stream order
    std::vector<Uuid> resourceSets_;
};

// Reads an IMF (AS-02) track file back: what it carries, its track file id (the file package's), each picture
// frame, the samples of clip-wrapped sound, or the timed text document. Every length is checked against the file.
class ImfTrackReader {
public:
    enum class Kind { Unknown, Picture, Sound, TimedText };
    // `imfPictureOnly`: give up at the end of the header unless it is an IMF picture (imfRgbJpeg2000 or imfAces), without walking
    // the rest of a file that is not.
    bool open(const std::string& path, std::string* error = nullptr, bool imfPictureOnly = false);
    Kind kind() const { return kind_; }
    const Uuid& asset() const { return asset_; }
    int64_t frames() const { return int64_t(elements_.size()); }  // picture
    uint64_t largestFrame() const;                                // picture: the largest codestream's bytes
    int channels() const { return channels_; }                    // sound
    EditRate rate() const { return rate_; }                       // the descriptor's sample rate
    int width() const { return width_; }                          // picture: the stored size (0: not given)
    int height() const { return height_; }
    // An IMF picture: OP1a, JPEG 2000 in an RGBA descriptor (full-range RGB), as Application #2E has it (a DCP's
    // OP-Atom pictures and other MXF are not).
    bool imfRgbJpeg2000() const { return kind_ == Kind::Picture && op1a_ && rgba_ && j2k_; }
    // An IMF Application #5 picture: OP1a, ACES image containers in an RGBA descriptor (ST 2065-5).
    bool imfAces() const { return kind_ == Kind::Picture && op1a_ && rgba_ && aces_; }
    // Its ACES picture sub-descriptor's output transform, and its target frames, each found in its stream or not.
    const std::string& acesAuthoring() const { return acesAuthoring_; }
    struct TargetFrame {
        Uuid resource{};
        std::string mediaType;
        int64_t index = 0;
        uint32_t stream = 0;
        uint64_t at = 0, size = 0;
        bool present = false;
    };
    const std::vector<TargetFrame>& targetFrames() const { return targets_; }
    bool targetFrame(size_t i, std::vector<uint8_t>& out, std::string* error = nullptr) const;
    int64_t samples() const { return channels_ > 0 ? int64_t(clipSize_ / (3 * uint64_t(channels_))) : 0; }
    bool frame(int64_t index, std::vector<uint8_t>& out, std::string* error = nullptr) const;
    // `count` sample frames from `first`, interleaved, as 24-bit integers (past the end: false).
    bool pcm(int64_t first, int64_t count, std::vector<int32_t>& out, std::string* error = nullptr) const;
    bool text(std::string& out, std::string* error = nullptr) const;
    // Timed text: the resources the descriptor lists (an Image document's PNGs), each found in its generic stream or not.
    struct Resource {
        Uuid id{};
        uint64_t at = 0, size = 0;
        bool present = false;
    };
    const std::vector<Resource>& resources() const { return resources_; }
    bool resource(const Uuid& id, std::vector<uint8_t>& out, std::string* error = nullptr) const;

private:
    std::string path_;
    std::vector<Resource> resources_;
    Kind kind_ = Kind::Unknown;
    Uuid asset_{};
    int channels_ = 0, width_ = 0, height_ = 0;
    EditRate rate_;
    bool op1a_ = false, rgba_ = false, j2k_ = false, aces_ = false;
    std::string acesAuthoring_;
    std::vector<TargetFrame> targets_;
    std::vector<std::pair<uint64_t, uint64_t>> elements_;  // picture frames: offset and size of each value
    uint64_t clipAt_ = 0, clipSize_ = 0;                    // sound or timed text: the one element's value
};

}  // namespace montage::dcp
