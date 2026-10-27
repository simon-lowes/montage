#include "ProjectMerge.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <functional>
#include <optional>
#include <set>

#include "Effects.h"
#include "ProjectIO.h"

namespace montage {

const char* mergeSideName(MergeSide s) {
    switch (s) {
    case MergeSide::Mine: return "Mine";
    case MergeSide::Theirs: return "Theirs";
    case MergeSide::Both: return "Both";
    }
    return "";
}

const char* mergeChangeKindName(MergeChange::Kind k) {
    switch (k) {
    case MergeChange::Added: return "Added";
    case MergeChange::Removed: return "Removed";
    case MergeChange::Changed: return "Changed";
    }
    return "";
}

namespace {

QJsonObject toJson(const Project& p) {
    Project q = p;
    q.mergeBase.reset();
    q.mergeIds.clear();
    return QJsonDocument::fromJson(QByteArray::fromStdString(projectToJson(q))).object();
}

// Every id in the project, those things are known by and those that refer to them, for `fn` to change.
void forEachId(Project& p, const std::function<void(Id&)>& fn) {
    // Ids written as text: an effect's media (Dialogue Alignment's take), a graphic's media fields.
    auto text = [&](std::string& v) {
        if (v.empty() || !std::all_of(v.begin(), v.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) return;
        Id id = Id(std::strtoull(v.c_str(), nullptr, 10));
        fn(id);
        v = std::to_string(id);
    };
    std::set<std::string> mediaFields;
    for (const Sequence& s : p.sequences)
        for (const TemplateField& f : s.fields)
            if (f.kind == "media") mediaFields.insert(f.id);
    auto fx = [&](Effect& e) {
        fn(e.id);
        if (const auto it = e.strings.find("media"); it != e.strings.end()) text(it->second);
    };
    auto fxs = [&](std::vector<Effect>& v) {
        for (Effect& e : v) fx(e);
    };
    auto graph = [&](GradeGraph& g) {
        for (GradeNode& n : g.nodes) fn(n.shared);
    };
    auto clip = [&](Clip& c) {
        fn(c.id), fn(c.mediaId), fn(c.linkGroup), fn(c.colorGroup);
        fx(c.generator), fx(c.motion), fx(c.audio), fx(c.timing);
        fxs(c.effects);
        for (Take& t : c.takes) fn(t.mediaId);
        for (GradeVersion& v : c.gradeVersions) fxs(v.effects), graph(v.grade);
        graph(c.grade);
        for (auto& [target, e] : c.trims) fx(e);
        for (auto& [field, v] : c.fields)
            if (mediaFields.count(field)) text(v);
    };
    auto clipAndOriginal = [&](Clip& c) {
        clip(c);
        if (c.unrendered.empty()) return;
        Clip was;
        if (!clipFromJsonString(c.unrendered, was)) return;
        const Clip before = was;
        clip(was);
        if (!(was == before)) c.unrendered = clipToJsonString(was);
    };
    auto transitions = [&](std::vector<Transition>& v) {
        for (Transition& t : v) fn(t.id), fn(t.clipA), fn(t.clipB), fx(t.params);
    };
    auto track = [&](Track& t) {
        fn(t.id), fn(t.output);
        for (Clip& c : t.clips) clipAndOriginal(c);
        transitions(t.transitions);
        fxs(t.effects);
        for (Send& s : t.sends) fn(s.bus);
        for (TakeLane& l : t.lanes) fn(l.id), fn(l.mediaId);
        for (TrackVersion& v : t.versions) {
            for (Clip& c : v.clips) clipAndOriginal(c);
            transitions(v.transitions);
        }
    };
    for (MediaItem& m : p.media) fn(m.id), fn(m.sequenceId), fn(m.subclipOf);
    for (SmartBin& b : p.smartBins) fn(b.id);
    for (Sequence& s : p.sequences) {
        fn(s.id);
        for (Track& t : s.videoTracks) track(t);
        for (Track& t : s.audioTracks) track(t);
        for (CaptionTrack& t : s.captionTracks) fn(t.id);
        for (Bus& b : s.buses) fn(b.id), fxs(b.effects);
        fxs(s.masterEffects);
        for (ColorGroup& g : s.colorGroups) fn(g.id), fn(g.postId), fxs(g.pre), fxs(g.post);
        for (SharedNode& n : s.sharedNodes) fn(n.id), fxs(n.effects);
        for (AdrCue& c : s.adrCues) fn(c.id), fn(c.clip);
        for (TemplateField& f : s.fields) fn(f.clip), fn(f.effect);
    }
    for (auto& line : p.script.readings)
        for (ScriptReading& r : line) fn(r.media);
    fn(p.activeSequence);
}

// Fields that are where the editor was looking rather than the work: mine's are kept, and they are not listed.
bool viewField(const QString& key) {
    return key == "activeSequence" || key == "nextId" || key == "playhead" || key == "height" || key == "lanesShown" || key == "collapsedFolders";
}
// Fields merged whole (analyses, masks and drawings are not merged part by part).
bool wholeField(const QString& key) {
    return key == "transcript" || key == "visual" || key == "faces" || key == "sounds" || key == "object" ||
           key == "adaptiveHdr" || key == "script" || key == "drawing" || key == "unrendered" || key == "hdr10Plus";
}

QString valueText(const QJsonValue& v) {
    switch (v.type()) {
    case QJsonValue::Undefined: return QStringLiteral("removed");
    case QJsonValue::Null: return QStringLiteral("none");
    case QJsonValue::Bool: return v.toBool() ? QStringLiteral("on") : QStringLiteral("off");
    case QJsonValue::Double: return QString::number(v.toDouble(), 'g', 10);
    case QJsonValue::String: return v.toString();
    default: break;
    }
    QString s = QString::fromUtf8(v.isArray() ? QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact)
                                              : QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    if (s.size() > 160) s = s.left(157) + QStringLiteral("…");
    return s;
}

struct Where {
    std::string path;  // ids and keys, '/' between
    std::string what;  // names of the things it is in
    std::string sub;   // keys below the last named thing
    Id sequence = 0, clip = 0, media = 0;
};

// How the elements of a list are told apart from one version to another.
enum class Ident { None, Id, Key, Bus, Time, First, Strings };

std::optional<QString> elementKey(const QJsonValue& e, Ident how) {
    auto number = [](const QJsonValue& v) -> std::optional<QString> {
        if (!v.isDouble()) return std::nullopt;
        return QString::number(v.toDouble(), 'g', 17);
    };
    switch (how) {
    case Ident::Id: {
        if (!e.isObject() || !e.toObject().value("id").isDouble() || e.toObject().value("id").toDouble() == 0) return std::nullopt;
        return number(e.toObject().value("id"));
    }
    case Ident::Key: return e.isObject() ? number(e.toObject().value("key")) : std::nullopt;
    case Ident::Bus: return e.isObject() ? number(e.toObject().value("bus")) : std::nullopt;
    case Ident::Time: {
        if (!e.isObject()) return std::nullopt;
        auto t = number(e.toObject().value("t"));
        if (t && e.toObject().contains("sub")) *t += QStringLiteral("+") + QString::number(e.toObject().value("sub").toDouble(), 'g', 17);
        return t;
    }
    case Ident::First: return e.isArray() && !e.toArray().isEmpty() ? number(e.toArray().at(0)) : std::nullopt;
    case Ident::Strings: return e.isString() ? std::optional<QString>(e.toString()) : std::nullopt;
    case Ident::None: break;
    }
    return std::nullopt;
}

Ident identify(const std::vector<const QJsonArray*>& lists) {
    bool any = false;
    for (const QJsonArray* a : lists) any = any || !a->isEmpty();
    if (!any) return Ident::None;
    for (Ident how : {Ident::Id, Ident::Key, Ident::Bus, Ident::Time, Ident::First, Ident::Strings}) {
        bool fits = true;
        for (const QJsonArray* a : lists) {
            std::set<QString> seen;
            for (const QJsonValue& e : *a) {
                const auto k = elementKey(e, how);
                if (!k || !seen.insert(*k).second) {
                    fits = false;
                    break;
                }
            }
            if (!fits) break;
        }
        if (fits) return how;
    }
    return Ident::None;
}

std::string label(const QJsonValue& e) {
    if (!e.isObject()) return {};
    const QJsonObject o = e.toObject();
    if (const QString n = o.value("name").toString(); !n.isEmpty()) return n.toStdString();
    if (const QString t = o.value("type").toString(); !t.isEmpty()) {
        const EffectInfo* info = findEffectInfo(t.toStdString());
        return info ? info->displayName : t.toStdString();
    }
    return {};
}

class Merger {
public:
    Merger(const std::map<std::string, MergeSide>& choices, MergeResult& r) : choices_(choices), r_(r) {}

    QJsonValue merge(const QJsonValue& b, const QJsonValue& m, const QJsonValue& t, const Where& w, const QString& field) {
        const bool view = viewField(field);
        if (m == t) {
            if (m != b && !view) describe(MergeSide::Both, b, m, w, field);
            return m;
        }
        if (m == b) {
            if (!view) describe(MergeSide::Theirs, b, t, w, field);
            return t;
        }
        if (t == b) {
            if (!view) describe(MergeSide::Mine, b, m, w, field);
            return m;
        }
        if (view) return m;
        if (!m.isUndefined() && !t.isUndefined() && !wholeField(field)) {
            if (m.isObject() && t.isObject() && (b.isObject() || b.isUndefined())) return mergeObjects(b.toObject(), m.toObject(), t.toObject(), w);
            if (m.isArray() && t.isArray() && (b.isArray() || b.isUndefined()))
                if (auto merged = mergeArrays(b.toArray(), m.toArray(), t.toArray(), w, field)) return *merged;
        }
        MergeConflict c;
        c.kind = m.isUndefined() || t.isUndefined() ? MergeConflict::Removed : MergeConflict::Field;
        c.key = w.path;
        c.what = w.what;
        c.field = w.sub;
        c.mine = valueText(m).toStdString();
        c.theirs = valueText(t).toStdString();
        c.sequence = w.sequence, c.clip = w.clip, c.media = w.media;
        const auto it = choices_.find(c.key);
        c.choice = it != choices_.end() && it->second == MergeSide::Theirs ? MergeSide::Theirs : MergeSide::Mine;
        r_.conflicts.push_back(c);
        return c.choice == MergeSide::Theirs ? t : m;
    }

    QJsonObject mergeObjects(const QJsonObject& b, const QJsonObject& m, const QJsonObject& t, const Where& w) {
        std::set<QString> keys;
        for (const QJsonObject* o : {&b, &m, &t})
            for (auto it = o->begin(); it != o->end(); ++it) keys.insert(it.key());
        QJsonObject out;
        for (const QString& k : keys) {
            Where child = w;
            child.path += "/" + k.toStdString();
            child.sub += (child.sub.empty() ? "" : " › ") + k.toStdString();
            const QJsonValue v = merge(b.value(k), m.value(k), t.value(k), child, k);
            if (!v.isUndefined()) out[k] = v;
        }
        return out;
    }

private:
    // Lists what one side changed from `before` to `after`, thing by thing and field by field (keys, analyses and
    // other whole fields as one change).
    void describe(MergeSide side, const QJsonValue& before, const QJsonValue& after, const Where& w, const QString& field) {
        if (viewField(field)) return;
        if (before.isUndefined() != after.isUndefined() && !wholeField(field)) {  // a list or group made or emptied: its contents
            const QJsonValue& v = before.isUndefined() ? after : before;
            if (v.isArray()) return describe(side, before.isUndefined() ? QJsonArray() : before, after.isUndefined() ? QJsonArray() : after, w, field);
        }
        if (before.isObject() && after.isObject() && !wholeField(field)) {
            const QJsonObject b = before.toObject(), a = after.toObject();
            std::set<QString> keys;
            for (const QJsonObject* o : {&b, &a})
                for (auto it = o->begin(); it != o->end(); ++it) keys.insert(it.key());
            for (const QString& k : keys)
                if (b.value(k) != a.value(k)) {
                    Where child = w;
                    child.path += "/" + k.toStdString();
                    child.sub += (child.sub.empty() ? "" : " › ") + k.toStdString();
                    describe(side, b.value(k), a.value(k), child, k);
                }
            return;
        }
        if (before.isArray() && after.isArray() && !wholeField(field) && field != "keys") {
            const QJsonArray b = before.toArray(), a = after.toArray();
            const Ident how = identify({&b, &a});
            if (how != Ident::None) {
                std::map<QString, QJsonValue> bm, am;
                std::vector<QString> order;
                for (const QJsonValue& e : b) bm[*elementKey(e, how)] = e;
                for (const QJsonValue& e : a) am[*elementKey(e, how)] = e, order.push_back(*elementKey(e, how));
                for (const QJsonValue& e : b)
                    if (!am.count(*elementKey(e, how))) order.push_back(*elementKey(e, how));
                for (const QString& k : order) {
                    const QJsonValue bv = bm.count(k) ? bm[k] : QJsonValue(QJsonValue::Undefined);
                    const QJsonValue av = am.count(k) ? am[k] : QJsonValue(QJsonValue::Undefined);
                    if (bv != av) describe(side, bv, av, element(w, field, k, av.isUndefined() ? bv : av), QString());
                }
                return;
            }
        }
        note(side, before, after, w);
    }

    void note(MergeSide side, const QJsonValue& before, const QJsonValue& after, const Where& w) {
        MergeChange c;
        c.kind = before.isUndefined() ? MergeChange::Added : after.isUndefined() ? MergeChange::Removed : MergeChange::Changed;
        c.side = side;
        c.path = w.path;
        c.what = w.what;
        c.field = w.sub;
        c.sequence = w.sequence, c.clip = w.clip, c.media = w.media;
        r_.changes.push_back(std::move(c));
    }

    Where element(const Where& w, const QString& field, const QString& key, const QJsonValue& e) const {
        Where child = w;
        child.path += "/" + key.toStdString();
        if (const std::string name = label(e); !name.empty()) {
            child.what += (child.what.empty() ? "" : " › ") + name;
            child.sub.clear();
        } else {
            child.sub += (child.sub.empty() ? "" : " › ") + key.toStdString();
        }
        if (e.isObject()) {
            const Id id = Id(e.toObject().value("id").toDouble());
            if (field == "sequences") child.sequence = id, child.clip = 0;
            else if (field == "clips") child.clip = id;
            else if (field == "media" && w.path == "/media") child.media = id;
        }
        return child;
    }

    std::optional<QJsonValue> mergeArrays(const QJsonArray& b, const QJsonArray& m, const QJsonArray& t, const Where& w, const QString& field) {
        const Ident how = identify({&b, &m, &t});
        if (how == Ident::None) return std::nullopt;
        auto keysOf = [&](const QJsonArray& a) {
            std::vector<QString> order;
            std::map<QString, QJsonValue> byKey;
            for (const QJsonValue& e : a) {
                const QString k = *elementKey(e, how);
                order.push_back(k);
                byKey[k] = e;
            }
            return std::make_pair(order, byKey);
        };
        const auto [bo, bm] = keysOf(b);
        const auto [mo, mm] = keysOf(m);
        const auto [to, tm] = keysOf(t);
        auto at = [](const std::map<QString, QJsonValue>& map, const QString& k) {
            const auto it = map.find(k);
            return it == map.end() ? QJsonValue(QJsonValue::Undefined) : it->second;
        };
        if (how == Ident::Strings) {  // a set: each side's additions and removals
            QJsonArray out;
            std::set<QString> in;
            for (const QString& k : mo)
                if (!(bm.count(k) && !tm.count(k)) && in.insert(k).second) out.append(k);
            for (const QString& k : to)
                if (!bm.count(k) && in.insert(k).second) out.append(k);
            for (const QString& k : mo)
                if (!bm.count(k)) note(tm.count(k) ? MergeSide::Both : MergeSide::Mine, QJsonValue::Undefined, k, element(w, field, k, k));
            for (const QString& k : to)
                if (!bm.count(k) && !mm.count(k)) note(MergeSide::Theirs, QJsonValue::Undefined, k, element(w, field, k, k));
            for (const QString& k : bo)
                if (!mm.count(k) || !tm.count(k))
                    note(!mm.count(k) && !tm.count(k) ? MergeSide::Both : !mm.count(k) ? MergeSide::Mine : MergeSide::Theirs, k,
                         QJsonValue::Undefined, element(w, field, k, k));
            return out;
        }
        std::map<QString, QJsonValue> merged;
        std::set<QString> keys(bo.begin(), bo.end());
        keys.insert(mo.begin(), mo.end());
        keys.insert(to.begin(), to.end());
        for (const QString& k : keys) {
            const QJsonValue mv = at(mm, k), tv = at(tm, k), bv = at(bm, k);
            const QJsonValue v = merge(bv, mv, tv, element(w, field, k, !mv.isUndefined() ? mv : !tv.isUndefined() ? tv : bv), QString());
            if (!v.isUndefined()) merged[k] = v;
        }
        // The order: mine's, unless only theirs has changed it; the other side's additions after what comes before
        // them there.
        auto common = [&](const std::vector<QString>& order) {
            std::vector<QString> c;
            for (const QString& k : order)
                if (bm.count(k) && mm.count(k) && tm.count(k)) c.push_back(k);
            return c;
        };
        const bool theirsFirst = common(mo) == common(bo) && common(to) != common(bo);
        const std::vector<QString>& primary = theirsFirst ? to : mo;
        const std::vector<QString>& secondary = theirsFirst ? mo : to;
        std::vector<QString> order;
        for (const QString& k : primary)
            if (merged.count(k)) order.push_back(k);
        for (size_t i = 0; i < secondary.size(); ++i) {
            const QString& k = secondary[i];
            if (!merged.count(k) || std::find(order.begin(), order.end(), k) != order.end()) continue;
            auto pos = order.begin();
            for (size_t j = i; j-- > 0;)
                if (auto it = std::find(order.begin(), order.end(), secondary[j]); it != order.end()) {
                    pos = it + 1;
                    break;
                }
            order.insert(pos, k);
        }
        for (const QString& k : bo)
            if (merged.count(k) && std::find(order.begin(), order.end(), k) == order.end()) order.push_back(k);
        // Where both sides added things in the same place: each side's together, the side whose first key is lower
        // first (so the order does not depend on which side is mine).
        auto lower = [](const QString& a, const QString& b) {
            bool na = false, nb = false;
            const double x = a.toDouble(&na), y = b.toDouble(&nb);
            return na && nb ? x < y : a < b;
        };
        for (size_t i = 0; i < order.size();) {
            if (bm.count(order[i])) {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < order.size() && !bm.count(order[j])) ++j;
            std::vector<QString> mineAdded, theirsAdded;
            for (size_t k = i; k < j; ++k) (mm.count(order[k]) ? mineAdded : theirsAdded).push_back(order[k]);
            if (!mineAdded.empty() && !theirsAdded.empty()) {
                const bool mineFirst = lower(mineAdded.front(), theirsAdded.front());
                std::vector<QString> run = mineFirst ? mineAdded : theirsAdded;
                const std::vector<QString>& rest = mineFirst ? theirsAdded : mineAdded;
                run.insert(run.end(), rest.begin(), rest.end());
                std::copy(run.begin(), run.end(), order.begin() + std::ptrdiff_t(i));
            }
            i = j;
        }
        if (how == Ident::Time || how == Ident::First)
            std::stable_sort(order.begin(), order.end(), [](const QString& a, const QString& b) {
                return a.section('+', 0, 0).toDouble() < b.section('+', 0, 0).toDouble();
            });
        QJsonArray out;
        for (const QString& k : order) out.append(merged[k]);
        return out;
    }

    const std::map<std::string, MergeSide>& choices_;
    MergeResult& r_;
};

const Clip* findClip(const Project& p, Id id, Id* track = nullptr) {
    for (const Sequence& s : p.sequences)
        for (const auto* tracks : {&s.videoTracks, &s.audioTracks})
            for (const Track& t : *tracks)
                for (const Clip& c : t.clips)
                    if (c.id == id) {
                        if (track) *track = t.id;
                        return &c;
                    }
    return nullptr;
}

std::string clipText(const Clip& c) {
    return c.name + " (" + std::to_string(c.start) + "–" + std::to_string(c.end()) + ")";
}

void dropTransitions(Track& t, Id clip) {
    t.transitions.erase(std::remove_if(t.transitions.begin(), t.transitions.end(),
                                       [&](const Transition& tr) { return tr.clipA == clip || tr.clipB == clip; }),
                        t.transitions.end());
}

// A clip that each side moved to a different track ends up on both: a conflict, mine's place kept unless theirs is
// chosen.
void settleMovedClips(Project& p, const Project& mine, const std::map<std::string, MergeSide>& choices, MergeResult& r) {
    for (Sequence& s : p.sequences) {
        std::map<Id, std::vector<std::pair<Track*, size_t>>> where;
        for (auto* tracks : {&s.videoTracks, &s.audioTracks})
            for (Track& t : *tracks)
                for (size_t i = 0; i < t.clips.size(); ++i) where[t.clips[i].id].push_back({&t, i});
        std::vector<std::pair<Track*, Id>> drop;
        for (const auto& [id, places] : where) {
            if (places.size() < 2) continue;
            Id mineTrack = 0;
            findClip(mine, id, &mineTrack);
            size_t ours = 0;
            for (size_t i = 0; i < places.size(); ++i)
                if (places[i].first->id == mineTrack) ours = i;
            const size_t theirsAt = ours == 0 ? 1 : 0;
            MergeConflict k;
            k.kind = MergeConflict::Moved;
            k.key = "moved/" + std::to_string(s.id) + "/" + std::to_string(id);
            const Clip& c = places[ours].first->clips[places[ours].second];
            k.what = s.name + " › " + c.name;
            k.field = "track";
            k.mine = places[ours].first->name;
            k.theirs = places[theirsAt].first->name;
            k.sequence = s.id, k.clip = id;
            const auto it = choices.find(k.key);
            k.choice = it != choices.end() && it->second == MergeSide::Theirs ? MergeSide::Theirs : MergeSide::Mine;
            r.conflicts.push_back(k);
            for (size_t i = 0; i < places.size(); ++i)
                if (k.choice == MergeSide::Mine ? i != ours : i != theirsAt) drop.push_back({places[i].first, id});
        }
        for (const auto& [t, id] : drop) {
            std::erase_if(t->clips, [&](const Clip& c) { return c.id == id; });
            dropTransitions(*t, id);
        }
    }
}

// Their clips that overlap mine on a track: settled by `choices` (Both, the default: theirs to a new track).
void settleOverlaps(Project& p, const Project& mine, const std::map<std::string, MergeSide>& choices, MergeResult& r) {
    for (Sequence& s : p.sequences) {
        for (TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
            std::vector<Track>& tracks = kind == TrackKind::Video ? s.videoTracks : s.audioTracks;
            const size_t count = tracks.size();
            std::vector<Track> made;
            for (size_t ti = 0; ti < count; ++ti) {
                std::vector<Clip> placed, touched;
                for (Clip& c : tracks[ti].clips) {
                    const Clip* before = findClip(mine, c.id);
                    (before && *before == c ? placed : touched).push_back(std::move(c));
                }
                std::stable_sort(touched.begin(), touched.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
                auto overlapping = [](const std::vector<Clip>& list, const Clip& c) {
                    std::vector<size_t> hits;
                    for (size_t i = 0; i < list.size(); ++i)
                        if (list[i].start < c.end() && c.start < list[i].end()) hits.push_back(i);
                    return hits;
                };
                std::vector<Clip> moved;
                for (Clip& c : touched) {
                    const std::vector<size_t> hits = overlapping(placed, c);
                    if (hits.empty()) {
                        placed.push_back(std::move(c));
                        continue;
                    }
                    MergeConflict k;
                    k.kind = MergeConflict::Overlap;
                    k.key = "overlap/" + std::to_string(s.id) + "/" + std::to_string(c.id);
                    k.what = s.name + " › " + tracks[ti].name;
                    k.field = "clips overlap";
                    for (size_t i : hits) k.mine += (k.mine.empty() ? "" : ", ") + clipText(placed[i]);
                    k.theirs = clipText(c);
                    k.canBoth = true;
                    k.sequence = s.id, k.clip = c.id;
                    const auto it = choices.find(k.key);
                    k.choice = it == choices.end() ? MergeSide::Both : it->second;
                    r.conflicts.push_back(k);
                    if (k.choice == MergeSide::Mine) {  // their clip out; mine's version of it back, if mine has it here
                        Id on = 0;
                        const Clip* own = findClip(mine, c.id, &on);
                        if (own && on == tracks[ti].id && overlapping(placed, *own).empty()) placed.push_back(*own);
                        else dropTransitions(tracks[ti], c.id);
                    } else if (k.choice == MergeSide::Theirs) {
                        bool clear = true;  // (only mine's are taken out; one of theirs in the way sends it to a new track)
                        for (size_t i : hits) clear = clear && findClip(mine, placed[i].id) && *findClip(mine, placed[i].id) == placed[i];
                        if (clear) {
                            for (size_t i = hits.size(); i-- > 0;) {
                                dropTransitions(tracks[ti], placed[hits[i]].id);
                                placed.erase(placed.begin() + std::ptrdiff_t(hits[i]));
                            }
                            placed.push_back(std::move(c));
                        } else {
                            moved.push_back(std::move(c));
                        }
                    } else {
                        moved.push_back(std::move(c));
                    }
                }
                std::stable_sort(placed.begin(), placed.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
                tracks[ti].clips = std::move(placed);
                // Theirs moved off: onto new tracks of the same kind, as few as hold them.
                std::vector<Track> extra;
                for (Clip& c : moved) {
                    dropTransitions(tracks[ti], c.id);
                    Track* into = nullptr;
                    for (Track& x : extra)
                        if (overlapping(x.clips, c).empty()) {
                            into = &x;
                            break;
                        }
                    if (!into) {
                        extra.push_back(makeTrack(p, kind, tracks[ti].name + " Theirs"));
                        into = &extra.back();
                    }
                    into->clips.push_back(std::move(c));
                }
                for (Track& x : extra) {
                    std::stable_sort(x.clips.begin(), x.clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
                    made.push_back(std::move(x));
                }
            }
            r.newTracks += int(made.size());
            for (Track& x : made) tracks.push_back(std::move(x));  // (above all others, so no track's number changes)
        }
    }
}

// Clips and lanes of media one side removed while the other used it: the media is kept.
void keepUsedMedia(Project& p, const Project& mine, const Project& theirs) {
    std::set<Id> have;
    for (const MediaItem& m : p.media) have.insert(m.id);
    std::vector<Id> wanted;
    auto want = [&](Id id) {
        if (id && !have.count(id)) wanted.push_back(id), have.insert(id);
    };
    for (const Sequence& s : p.sequences)
        for (const auto* tracks : {&s.videoTracks, &s.audioTracks})
            for (const Track& t : *tracks) {
                for (const Clip& c : t.clips) {
                    want(c.mediaId);
                    for (const Take& tk : c.takes) want(tk.mediaId);
                }
                for (const TakeLane& l : t.lanes) want(l.mediaId);
            }
    for (size_t i = 0; i < wanted.size(); ++i) {
        const MediaItem* m = theirs.findMedia(wanted[i]);
        if (!m) m = mine.findMedia(wanted[i]);
        if (!m) continue;
        p.media.push_back(*m);
        if (m->subclipOf) want(m->subclipOf);
    }
}

std::set<Id> idsOf(const Project& p) {
    Project q = p;
    std::set<Id> ids;
    forEachId(q, [&](Id& id) {
        if (id) ids.insert(id);
    });
    return ids;
}

void renumber(Project& p, const std::map<Id, Id>& map) {
    if (map.empty()) return;
    forEachId(p, [&](Id& id) {
        if (const auto it = map.find(id); it != map.end()) id = it->second;
    });
}

}  // namespace

MergeResult mergeProjects(const MergeBase& from, const Project& mine, const Project& theirs, const std::map<std::string, MergeSide>& choices) {
    MergeResult r;
    Project base = from.base, their = theirs;
    Id next = std::max({mine.nextId, theirs.nextId, from.base.nextId});
    // Their ids an earlier merge gave new ones, as mine knows them (any of theirs that would land on one of those moved
    // aside first).
    std::map<Id, Id> known = from.theirIds;
    if (!known.empty()) {
        std::set<Id> sources, targets, present = idsOf(their);
        for (const auto& [a, b] : from.theirIds) sources.insert(a), targets.insert(b);
        if (from.baseInTheirIds) {
            const std::set<Id> inBase = idsOf(base);
            present.insert(inBase.begin(), inBase.end());
        }
        for (Id id : present)
            if (targets.count(id) && !sources.count(id)) known[id] = next++;
        renumber(their, known);
        if (from.baseInTheirIds) renumber(base, known);
    }
    // Ids both sides made since the base (in neither's base): theirs are given new ones.
    {
        const std::set<Id> inBase = idsOf(base), mineIds = idsOf(mine);
        for (Id id : idsOf(their))
            if (!inBase.count(id) && mineIds.count(id)) r.renumbered[id] = next++;
        renumber(their, r.renumbered);
        their.nextId = std::max(their.nextId, next);
    }
    const QJsonObject b = toJson(base), m = toJson(mine), t = toJson(their);
    Merger merger(choices, r);
    QJsonObject root = merger.mergeObjects(b, m, t, Where{});
    root["nextId"] = double(std::max(mine.nextId, their.nextId));
    root["format"] = m.value("format");
    root["version"] = m.value("version");
    std::string error;
    Project out;
    if (!projectFromJson(QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString(), out, &error)) {
        r.error = error.empty() ? "The merged project could not be read" : error;
        return r;
    }
    out.stereoChecked = mine.stereoChecked && theirs.stereoChecked;
    keepUsedMedia(out, mine, their);
    settleMovedClips(out, mine, choices, r);
    settleOverlaps(out, mine, choices, r);
    // Transitions whose clips are no longer beside them on their track.
    for (Sequence& s : out.sequences)
        for (auto* tracks : {&s.videoTracks, &s.audioTracks})
            for (Track& tr : *tracks)
                tr.transitions.erase(std::remove_if(tr.transitions.begin(), tr.transitions.end(),
                                                    [&](const Transition& x) {
                                                        auto on = [&](Id id) {
                                                            return id == 0 || std::any_of(tr.clips.begin(), tr.clips.end(),
                                                                                          [&](const Clip& c) { return c.id == id; });
                                                        };
                                                        return !on(x.clipA) || !on(x.clipB);
                                                    }),
                                     tr.transitions.end());
    if (!out.findSequence(out.activeSequence) && !out.sequences.empty()) out.activeSequence = out.sequences.front().id;
    // From here on the two have theirs in common, their ids known here as renumbered.
    setMergeBase(out, theirs);
    for (Id id : idsOf(theirs)) {
        Id now = id;
        if (const auto it = known.find(now); it != known.end()) now = it->second;
        if (const auto it = r.renumbered.find(now); it != r.renumbered.end()) now = it->second;
        if (now != id) out.mergeIds[id] = now;
    }
    r.project = std::move(out);
    r.ok = true;
    return r;
}

MergeResult mergeProjects(const Project& base, const Project& mine, const Project& theirs, const std::map<std::string, MergeSide>& choices) {
    return mergeProjects(MergeBase{base, {}, false}, mine, theirs, choices);
}

bool mergeBaseOf(const Project& p, Project& base) {
    if (!p.mergeBase || p.mergeBase->empty()) return false;
    Project b;
    if (!projectFromJson(*p.mergeBase, b)) return false;
    base = std::move(b);
    return true;
}

void setMergeBase(Project& copy, const Project& base) {
    Project b = base;
    b.mergeBase.reset();
    b.mergeIds.clear();
    copy.mergeIds.clear();
    copy.mergeBase = std::make_shared<const std::string>(QJsonDocument(toJson(b)).toJson(QJsonDocument::Compact).toStdString());
}

bool commonMergeBase(const Project& mine, const Project& theirs, MergeBase& base) {
    Project fromMine, fromTheirs;
    const bool hasMine = mergeBaseOf(mine, fromMine), hasTheirs = mergeBaseOf(theirs, fromTheirs);
    if (!hasMine && !hasTheirs) return false;
    if (hasTheirs && (!hasMine || fromTheirs.nextId > fromMine.nextId)) {
        // Theirs' record of mine: the ids it renumbered are mine's as theirs knows them, so theirs is read back.
        base = MergeBase{std::move(fromTheirs), {}, false};
        for (const auto& [mineId, theirId] : theirs.mergeIds) base.theirIds[theirId] = mineId;
    } else {
        base = MergeBase{std::move(fromMine), mine.mergeIds, true};
    }
    return true;
}

}  // namespace montage
