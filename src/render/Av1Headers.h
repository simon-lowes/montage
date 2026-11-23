// Montage — AV1 headers: a stream's sequence header and each frame header read as far as its film grain parameters
// (the last thing a frame header carries, so everything before them: the frame's size, its references, tiles,
// quantisers, segmentation, loop filters and global motion), with what later headers depend on kept for each of the
// eight reference slots; and frame headers written again with other film grain parameters, the rest of the stream
// untouched. So Montage can carry a picture's grain as parameters whichever AV1 encoder made the stream, and can say
// what grain a stream's frames carry. Reading follows the AV1 specification's syntax (5.5, 5.9) and its reference
// update process (7.20).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "render/Av1FilmGrain.h"

namespace montage {

// What of a sequence header the frame headers' reading needs.
struct Av1Sequence {
    bool valid = false;
    int profile = 0;
    bool stillPicture = false, reducedStill = false;
    bool decoderModel = false, equalPictureInterval = false;
    int bufferRemovalBits = 0, presentationBits = 0;
    int operatingPoints = 1;
    int operatingPointIdc[32] = {};
    bool decoderModelForOp[32] = {};
    int widthBits = 0, heightBits = 0, maxWidth = 0, maxHeight = 0;
    bool frameIds = false;
    int deltaFrameIdBits = 0, frameIdBits = 0;
    bool use128 = false, enableOrderHint = false, enableRefFrameMvs = false, enableWarpedMotion = false;
    int forceScreenContentTools = 2, forceIntegerMv = 2;  // 2: chosen in each frame
    int orderHintBits = 0;
    bool enableSuperres = false, enableCdef = false, enableRestoration = false;
    int bitDepth = 8;
    bool mono = false, subX = true, subY = true, separateUvDeltaQ = false;
    int matrix = 2;
    bool fullRange = false;
    bool filmGrainPresent = false;
    size_t filmGrainBit = 0;  // where film_grain_params_present is, in bits from the start of the OBU's payload
};

// A frame header as read.
struct Av1FrameHeader {
    bool showExisting = false;  // shows a frame kept in a slot: no frame of its own
    int showSlot = -1;
    int frameType = 0;          // 0 key, 1 inter, 2 intra only, 3 switch
    bool showFrame = false, showableFrame = false;
    int orderHint = 0;
    int refreshFlags = 0;
    int refSlot[7] = {-1, -1, -1, -1, -1, -1, -1};  // the slots it predicts from (an inter frame's)
    int width = 0, height = 0;  // as shown (after any super-resolution)
    int temporalId = 0, spatialId = 0;
    // Its film grain: whether its header carries the parameters' syntax at all, whether they are its own (update) or
    // a reference slot's, and the parameters themselves as a decoder applies them when it shows the frame.
    bool grainCoded = false, grainUpdate = false;
    int grainRefSlot = -1;
    Av1FilmGrain grain;
    size_t grainBit = 0, grainEnd = 0;  // the film grain syntax's bits in the OBU's payload
    bool shown() const { return showFrame || showExisting; }
};

// Reads a stream's headers, a temporal unit (a packet of OBUs, each with its size) at a time, in decoding order.
class Av1HeaderReader {
public:
    // A codec configuration: an av1C box's payload, or OBUs. True when it held a sequence header that could be read, or
    // was an av1C box with no OBUs (the sequence header then comes with the first packet).
    bool configure(const uint8_t* data, size_t size, std::string* error = nullptr);
    // A temporal unit's frame headers, in order (copies of a header within a frame are not listed again).
    bool read(const uint8_t* data, size_t size, std::vector<Av1FrameHeader>& frames, std::string* error = nullptr);
    const Av1Sequence& sequence() const { return seq_; }
    // How far frame order hint `a` is after `b` (frames; negative: before).
    int relativeDistance(int a, int b) const;
    // The order hint of the frame kept in a slot (-1: none).
    int slotOrderHint(int slot) const { return slot >= 0 && slot < 8 && slots_[slot].valid ? slots_[slot].orderHint : -1; }

private:
    friend class Av1GrainWriter;
    struct Slot {
        bool valid = false;
        int frameType = 0;
        int upscaledWidth = 0, frameWidth = 0, frameHeight = 0, renderWidth = 0, renderHeight = 0;
        int orderHint = 0;
        bool showable = false;
        bool featureEnabled[8][8] = {};
        int featureData[8][8] = {};
        Av1FilmGrain grain;
    };
    struct Obu {
        int type = 0;
        bool extension = false, hasSize = false;
        int temporalId = 0, spatialId = 0;
        size_t start = 0, payload = 0, size = 0;  // of the whole OBU, of its payload, the payload's bytes
    };
    // What reading a frame header leaves for the tile groups and for writing it again.
    struct Parsed {
        Av1FrameHeader h;
        int tileCols = 1, tileRows = 1, tileColsLog2 = 0, tileRowsLog2 = 0;
        size_t headerBits = 0;  // the uncompressed header's
    };
    static bool splitObus(const uint8_t* data, size_t size, std::vector<Obu>& out, std::string* error);
    bool readSequence(const uint8_t* p, size_t n, std::string* error);
    bool readFrameHeader(const uint8_t* p, size_t n, const Obu& obu, Parsed& out, std::string* error);
    void finishFrame(const Parsed& f);

    Av1Sequence seq_;
    Slot slots_[8];
    // The frame being read: its header seen, and whether its tile groups have ended.
    bool seenFrameHeader_ = false;
    Parsed current_;
    Slot pending_;  // what the frame being read will leave in the slots it refreshes
};

// Writes film grain parameters into a stream's frame headers. The stream is read as it goes (so packets must come in
// decoding order, from the first); every header that may carry film grain gets the parameters `grain` gives for the
// frame it shows, with a new seed each frame, as a reference slot's parameters where a frame it predicts from already
// carries the same (a few bits), else in full.
class Av1GrainWriter {
public:
    // The parameters for the frame shown `frame` frames from the stream's first (apply false: no grain there). The
    // seed it returns is used as it is.
    using Source = std::function<Av1FilmGrain(int64_t frame)>;
    // The codec configuration with film grain marked present (an av1C box's payload or OBUs, as given). Also starts
    // the reader from it.
    bool configure(std::vector<uint8_t>& config, std::string* error = nullptr);
    // A temporal unit, shown at `frame`, written again. Frames in it that are shown later (kept for a later
    // "show existing frame") get the parameters of the frame they will be shown at, by their order hints.
    bool rewrite(const uint8_t* data, size_t size, int64_t frame, const Source& grain, std::vector<uint8_t>& out, std::string* error = nullptr);
    const Av1HeaderReader& reader() const { return reader_; }
    // What was written: frames given grain, of those how many as a reference's parameters, and the bytes added.
    int64_t framesWithGrain = 0, framesByReference = 0, bytesAdded = 0;

private:
    Av1HeaderReader reader_;
    Av1FilmGrain written_[8];  // what the frame in each slot was given
    std::vector<uint8_t> lastHeader_;  // the header last written, for its copies within the frame
    size_t lastHeaderBits_ = 0, lastOriginalBits_ = 0;
};

// film_grain_params() as bits, for a frame of this kind in this sequence (`update` false: `refSlot`'s parameters).
void av1WriteFilmGrain(const Av1FilmGrain& g, const Av1Sequence& seq, bool interFrame, bool update, int refSlot, std::vector<bool>& bits);

}  // namespace montage
