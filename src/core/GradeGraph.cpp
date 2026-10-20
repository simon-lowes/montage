#include "GradeGraph.h"

#include <algorithm>
#include <functional>
#include <set>

#include "Effects.h"
#include "GradeVersions.h"

namespace montage {

bool runsBeforeGrade(const std::string& type) {
    static const std::set<std::string> types = {"video_denoise", "super_scale", "motion_deblur", "reframe_360", "redact_faces",
                                                "stereo_3d",     "motion_blur", "deflicker",     "stabilize",   "rolling_shutter",
                                                "lens_correction", "camera_track", "dirt_removal", "dust_spots", "lip_sync", "fade_restore",
                                                "film_negative", "perf_lock"};
    return types.count(type) > 0;
}

bool alwaysRanBeforeGrade(const std::string& type) {
    return runsBeforeGrade(type) && type != "stabilize" && type != "rolling_shutter" && type != "lens_correction";
}

bool isGradeMixer(const GradeNode& n) { return n.kind == "parallel" || n.kind == "layer"; }

const GradeNode* findGradeNode(const GradeGraph& g, int key) {
    for (const GradeNode& n : g.nodes)
        if (n.key == key) return &n;
    return nullptr;
}

GradeNode* findGradeNode(GradeGraph& g, int key) { return const_cast<GradeNode*>(findGradeNode(std::as_const(g), key)); }

const GradeLink* gradeLinkInto(const GradeGraph& g, int to, int input) {
    for (const GradeLink& l : g.links)
        if (l.to == to && l.input == input) return &l;
    return nullptr;
}

bool validGrade(const GradeGraph& g, std::string* why) {
    auto fail = [&](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    std::set<int> keys;
    for (const GradeNode& n : g.nodes) {
        if (n.key <= 0 || !keys.insert(n.key).second) return fail("Each node needs a key of its own");
        if (n.kind != "corrector" && !isGradeMixer(n) && n.kind != "key_mixer") return fail("Unknown node kind \"" + n.kind + "\"");
    }
    std::set<std::pair<int, int>> fed;
    for (const GradeLink& l : g.links) {
        const GradeNode* from = l.from ? findGradeNode(g, l.from) : nullptr;
        const GradeNode* to = l.to ? findGradeNode(g, l.to) : nullptr;
        if ((l.from && !from) || (l.to && !to)) return fail("A link goes to or from a node that is not there");
        if (l.from && l.from == l.to) return fail("A node cannot feed itself");
        if (!fed.insert({l.to, l.input}).second) return fail("An input takes one link");
        if (l.input < 0) return fail("No such input");
        // What leaves: the clip's picture (no key), a key mixer's key, or a node's picture or key.
        if (!l.from && l.key) return fail("The clip's picture has no key");
        if (from && from->kind == "key_mixer" && !l.key) return fail("A key mixer gives a key, not a picture");
        // What it goes into.
        if (!to) {
            if (l.input != 0 || l.key) return fail("The output takes a picture");
        } else if (to->kind == "corrector") {
            if (l.input > 1) return fail("A corrector has a picture input and a key input");
            if ((l.input == 1) != l.key) return fail(l.key ? "A key goes into a key input" : "A picture goes into a picture input");
        } else if (to->kind == "key_mixer") {
            if (!l.key) return fail("A key mixer takes keys");
        } else if (l.key) {
            return fail("A mixer takes pictures");
        }
    }
    if (!gradeLinkInto(g, 0, 0)) return fail("The graph's output needs a link");
    // No loop: each node's inputs are settled before it.
    std::map<int, int> state;  // 1 being visited, 2 done
    std::function<bool(int)> visit = [&](int key) {
        if (!key || state[key] == 2) return true;
        if (state[key] == 1) return false;
        state[key] = 1;
        for (const GradeLink& l : g.links)
            if (l.to == key && !visit(l.from)) return false;
        state[key] = 2;
        return true;
    };
    for (const GradeNode& n : g.nodes)
        if (!visit(n.key)) return fail("The links would make a loop");
    return true;
}

bool liveGradeNode(const GradeGraph& g, int key) {
    const GradeNode* n = findGradeNode(g, key);
    if (!n || !n->enabled) return false;
    const std::vector<int> order = gradeOrder(g);
    return std::find(order.begin(), order.end(), key) != order.end();
}

std::vector<int> gradeOrder(const GradeGraph& g) {
    std::vector<int> out;
    std::set<int> done, visiting;
    std::function<void(int)> visit = [&](int key) {
        if (!key || done.count(key) || visiting.count(key)) return;
        visiting.insert(key);
        std::vector<int> inputs;
        for (const GradeLink& l : g.links)
            if (l.to == key && l.from) inputs.push_back(l.from);
        std::sort(inputs.begin(), inputs.end());
        for (int k : inputs) visit(k);
        visiting.erase(key);
        done.insert(key);
        out.push_back(key);
    };
    if (const GradeLink* last = gradeLinkInto(g, 0, 0)) visit(last->from);
    return out;
}

bool serialGrade(const GradeGraph& g) {
    if (g.empty()) return true;
    for (const GradeLink& l : g.links)
        if (l.key) return false;
    for (const GradeNode& n : g.nodes)
        if (n.kind != "corrector") return false;
    // From the output back to the picture, through every node once.
    std::set<int> seen;
    const GradeLink* l = gradeLinkInto(g, 0, 0);
    while (l && l->from) {
        if (!seen.insert(l->from).second) return false;
        l = gradeLinkInto(g, l->from, 0);
    }
    return l && seen.size() == g.nodes.size() && g.links.size() == g.nodes.size() + 1;
}

int nextGradeNodeKey(const GradeGraph& g) {
    int k = 0;
    for (const GradeNode& n : g.nodes) k = std::max(k, n.key);
    return k + 1;
}

const Effect* gradeNodeSettings(const Clip& c, int key) {
    for (const Effect& e : c.effects)
        if (e.node == key && e.type == "grade_node") return &e;
    return nullptr;
}

const SharedNode* findSharedNode(const Sequence& s, Id id) {
    for (const SharedNode& n : s.sharedNodes)
        if (id && n.id == id) return &n;
    return nullptr;
}

std::vector<const Effect*> gradeNodeEffects(const Sequence& s, const Clip& c, int key) {
    std::vector<const Effect*> out;
    const GradeNode* n = findGradeNode(c.grade, key);
    if (!n) return out;
    if (n->shared) {
        if (const SharedNode* sn = findSharedNode(s, n->shared))
            for (const Effect& e : sn->effects) out.push_back(&e);
        return out;
    }
    for (const Effect& e : c.effects)
        if (e.node == key && e.type != "grade_node") out.push_back(&e);
    return out;
}

std::string gradeNodeName(const GradeNode& n) {
    if (!n.label.empty()) return n.label;
    const std::string kind = n.kind == "parallel" ? "Parallel Mixer" : n.kind == "layer" ? "Layer Mixer" : n.kind == "key_mixer" ? "Key Mixer" : "Node";
    return kind + " " + std::to_string(n.key);
}

std::vector<SharedNode> sharedNodesUsed(const Sequence& s, const Clip& c) {
    std::vector<SharedNode> out;
    auto take = [&](const GradeGraph& g) {
        for (const GradeNode& n : g.nodes)
            if (const SharedNode* sn = findSharedNode(s, n.shared))
                if (std::none_of(out.begin(), out.end(), [&](const SharedNode& x) { return x.id == sn->id; })) out.push_back(*sn);
    };
    take(c.grade);
    for (const GradeVersion& v : c.gradeVersions) take(v.grade);
    return out;
}

std::map<Id, Id> adoptSharedNodes(Project& p, Sequence& s, const std::vector<SharedNode>& nodes) {
    std::map<Id, Id> ids;
    for (const SharedNode& n : nodes) {
        if (findSharedNode(s, n.id)) {
            ids[n.id] = n.id;
            continue;
        }
        // One of this project's (from another of its sequences) keeps its id; one from elsewhere gets fresh ids, which
        // could otherwise be anything's here.
        bool ours = false;
        for (const Sequence& q : p.sequences) ours = ours || findSharedNode(q, n.id);
        SharedNode copy = n;
        if (!ours) {
            copy.id = p.newId();
            for (Effect& e : copy.effects) e.id = p.newId();
        }
        ids[n.id] = copy.id;
        s.sharedNodes.push_back(std::move(copy));
    }
    return ids;
}

void remapSharedNodes(GradeGraph& g, const std::map<Id, Id>& ids) {
    for (GradeNode& n : g.nodes)
        if (auto it = ids.find(n.shared); n.shared && it != ids.end()) n.shared = it->second;
}

void remapSharedNodes(Clip& c, const std::map<Id, Id>& ids) {
    remapSharedNodes(c.grade, ids);
    for (GradeVersion& v : c.gradeVersions) remapSharedNodes(v.grade, ids);
}

namespace {

// How many effects outside the graph run before it (it runs where its first node's effect is, else after them all).
size_t graphSlot(const Clip& c) {
    size_t n = 0;
    for (const Effect& e : c.effects) {
        if (e.node) return n;
        ++n;
    }
    return n;
}

// The nodes' effects together after `slot` effects outside the graph, so an edit taking or adding node effects
// leaves the graph where it ran; a graph whose nodes have no effects of their own there (all shared, or empty) is held
// there by the first corrector's settings.
void keepGraphAt(Project& p, Clip& c, size_t slot) {
    if (c.grade.empty()) return;
    std::vector<Effect> outside, inside;
    for (Effect& e : c.effects) (e.node ? inside : outside).push_back(std::move(e));
    slot = std::min(slot, outside.size());
    if (inside.empty() && slot < outside.size()) {
        for (int key : gradeOrder(c.grade))
            if (const GradeNode* n = findGradeNode(c.grade, key); n && n->kind == "corrector") {
                Effect anchor = makeEffect(p, "grade_node");
                anchor.node = key;
                inside.push_back(std::move(anchor));
                break;
            }
    }
    c.effects.clear();
    c.effects.insert(c.effects.end(), std::make_move_iterator(outside.begin()), std::make_move_iterator(outside.begin() + std::ptrdiff_t(slot)));
    c.effects.insert(c.effects.end(), std::make_move_iterator(inside.begin()), std::make_move_iterator(inside.end()));
    c.effects.insert(c.effects.end(), std::make_move_iterator(outside.begin() + std::ptrdiff_t(slot)), std::make_move_iterator(outside.end()));
}

}  // namespace

void separateEffectIds(Project& p, const Sequence& s, Clip& c) {
    std::set<Id> taken;
    for (const SharedNode& n : s.sharedNodes)
        for (const Effect& e : n.effects) taken.insert(e.id);
    std::set<Id> seen;
    for (Effect& e : c.effects)
        if (taken.count(e.id) || !seen.insert(e.id).second) {
            e.id = p.newId();
            seen.insert(e.id);
        }
}

std::vector<Effect> flattenedEffects(const Sequence& s, const Clip& c) {
    if (c.grade.empty()) return c.effects;
    std::vector<Effect> graph;
    // (a graph that does not hold together: every node, in their order)
    std::vector<int> order = gradeOrder(c.grade);
    if (!validGrade(c.grade)) {
        order.clear();
        for (const GradeNode& n : c.grade.nodes) order.push_back(n.key);
    }
    for (int key : order) {
        const GradeNode* n = findGradeNode(c.grade, key);
        if (!n || n->kind != "corrector") continue;
        // A node's window goes onto its effects without one of their own (its key input and mix cannot follow).
        const Effect* st = gradeNodeSettings(c, key);
        const bool window = st && st->enabled && hasMask(*st, 0);
        for (const Effect* x : gradeNodeEffects(s, c, key)) {
            Effect copy = *x;
            copy.node = 0;
            copy.enabled = copy.enabled && n->enabled;
            if (window && !hasMask(copy, 0))
                for (const auto& [name, prm] : st->params)
                    if (name.rfind("mask.", 0) == 0) copy.params[name] = prm;
            if (window && !hasMask(copy, 0)) copy.strings.insert(st->strings.begin(), st->strings.end());
            graph.push_back(copy);
        }
    }
    std::vector<Effect> out;
    const size_t slot = graphSlot(c);
    size_t seen = 0;
    for (const Effect& e : c.effects) {
        if (e.node) continue;
        if (seen++ == slot) out.insert(out.end(), graph.begin(), graph.end());
        out.push_back(e);
    }
    if (seen <= slot) out.insert(out.end(), graph.begin(), graph.end());
    return out;
}

int sharedNodeUsers(const Sequence& s, Id id) {
    int users = 0;
    for (const auto* tracks : {&s.videoTracks, &s.audioTracks})
        for (const Track& t : *tracks)
            for (const Clip& c : t.clips) {
                bool uses = std::any_of(c.grade.nodes.begin(), c.grade.nodes.end(), [&](const GradeNode& n) { return n.shared == id; });
                for (const GradeVersion& v : c.gradeVersions)
                    uses = uses || std::any_of(v.grade.nodes.begin(), v.grade.nodes.end(), [&](const GradeNode& n) { return n.shared == id; });
                users += uses;
            }
    return users;
}

namespace edit {

namespace {

Clip* videoClip(Sequence& s, Id clip) {
    for (Track& t : s.videoTracks)
        for (Clip& c : t.clips)
            if (c.id == clip) return &c;
    return nullptr;
}

// The graph changed by `change`, kept only when it still holds together.
Result changeGraph(Clip& c, const std::function<Result(GradeGraph&)>& change) {
    GradeGraph g = c.grade;
    if (Result r = change(g); !r.ok) return r;
    if (g.nodes.empty()) {
        c.grade = {};  // (no nodes left: a stack again)
        return {};
    }
    std::string why;
    if (!validGrade(g, &why)) return Result::fail(why);
    c.grade = std::move(g);
    return {};
}

// A mixer's inputs numbered 0, 1, 2... in their order (a layer mixer's blend modes one for each after the first);
// how many it has.
size_t compactInputs(GradeGraph& g, int mixer) {
    std::vector<GradeLink*> ins;
    for (GradeLink& x : g.links)
        if (x.to == mixer) ins.push_back(&x);
    std::sort(ins.begin(), ins.end(), [](const GradeLink* a, const GradeLink* b) { return a->input < b->input; });
    for (size_t i = 0; i < ins.size(); ++i) ins[i]->input = int(i);
    if (GradeNode* m = findGradeNode(g, mixer); m && m->kind == "layer") m->blend.resize(ins.empty() ? 0 : ins.size() - 1, "normal");
    return ins.size();
}

// Where a new node goes on screen: right of `anchor` (lower when beside it).
void placeNear(GradeGraph& g, GradeNode& n, int anchor, bool below) {
    const GradeNode* a = findGradeNode(g, anchor);
    n.x = a ? a->x + (below ? 0 : 160) : 160.0 * double(g.nodes.size());
    n.y = a ? a->y + (below ? 110 : 0) : 0;
}

}  // namespace

Result convertToNodes(Sequence& s, Id clip) {
    Clip* c = videoClip(s, clip);
    if (!c) return Result::fail("Nodes are for picture clips");
    if (!c->grade.empty()) return Result::fail("The clip's grade is in nodes already");
    GradeGraph g;
    int key = 0;
    // The colour effects and whatever runs between them (a blur between two corrections): each a node, so the graph,
    // running where the first one was, renders the same.
    auto colour = [](const Effect& e) { return isGradeEffect(e) && e.type != "grade_node"; };
    const auto first = std::find_if(c->effects.begin(), c->effects.end(), colour);
    const auto last = std::find_if(c->effects.rbegin(), c->effects.rend(), colour).base();
    for (auto it = first; it < last; ++it) {
        Effect& e = *it;
        if (e.type == "grade_node" || runsBeforeGrade(e.type)) continue;
        e.node = ++key;
        GradeNode n;
        n.key = key;
        n.x = 160.0 * (key - 1);
        g.nodes.push_back(n);
    }
    if (g.nodes.empty()) {
        GradeNode n;
        n.key = 1;
        g.nodes.push_back(n);
    }
    for (size_t i = 0; i <= g.nodes.size(); ++i)
        g.links.push_back({i ? g.nodes[i - 1].key : 0, i < g.nodes.size() ? g.nodes[i].key : 0, 0, false});
    c->grade = std::move(g);
    return {};
}

Result addGradeNode(Project& p, Sequence& s, Id clip, NodeHow how, int anchor, int* created) {
    Clip* c = videoClip(s, clip);
    if (!c) return Result::fail("Nodes are for picture clips");
    if (c->grade.empty())
        if (Result r = convertToNodes(s, clip); !r.ok) return r;
    if (!anchor) {
        const GradeLink* last = gradeLinkInto(c->grade, 0, 0);
        anchor = last ? last->from : 0;
    }
    const GradeNode* at = anchor ? findGradeNode(c->grade, anchor) : nullptr;
    if (anchor && !at) return Result::fail("No such node");
    if (at && at->kind == "key_mixer") return Result::fail("A key mixer gives a key: add the node after a corrector or mixer");
    const size_t slot = graphSlot(*c);
    int key = nextGradeNodeKey(c->grade);
    const int made = key;
    Result r = changeGraph(*c, [&](GradeGraph& g) -> Result {
        GradeNode n;
        n.key = key;
        if (how == NodeHow::Serial || how == NodeHow::Outside) {
            placeNear(g, n, anchor, false);
            // What the anchor's picture fed now comes from the new node, which the anchor feeds.
            for (GradeLink& l : g.links)
                if (l.from == anchor && !l.key) l.from = key;
            g.links.push_back({anchor, key, 0, false});
            if (how == NodeHow::Outside) {
                if (!anchor) return Result::fail("The outside is of a node: choose one");
                g.links.push_back({anchor, key, 1, true});
            }
            g.nodes.push_back(n);
            return {};
        }
        // Beside the anchor: from what feeds it, into a mixer after it (the one it feeds already, when of this kind).
        if (!anchor) return Result::fail("A node goes beside another: choose one");
        const std::string kind = how == NodeHow::Parallel ? "parallel" : "layer";
        const GradeLink* in = gradeLinkInto(g, anchor, 0);
        const int source = in ? in->from : 0;
        placeNear(g, n, anchor, true);
        GradeNode* mixer = nullptr;
        for (const GradeLink& l : g.links)
            if (l.from == anchor && !l.key)
                if (GradeNode* m = findGradeNode(g, l.to); m && m->kind == kind) mixer = m;
        if (!mixer) {
            GradeNode m;
            m.key = key + 1;
            m.kind = kind;
            m.x = (findGradeNode(g, anchor) ? findGradeNode(g, anchor)->x : 0) + 160;
            m.y = findGradeNode(g, anchor) ? findGradeNode(g, anchor)->y + 55 : 55;
            for (GradeLink& l : g.links)
                if (l.from == anchor && !l.key) l.from = m.key;
            g.links.push_back({anchor, m.key, 0, false});
            g.nodes.push_back(m);
            mixer = &g.nodes.back();
        }
        int input = 0;
        for (const GradeLink& l : g.links)
            if (l.to == mixer->key) input = std::max(input, l.input + 1);
        const int mixerKey = mixer->key;
        if (kind == "layer") {
            mixer->blend.resize(size_t(std::max(0, input)), "normal");  // (one for each input after the first)
        }
        g.links.push_back({source, n.key, 0, false});
        g.links.push_back({n.key, mixerKey, input, false});
        // (the new node sits below the ones already beside the anchor)
        for (const GradeNode& o : g.nodes)
            if (o.key != anchor && o.x == n.x && o.y >= n.y) n.y = o.y + 110;
        g.nodes.push_back(n);
        return {};
    });
    if (!r.ok) return r;
    if (how == NodeHow::Outside) {
        // The node's key: the anchor's, inverted.
        Effect settings = makeEffect(p, "grade_node");
        settings.node = made;
        settings.params["key_invert"] = Param(1.0);
        c->effects.push_back(settings);
    }
    keepGraphAt(p, *c, slot);
    if (created) *created = made;
    return {};
}

Result addKeyMixer(Sequence& s, Id clip, const std::vector<int>& keys, int to, int* created) {
    Clip* c = videoClip(s, clip);
    if (!c || c->grade.empty()) return Result::fail("The clip has no nodes");
    if (keys.empty()) return Result::fail("Choose the nodes whose keys are joined");
    const int key = nextGradeNodeKey(c->grade);
    Result r = changeGraph(*c, [&](GradeGraph& g) -> Result {
        GradeNode m;
        m.key = key;
        m.kind = "key_mixer";
        const GradeNode* target = findGradeNode(g, to);
        if (!target || target->kind != "corrector") return Result::fail("A key mixer's key goes into a corrector");
        m.x = target->x - 80;
        m.y = target->y - 110;
        for (size_t i = 0; i < keys.size(); ++i) g.links.push_back({keys[i], key, int(i), true});
        std::erase_if(g.links, [&](const GradeLink& l) { return l.to == to && l.input == 1; });
        g.links.push_back({key, to, 1, true});
        g.nodes.push_back(m);
        return {};
    });
    if (r.ok && created) *created = key;
    return r;
}

Result removeGradeNode(Project& p, Sequence& s, Id clip, int key) {
    Clip* c = videoClip(s, clip);
    if (!c) return Result::fail("No such clip");
    if (!findGradeNode(c->grade, key)) return Result::fail("No such node");
    const size_t slot = graphSlot(*c);
    Result r = changeGraph(*c, [&](GradeGraph& g) -> Result {
        // Takes a node out: what its picture fed takes what fed its picture (a mixer's first input) instead, a
        // mixer it fed loses that input, and its keys go.
        std::function<void(int)> remove = [&](int k) {
            const GradeLink* in = gradeLinkInto(g, k, 0);
            const int source = in && !in->key ? in->from : 0;
            std::vector<std::pair<int, int>> mixers;  // the mixers it fed, and which input
            for (GradeLink& l : g.links)
                if (l.from == k) {
                    if (const GradeNode* m = findGradeNode(g, l.to); m && m->kind != "corrector") mixers.push_back({m->key, l.input});
                    else if (!l.key) l.from = source;
                }
            std::erase_if(g.links, [&](const GradeLink& l) {
                return l.to == k || l.from == k;  // (its own inputs, its keys and its pictures into mixers)
            });
            std::erase_if(g.nodes, [&](const GradeNode& x) { return x.key == k; });
            for (const auto& [m, input] : mixers) {
                GradeNode* mn = findGradeNode(g, m);
                if (!mn) continue;
                // Each remaining input keeps its blend mode (the first input has none).
                if (mn->kind == "layer" && !mn->blend.empty()) mn->blend.erase(mn->blend.begin() + std::clamp<std::ptrdiff_t>(input - 1, 0, std::ptrdiff_t(mn->blend.size()) - 1));
                const size_t left = compactInputs(g, m);
                if (mn->kind == "key_mixer" ? left == 0 : left <= 1) remove(m);  // (a mixer of one picture passes it on; a key mixer of none goes)
            }
        };
        remove(key);
        return {};
    });
    if (!r.ok) return r;
    std::set<int> left;
    for (const GradeNode& n : c->grade.nodes) left.insert(n.key);
    std::erase_if(c->effects, [&](const Effect& e) { return e.node && !left.count(e.node); });
    keepGraphAt(p, *c, slot);
    return {};
}

Result connectGradeNodes(Sequence& s, Id clip, const GradeLink& link) {
    Clip* c = videoClip(s, clip);
    if (!c || c->grade.empty()) return Result::fail("The clip has no nodes");
    if (const GradeLink* l = gradeLinkInto(c->grade, link.to, link.input); l && *l == link) return Result::fail("");
    if (const GradeNode* m = findGradeNode(c->grade, link.to); m && m->kind != "corrector") {
        int inputs = 0;
        for (const GradeLink& l : c->grade.links) inputs += l.to == link.to;
        if (link.input > inputs) return Result::fail("A mixer's inputs are numbered from 0: the next one is " + std::to_string(inputs));
    }
    return changeGraph(*c, [&](GradeGraph& g) -> Result {
        std::erase_if(g.links, [&](const GradeLink& l) { return l.to == link.to && l.input == link.input; });
        g.links.push_back(link);
        if (GradeNode* m = findGradeNode(g, link.to); m && m->kind == "layer")
            if (m->blend.size() < size_t(std::max(0, link.input))) m->blend.resize(size_t(link.input), "normal");
        return {};
    });
}

Result disconnectGradeNodes(Sequence& s, Id clip, int to, int input) {
    Clip* c = videoClip(s, clip);
    if (!c || c->grade.empty()) return Result::fail("The clip has no nodes");
    if (!to) return Result::fail("The graph's output needs a link");
    if (!gradeLinkInto(c->grade, to, input)) return Result::fail("");
    return changeGraph(*c, [&](GradeGraph& g) -> Result {
        std::erase_if(g.links, [&](const GradeLink& l) { return l.to == to && l.input == input; });
        if (GradeNode* m = findGradeNode(g, to); m && m->kind != "corrector") {
            if (m->kind == "layer" && input >= 1 && size_t(input - 1) < m->blend.size()) m->blend.erase(m->blend.begin() + (input - 1));
            else if (m->kind == "layer" && input == 0 && !m->blend.empty()) m->blend.erase(m->blend.begin());
            compactInputs(g, to);
        }
        return {};
    });
}

Result setGradeNode(Sequence& s, Id clip, const GradeNode& node) {
    Clip* c = videoClip(s, clip);
    if (!c) return Result::fail("No such clip");
    GradeNode* n = findGradeNode(c->grade, node.key);
    if (!n) return Result::fail("No such node");
    GradeNode changed = *n;
    changed.label = node.label;
    changed.enabled = node.enabled;
    changed.x = node.x;
    changed.y = node.y;
    if (n->kind == "layer") {
        for (const std::string& b : node.blend)
            if (std::find(blendModes().begin(), blendModes().end(), b) == blendModes().end()) return Result::fail("Unknown blend mode \"" + b + "\"");
        changed.blend = node.blend;
        changed.blend.resize(n->blend.size(), "normal");  // (one for each input after the first)
    }
    if (changed == *n) return Result::fail("");
    *n = changed;
    return {};
}

Result moveEffectToNode(Project& p, Sequence& s, Id clip, Id effect, int key) {
    Clip* c = videoClip(s, clip);
    if (!c) return Result::fail("No such clip");
    const size_t slot = graphSlot(*c);
    if (key) {
        const GradeNode* n = findGradeNode(c->grade, key);
        if (!n || n->kind != "corrector") return Result::fail("Effects go into a corrector node");
        if (n->shared) return Result::fail("That node is shared: add to the shared node instead");
    }
    for (Effect& e : c->effects)
        if (e.id == effect) {
            if (e.type == "grade_node") return Result::fail("That is a node's own settings");
            if (key && runsBeforeGrade(e.type)) return Result::fail("That effect works on the picture as it comes, before the grade: it stays outside the nodes");
            if (e.node == key) return Result::fail("");
            const bool out = e.node && !key;
            e.node = key;
            if (out) {
                // Outside the graph, after it.
                Effect moved = std::move(e);
                std::erase_if(c->effects, [&](const Effect& x) { return x.id == effect; });
                auto last = std::find_if(c->effects.rbegin(), c->effects.rend(), [](const Effect& x) { return x.node != 0; });
                c->effects.insert(last.base(), std::move(moved));
            }
            keepGraphAt(p, *c, slot);
            return {};
        }
    return Result::fail("No such effect");
}

Result flattenGrade(Project& p, Sequence& s, Id clip) {
    Clip* c = videoClip(s, clip);
    if (!c) return Result::fail("No such clip");
    if (c->grade.empty()) return Result::fail("");
    // (where the graph ran; a node switched off adds nothing)
    c->effects = flattenedEffects(s, *c);
    c->grade = {};
    separateEffectIds(p, s, *c);
    return {};
}

Result shareGradeNode(Project& p, Sequence& s, Id clip, int key, const std::string& name, Id* created) {
    Clip* c = videoClip(s, clip);
    if (!c) return Result::fail("No such clip");
    GradeNode* n = findGradeNode(c->grade, key);
    if (!n || n->kind != "corrector") return Result::fail("Only a corrector node can be shared");
    if (n->shared) return Result::fail("That node is shared already");
    const size_t slot = graphSlot(*c);
    SharedNode sn;
    sn.id = p.newId();
    sn.name = name.empty() ? "Shared Node " + std::to_string(s.sharedNodes.size() + 1) : name;
    std::vector<Effect> rest;
    // (an old project's that ran ahead of the graph wherever they were stay on the clip, off if the node was)
    const bool live = liveGradeNode(c->grade, key);
    for (Effect& e : c->effects) {
        if (e.node == key && alwaysRanBeforeGrade(e.type)) e.node = 0, e.enabled = e.enabled && live;
        if (e.node == key && e.type != "grade_node") {
            Effect moved = e;
            moved.node = 0;
            moved.id = p.newId();
            sn.effects.push_back(std::move(moved));
        } else {
            rest.push_back(std::move(e));
        }
    }
    c->effects = std::move(rest);
    n->shared = sn.id;
    if (n->label.empty()) n->label = sn.name;
    if (created) *created = sn.id;
    s.sharedNodes.push_back(std::move(sn));
    keepGraphAt(p, *c, slot);
    return {};
}

Result useSharedNode(Project& p, Sequence& s, Id clip, Id shared, int* created) {
    const SharedNode* sn = findSharedNode(s, shared);
    if (!sn) return Result::fail("No such shared node");
    const std::string name = sn->name;
    int key = 0;
    if (Result r = addGradeNode(p, s, clip, NodeHow::Serial, 0, &key); !r.ok) return r;
    GradeNode* n = findGradeNode(videoClip(s, clip)->grade, key);
    n->shared = shared;
    n->label = name;
    if (created) *created = key;
    return {};
}

Result unshareGradeNode(Project& p, Sequence& s, Id clip, int key) {
    Clip* c = videoClip(s, clip);
    if (!c) return Result::fail("No such clip");
    GradeNode* n = findGradeNode(c->grade, key);
    if (!n || !n->shared) return Result::fail("That node is not shared");
    const size_t slot = graphSlot(*c);
    if (const SharedNode* sn = findSharedNode(s, n->shared)) {
        if (n->label == sn->name) n->label.clear();
        for (const Effect& e : sn->effects) {
            Effect copy = e;
            copy.id = p.newId();
            copy.node = key;
            c->effects.push_back(std::move(copy));
        }
    }
    n->shared = 0;
    keepGraphAt(p, *c, slot);
    return {};
}

Result renameSharedNode(Sequence& s, Id shared, const std::string& name) {
    for (SharedNode& n : s.sharedNodes)
        if (n.id == shared) {
            if (name.empty()) return Result::fail("Give the shared node a name");
            if (n.name == name) return Result::fail("");
            // (nodes showing it under its old name take the new one)
            for (Track& t : s.videoTracks)
                for (Clip& c : t.clips)
                    for (GradeNode& g : c.grade.nodes)
                        if (g.shared == shared && g.label == n.name) g.label = name;
            n.name = name;
            return {};
        }
    return Result::fail("No such shared node");
}

Result removeSharedNode(Sequence& s, Id shared) {
    if (!findSharedNode(s, shared)) return Result::fail("No such shared node");
    if (const int users = sharedNodeUsers(s, shared))
        return Result::fail(std::to_string(users) + (users == 1 ? " clip uses" : " clips use") + " that shared node: unshare it there first");
    std::erase_if(s.sharedNodes, [&](const SharedNode& n) { return n.id == shared; });
    return {};
}

Result setGradeNodeParams(Project& p, Sequence& s, Id clip, int key, const std::vector<std::pair<std::string, double>>& values) {
    Clip* c = videoClip(s, clip);
    if (!c) return Result::fail("No such clip");
    const GradeNode* n = findGradeNode(c->grade, key);
    if (!n || n->kind != "corrector") return Result::fail("Settings are a corrector node's");
    const EffectInfo* info = findEffectInfo("grade_node");
    for (const auto& [name, v] : values) {
        const bool known = name.rfind("mask.", 0) == 0 ||
                           std::any_of(info->params.begin(), info->params.end(), [&](const ParamInfo& pi) { return pi.name == name; });
        if (!known) return Result::fail("A node has no setting \"" + name + "\"");
    }
    Effect* settings = nullptr;
    for (Effect& e : c->effects)
        if (e.node == key && e.type == "grade_node") settings = &e;
    const size_t slot = graphSlot(*c);
    if (!settings) {
        Effect e = makeEffect(p, "grade_node");
        e.node = key;
        c->effects.push_back(std::move(e));
        settings = &c->effects.back();
    }
    for (const auto& [name, v] : values) settings->params[name] = Param(v);
    keepGraphAt(p, *c, slot);
    return {};
}

}  // namespace edit

}  // namespace montage
