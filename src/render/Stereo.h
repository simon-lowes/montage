// Montage — stereoscopic 3D (Premiere's and Resolve's stereoscopic workflows, Final Cut's 360° and spatial video): a
// stereo sequence is rendered once per eye, stereo footage giving each eye its own picture (media/Decoder.h reads the
// eye's half of a side-by-side or top-and-bottom frame) and clips' Stereo 3D effect setting their depth by moving the
// eyes apart. The two eyes are then shown or delivered as one picture: either eye alone, a red-cyan anaglyph for
// checking depth with glasses, the eyes side by side or top and bottom (full size, or squeezed into one frame as 3D TV
// and Blu-ray masters are), or their difference, which shows where the eyes disagree.
#pragma once

#include <string>
#include <vector>

#include "media/Image.h"

namespace montage {

enum class StereoView { Left, Right, Anaglyph, SideBySide, SideBySideHalf, TopBottom, TopBottomHalf, Difference };

// Names as Montage's files, exports and MCP use them: "left", "right", "anaglyph", "sbs", "sbs_half", "tb", "tb_half",
// "difference".
const std::vector<std::string>& stereoViewNames();
bool stereoViewFromName(const std::string& name, StereoView& out);
std::string stereoViewName(StereoView v);
// How many eyes across and down the picture is (2 and 1 for full side by side, 1 and 2 for full top and bottom).
void stereoPacking(StereoView v, int& across, int& down);

// The two eyes (the same size) as one picture.
Image combineStereo(const Image& left, const Image& right, StereoView view);

}  // namespace montage
