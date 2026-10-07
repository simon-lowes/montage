// Montage — picking objects out of video: EdgeTAM (Meta's on-device member
// of the Segment Anything 2 family, Apache-2.0) run by ONNX Runtime.
//
// The model is four graphs: an image encoder, a prompt/mask decoder, memory
// attention and a memory encoder. Following an object is SAM 2's video
// method: the clicked frames and the last few tracked frames are encoded
// into a memory bank that each new frame attends to, so the object is found
// again after it moves, turns or is briefly hidden. The bank is assembled
// here exactly as the reference implementation does (checked against it to
// within 2e-4 on the mask logits).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Image.h"
#include "core/ObjectMask.h"

namespace montage {

// True when Montage was built with ONNX Runtime.
bool segmenterAvailable();
std::string segmenterRuntimeVersion();

// The model's files, downloaded on first use.
struct SegmenterFile {
    std::string name;
    std::string sha256;
    int64_t bytes = 0;
};
const std::vector<SegmenterFile>& segmenterFiles();
int64_t segmenterDownloadBytes();
// $MONTAGE_OBJECT_MODEL, or a folder in the app data location.
std::string segmenterModelDirectory();
// Download location of one file ($MONTAGE_OBJECT_MODEL_URL replaces the folder: a mirror, or file:// in tests).
std::string segmenterFileUrl(const std::string& name);
// Every file is present with the expected size.
bool segmenterModelInstalled();
// SHA-256 of a downloaded file matches (run after downloading, before renaming into place).
bool segmenterFileVerified(const std::string& path, const SegmenterFile& f);

// The image size the model sees: frames are scaled to this square.
constexpr int kSegmenterInput = 1024;

struct SegmentResult {
    std::vector<float> logits;  // kObjectGrid² mask logits spanning the frame, > 0 inside
    float objectScore = 0;      // > 0 when the object is visible
};

// One object followed through consecutive frames (in either direction).
// Loads the model once per process; one tracker per thread.
class ObjectTracker {
public:
    ObjectTracker();
    ~ObjectTracker();
    ObjectTracker(const ObjectTracker&) = delete;
    ObjectTracker& operator=(const ObjectTracker&) = delete;

    bool load(std::string* error = nullptr);
    // Starts a new track.
    void reset();
    // Segments the next frame (RGBA16, ideally kSegmenterInput square; other
    // sizes are resampled). Frames with `points` are conditioning frames; the
    // first frame of a track needs them. Later frames are found from memory.
    bool step(const Frame16& frame, const std::vector<ObjectPoint>& points, SegmentResult& out,
              std::string* error = nullptr);
    int framesTracked() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace montage
