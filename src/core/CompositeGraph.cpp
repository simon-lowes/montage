#include "CompositeGraph.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <set>

#include "Effects.h"
#include "GradeGraph.h"
#include "GradeVersions.h"

namespace montage {

const std::vector<std::string>& compositeNodeKinds() {
    static const std::vector<std::string> kinds = {"input", "merge", "transform", "mask", "key", "track", "effect", "grade"};
    return kinds;
}

const std::vector<std::string>& compositeSources() {
    static const std::vector<std::string> sources = {"clip", "media", "track", "generator"};
    return sources;
}

int compositeInputCount(const CompositeNode& n) {
    if (n.kind == "input") return 0;
    if (n.kind == "merge") return kMaxMergeInputs;
    if (n.kind == "mask") return 2;
    return 1;
}

std::string compositeSettingsType(const std::string& kind) {
    if (kind == "merge") return "composite_merge";
    if (kind == "transform") return "transform";
    if (kind == "mask") return "composite_mask";
    if (kind == "track") return "composite_track";
    return {};
}

const Effect* compositeSettings(const CompositeNode& n) {
    const std::string type = compositeSettingsType(n.kind);
    return !type.empty() && !n.effects.empty() && n.effects.front().type == type ? &n.effects.front() : nullptr;
}

Effect* compositeSettings(CompositeNode& n) { return const_cast<Effect*>(compositeSettings(std::as_const(n))); }

const CompositeNode* findCompositeNode(const CompositeGraph& g, int key) {
    for (const CompositeNode& n : g.nodes)
        if (n.key == key) return &n;
    return nullptr;
}

CompositeNode* findCompositeNode(CompositeGraph& g, int key) { return const_cast<CompositeNode*>(findCompositeNode(std::as_const(g), key)); }

const CompositeLink* compositeLinkInto(const CompositeGraph& g, int to, int input) {
    for (const CompositeLink& l : g.links)
        if (l.to == to && l.input == input) return &l;
    return nullptr;
}

bool validComposite(const CompositeGraph& g, std::string* why) {
    auto fail = [&](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    // (bounds first: a project file is untrusted, and the checks below walk the links from each node)
    if (g.nodes.size() > size_t(kMaxCompositeNodes)) return fail("A graph holds at most " + std::to_string(kMaxCompositeNodes) + " nodes");
    if (g.links.size() > g.nodes.size() * size_t(kMaxMergeInputs) + 1) return fail("Too many links");
    const std::vector<std::string>& kinds = compositeNodeKinds();
    const std::vector<std::string>& sources = compositeSources();
    std::set<int> keys;
    std::set<Id> ids;
    for (const CompositeNode& n : g.nodes) {
        if (n.key <= 0 || n.key > kMaxCompositeKey || !keys.insert(n.key).second) return fail("Each node needs a key of its own");
        if (n.id && !ids.insert(n.id).second) return fail("Each node needs an id of its own");
        if (std::find(kinds.begin(), kinds.end(), n.kind) == kinds.end()) return fail("Unknown node kind \"" + n.kind + "\"");
        if (n.kind == "input" && std::find(sources.begin(), sources.end(), n.source) == sources.end())
            return fail("Unknown input source \"" + n.source + "\"");
        if (n.kind == "merge" && n.blend != "depth" && std::find(blendModes().begin(), blendModes().end(), n.blend) == blendModes().end())
            return fail("Unknown blend mode \"" + n.blend + "\"");
    }
    std::set<std::pair<int, int>> fed;
    for (const CompositeLink& l : g.links) {
        const CompositeNode* from = findCompositeNode(g, l.from);
        const CompositeNode* to = l.to ? findCompositeNode(g, l.to) : nullptr;
        if (!from || (l.to && !to)) return fail("A link goes to or from a node that is not there");
        if (l.from == l.to) return fail("A node cannot feed itself");
        if (!fed.insert({l.to, l.input}).second) return fail("An input takes one link");
        if (l.input < 0 || (!to && l.input != 0) || (to && l.input >= compositeInputCount(*to)))
            return fail(to && to->kind == "input" ? "An input node takes no inputs" : "No such input");
    }
    if (!compositeLinkInto(g, 0, 0)) return fail("The graph's output needs a link");
    std::map<int, int> state;  // 1 being visited, 2 done
    std::function<bool(int)> visit = [&](int key) {
        if (state[key] == 2) return true;
        if (state[key] == 1) return false;
        state[key] = 1;
        for (const CompositeLink& l : g.links)
            if (l.to == key && !visit(l.from)) return false;
        state[key] = 2;
        return true;
    };
    for (const CompositeNode& n : g.nodes)
        if (!visit(n.key)) return fail("The links would make a loop");
    return true;
}

std::vector<int> compositeOrder(const CompositeGraph& g) {
    std::vector<int> out;
    std::set<int> done, visiting;
    std::function<void(int)> visit = [&](int key) {
        if (!key || done.count(key) || visiting.count(key)) return;
        visiting.insert(key);
        std::vector<std::pair<int, int>> inputs;  // (input, node)
        for (const CompositeLink& l : g.links)
            if (l.to == key) inputs.emplace_back(l.input, l.from);
        std::sort(inputs.begin(), inputs.end());
        for (const auto& [input, from] : inputs) visit(from);
        visiting.erase(key);
        done.insert(key);
        out.push_back(key);
    };
    if (const CompositeLink* last = compositeLinkInto(g, 0, 0)) visit(last->from);
    return out;
}

int nextCompositeNodeKey(const CompositeGraph& g) {
    int key = 0;
    for (const CompositeNode& n : g.nodes) key = std::max(key, n.key);
    return key + 1;
}

std::string compositeNodeName(const CompositeNode& n) {
    if (!n.label.empty()) return n.label;
    std::string kind = n.kind;
    if (!kind.empty()) kind[0] = char(std::toupper(static_cast<unsigned char>(kind[0])));
    return kind + " " + std::to_string(n.key);
}

CompositeNode makeCompositeNode(Project& p, const std::string& kind) {
    CompositeNode n;
    n.kind = kind;
    n.id = p.newId();
    if (const std::string type = compositeSettingsType(kind); !type.empty()) n.effects.push_back(makeEffect(p, type));
    if (kind == "key") n.effects.push_back(makeEffect(p, "screen_key"));
    return n;
}

CompositeNode* compositeNodeByOwner(Sequence& s, Id owner, Clip** clip) {
    if (!owner) return nullptr;
    for (Track& t : s.videoTracks)
        for (Clip& c : t.clips)
            for (CompositeNode& n : c.composite.nodes)
                if (n.id == owner) {
                    if (clip) *clip = &c;
                    return &n;
                }
    return nullptr;
}

void freshCompositeIds(Project& p, Clip& c) {
    for (CompositeNode& n : c.composite.nodes) {
        n.id = p.newId();
        if (!n.generator.empty()) n.generator.id = p.newId();
        for (Effect& e : n.effects) e.id = p.newId();
    }
}

void separateCompositeIds(Project& p, Sequence& s) {
    std::set<Id> seen;
    for (Track& t : s.videoTracks)
        for (Clip& c : t.clips) {
            bool repeated = false;
            for (const CompositeNode& n : c.composite.nodes) repeated = repeated || !n.id || seen.count(n.id);
            if (repeated) freshCompositeIds(p, c);
            for (const CompositeNode& n : c.composite.nodes) seen.insert(n.id);
        }
}

namespace edit {

namespace {

Clip* pictureClip(Sequence& s, Id clip, int* track = nullptr) {
    for (size_t ti = 0; ti < s.videoTracks.size(); ++ti)
        for (Clip& c : s.videoTracks[ti].clips)
            if (c.id == clip) {
                if (track) *track = int(ti);
                return &c;
            }
    return nullptr;
}

// The graph changed by `change`, kept only when it still holds together.
Result changeComposite(Clip& c, const std::function<Result(CompositeGraph&)>& change) {
    CompositeGraph g = c.composite;
    if (Result r = change(g); !r.ok) return r;
    std::string why;
    if (!validComposite(g, &why)) return Result::fail(why);
    c.composite = std::move(g);
    return {};
}

// What a node names outside the graph is there: its media, a track other than the clip's own, the clip it follows.
Result checkReferences(const Project& p, const Sequence& s, int ownTrack, const CompositeNode& n) {
    if (n.kind == "input" && n.source == "media") {
        const MediaItem* m = p.findMedia(n.media);
        if (!m) return Result::fail("No such media");
        if (m->kind == MediaKind::Audio) return Result::fail("That media has no picture");
    }
    if (n.kind == "input" && n.source == "track") {
        if (n.track < 0 || n.track >= int(s.videoTracks.size())) return Result::fail("No such video track");
        if (n.track == ownTrack) return Result::fail("A clip cannot take its own track as an input (use the clip input)");
    }
    if (n.kind == "input" && n.source == "generator" && (n.generator.empty() || !findEffectInfo(n.generator.type) ||
                                                         findEffectInfo(n.generator.type)->category != EffectCategory::Generator))
        return Result::fail("A generator input needs a generator");
    if (n.kind == "input" && n.source == "generator" && n.generator.type == "adjustment")
        return Result::fail("An adjustment layer changes the tracks under it; use an Effect node instead");
    if (n.kind == "track" && n.follow && !clipById(s, n.follow)) return Result::fail("No such clip to follow");
    return {};
}

// The nodes fed by `key`'s output.
std::vector<CompositeLink> linksFrom(const CompositeGraph& g, int key) {
    std::vector<CompositeLink> out;
    for (const CompositeLink& l : g.links)
        if (l.from == key) out.push_back(l);
    return out;
}

// A merge's inputs numbered from 0 again, in order, after one went.
void compactMergeInputs(CompositeGraph& g, int key) {
    std::vector<CompositeLink*> in;
    for (CompositeLink& l : g.links)
        if (l.to == key) in.push_back(&l);
    std::sort(in.begin(), in.end(), [](const CompositeLink* a, const CompositeLink* b) { return a->input < b->input; });
    for (size_t i = 0; i < in.size(); ++i) in[i]->input = int(i);
}

}  // namespace

Result startComposite(Project& p, Sequence& s, Id clip, int* created) {
    Clip* c = pictureClip(s, clip);
    if (!c) return Result::fail("A compositing graph is for picture clips");
    if (!c->composite.empty()) return Result::fail("The clip has a compositing graph already");
    CompositeNode in = makeCompositeNode(p, "input");
    in.key = 1;
    c->composite.nodes.push_back(std::move(in));
    c->composite.links.push_back({1, 0, 0});
    if (created) *created = 1;
    return {};
}

Result addCompositeNode(Project& p, Sequence& s, Id clip, const std::string& kind, int after, int* created) {
    const std::vector<std::string>& kinds = compositeNodeKinds();
    if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end()) return Result::fail("Unknown node kind \"" + kind + "\"");
    Clip* c = pictureClip(s, clip);
    if (!c) return Result::fail("A compositing graph is for picture clips");
    if (c->composite.empty())
        if (Result r = startComposite(p, s, clip); !r.ok) return r;
    if (!after)
        if (const CompositeLink* out = compositeLinkInto(c->composite, 0, 0)) after = out->from;
    const CompositeNode* anchor = findCompositeNode(c->composite, after);
    if (!anchor) return Result::fail("No such node");
    CompositeNode n = makeCompositeNode(p, kind);
    n.key = nextCompositeNodeKey(c->composite);
    n.x = anchor->x + 160, n.y = anchor->y;
    const int key = n.key;
    const bool anchorIsMerge = anchor->kind == "merge";
    Result r = changeComposite(*c, [&](CompositeGraph& g) -> Result {
        if (kind == "input") {
            n.y += 120;
            if (anchorIsMerge) {
                int inputs = 0;
                for (const CompositeLink& l : g.links) inputs += l.to == after;
                if (inputs >= kMaxMergeInputs) return Result::fail("The merge takes no more inputs");
                n.x -= 320;
                g.links.push_back({key, after, inputs});
            }
            g.nodes.push_back(std::move(n));
            return {};
        }
        // In series after the anchor: what it fed now takes the new node.
        for (CompositeLink& l : g.links)
            if (l.from == after) l.from = key;
        g.links.push_back({after, key, 0});
        for (CompositeNode& m : g.nodes)
            if (m.x > n.x - 1e-9 && m.key != after && std::fabs(m.y - n.y) < 1e-9) m.x += 160;
        g.nodes.push_back(std::move(n));
        return {};
    });
    if (r.ok && created) *created = key;
    return r;
}

Result removeCompositeNode(Sequence& s, Id clip, int key) {
    Clip* c = pictureClip(s, clip);
    if (!c || !findCompositeNode(c->composite, key)) return Result::fail("No such node");
    if (c->composite.nodes.size() == 1) {
        c->composite = {};  // (nothing left: the picture as it comes)
        return {};
    }
    return changeComposite(*c, [&](CompositeGraph& g) -> Result {
        const CompositeLink* first = compositeLinkInto(g, key, 0);
        const int feeder = first ? first->from : 0;
        const std::vector<CompositeLink> fed = linksFrom(g, key);
        std::erase_if(g.links, [&](const CompositeLink& l) { return l.from == key || l.to == key; });
        std::erase_if(g.nodes, [&](const CompositeNode& n) { return n.key == key; });
        if (feeder)
            for (const CompositeLink& l : fed) g.links.push_back({feeder, l.to, l.input});
        for (const CompositeLink& l : fed)
            if (const CompositeNode* m = findCompositeNode(g, l.to); m && m->kind == "merge") compactMergeInputs(g, l.to);
        if (!compositeLinkInto(g, 0, 0)) return Result::fail("Nothing would be left to feed the output: link another node to it first");
        return {};
    });
}

Result connectCompositeNodes(Sequence& s, Id clip, const CompositeLink& link) {
    Clip* c = pictureClip(s, clip);
    if (!c || c->composite.empty()) return Result::fail("The clip has no compositing graph");
    if (const CompositeLink* l = compositeLinkInto(c->composite, link.to, link.input); l && *l == link) return Result::fail("");
    if (const CompositeNode* m = findCompositeNode(c->composite, link.to); m && m->kind == "merge") {
        int inputs = 0;
        for (const CompositeLink& l : c->composite.links) inputs += l.to == link.to;
        if (link.input > inputs) return Result::fail("A merge's inputs are numbered from 0: the next one is " + std::to_string(inputs));
    }
    return changeComposite(*c, [&](CompositeGraph& g) -> Result {
        std::erase_if(g.links, [&](const CompositeLink& l) { return l.to == link.to && l.input == link.input; });
        g.links.push_back(link);
        return {};
    });
}

Result disconnectCompositeNodes(Sequence& s, Id clip, int to, int input) {
    Clip* c = pictureClip(s, clip);
    if (!c || c->composite.empty()) return Result::fail("The clip has no compositing graph");
    if (!to) return Result::fail("The graph's output needs a link");
    if (!compositeLinkInto(c->composite, to, input)) return Result::fail("");
    return changeComposite(*c, [&](CompositeGraph& g) -> Result {
        std::erase_if(g.links, [&](const CompositeLink& l) { return l.to == to && l.input == input; });
        if (const CompositeNode* m = findCompositeNode(g, to); m && m->kind == "merge") compactMergeInputs(g, to);
        return {};
    });
}

Result setCompositeNode(Project& p, Sequence& s, Id clip, const CompositeNode& node) {
    int track = -1;
    Clip* c = pictureClip(s, clip, &track);
    if (!c) return Result::fail("No such clip");
    const CompositeNode* n = findCompositeNode(c->composite, node.key);
    if (!n) return Result::fail("No such node");
    CompositeNode changed = *n;
    changed.label = node.label;
    changed.enabled = node.enabled;
    changed.x = node.x;
    changed.y = node.y;
    if (n->kind == "input") {
        changed.source = node.source;
        changed.media = node.source == "media" ? node.media : 0;
        changed.track = node.source == "track" ? node.track : -1;
        changed.offset = node.source == "media" ? node.offset : 0;
        if (node.source != "generator") changed.generator = {};
        else if (node.generator.type != n->generator.type) {
            changed.generator = node.generator.empty() ? Effect() : makeEffect(p, node.generator.type);
            for (const auto& [k, v] : node.generator.params) changed.generator.params[k] = v;
            for (const auto& [k, v] : node.generator.strings) changed.generator.strings[k] = v;
        } else {
            changed.generator.params = node.generator.params;
            changed.generator.strings = node.generator.strings;
        }
    }
    if (n->kind == "merge") changed.blend = node.blend;
    if (n->kind == "track") changed.follow = node.follow;
    if (Result r = checkReferences(p, s, track, changed); !r.ok) return r;
    if (changed == *n) return Result::fail("");
    return changeComposite(*c, [&](CompositeGraph& g) -> Result {
        *findCompositeNode(g, node.key) = changed;
        return {};
    });
}

Result setCompositeNodeParams(Sequence& s, Id clip, int key, const std::vector<std::pair<std::string, double>>& values) {
    Clip* c = pictureClip(s, clip);
    CompositeNode* n = c ? findCompositeNode(c->composite, key) : nullptr;
    if (!n) return Result::fail("No such node");
    Effect* e = n->kind == "input" && n->source == "generator" ? &n->generator : compositeSettings(*n);
    if (!e || e->empty()) return Result::fail("The node has no settings of that kind");
    std::vector<std::string> names;
    for (const ParamInfo& pi : effectParams(*e)) names.push_back(pi.name);
    for (const auto& [name, v] : values)
        if (std::find(names.begin(), names.end(), name) == names.end() && !e->params.count(name) &&
            !(name.rfind("mask.", 0) == 0 && supportsMask(e->type)))  // (a mask's shape, as on any picture effect)
            return Result::fail("No setting \"" + name + "\" on " + compositeNodeName(*n));
    for (const auto& [name, v] : values) e->params[name] = Param(v);
    return {};
}

Result gradeNodeFromClip(Project& p, Sequence& s, Id clip, int key, Id from) {
    Clip* c = pictureClip(s, clip);
    CompositeNode* n = c ? findCompositeNode(c->composite, key) : nullptr;
    if (!n || n->kind != "grade") return Result::fail("No such grade node");
    const Clip* src = clipById(s, from);
    if (!src) return Result::fail("No such clip");
    std::vector<Effect> fx;
    for (const Effect& e : src->effects)
        if (isGradeEffect(e) || (e.node && findGradeNode(src->grade, e.node))) {
            Effect x = e;
            x.id = p.newId();
            fx.push_back(std::move(x));
        }
    n->effects = std::move(fx);
    n->grade = src->grade;
    return {};
}

Result removeComposite(Sequence& s, Id clip) {
    Clip* c = pictureClip(s, clip);
    if (!c || c->composite.empty()) return Result::fail("The clip has no compositing graph");
    c->composite = {};
    return {};
}

Result applyComposite(Project& p, Sequence& s, Id clip, const CompositeGraph& g) {
    int track = -1;
    Clip* c = pictureClip(s, clip, &track);
    if (!c) return Result::fail("A compositing graph is for picture clips");
    std::string why;
    if (!validComposite(g, &why)) return Result::fail(why);
    for (const CompositeNode& n : g.nodes)
        if (Result r = checkReferences(p, s, track, n); !r.ok) return Result::fail(compositeNodeName(n) + ": " + r.error);
    c->composite = g;
    freshCompositeIds(p, *c);
    return {};
}

}  // namespace edit

}  // namespace montage
