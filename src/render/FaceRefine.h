// Montage — Face Refinement (like Resolve Studio's): the faces in a frame
// (media/Faces.h) found again each frame and touched up within a soft mask
// from their box and landmarks: skin smoothed where it is skin-coloured and
// the change is small (blemishes and texture, not edges), eyes brightened
// and sharpened, the face lightened.
#pragma once

#include <vector>

#include "media/Faces.h"
#include "media/Image.h"

namespace montage {

struct FaceRefineSettings {
    double smooth = 0.4;    // 0..1
    double eyesBright = 0.2, eyesSharp = 0.3, lighten = 0;
    bool showMask = false;  // the face mask as grey instead
};

// Touches up each face in `img` (premultiplied) in place; nothing without faces.
void refineFaces(Image& img, const std::vector<FaceBox>& faces, const FaceRefineSettings& s);

// The soft mask of where a face's skin is (0..1 per pixel of a w x h picture), eyes and mouth left out.
std::vector<float> faceSkinMask(const FaceBox& f, int w, int h);

}  // namespace montage
