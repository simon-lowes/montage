// Montage — Magic Lantern Video (.mlv): the RAW video Magic Lantern records on Canon EOS cameras (the open-source
// stand-in for camera RAW video codecs whose decoders are closed, such as Blackmagic RAW). The file is a run of blocks
// (the MLV 2.0 layout Magic Lantern's mlv.h sets out): the sensor's size, bit depth, black and white levels, colour
// matrix and CFA pattern (RAWI), its binning and skipping (RAWC: anamorphic modes' pixel aspect), each frame's Bayer
// mosaic (VIDF), uncompressed (10, 12 or 14 bits packed) or lossless (LJ92: lossless JPEG, ITU T.81 process 14, decoded
// here), the sound's format and its PCM (WAVI, AUDF), and the camera, lens, exposure, white balance and date (IDNT,
// LENS, EXPO, WBAL, RTCI). A recording split across chunks (.MLV, .M00 to .M99, with the main file's GUID) is read as
// one. Frames are developed as ProRes RAW's are (media/ProResRaw.h: black and white levels, white balance as shot or
// chosen, a bilinear demosaic, the camera's matrix) into ACEScct, so the whole range of the sensor is kept; at half size
// for playback. The blocks are indexed once per file (shared by every reader of it while unchanged).
// Not here: dual ISO and focus pixel removal, LZMA or delta-compressed frames (MLV App's own), the MJPEG/H.264 classes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ProResRaw.h"

class QFile;

namespace montage {

bool isMlvPath(const std::string& path);

struct MlvInfo {
    int width = 0, height = 0;  // the mosaic's (each frame's)
    int bitsPerPixel = 14;
    int black = 2048, white = 15000;  // in the sensor's bits
    int cfa = 0x02010100;             // DNG's CFAPattern as Magic Lantern packs it (0x02010100: red, green / green, blue)
    int fpsNum = 25, fpsDen = 1;
    int frames = 0;  // from the first frame recorded to the last (one missing is shown as the one before)
    double pixelAspect = 1;  // from the binning and skipping (3 in the 1x3 anamorphic modes)
    bool lossless = false;
    // As shot.
    std::string camera, lens, created;  // created: ISO 8601, from the camera's clock
    int iso = 0, kelvin = 0;
    double shutter = 0, aperture = 0, focalLength = 0;  // seconds, f-number, millimetres
    ProResRawColor color;  // the levels, white balance and matrix, as development takes them
    // Sound (16 or 24-bit PCM).
    bool hasAudio = false;
    int sampleRate = 0, channels = 0, bitsPerSample = 0;
    int64_t soundFrames = 0;  // samples per channel
    int chunks = 1;
};

struct MlvIndex;

class MlvReader {
public:
    MlvReader();
    ~MlvReader();
    MlvReader(const MlvReader&) = delete;
    MlvReader& operator=(const MlvReader&) = delete;

    // Reads the file's blocks (and its chunks'); false with the reason if it is not MLV or holds no RAW frames.
    bool open(const std::string& path, std::string* error = nullptr);
    const MlvInfo& info() const;
    // Frame n's mosaic as 16-bit samples (the sensor's values scaled to 16 bits), width x height, rows top to bottom.
    bool readMosaic(int n, std::vector<uint16_t>& out, std::string* error = nullptr);
    // Frame n developed (ACEScct AP1, 16-bit RGB, mlvPictureSize's; half size with settings.half), into `rgb` with
    // `stride` samples a row, or into `out`.
    bool develop(int n, const RawSettings& settings, uint16_t* rgb, ptrdiff_t stride, std::string* error = nullptr);
    bool develop(int n, const RawSettings& settings, DevelopedRaw& out, std::string* error = nullptr);
    // The sound from sample frame `from` on, handed to `sink` a block at a time as interleaved 32-bit samples (the
    // PCM's in the top bits); false if a read failed (or `sink` returned false, to stop).
    bool readSound(int64_t from, const std::function<bool(const int32_t* samples, size_t frames)>& sink, std::string* error = nullptr);

private:
    bool readBlock(int chunk, int64_t offset, int64_t size, std::string* error);
    bool loadMosaic(int n, std::string* error);
    std::shared_ptr<const MlvIndex> index_;
    std::unique_ptr<QFile> file_;  // the chunk read last
    int fileChunk_ = -1;
    std::vector<uint8_t> buffer_;
    std::vector<uint16_t> mosaic_;
    std::vector<int32_t> sound_;
};

bool readMlvInfo(const std::string& path, MlvInfo& out, std::string* error = nullptr);
// The developed picture's size (the mosaic from its first red site, in whole 2x2 cells; halved, with `half`).
void mlvPictureSize(const MlvInfo& info, bool half, int& width, int& height);

// Lossless JPEG (SOF3, any predictor, one to four interleaved components, 2 to 16 bits): the samples in raster order,
// each row `width * components` of them. False with the reason if it is not one this reads, or (with `samples` not 0)
// if it does not hold exactly that many samples.
bool decodeLosslessJpeg(const uint8_t* data, size_t size, std::vector<uint16_t>& out, int& width, int& height, int& components,
                        int& bits, std::string* error = nullptr, size_t samples = 0);

}  // namespace montage
