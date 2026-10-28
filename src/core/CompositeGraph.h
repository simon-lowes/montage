// Montage — a clip's compositing node graph: pictures brought in (the clip, any bin
// item, what any video track shows, a generator such as a title or a 3D model), joined by merges and changed by
// transforms, masks, keys, trackers, effects and grades, the output becoming the clip's picture where its track shows
// it. Rendered by render/CompositeRender.h through the same renderer as the timeline.
//
// What each kind of node takes and holds:
//   input      no inputs. Its picture is its `source`: "clip" (the clip itself as its track would show it: its effects,
//              grade and transform), "media" (bin item `media`, placed as a new clip would be, from `offset` frames in),
//              "track" (what video track `track` shows at the time) or "generator" (`generator`: a title, a 3D model or
//              Gaussian splats, a colour...). Its effects, if it is given any, run on that picture.
//   merge      input 0 the background, then each layer over the ones before it in order, blended by `blend` (one of
//              Montage's blend modes) at its settings' opacity; or with `blend` "depth", every input merged by depth:
//              deep pictures and 3D models at their own depths, flat pictures behind them.
//   transform  one input, moved within the frame by its settings (a "transform": the clip Transform's position, scale,
//              rotation, anchor, crop and opacity).
//   mask       input 0 the picture, input 1 (optional) a picture whose alpha or luma is the mask: the picture's alpha
//              multiplied by that and by its settings' mask (a "composite_mask": shapes, qualifiers, objects, people).
//   key        one input, through its effects (keyers: Chroma Key, Keyer, Luma Key, Remove Background) in order.
//   track      one input, moved as clip `follow`'s transform moves from its settings' reference frame (position, scale,
//              rotation) or put through that clip's Corner Pin (planar tracking keys it).
//   effect     one input, through its effects (any video effects) in order.
//   grade      one input, through its effects as a grade: in `grade`'s colour nodes when it has some (core/GradeGraph.h).
// A node switched off passes its first input on unchanged (an input switched off gives nothing). Settings are the
// node's first effect (merge, transform, mask, track); its other effects are its own.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "EditOps.h"
#include "Model.h"

namespace montage {

const std::vector<std::string>& compositeNodeKinds();
const std::vector<std::string>& compositeSources();
// How many inputs a node takes (a merge: up to kMaxMergeInputs).
constexpr int kMaxMergeInputs = 32;
// A graph's most nodes, and its keys' highest.
constexpr int kMaxCompositeNodes = 512;
constexpr int kMaxCompositeKey = 1000000;
int compositeInputCount(const CompositeNode& n);
// The type of the settings effect a kind keeps first ("" for kinds without one).
std::string compositeSettingsType(const std::string& kind);
const Effect* compositeSettings(const CompositeNode& n);
Effect* compositeSettings(CompositeNode& n);

const CompositeNode* findCompositeNode(const CompositeGraph& g, int key);
CompositeNode* findCompositeNode(CompositeGraph& g, int key);
// The link into `to`'s input `input` (to 0: the graph's output), or null.
const CompositeLink* compositeLinkInto(const CompositeGraph& g, int to, int input);
// Whether the graph holds together: unique keys and ids, known kinds and sources, links between nodes there, inputs
// each fed once and within what the node takes, no loop, and the output fed.
bool validComposite(const CompositeGraph& g, std::string* why = nullptr);
// The nodes the output depends on, each after those feeding it.
std::vector<int> compositeOrder(const CompositeGraph& g);
int nextCompositeNodeKey(const CompositeGraph& g);
// "Merge 3", or its label.
std::string compositeNodeName(const CompositeNode& n);

// A new node of `kind` with its settings at their defaults (and an input's source the clip), ids from `p`.
CompositeNode makeCompositeNode(Project& p, const std::string& kind);
// The node whose effects `owner` names, in any clip of the sequence (and that clip).
CompositeNode* compositeNodeByOwner(Sequence& s, Id owner, Clip** clip = nullptr);
// Fresh ids for a copied clip's nodes and their effects (each node's id is the owner of its effects, so two clips
// cannot share one). On reading a project, separateCompositeIds gives fresh ones to any shared between clips.
void freshCompositeIds(Project& p, Clip& c);
void separateCompositeIds(Project& p, Sequence& s);
// (As JSON text, for presets and the MCP tool: core/ProjectIO.h compositeGraphToJsonString.)

namespace edit {

// A graph for the clip: an input of the clip itself feeding the output (renders as the clip did). `created` gets the
// input's key.
Result startComposite(Project& p, Sequence& s, Id clip, int* created = nullptr);
// A new node of `kind` placed after node `after` (0: the node feeding the output): a one-input node (or a mask) takes
// its picture and feeds what it fed; a merge takes it as its background and feeds what it fed; an input feeds the
// merge `after` as its next layer when `after` is one, else nothing yet. A clip without a graph gets one first.
Result addCompositeNode(Project& p, Sequence& s, Id clip, const std::string& kind, int after = 0, int* created = nullptr);
// The node gone, what it fed taking what fed its first input.
Result removeCompositeNode(Sequence& s, Id clip, int key);
// A link (replacing the one into the same input); refused when the graph would not hold together.
Result connectCompositeNodes(Sequence& s, Id clip, const CompositeLink& link);
// The link into `to`'s input `input` gone (not the output's).
Result disconnectCompositeNodes(Sequence& s, Id clip, int to, int input);
// A node's label, switch, place, source, media, track, offset, generator, blend and followed clip (by its key; its kind,
// id, effects and grade are kept). An input's media, track and followed clip must be there; a track input cannot be
// the clip's own track.
Result setCompositeNode(Project& p, Sequence& s, Id clip, const CompositeNode& node);
// Values for the node's settings effect (a generator input's: its generator's).
Result setCompositeNodeParams(Sequence& s, Id clip, int key, const std::vector<std::pair<std::string, double>>& values);
// A grade node given clip `from`'s grade: its colour effects and colour nodes (shared nodes still shared).
Result gradeNodeFromClip(Project& p, Sequence& s, Id clip, int key, Id from);
// The clip's graph gone: its picture as it comes.
Result removeComposite(Sequence& s, Id clip);
// The clip's graph replaced by `g` (a preset), its nodes and effects given fresh ids; refused when it does not hold
// together or names media, tracks or clips the sequence lacks.
Result applyComposite(Project& p, Sequence& s, Id clip, const CompositeGraph& g);

}  // namespace edit

}  // namespace montage
