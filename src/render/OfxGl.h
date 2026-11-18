// Montage — the OpenGL that OpenFX plugins render and draw their on-screen controls with (render/Ofx's OpenGL render
// suite and V1 overlays): one context of Montage's own on a thread of its own, made current on an offscreen surface,
// with float textures in and a float framebuffer out. Separate from the GPU compositor (render/GpuCompositor), which
// may run on Vulkan, Metal or Direct3D: plugins get OpenGL whatever draws the picture.
#pragma once

#include <QImage>
#include <atomic>
#include <functional>
#include <string>

#include "media/Image.h"

class QOpenGLContext;

namespace montage::ofx::gl {

// The offscreen surface, made on the GUI thread (once; false, with why, where OpenGL cannot start).
bool initialize(std::string* why = nullptr);
bool available(std::string* why = nullptr);
// Runs `fn` on the OpenGL thread with the context current, and waits (inline when already there). False without
// OpenGL, or when `fn` threw. `first`: ahead of the jobs already waiting (an on-screen control the view waits for).
bool run(const std::function<void()>& fn, bool first = false);
bool onThread();
// This thread's flag, true while it waits in run() for the OpenGL thread (read from other threads to tell whether
// waiting for this thread could deadlock).
const std::atomic<bool>* waitingFlag();
// True on a thread doing what a job on the OpenGL thread handed back to it (callBack): the OpenGL thread waits for it.
bool servingOpenGl();
// From a job on the OpenGL thread: runs `fn` on the thread waiting for the job in run() (with that thread's scopes:
// the working space, expressions, cancelling), and waits. Elsewhere it runs `fn` here.
void callBack(const std::function<void()>& fn);

// Textures shared with the GPU compositor (render/GpuCompositor, where it draws with OpenGL): the two contexts share
// their textures when the later of them is made sharing with the earlier. The plug-ins' context, once initialize() has
// made it (null before, and where there is no OpenGL), for the compositor's to be made sharing with; and the
// compositor's, told here when it is made and (null) before it goes, for the plug-ins' to be made sharing with.
QOpenGLContext* context();
void shareWith(QOpenGLContext* compositor);
// Whether `other`'s textures are the plug-ins' context's too, and it can copy between them (flip()).
bool sharesWith(QOpenGLContext* other);

// (The rest only inside run().)
// A premultiplied picture as an RGBA float texture, bottom row first (as OpenFX counts rows); 0 when it failed.
unsigned upload(const Image& img);
// An RGBA float texture of that size with nothing in it yet (`half`: 16-bit floats, which a DMA-BUF can carry, not 32);
// 0 when it failed.
unsigned makeTexture(int width, int height, bool half = false);
void destroyTexture(unsigned texture);
// Copies RGBA float texture `from` into `to` (both `width` x `height`) with its rows in the other order, on the GPU:
// the compositor's textures have the picture's top row first, OpenFX's its bottom row. False when it could not.
bool flip(unsigned from, unsigned to, int width, int height);
// Texture `texture`'s pixels read back (top row first when `topFirst` says its rows are stored so, else flipped to it).
void readTexture(unsigned texture, int width, int height, bool topFirst, Image& out);
// `img` written into texture `texture` (the same size), top row first.
void writeTexture(unsigned texture, const Image& img);
// Everything drawn so far finished, so that another context sharing these textures reads what was drawn.
void finish();

// A picture shared between processes from the GPU's memory: a Linux DMA-BUF of one plane, with a fence. A plug-in host
// exports its plug-in's picture as one and Montage imports it (render/OfxRemote.h), where both contexts are EGL's and
// the driver has the extensions; everywhere else the picture travels through shared memory.
struct DmaBuf {
    int fd = -1;
    int fence = -1;  // a sync file signalled once the picture is drawn (-1: it is drawn already)
    unsigned fourcc = 0, stride = 0, offset = 0;
    unsigned long long modifier = 0;
    int width = 0, height = 0;
};
// Whether this context exports textures as DMA-BUFs, and takes them (asked from any thread; `why` says why not).
bool exportsDmaBuf(std::string* why = nullptr);
bool importsDmaBuf(std::string* why = nullptr);
// (inside run()) Half-float texture `texture` shared as `out`, with a fence for what is still being drawn into it where
// the driver gives one (drawn to the end first where it does not). False, with why.
bool exportDmaBuf(unsigned texture, int width, int height, DmaBuf& out, std::string* why = nullptr);
// (inside run()) A texture of this context showing `buf`'s picture, once its fence has signalled; 0, with why.
unsigned importDmaBuf(const DmaBuf& buf, std::string* why = nullptr);
void closeDmaBuf(DmaBuf& buf);  // (its descriptors closed; any thread)
// A DMA-BUF's descriptors handed from one process to another (any thread): the taker opens a post, a socket under a
// name of its own (-1, with why, when none can be made), and the giver sends them there, keeping its own.
int openDmaBufPost(const std::string& name, std::string* why = nullptr);
void closeDmaBufPost(int post);
bool sendDmaBuf(const std::string& post, const DmaBuf& buf);
// What process `pid` last sent to `post`, waited for up to `waitMs`: `buf`'s descriptor, and its fence's when `fence`
// says one came with it. Anything else found waiting there is let go. False, with why, when they did not come whole.
bool receiveDmaBuf(int post, long long pid, bool fence, int waitMs, DmaBuf& buf, std::string* why = nullptr);
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
