#include "ShapeContents.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace montage {

namespace {

ParamInfo num(std::string name, std::string label, double min, double max, double def, double step = 0.1) {
    ParamInfo p;
    p.name = std::move(name);
    p.label = std::move(label);
    p.min = min, p.max = max, p.def = def, p.step = step;
    return p;
}
ParamInfo pct(std::string name, std::string label, double min, double max, double def) {
    ParamInfo p = num(std::move(name), std::move(label), min, max, def, 0.1);
    p.kind = ParamKind::Percent;
    return p;
}
ParamInfo angle(std::string name, std::string label, double def = 0) {
    ParamInfo p = num(std::move(name), std::move(label), -3600, 3600, def, 0.1);
    p.kind = ParamKind::Angle;
    return p;
}
ParamInfo choice(std::string name, std::string label, std::vector<std::string> choices, int def = 0) {
    ParamInfo p = num(std::move(name), std::move(label), 0, double(choices.size() - 1), def, 1);
    p.kind = ParamKind::Choice;
    p.choices = std::move(choices);
    p.keyframeable = false;
    return p;
}
ParamInfo color(std::string name, std::string label, double r, double g, double b) {
    ParamInfo p = num(std::move(name), std::move(label), 0, 1, r, 0.01);
    p.kind = ParamKind::Color;
    p.defG = g, p.defB = b;
    return p;
}

// The settings a fill and a stroke share: colour, opacity and a gradient between two points.
std::vector<ParamInfo> paint(double r, double g, double b) {
    return {color("color", "Color", r, g, b), pct("opacity", "Opacity", 0, 100, 100),
            choice("gradient", "Gradient", {"None", "Linear", "Radial"}, 0), color("color2", "Gradient End Color", 0.9, 0.2, 0.4),
            num("start_x", "Gradient Start X", -8000, 8000, -100), num("start_y", "Gradient Start Y", -8000, 8000, 0),
            num("end_x", "Gradient End X (radial: its edge)", -8000, 8000, 100), num("end_y", "Gradient End Y", -8000, 8000, 0)};
}

const char* const kPathComponents[6] = {"x", "y", "ix", "iy", "ox", "oy"};

std::string pointParam(const std::string& id, int i, int c) { return id + ".p" + std::to_string(i) + "." + kPathComponents[c]; }

double& component(PathPoint& p, int c) {
    double* f[6] = {&p.x, &p.y, &p.ix, &p.iy, &p.ox, &p.oy};
    return *f[c];
}

QJsonArray numbers(const double* v, int n) {
    QJsonArray a;
    for (int i = 0; i < n; ++i) a.append(v[i]);
    return a;
}

QJsonObject toJson(const ShapeItem& it) {
    QJsonObject o{{"type", QString::fromStdString(it.type)}, {"id", QString::fromStdString(it.id)}};
    if (!it.name.empty()) o["name"] = QString::fromStdString(it.name);
    if (!it.enabled) o["off"] = true;
    if (it.type == "path" && !it.closed) o["closed"] = false;
    if (it.type == "group") {
        QJsonArray items;
        for (const ShapeItem& c : it.items) items.append(toJson(c));
        o["items"] = items;
        if (it.hasMatrix) o["matrix"] = numbers(it.matrix.data(), 6);
    }
    if (!it.stops.empty()) {
        QJsonArray stops;
        for (const auto& s : it.stops) stops.append(numbers(s.data(), 5));
        o["stops"] = stops;
    }
    if (it.spread) o["spread"] = it.spread;
    if (it.hasFocal) o["focal"] = QJsonArray{it.focalX, it.focalY};
    if (it.hasGradientMatrix) o["gradient_matrix"] = numbers(it.gradientMatrix.data(), 6);
    return o;
}

ShapeItem fromJson(const QJsonObject& o) {
    ShapeItem it;
    it.type = o.value("type").toString().toStdString();
    it.id = o.value("id").toString().toStdString();
    it.name = o.value("name").toString().toStdString();
    it.enabled = !o.value("off").toBool(false);
    it.closed = o.value("closed").toBool(true);
    for (const QJsonValue& v : o.value("items").toArray())
        if (v.isObject()) it.items.push_back(fromJson(v.toObject()));
    auto six = [](const QJsonValue& v, std::array<double, 6>& out) {
        const QJsonArray a = v.toArray();
        if (a.size() != 6) return false;
        for (int i = 0; i < 6; ++i) out[size_t(i)] = a[i].toDouble();
        return true;
    };
    it.hasMatrix = six(o.value("matrix"), it.matrix);
    it.hasGradientMatrix = six(o.value("gradient_matrix"), it.gradientMatrix);
    for (const QJsonValue& v : o.value("stops").toArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() == 5) it.stops.push_back({a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble(), a[4].toDouble()});
    }
    it.spread = std::clamp(o.value("spread").toInt(0), 0, 2);
    if (const QJsonArray f = o.value("focal").toArray(); f.size() == 2) it.hasFocal = true, it.focalX = f[0].toDouble(), it.focalY = f[1].toDouble();
    return it;
}

}  // namespace

const std::vector<ShapeItemKind>& shapeItemKinds() {
    static const std::vector<ShapeItemKind> kinds = [] {
        std::vector<ShapeItemKind> k;
        k.push_back({"group", "Group",
                     {num("pos_x", "Position X", -8000, 8000, 0), num("pos_y", "Position Y", -8000, 8000, 0),
                      num("anchor_x", "Anchor X", -8000, 8000, 0), num("anchor_y", "Anchor Y", -8000, 8000, 0),
                      pct("scale_x", "Scale Width", -1000, 1000, 100), pct("scale_y", "Scale Height", -1000, 1000, 100),
                      angle("rotation", "Rotation"), pct("opacity", "Opacity", 0, 100, 100)}});
        k.push_back({"rect", "Rectangle",
                     {num("width", "Width (px)", 0, 8000, 200, 1), num("height", "Height (px)", 0, 8000, 150, 1),
                      num("pos_x", "Position X", -8000, 8000, 0), num("pos_y", "Position Y", -8000, 8000, 0),
                      num("roundness", "Corner Roundness (px)", 0, 4000, 0, 0.5)}});
        k.push_back({"ellipse", "Ellipse",
                     {num("width", "Width (px)", 0, 8000, 200, 1), num("height", "Height (px)", 0, 8000, 200, 1),
                      num("pos_x", "Position X", -8000, 8000, 0), num("pos_y", "Position Y", -8000, 8000, 0)}});
        k.push_back({"polygon", "Polygon",
                     {num("points", "Sides", 3, 64, 5, 1), num("radius", "Radius (px)", 0, 8000, 100, 1),
                      num("pos_x", "Position X", -8000, 8000, 0), num("pos_y", "Position Y", -8000, 8000, 0), angle("rotation", "Rotation"),
                      num("roundness", "Corner Roundness (px)", 0, 4000, 0, 0.5)}});
        k.push_back({"star", "Star",
                     {num("points", "Points", 3, 64, 5, 1), num("outer", "Outer Radius (px)", 0, 8000, 100, 1),
                      num("inner", "Inner Radius (px)", 0, 8000, 45, 1), num("pos_x", "Position X", -8000, 8000, 0),
                      num("pos_y", "Position Y", -8000, 8000, 0), angle("rotation", "Rotation"),
                      num("roundness", "Corner Roundness (px)", 0, 4000, 0, 0.5)}});
        k.push_back({"path", "Path", {}});
        {
            std::vector<ParamInfo> p = paint(0.95, 0.75, 0.1);
            p.push_back(choice("rule", "Fill Rule", {"Non-Zero", "Even-Odd"}, 0));
            k.push_back({"fill", "Fill", p});
        }
        {
            std::vector<ParamInfo> p = paint(1, 1, 1);
            p.insert(p.begin(), num("width", "Width (px)", 0, 2000, 4, 0.5));
            p.push_back(choice("cap", "Ends", {"Flat", "Round", "Square"}, 0));
            p.push_back(choice("join", "Corners", {"Sharp", "Round", "Bevelled"}, 0));
            p.push_back(num("miter", "Sharp Corner Limit", 1, 100, 4));
            p.push_back(num("dash", "Dash (px, 0 solid)", 0, 2000, 0, 0.5));
            p.push_back(num("gap", "Dash Gap (px)", 0, 2000, 10, 0.5));
            p.push_back(num("dash_offset", "Dash Offset (px)", -8000, 8000, 0, 0.5));
            k.push_back({"stroke", "Stroke", p});
        }
        k.push_back({"merge", "Combine Paths", {choice("mode", "Mode", {"Keep Each", "Union", "Subtract", "Intersect", "Exclude Overlaps"}, 1)}});
        k.push_back({"offset", "Offset Paths",
                     {num("amount", "Amount (px)", -2000, 2000, 10, 0.5), choice("join", "Corners", {"Sharp", "Round", "Bevelled"}, 0),
                      num("miter", "Sharp Corner Limit", 1, 100, 4)}});
        k.push_back({"round", "Round Corners", {num("radius", "Radius (px)", 0, 4000, 20, 0.5)}});
        k.push_back({"pucker", "Pinch and Bulge", {pct("amount", "Pinch (-) or Bulge (+)", -100, 100, 0)}});
        k.push_back({"twist", "Twist",
                     {angle("angle", "Angle", 90), num("center_x", "Centre X", -8000, 8000, 0), num("center_y", "Centre Y", -8000, 8000, 0)}});
        k.push_back({"zigzag", "Zig Zag",
                     {num("size", "Size (px)", -1000, 1000, 10, 0.5), num("ridges", "Ridges a Segment", 0, 100, 5, 1),
                      choice("points", "Points", {"Corner", "Smooth"}, 0)}});
        k.push_back({"wiggle", "Wiggle",
                     {num("size", "Size (px)", 0, 1000, 10, 0.5), num("spacing", "Points Every (px)", 2, 1000, 20, 0.5),
                      num("speed", "Wiggles a Second", 0, 60, 2), pct("correlation", "Points Moving Together", 0, 100, 50),
                      num("seed", "Seed", 0, 99999, 1, 1)}});
        k.push_back({"trim", "Trim",
                     {pct("start", "Start", 0, 100, 0), pct("end", "End", 0, 100, 100), angle("offset", "Offset"),
                      choice("each", "Paths", {"Together", "One After Another"}, 0)}});
        k.push_back({"repeater", "Repeater",
                     {num("copies", "Copies", 0, 1000, 3, 1), num("offset", "Offset", -1000, 1000, 0), num("pos_x", "Position X", -8000, 8000, 100),
                      num("pos_y", "Position Y", -8000, 8000, 0), num("anchor_x", "Anchor X", -8000, 8000, 0),
                      num("anchor_y", "Anchor Y", -8000, 8000, 0), pct("scale", "Scale", -1000, 1000, 100), angle("rotation", "Rotation"),
                      pct("start_opacity", "First Copy's Opacity", 0, 100, 100), pct("end_opacity", "Last Copy's Opacity", 0, 100, 100),
                      choice("composite", "Copies Go", {"Below", "Above"}, 0)}});
        return k;
    }();
    return kinds;
}

const ShapeItemKind* shapeItemKind(const std::string& type) {
    for (const ShapeItemKind& k : shapeItemKinds())
        if (k.type == type) return &k;
    return nullptr;
}

std::vector<ShapeItem> shapeContents(const Effect& g) {
    std::vector<ShapeItem> out;
    const std::string s = g.s("contents");
    if (s.empty()) return out;
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(s));
    for (const QJsonValue& v : doc.object().value("items").toArray())
        if (v.isObject()) out.push_back(fromJson(v.toObject()));
    return out;
}

std::string shapeContentsToString(const std::vector<ShapeItem>& items) {
    if (items.empty()) return {};
    QJsonArray a;
    for (const ShapeItem& it : items) a.append(toJson(it));
    return QJsonDocument(QJsonObject{{"items", a}}).toJson(QJsonDocument::Compact).toStdString();
}

void setShapeContents(Effect& g, const std::vector<ShapeItem>& items) {
    const std::string s = shapeContentsToString(items);
    if (s.empty()) g.strings.erase("contents");
    else g.strings["contents"] = s;
}

double shapeItemParam(const Effect& g, const ShapeItem& item, const std::string& name, FrameTime t) {
    double def = 0;
    if (const ShapeItemKind* k = shapeItemKind(item.type)) {
        const auto dot = name.find('.');
        const std::string base = dot == std::string::npos ? name : name.substr(0, dot);
        for (const ParamInfo& p : k->params)
            if (p.name == base) {
                def = p.kind == ParamKind::Color && dot != std::string::npos ? (name[dot + 1] == 'g' ? p.defG : name[dot + 1] == 'b' ? p.defB : p.def)
                                                                             : p.def;
                break;
            }
    }
    return g.p(item.id + "." + name, t, def);
}

std::string newShapeItemId(const std::vector<ShapeItem>& items, const std::string& type) {
    std::set<std::string> used;
    eachShapeItem(items, [&](const ShapeItem& it, int) { used.insert(it.id); });
    for (int n = 1;; ++n)
        if (const std::string id = type + std::to_string(n); !used.count(id)) return id;
}

ShapeItem* findShapeItem(std::vector<ShapeItem>& items, const std::string& id, std::vector<ShapeItem>** list, size_t* index) {
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].id == id) {
            if (list) *list = &items;
            if (index) *index = i;
            return &items[i];
        }
        if (ShapeItem* in = findShapeItem(items[i].items, id, list, index)) return in;
    }
    return nullptr;
}

const ShapeItem* findShapeItem(const std::vector<ShapeItem>& items, const std::string& id) {
    return findShapeItem(const_cast<std::vector<ShapeItem>&>(items), id);
}

void eraseShapeItemParams(Effect& g, const ShapeItem& item) {
    const std::string prefix = item.id + ".";
    for (auto it = g.params.begin(); it != g.params.end();) it = it->first.rfind(prefix, 0) == 0 ? g.params.erase(it) : std::next(it);
    for (const ShapeItem& c : item.items) eraseShapeItemParams(g, c);
}

void eachShapeItem(const std::vector<ShapeItem>& items, const std::function<void(const ShapeItem&, int depth)>& fn) {
    std::function<void(const std::vector<ShapeItem>&, int)> walk = [&](const std::vector<ShapeItem>& list, int depth) {
        for (const ShapeItem& it : list) {
            fn(it, depth);
            walk(it.items, depth + 1);
        }
    };
    walk(items, 0);
}

std::vector<PathPoint> shapePathPoints(const Effect& g, const std::string& id, FrameTime t) {
    std::vector<PathPoint> pts;
    for (int i = 0; g.params.count(pointParam(id, i, 0)); ++i) {
        PathPoint p;
        for (int c = 0; c < 6; ++c) component(p, c) = g.p(pointParam(id, i, c), t);
        pts.push_back(p);
    }
    return pts;
}

void setShapePathPoints(Effect& g, const std::string& id, FrameTime t, const std::vector<PathPoint>& pts, bool key) {
    const auto first = g.params.find(pointParam(id, 0, 0));
    const bool animated = key || (first != g.params.end() && first->second.animated());
    int n = 0;
    while (g.params.count(pointParam(id, n, 0))) ++n;
    for (int i = int(pts.size()); i < n; ++i)
        for (int c = 0; c < 6; ++c) g.params.erase(pointParam(id, i, c));
    for (size_t i = 0; i < pts.size(); ++i)
        for (int c = 0; c < 6; ++c) {
            Param& p = g.params[pointParam(id, int(i), c)];
            PathPoint q = pts[i];
            if (animated) p.addKey(t, component(q, c));
            else p.set(t, component(q, c));
        }
}

bool isShapePathParam(const std::string& name) {
    const auto at = name.find(".p");
    if (at == std::string::npos || at == 0) return false;
    size_t i = at + 2;
    const size_t digits = i;
    while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i]))) ++i;
    if (i == digits || i >= name.size() || name[i] != '.') return false;
    const std::string c = name.substr(i + 1);
    return std::any_of(std::begin(kPathComponents), std::end(kPathComponents), [&c](const char* k) { return c == k; });
}

std::optional<ShapeItem> shapePathFromMask(Effect& g, const std::vector<ShapeItem>& items, const Effect& mask, double frameW, double frameH) {
    if (std::lround(mask.p("mask.shape", 0)) != 5 || maskPathCount(mask) < 2) return std::nullopt;
    ShapeItem it;
    it.type = "path";
    it.id = newShapeItemId(items, "path");
    it.closed = mask.p("mask.open", 0) <= 0.5;
    const bool keyed = maskPathAnimated(mask);
    for (const FrameTime t : keyed ? maskPathKeyTimes(mask) : std::vector<FrameTime>{0}) {
        // Box units to the frame, then back through the layer's own position and rotation.
        const MaskBox box = maskBox(mask, t);
        const double px = g.p("pos_x", t), py = g.p("pos_y", t), r = -g.p("rotation", t) * M_PI / 180, c = std::cos(r), s = std::sin(r);
        auto toLayer = [&](double bx, double by) {
            double u = 0, v = 0;
            boxToFrame(box, frameW, frameH, bx, by, u, v);
            const double x = u * frameW - frameW / 2 - px, y = v * frameH - frameH / 2 - py;
            return std::pair{x * c - y * s, x * s + y * c};
        };
        std::vector<PathPoint> pts;
        for (const PathPoint& p : maskPath(mask, t)) {
            const auto [x, y] = toLayer(p.x, p.y);
            const auto [ix, iy] = toLayer(p.x + p.ix, p.y + p.iy);
            const auto [ox, oy] = toLayer(p.x + p.ox, p.y + p.oy);
            pts.push_back({x, y, ix - x, iy - y, ox - x, oy - y});
        }
        setShapePathPoints(g, it.id, t, pts, keyed);
    }
    return it;
}

}  // namespace montage
