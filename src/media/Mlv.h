// Montage — Magic Lantern Video (.mlv): the RAW video Magic Lantern records on Canon EOS cameras (the open-source
// stand-in for camera RAW video codecs whose decoders are closed, such as Blackmagic RAW). The file is a run of blocks
// (the MLV 2.0 layout Magic Lantern's mlv.h sets out): the sensor's size, bit depth, black and white levels, colour
// matrix and CFA pattern (RAWI), each frame's Bayer mosaic (VIDF), uncompressed (10, 12 or 14 bits packed) or lossless
// (LJ92: lossless JPEG, ITU T.81 process 14, decoded here), the sound's format and its PCM (WAVI, AUDF), and the
// camera, lens, exposure, white balance and date (IDNT, LENS, EXPO, WBAL, RTCI). A recording split across chunks
// (.MLV, .M00, .M01...) is read as one. Frames are developed as ProRes RAW's are (media/ProResRaw.h: black and white
// levels, white balance as shot or chosen, a bilinear demosaic, the camera's matrix) into ACEScct, so the whole range
// of the sensor is kept; at half size for playback. The sound is read by FFmpeg (its MLV demuxer).
// Not here: dual ISO and focus pixel removal, LZMA or delta-compressed frames (MLV App's own), the MJPEG/H.264 classes.
#pragma once

#include <cstdint>
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
    int frames = 0;  // the frames' numbers run 0 to frames - 1 (a frame missing is shown as the one before)
    bool lossless = false;
    // As shot.
    std::string camera, lens, created;  // created: ISO 8601, from the camera's clock
    int iso = 0, kelvin = 0;
    double shutter = 0, aperture = 0, focalLength = 0;  // seconds, f-number, millimetres
    ProResRawColor color;  // the levels, white balance and matrix, as development takes them
    // Sound (read by FFmpeg).
    bool hasAudio = false;
    int sampleRate = 0, channels = 0, bitsPerSample = 0;
    int chunks = 1;
};

class MlvReader {
public:
    MlvReader();
    ~MlvReader();
    MlvReader(const MlvReader&) = delete;
    MlvReader& operator=(const MlvReader&) = delete;

    // Reads the file's blocks (and its chunks'); false with the reason if it is not MLV or holds no RAW frames.
    bool open(const std::string& path, std::string* error = nullptr);
    const MlvInfo& info() const { return info_; }
    // Frame n's mosaic as 16-bit samples (the sensor's values scaled to 16 bits), width x height, rows top to bottom.
    bool readMosaic(int n, std::vector<uint16_t>& out, std::string* error = nullptr);
    // Frame n developed (ACEScct AP1, 16-bit RGB; half size with settings.half).
    bool develop(int n, const RawSettings& settings, DevelopedRaw& out, std::string* error = nullptr);

private:
    struct Frame {
        int chunk = 0;
        int64_t offset = 0;  // the frame's data in its chunk
        int64_t size = 0;
    };
    MlvInfo info_;
    std::vector<std::unique_ptr<QFile>> files_;
    std::vector<Frame> frames_;  // by frame number (size 0: missing)
    std::vector<uint8_t> buffer_;
};

bool readMlvInfo(const std::string& path, MlvInfo& out, std::string* error = nullptr);
// The developed picture's size (the mosaic from its first red site; whole 2x2 cells, halved, with `half`).
void mlvPictureSize(const MlvInfo& info, bool half, int& width, int& height);

// Lossless JPEG (SOF3, any predictor, one to four interleaved components, 2 to 16 bits): the samples in raster order,
// each row `width * components` of them. False with the reason if it is not one this reads.
bool decodeLosslessJpeg(const uint8_t* data, size_t size, std::vector<uint16_t>& out, int& width, int& height, int& components,
                        int& bits, std::string* error = nullptr);

}  // namespace montage
