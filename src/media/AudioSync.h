// Montage — synchronising recordings by their audio (cross-correlation of
// loudness envelopes), e.g. a camera's scratch audio against a field recorder.
#pragma once

#include "Decoder.h"

namespace montage {

struct SyncResult {
    double offset = 0;      // seconds: content at time y in `other` occurs at y + offset in `ref`
    double confidence = 0;  // normalised correlation peak, 0..1
    bool found = false;
};

// Searches lags within +-maxLagSeconds.
SyncResult findAudioOffset(const AudioBuffer& ref, const AudioBuffer& other, double maxLagSeconds = 120.0);

}  // namespace montage
