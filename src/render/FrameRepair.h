// Montage — damaged-frame repair: frames a recording or a transfer broke (the decoder's errors, blocky digital hits, tape
// drop-out lines, a single flash or black frame, a frame missing from the timestamps) found by comparing each frame with
// the one its neighbours make between them, and rebuilt from the good frames either side. Used by the Frame Repair
// effect (render/Compositor.cpp, where the source frame is read), Clip › Repair Damaged Frames… and MCP
// `montage_find_damaged_frames`.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "media/Image.h"

namespace montage {

struct Project;
struct Sequence;
struct Clip;

enum class FrameDamage { Decode, Hit, Dropout, Flash, Black, Missing };
std::string frameDamageName(FrameDamage d);  // "decode", "hit", "dropout", "flash", "black", "missing"
bool frameDamageFromName(const std::string& name, FrameDamage& out);
// Whether only part of the frame is broken (blocks or lines), so only that part is replaced.
inline bool localDamage(FrameDamage d) { return d == FrameDamage::Hit || d == FrameDamage::Dropout; }

struct DamagedFrame {
    int64_t frame = 0;  // in the file, from 0
    FrameDamage kind = FrameDamage::Hit;
    double extent = 1;  // the share of the picture broken (1 for a whole frame)
};

// The file's frames [first, last) (last < 0: to its end) that are broken. A frame is damaged when the decoder says so,
// when it repeats the frame before it because its own time is missing, when it is black or a flat flash between frames
// that are not, or when it differs from the picture its neighbours make between them (along their motion) where they
// are alike: in whole lines (drop-out) or in patches (a hit). A camera flash keeps its picture's structure and is left
// alone, and no frame next to a cut counts. Analysed at most 480 pixels wide.
std::vector<DamagedFrame> findDamagedFrames(const std::string& path, int64_t first = 0, int64_t last = -1,
                                            const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                                            std::string* error = nullptr);

// The Frame Repair effect's list ("frames"): a line for each frame, "<frame> <kind>".
std::map<int64_t, FrameDamage> damagedFramesFromString(const std::string& s);
std::string damagedFramesToString(const std::map<int64_t, FrameDamage>& frames);

// Rebuilds `img`, the file's frame `n` (at the size `img` has), when the list names it: made from the nearest good
// frames either side by `fetch` (RIFE when its model is installed, else along the optical flow), the whole of it or,
// for a hit or drop-out, only where it differs from what is made. `show` tints what was replaced. False when `n` is not
// listed or no good frame was found near it.
bool repairFrame(const std::map<int64_t, FrameDamage>& frames, int64_t n, Image& img,
                 const std::function<bool(int64_t frame, Image& out)>& fetch, const std::string& cacheKey, bool show = false);

// The damaged frames of the stretch of the clip's file it plays (and a frame either side), in file frames. False (with
// `error`) when the clip is not video or its media cannot be read.
bool damagedFramesInClip(const Project& p, const Sequence& s, const Clip& c, std::vector<DamagedFrame>& out,
                         const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                         std::string* error = nullptr);
// Lists `frames` on the clip's Frame Repair effect and marks them, as repairClip does. How many it did not list before.
int listDamagedFrames(Project& p, Sequence& s, Clip& c, const std::vector<DamagedFrame>& frames);

// For a video clip: the damaged frames of the stretch of its file it plays, found (`found`, in file frames) and listed
// on its Frame Repair effect (one added when it has none), each marked on the timeline (red, "Damaged: <kind>") where the
// clip shows it. Returns how many frames the effect lists that it did not before; -1 (with `error`) when the clip's
// media cannot be read.
int repairClip(Project& p, Sequence& s, Clip& c, std::vector<DamagedFrame>* found = nullptr, const std::function<void(double)>& progress = {},
               const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);

}  // namespace montage
