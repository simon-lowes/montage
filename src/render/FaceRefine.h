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

// Blemish Remover (like Resolve 21's): small spots on the skin of each face, darker or redder than
// the skin round them and no bigger than `maxSize` of the face's width, healed from that skin.
// Eyebrows, eyes, lips, hair and the face's edges are left alone.
struct BlemishSettings {
    double amount = 1;        // 0..1
    double sensitivity = 0.5; // 0..1: how faint a spot is still found
    double maxSize = 0.06;    // of the face's width
    bool showSpots = false;   // the spots found in red instead
};
// Returns the number of spots healed.
int removeBlemishes(Image& img, const std::vector<FaceBox>& faces, const BlemishSettings& s);

// The soft mask of where a face's skin is (0..1 per pixel of a w x h picture), eyes and mouth left out.
std::vector<float> faceSkinMask(const FaceBox& f, int w, int h);

}  // namespace montage

namespace montage {

// Redact Faces: each face covered in a soft-edged ellipse or rectangle grown from its box (hair and ears included),
// blurred, pixelated or filled with a colour.
struct RedactSettings {
    int style = 0;           // 0: blur, 1: pixelate, 2: solid colour
    double strength = 0.7;   // 0..1: how far it is blurred, how coarse the blocks
    double expand = 0.3;     // the face's box grown by this share of its size
    double feather = 0.15;   // the edge softened over this share, outside the shape (so the face itself stays covered)
    bool ellipse = true;
    float color[3] = {0, 0, 0};
};
void redactFaces(Image& img, const std::vector<FaceBox>& faces, const RedactSettings& s);
// The area redactFaces() covers for a face, in pixels of a w x h picture: centre and half sizes.
void redactionShape(const FaceBox& f, int w, int h, double expand, double& cx, double& cy, double& rx, double& ry);
// Outlines (for checking the tracking): red round the faces being covered, green round those left showing.
void outlineFaces(Image& img, const std::vector<FaceBox>& faces, const std::vector<bool>& covered, double expand);

}  // namespace montage
