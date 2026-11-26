// Montage — extruded 3D titles (Final Cut's 3D titles, Resolve's Fusion 3D text): the text's outline pulled out into
// depth, turned in space and lit, seen through a perspective camera (its own, or a camera in a USD or glTF scene
// with the title standing in that scene), every setting keyframeable. Drawn by a small
// z-buffered rasteriser at twice the size and filtered down: the front and back faces fill the projected outline
// (nonzero winding, so letters keep their holes; each pixel's depth from the face's plane), and the sides are quads
// along every edge, smooth where the outline curves and sharp at its corners. Text animations select letters, words or
// lines as a 2D title's do and extrude them deeper, move them (Z towards the camera), turn them about X, Y and Z and
// scale them about their middles, and colour their faces and sides: each such letter is a solid of its own.
#pragma once

#include "core/Model.h"
#include "media/Image.h"
#include "media/Model3D.h"
#include "render/ModelRender.h"

#include <memory>

namespace montage {

// The tracks' pictures a 3D title's faces show where its Front, Sides or Back is set to a track's picture, each
// premultiplied and fitted to the title's box; an empty one leaves that face its colour.
struct Title3DPictures {
    Image front, sides, back;
};

// The "title3d" generator at frame t of a clip `duration` frames long (for its animation out), w x h output pixels,
// `scale` output pixels per sequence pixel.
Image renderTitle3D(const Effect& g, FrameTime t, int w, int h, double scale, FrameTime duration, double fps,
                    const Title3DPictures* pictures = nullptr);

// How many letter solids 3D titles have drawn into a raster since the program started (for tests: a still letter is
// drawn once under motion blur, a moving one once a sample).
uint64_t title3dSolidsDrawn();

// A 3D title at time t (clip frames) as triangles in the 3D layers' scene seen through its own camera (the frame at
// z = 0 about its middle, a unit a sequence pixel, y up, the camera on +z): its letters' faces (cut into triangles,
// the outline's corners among theirs) and sides, each corner coloured as the title paints it there (linear light; at
// a track's picture, its colour there), with uvs across the title's box as its own drawing maps a picture, in one
// material, and one more for each track's picture it shows (given `pictures`), that picture as its texture
// (premultiplied and display-referred, as the title is drawn with it). Each corner also has the title's own light:
// how its own drawing lights it. False when nothing of it shows, or for a title seen through a camera in a file.
struct Title3DMesh {
    PosedTriangles triangles;
    std::shared_ptr<const Model> model;  // its materials
    std::vector<int> letterOf;           // each triangle's letter solid (in the title's order)
    std::vector<char> onFace;            // each triangle: 1 on its letter's front face
    double opacity = 1;
    // The title's own camera in that space (its lens, at the picture's middle; scale 1: sequence pixels), through which
    // it is drawn alone.
    ModelCamera camera;
    // Its own light, for ModelScene::ownLight: 6 a corner (the factor and highlight its own drawing lights it with
    // there, then keep and the colour added, display-referred), the way towards the light and its share of ambient.
    std::vector<float> ownLight;
    std::array<double, 3> toLight{0, 0, 1};
    float ambient = 0;
};
bool title3dMesh(const Effect& g, double t, double seqWidth, double seqHeight, FrameTime duration, double fps, Title3DMesh& out,
                 const Title3DPictures* pictures = nullptr);

}  // namespace montage
