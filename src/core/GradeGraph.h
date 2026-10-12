// Montage — a clip's grade as a node graph (Resolve's Color page nodes; Premiere 26.2's colour layers): correctors
// joined in series, side by side through a parallel mixer (each branch from the same picture, averaged, or each where
// its key is), stacked through a layer mixer (each input over the ones before it, with a blend mode, where its key
// is), and keys passed from one node to another (an outside node: the rest of the picture). A node's effects are the
// clip's effects marked with its key (Effect::node), or a shared node's (Sequence::sharedNodes), edited once for
// every clip using it. A clip without a graph runs its effects as a stack, as before; "Convert to Nodes" makes each
// colour effect a node of its own, in series, which renders the same.
#pragma once

#include <string>
#include <vector>

#include "EditOps.h"
#include "Model.h"

namespace montage {

// The node kinds: "corrector" (effects, its window, qualifier and key input), "parallel" and "layer" (mixers),
// "key_mixer" (keys combined).
bool isGradeMixer(const GradeNode& n);
const GradeNode* findGradeNode(const GradeGraph& g, int key);
GradeNode* findGradeNode(GradeGraph& g, int key);
// The link into `to`'s input `input` (to 0: the graph's output), or null.
const GradeLink* gradeLinkInto(const GradeGraph& g, int to, int input);
// Whether the graph holds together: unique keys, known kinds, links between nodes there, each input fed once, picture
// to picture inputs and keys to key inputs, no loop, and the output fed by a picture. An input left unconnected takes
// the clip's picture (a picture input) or no key (a key input).
bool validGrade(const GradeGraph& g, std::string* why = nullptr);
// The nodes the output depends on, each after those feeding it (by key where there is a choice).
std::vector<int> gradeOrder(const GradeGraph& g);
// Whether it is a plain chain of correctors from the picture to the output (with nothing keyed between them).
bool serialGrade(const GradeGraph& g);
// A key for a new node.
int nextGradeNodeKey(const GradeGraph& g);
// A corrector's settings (its "grade_node" effect), or null for the defaults.
const Effect* gradeNodeSettings(const Clip& c, int key);
// A corrector's effects in order: the clip's own marked with its key (its settings aside), or its shared node's.
std::vector<const Effect*> gradeNodeEffects(const Sequence& s, const Clip& c, int key);
const SharedNode* findSharedNode(const Sequence& s, Id id);
// "Node 2", or its label.
std::string gradeNodeName(const GradeNode& n);

namespace edit {

enum class NodeHow { Serial, Parallel, Layer, Outside };

// The clip's colour effects as a chain of nodes, one each, in their order (an empty node when it has none).
Result convertToNodes(Sequence& s, Id clip);
// A new corrector after node `anchor` (0: the last before the output): in series; beside it through a parallel or
// layer mixer (joining the mixer it already feeds); or in series taking its key inverted (the outside). A clip
// without a graph is converted first. `created` gets the new corrector's key.
Result addGradeNode(Project& p, Sequence& s, Id clip, NodeHow how, int anchor = 0, int* created = nullptr);
// A key mixer joining the keys of `keys` (correctors or mixers), and its key into corrector `to`'s key input.
Result addKeyMixer(Sequence& s, Id clip, const std::vector<int>& keys, int to, int* created = nullptr);
// The node gone (its effects with it), what it fed taking what fed it.
Result removeGradeNode(Sequence& s, Id clip, int key);
// A link (replacing the one into the same input); refused when the graph would not hold together.
Result connectGradeNodes(Sequence& s, Id clip, const GradeLink& link);
// The link into `to`'s input `input` gone (not the output's).
Result disconnectGradeNodes(Sequence& s, Id clip, int to, int input);
// A node's label, switch, blend modes and place (by its key; its kind and sharing are kept).
Result setGradeNode(Sequence& s, Id clip, const GradeNode& node);
// The effect into corrector `key` (0: outside the graph, after it).
Result moveEffectToNode(Sequence& s, Id clip, Id effect, int key);
// No graph: the nodes' effects stay, as a stack; the nodes' settings go.
Result flattenGrade(Sequence& s, Id clip);
// Corrector `key`'s effects into a new shared node named `name`, which the node then shows.
Result shareGradeNode(Project& p, Sequence& s, Id clip, int key, const std::string& name, Id* created = nullptr);
// A new corrector at the end of the clip's graph showing shared node `shared`.
Result useSharedNode(Project& p, Sequence& s, Id clip, Id shared, int* created = nullptr);
// The shared node's effects copied into the node, which no longer follows it.
Result unshareGradeNode(Project& p, Sequence& s, Id clip, int key);

}  // namespace edit

}  // namespace montage
