// Montage — surface tracking: a triangle mesh laid over a surface on one frame (the reference) and followed as the
// surface moves and bends (cloth, skin, a flag), so a picture, paint or a grade's window stays on it.
//
// The mesh is render/MeshWarp.h's lattice, kept to the squares inside the surface's outline. From frame to frame,
// features inside the mesh (Shi–Tomasi corners) are followed by pyramidal Lucas–Kanade (media/Tracking.h) and the
// vertices solved so each feature keeps its place in its triangle, while every triangle stays as near as it can to a
// turned and scaled copy of itself on the frame before (the similarity step of Igarashi et al.'s as-rigid-as-possible
// shape manipulation): one sparse least-squares system a frame. Then, so nothing drifts, the reference frame is warped
// by the mesh onto the frame and the features are matched again against it (single-level Lucas–Kanade over a
// residual of a pixel or so) and the vertices solved again. A solve that would turn a triangle over is redone stiffer.
// Vertices put by hand on a frame hold there, and the track is solved again from that frame on.
//
// The track maps each frame's pixels to the reference frame's (the surface held still: Stabilise, an ST map) and back
// (a picture from the reference carried on to every frame: Picture, Put Back), and gives the surface's motion between
// frames (motion vectors).
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "MeshWarp.h"
#include "core/Model.h"
#include "media/Image.h"
#include "media/Tracking.h"

namespace montage {

struct SurfaceTrack {
    int width = 0, height = 0;           // the frames' size it was tracked at (pixels)
    double start = 0;                    // media seconds of frame 0
    double frameSeconds = 1.0 / 24;
    int reference = 0;                   // the frame the mesh was laid on
    WarpMesh mesh;                       // at rest: where it lies on the reference frame (pixels, centres at +0.5)
    std::vector<std::vector<MeshPoint>> frames;   // the vertices on each frame; empty where not tracked (lost)
    std::map<int, std::map<int, MeshPoint>> fixes;  // frame -> vertex -> where it was put by hand
    // The frame shown at a media time (nearest), or -1 outside the track or where it was not tracked.
    int frameAt(double mediaSeconds) const;
    bool operator==(const SurfaceTrack&) const = default;
};

// As text for an effect's strings (the vertices as base64 floats), and back. Parsed tracks are kept by their text.
std::string surfaceTrackToString(const SurfaceTrack& t);
bool surfaceTrackFromString(const std::string& text, SurfaceTrack& out);
std::shared_ptr<const SurfaceTrack> cachedSurfaceTrack(const std::string& text);
// An effect's "track", parsed (null when it has none).
std::shared_ptr<const SurfaceTrack> cachedSurfaceTrack(const Effect& e);

// A mesh of about `vertices` vertices over the outline (pixels of a w x h frame, the points in order) and a little
// round it (so features at its edge have whole triangles), from MeshWarp's lattice.
WarpMesh surfaceMesh(const std::vector<MeshPoint>& outline, int w, int h, int vertices);

struct SurfaceTrackOptions {
    int threads = 0;          // 0: the shared workers; 1: this thread; more: that many of its own
    double stiffness = 0.3;   // how much each triangle keeps its shape from the frame before, against the features
    int refine = 2;           // matches against the warped reference each frame
};
// The frames for tracking (luma, all the same size as the track); false where one cannot be read.
using SurfaceFrames = std::function<bool(int index, GrayImage& out)>;
// Tracks the surface over frames [0, count) of `t` (its mesh, reference and fixes set; t.width/height those of the
// frames) out from `from` (the reference, or a frame already tracked whose vertices are kept, with that frame's fixes
// put in first) to both ends, away from the reference. Frames beyond where it is lost are left empty. `progress`
// (0..1) may return false to stop. False when nothing could be tracked.
bool trackSurface(SurfaceTrack& t, int count, int from, const SurfaceFrames& frames, const SurfaceTrackOptions& options = {},
                  const std::function<bool(double)>& progress = {}, std::string* error = nullptr);

// Points (the track's pixels) on frame `from` carried to where the surface takes them on frame `to`: through the
// triangle each is in, or (off the mesh) moved as its nearest vertex is.
std::vector<MeshPoint> surfaceCarry(const SurfaceTrack& t, int from, int to, const std::vector<MeshPoint>& points);

// Each pixel of a w x h picture that the mesh at `moved` covers, as the point of the mesh at `to` it is (pixels,
// centres at +0.5; x then y, NaN where the mesh does not cover it). Pixel centres on a shared edge go to one triangle.
std::vector<float> meshMap(const WarpMesh& m, const std::vector<MeshPoint>& moved, const std::vector<MeshPoint>& to, int w, int h);
// As meshMap, with the pixels within `reach` of the mesh's edge too, carried as the nearest triangle takes them.
std::vector<float> meshMapReaching(const WarpMesh& m, const std::vector<MeshPoint>& moved, const std::vector<MeshPoint>& to, int w, int h,
                                   double reach);
// `src` (the reference frame's picture: what lies on the surface there) drawn on to `out` where the surface is on
// `frame` (over what is there, by its alpha); with `stabilise` the other way: `src` (that frame) drawn where the
// surface lies on the reference frame. Positions are scaled from the track's size to the pictures'.
void drawOnSurface(const SurfaceTrack& t, int frame, const Image& src, Image& out, bool stabilise);
// Nuke's ST map (red the reference frame's x over its width, green 1 minus its y over its height) for `frame`, at the
// track's size: each pixel of that frame from where it is on the reference frame (where the surface is; itself
// elsewhere). Width * height * 3 floats.
std::vector<float> surfaceStMap(const SurfaceTrack& t, int frame);
// The surface's motion from `frame` to frame + `step` (step ±1) at each of `frame`'s pixels it covers (u right, v
// down, pixels; 0 elsewhere). Width * height * 2 floats.
std::vector<float> surfaceMotion(const SurfaceTrack& t, int frame, int step);
// For every tracked frame in [first, last], `<base>.<frame number>.exr` (4 digits, from frame 0 of the track): R, G, B
// the ST map, and forward.u/v and backward.u/v the motion vectors. The files written, or empty on failure.
std::vector<std::string> writeSurfaceExrs(const SurfaceTrack& t, const std::string& base, int first, int last,
                                          std::string* error = nullptr);

// ---- The Surface Tracker effect ("surface_track": its "track" as text, a "picture" file) ------------------------

// The surface its mask outlines on clip frame `at` of video clip `c`, tracked over the whole clip (as text for
// "track"). With `again`, the track it has is kept up to `at` and solved again from there with its fixes. `progress`
// (0..1) may return false to stop.
bool trackClipSurface(const Project& p, const Sequence& seq, const Clip& c, const Effect& e, FrameTime at, std::string& track,
                      bool again = false, const std::function<bool(double)>& progress = {}, std::string* error = nullptr);
// The track's frame for clip frame `t` of `c` (-1 where it has none).
int surfaceFrameOf(const SurfaceTrack& t, const Sequence& seq, const Clip& c, FrameTime local);
// What it draws (by "mode"): the mesh, its picture on the surface, the surface held still as on the reference frame,
// such a picture put back on the surface (over the frame as it was before one held it still, earlier in the clip's
// effects), or nothing; on the frame of the media at `sourceSeconds`.
void surfaceTrackEffect(const Effect& e, FrameTime t, Image& img, double sourceSeconds);

}  // namespace montage
