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
// the outline's corners among theirs) and sides, each corner coloured as the title paints it there (linear light; a
// track's picture is not taken), in one material lit by the scene. False when nothing of it shows, or for a title seen
// through a camera in a file.
struct Title3DMesh {
    PosedTriangles triangles;
    std::shared_ptr<const Model> model;  // its material
    std::vector<int> letterOf;           // each triangle's letter solid (in the title's order)
    double opacity = 1;
};
bool title3dMesh(const Effect& g, double t, double seqWidth, double seqHeight, FrameTime duration, double fps, Title3DMesh& out);

}  // namespace montage
