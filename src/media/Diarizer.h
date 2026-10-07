// Montage — who speaks when (speaker diarization), for transcripts, captions
// and multicam. The method is pyannote's, as sherpa-onnx implements it:
// pyannote segmentation 3.0 (MIT) finds up to three speakers in each 10 s
// window of the audio, stepping 1 s; each window's speakers are embedded
// with CAM++ (3D-Speaker, Apache-2.0); the embeddings are clustered over the
// whole recording (average linkage on cosine distance, stray voices folded
// into the nearest speaker); and the windows are stitched back together
// frame by frame. Runs on ONNX Runtime.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "ModelFiles.h"
#include "core/Transcript.h"

namespace montage {

// The two models (36 MB), downloaded on first use; $MONTAGE_SPEAKER_MODEL
// names another folder, $MONTAGE_SPEAKER_MODEL_URL a mirror.
const ModelPack& speakerModel();
bool diarizerAvailable();

struct DiarizeOptions {
    int speakers = 0;         // how many people speak, 0 = find out
    double threshold = 0.55;  // when finding out: mean cosine distance below which voices are one person
    double minSpeech = 0.3;   // shorter turns are dropped (s)
    double minPause = 0.5;    // a person's turns closer than this are joined (s)
};

// Speaker turns in 16 kHz mono audio, speakers numbered from 0 in order of
// first appearance. Times are seconds from the first sample.
bool diarize(const std::vector<float>& mono16k, const DiarizeOptions& options, std::vector<SpeakerTurn>& turns,
             const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
             std::string* error = nullptr);

// ---- Building blocks (exposed for tests) ------------------------------------

// Kaldi-compatible log mel filterbank: 80 bins, 25 ms frames every 10 ms,
// Povey window, centred frames. Returns frames x 80 values.
std::vector<float> speakerFeatures(const float* samples, size_t n);
// Agglomerative clustering, average linkage on cosine distance: into `count`
// clusters, or (count 0) merging while the distance is <= threshold, then
// folding clusters of a few members into the nearest larger one. Labels are
// numbered in order of first appearance.
std::vector<int> clusterSpeakers(const std::vector<std::vector<float>>& embeddings, int count, double threshold);

}  // namespace montage
