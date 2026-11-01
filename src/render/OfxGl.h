// Montage — the OpenGL that OpenFX plugins render and draw their on-screen controls with (render/Ofx's OpenGL render
// suite and V1 overlays): one context of Montage's own on a thread of its own, made current on an offscreen surface,
// with float textures in and a float framebuffer out. Separate from the GPU compositor (render/GpuCompositor), which
// may run on Vulkan, Metal or Direct3D: plugins get OpenGL whatever draws the picture.
#pragma once

#include <QImage>
#include <functional>
#include <string>

#include "media/Image.h"

namespace montage::ofx::gl {

// The offscreen surface, made on the GUI thread (once; false, with why, where OpenGL cannot start).
bool initialize(std::string* why = nullptr);
bool available(std::string* why = nullptr);
// Runs `fn` on the OpenGL thread with the context current, and waits (inline when already there). False without
// OpenGL.
bool run(const std::function<void()>& fn);
bool onThread();
// From a job on the OpenGL thread: runs `fn` on the thread waiting for the job in run() (with that thread's scopes:
// the working space, expressions, cancelling), and waits. Elsewhere it runs `fn` here.
void callBack(const std::function<void()>& fn);

// (The rest only inside run().)
// A premultiplied picture as an RGBA float texture, bottom row first (as OpenFX counts rows); 0 when it failed.
unsigned upload(const Image& img);
void destroyTexture(unsigned texture);
// A float texture with a framebuffer to draw into.
struct Target {
    unsigned fbo = 0, texture = 0;
    int width = 0, height = 0;
};
bool makeTarget(Target& t, int width, int height);
void bind(const Target& t);  // drawn into from now, the viewport set to it
void read(const Target& t, Image& out);  // its pixels, top row first (as Image counts rows)
QImage readOverlay(const Target& t);     // its pixels as a premultiplied picture, top row first
void release(Target& t);
// The state a plugin may have left (the framebuffer, the program, textures, blending) put back.
void resetState();
// For a V1 overlay: the projection that puts canonical (x, y) at view (m[0] x + m[1] y + m[2], m[3] x + m[4] y + m[5]) in
// a `width` x `height` view whose y runs down.
void overlayProjection(int width, int height, const double m[6]);
void clear();

}  // namespace montage::ofx::gl
