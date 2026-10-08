#include "FaceIndex.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "Model.h"

namespace montage {

void FaceIndex::add(double time, float x, float y, float w, float h, float score, const std::vector<float>& embedding) {
    Face f;
    f.time = time;
    f.x = x, f.y = y, f.w = w, f.h = h;
    f.score = score;
    float peak = 0;
    for (float v : embedding) peak = std::max(peak, std::fabs(v));
    f.scale = peak > 0 ? peak / 127.0f : 1.0f;
    f.values.reserve(embedding.size());
    for (float v : embedding) f.values.push_back(int8_t(std::lround(std::clamp(v / f.scale, -127.0f, 127.0f))));
    faces.push_back(std::move(f));
}

std::vector<float> FaceIndex::embedding(size_t i) const {
    const Face& f = faces[i];
    std::vector<float> e(f.values.size());
    double len = 0;
    for (size_t k = 0; k < e.size(); ++k) {
        e[k] = float(f.values[k]) * f.scale;
        len += double(e[k]) * e[k];
    }
    if (len > 0)
        for (float& v : e) v = float(v / std::sqrt(len));
    return e;
}

std::string faceIndexToJson(const FaceIndex& f) {
    QJsonObject o{{"model", QString::fromStdString(f.model)}, {"step", f.step}};
    QJsonArray faces;
    for (const FaceIndex::Face& face : f.faces) {
        QByteArray bytes(reinterpret_cast<const char*>(face.values.data()), qsizetype(face.values.size()));
        faces.append(QJsonObject{{"t", face.time}, {"box", QJsonArray{face.x, face.y, face.w, face.h}}, {"score", face.score},
                                 {"scale", face.scale}, {"e", QString::fromLatin1(bytes.toBase64())}, {"person", face.person}});
    }
    o["faces"] = faces;
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
}

bool faceIndexFromJson(const std::string& json, FaceIndex& out) {
    const QJsonDocument d = QJsonDocument::fromJson(QByteArray::fromStdString(json));
    if (!d.isObject()) return false;
    const QJsonObject o = d.object();
    FaceIndex f;
    f.model = o.value("model").toString().toStdString();
    f.step = o.value("step").toDouble();
    for (const QJsonValue& v : o.value("faces").toArray()) {
        const QJsonObject fo = v.toObject();
        FaceIndex::Face face;
        face.time = fo.value("t").toDouble();
        const QJsonArray box = fo.value("box").toArray();
        if (box.size() == 4) face.x = float(box[0].toDouble()), face.y = float(box[1].toDouble()), face.w = float(box[2].toDouble()), face.h = float(box[3].toDouble());
        face.score = float(fo.value("score").toDouble());
        face.scale = float(fo.value("scale").toDouble());
        const QByteArray bytes = QByteArray::fromBase64(fo.value("e").toString().toLatin1());
        face.values.assign(reinterpret_cast<const int8_t*>(bytes.constData()), reinterpret_cast<const int8_t*>(bytes.constData()) + bytes.size());
        face.person = fo.value("person").toInt();
        f.faces.push_back(std::move(face));
    }
    out = std::move(f);
    return true;
}

int groupPeople(Project& p, float threshold, bool regroup) {
    // Every face, the clearest (largest, most certain) first, so groups form around good views.
    struct Ref {
        size_t media, face;
        float quality;
    };
    std::vector<Ref> refs;
    std::vector<std::shared_ptr<FaceIndex>> copies(p.media.size());
    for (size_t m = 0; m < p.media.size(); ++m) {
        if (!p.media[m].faces || p.media[m].faces->faces.empty()) continue;
        copies[m] = std::make_shared<FaceIndex>(*p.media[m].faces);
        for (size_t i = 0; i < copies[m]->faces.size(); ++i) {
            const auto& f = copies[m]->faces[i];
            refs.push_back({m, i, f.w * f.h * f.score});
        }
    }
    std::sort(refs.begin(), refs.end(), [](const Ref& a, const Ref& b) { return a.quality > b.quality; });
    struct Group {
        int id = 0;
        std::vector<double> sum;
        std::vector<float> mean;
        std::vector<Ref> members;
    };
    std::vector<Group> groups;
    auto join = [&](Group& g, const Ref& r, const std::vector<float>& e) {
        if (g.sum.empty()) g.sum.assign(e.size(), 0.0);
        g.members.push_back(r);
        double len = 0;
        for (size_t k = 0; k < e.size() && k < g.sum.size(); ++k) {
            g.sum[k] += e[k];
            len += g.sum[k] * g.sum[k];
        }
        g.mean.resize(g.sum.size());
        for (size_t k = 0; k < g.sum.size(); ++k) g.mean[k] = float(g.sum[k] / std::max(1e-12, std::sqrt(len)));
    };
    int next = 1;
    for (const Person& person : p.people) next = std::max(next, person.id + 1);
    for (const auto& c : copies)
        if (c)
            for (const auto& f : c->faces) next = std::max(next, f.person + 1);
    if (!regroup) {
        // The people found before keep their faces; only new faces are placed.
        std::map<int, size_t> at;
        std::vector<Ref> fresh;
        for (const Ref& r : refs) {
            const int person = copies[r.media]->faces[r.face].person;
            if (person <= 0) {
                fresh.push_back(r);
                continue;
            }
            auto [it, added] = at.emplace(person, groups.size());
            if (added) groups.push_back({person, {}, {}, {}});
            join(groups[it->second], r, copies[r.media]->embedding(r.face));
        }
        refs = std::move(fresh);
    }
    for (const Ref& r : refs) {
        const std::vector<float> e = copies[r.media]->embedding(r.face);
        int best = -1;
        float bestScore = threshold;
        for (size_t g = 0; g < groups.size(); ++g) {
            float s = 0;
            for (size_t k = 0; k < e.size() && k < groups[g].mean.size(); ++k) s += e[k] * groups[g].mean[k];
            if (s >= bestScore) bestScore = s, best = int(g);
        }
        if (best < 0) {
            groups.push_back({});
            best = int(groups.size()) - 1;
        }
        join(groups[size_t(best)], r, e);
    }
    // Ids: kept; or, regrouping, the one most members had before if no bigger group took it; else a new one.
    std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.members.size() > b.members.size(); });
    std::set<int> taken;
    for (const Group& g : groups)
        if (g.id) taken.insert(g.id);
    std::vector<Person> people;
    for (Group& g : groups) {
        if (!g.id) {
            std::map<int, int> votes;
            for (const Ref& r : g.members)
                if (int old = copies[r.media]->faces[r.face].person; old > 0) ++votes[old];
            int most = 0;
            for (const auto& [old, n] : votes)
                if (n > most && !taken.count(old)) g.id = old, most = n;
            if (!g.id) g.id = next++;
            taken.insert(g.id);
        }
        for (const Ref& r : g.members) copies[r.media]->faces[r.face].person = g.id;
        Person person;
        person.id = g.id;
        for (const Person& before : p.people)
            if (before.id == g.id) person.name = before.name;
        people.push_back(person);
    }
    for (size_t m = 0; m < p.media.size(); ++m)
        if (copies[m]) p.media[m].faces = copies[m];
    std::sort(people.begin(), people.end(), [](const Person& a, const Person& b) { return a.id < b.id; });
    p.people = people;
    return int(people.size());
}

bool mergePeople(Project& p, int from, int into) {
    if (from == into || from <= 0 || into <= 0) return false;
    bool any = false, hasInto = false;
    for (const Person& x : p.people) hasInto |= x.id == into;
    for (MediaItem& m : p.media) {
        if (!m.faces) continue;
        const auto& faces = m.faces->faces;
        if (std::none_of(faces.begin(), faces.end(), [&](const FaceIndex::Face& f) { return f.person == from; })) continue;
        auto copy = std::make_shared<FaceIndex>(*m.faces);
        for (FaceIndex::Face& f : copy->faces)
            if (f.person == from) f.person = into;
        m.faces = copy;
        any = true;
    }
    if (!any || !hasInto) return false;
    // The name kept is the one they were merged into, or else theirs.
    std::string name;
    for (const Person& x : p.people)
        if (x.id == from) name = x.name;
    for (Person& x : p.people)
        if (x.id == into && x.name.empty()) x.name = name;
    p.people.erase(std::remove_if(p.people.begin(), p.people.end(), [&](const Person& x) { return x.id == from; }), p.people.end());
    return true;
}

bool renamePerson(Project& p, int person, const std::string& name) {
    for (Person& x : p.people)
        if (x.id == person) {
            if (x.name == name) return false;
            x.name = name;
            return true;
        }
    return false;
}

std::string personName(const Project& p, int person) {
    for (const Person& x : p.people)
        if (x.id == person && !x.name.empty()) return x.name;
    return "Person " + std::to_string(person);
}

std::vector<PersonSummary> peopleIn(const Project& p) {
    std::map<int, PersonSummary> by;
    std::map<int, std::set<uint64_t>> mediaOf;
    std::map<int, float> bestSize;
    for (const MediaItem& m : p.media) {
        if (!m.faces) continue;
        for (const FaceIndex::Face& f : m.faces->faces) {
            if (f.person <= 0) continue;
            PersonSummary& s = by[f.person];
            s.id = f.person;
            ++s.faces;
            mediaOf[f.person].insert(m.id);
            const float size = f.w * f.h * f.score;
            if (size > bestSize[f.person]) {
                bestSize[f.person] = size;
                s.bestMedia = m.id;
                s.bestTime = f.time;
                s.x = f.x, s.y = f.y, s.w = f.w, s.h = f.h;
            }
        }
    }
    std::vector<PersonSummary> out;
    for (auto& [id, s] : by) {
        s.name = personName(p, id);
        s.media = int(mediaOf[id].size());
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const PersonSummary& a, const PersonSummary& b) { return a.faces > b.faces || (a.faces == b.faces && a.id < b.id); });
    return out;
}

std::vector<PersonMoment> findPerson(const Project& p, int person) {
    std::vector<PersonMoment> out;
    for (const MediaItem& m : p.media) {
        if (!m.faces) continue;
        const double step = m.faces->step > 0 ? m.faces->step : 1.0;
        std::vector<const FaceIndex::Face*> seen;
        for (const FaceIndex::Face& f : m.faces->faces)
            if (f.person == person) seen.push_back(&f);
        std::sort(seen.begin(), seen.end(), [](const auto* a, const auto* b) { return a->time < b->time; });
        float bestSize = -1;
        for (const FaceIndex::Face* f : seen) {
            const bool join = !out.empty() && out.back().media == m.id && f->time - out.back().end <= step * 1.01;
            if (!join) {
                PersonMoment pm;
                pm.media = m.id;
                pm.start = std::max(0.0, f->time - step / 2);
                pm.end = f->time;
                pm.best = f->time;
                out.push_back(pm);
                bestSize = -1;
            }
            out.back().end = f->time;
            if (f->w * f->h > bestSize) bestSize = f->w * f->h, out.back().best = f->time;
        }
        for (PersonMoment& pm : out)
            if (pm.media == m.id) pm.end = m.duration > 0 ? std::min(m.duration, pm.end + step / 2) : pm.end + step / 2;
    }
    return out;
}

}  // namespace montage
