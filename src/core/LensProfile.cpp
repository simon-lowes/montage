#include "LensProfile.h"

#include <QDir>
#include <QFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <map>
#include <regex>
#include <set>

namespace montage::lens {

namespace {

constexpr double kPi = 3.14159265358979323846;
// Half the diagonal of a 36 x 24 mm frame, which crop factors are measured against.
const double kHalfFullFrameDiagonal = std::sqrt(36.0 * 36.0 + 24.0 * 24.0) / 2;

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The words of a name: runs of letters and digits, lower case.
std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::string w;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            w += char(std::tolower(static_cast<unsigned char>(c)));
        } else if (!w.empty()) {
            out.push_back(w);
            w.clear();
        }
    }
    if (!w.empty()) out.push_back(w);
    return out;
}

double ratio(const QString& text) {
    const int colon = text.indexOf(':');
    if (colon < 0) return text.trimmed().toDouble();
    const double d = text.mid(colon + 1).trimmed().toDouble();
    return d > 0 ? text.left(colon).trimmed().toDouble() / d : 0;
}

Projection projectionFromName(const QString& s) {
    const QString n = s.trimmed().toLower();
    if (n == "rectilinear") return Projection::Rectilinear;
    if (n == "fisheye") return Projection::Fisheye;
    if (n == "panoramic") return Projection::Panoramic;
    if (n == "equirectangular") return Projection::Equirectangular;
    if (n == "orthographic") return Projection::Orthographic;
    if (n == "stereographic") return Projection::Stereographic;
    if (n == "equisolid") return Projection::Equisolid;
    if (n == "fisheye_thoby") return Projection::Thoby;
    return Projection::Unknown;
}

const char* projectionKey(Projection p) {
    switch (p) {
        case Projection::Rectilinear: return "rectilinear";
        case Projection::Fisheye: return "fisheye";
        case Projection::Panoramic: return "panoramic";
        case Projection::Equirectangular: return "equirectangular";
        case Projection::Orthographic: return "orthographic";
        case Projection::Stereographic: return "stereographic";
        case Projection::Equisolid: return "equisolid";
        case Projection::Thoby: return "fisheye_thoby";
        case Projection::Unknown: break;
    }
    return "unknown";
}

double attr(const QXmlStreamAttributes& a, const char* name, double def = 0) {
    const auto v = a.value(QLatin1String(name));
    return v.isEmpty() ? def : v.toString().toDouble();
}

// The calibration a lens's next measurements go in: the one for the same sensor, else a new one.
Calibration& calibrationFor(Lens& lens, double crop, double aspect) {
    for (Calibration& c : lens.calibrations)
        if (c.cropFactor == crop && c.aspectRatio == aspect) return c;
    lens.calibrations.push_back({});
    lens.calibrations.back().cropFactor = crop;
    lens.calibrations.back().aspectRatio = aspect;
    return lens.calibrations.back();
}

void addName(std::vector<std::string>& names, const std::string& n) {
    if (!n.empty() && std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
}

void parseCamera(QXmlStreamReader& x, Camera& c) {
    while (x.readNextStartElement()) {
        const auto n = x.name();
        const bool translated = !x.attributes().value(QLatin1String("lang")).isEmpty();
        const std::string text = x.readElementText().trimmed().toStdString();
        if (n == QLatin1String("maker")) {
            if (!translated) c.maker = text;
            addName(c.names, text);
            addName(c.makers, text);
        } else if (n == QLatin1String("model")) {
            if (!translated) c.model = text;
            addName(c.names, text);
        } else if (n == QLatin1String("variant")) {
            if (!translated) c.variant = text;
        } else if (n == QLatin1String("mount")) {
            c.mount = text;
        } else if (n == QLatin1String("cropfactor")) {
            c.cropFactor = QString::fromStdString(text).toDouble();
        }
    }
}

void parseCalibration(QXmlStreamReader& x, Lens& lens, double crop, double aspect) {
    const QXmlStreamAttributes a = x.attributes();
    crop = attr(a, "cropfactor", crop);
    if (!a.value(QLatin1String("aspect-ratio")).isEmpty()) aspect = ratio(a.value(QLatin1String("aspect-ratio")).toString());
    // Find the set after reading (its attributes are known now), adding to it as lensfun does.
    Calibration got;
    std::vector<std::pair<double, double>> realFocals;  // version 1's <real-focal-length focal real-focal>
    while (x.readNextStartElement()) {
        const auto n = x.name();
        const QXmlStreamAttributes e = x.attributes();
        const QString model = e.value(QLatin1String("model")).toString();
        if (n == QLatin1String("distortion")) {
            Distortion d;
            d.model = model == "poly3" ? Distortion::Poly3 : model == "poly5" ? Distortion::Poly5
                    : model == "ptlens" ? Distortion::PtLens : Distortion::None;
            d.focal = attr(e, "focal");
            d.terms[0] = attr(e, "a", attr(e, "k1"));
            d.terms[1] = attr(e, "b", attr(e, "k2"));
            d.terms[2] = attr(e, "c", attr(e, "k3"));
            d.realFocal = attr(e, "real-focal");
            d.realFocalMeasured = d.realFocal > 0;
            if (!d.realFocalMeasured)
                d.realFocal = d.model == Distortion::PtLens ? d.focal * (1 - d.terms[0] - d.terms[1] - d.terms[2])
                            : d.model == Distortion::Poly3  ? d.focal * (1 - d.terms[0])
                                                            : d.focal;
            if (d.model != Distortion::None) got.distortion.push_back(d);
        } else if (n == QLatin1String("tca")) {
            Tca t;
            t.model = model == "linear" ? Tca::Linear : model == "poly3" ? Tca::Poly3 : Tca::None;
            t.focal = attr(e, "focal");
            if (t.model == Tca::Linear) {
                t.terms[0] = attr(e, "kr", 1);
                t.terms[1] = attr(e, "kb", 1);
            } else {
                const char* keys[6] = {"vr", "vb", "cr", "cb", "br", "bb"};
                for (int i = 0; i < 6; ++i) t.terms[i] = attr(e, keys[i], i < 2 ? 1 : 0);
            }
            if (t.model != Tca::None) got.tca.push_back(t);
        } else if (n == QLatin1String("real-focal-length")) {
            realFocals.emplace_back(attr(e, "focal"), attr(e, "real-focal"));
        } else if (n == QLatin1String("vignetting")) {
            if (model == "pa") {
                Vignetting v;
                v.focal = attr(e, "focal");
                v.aperture = attr(e, "aperture");
                v.distance = attr(e, "distance");
                v.k[0] = attr(e, "k1");
                v.k[1] = attr(e, "k2");
                v.k[2] = attr(e, "k3");
                got.vignetting.push_back(v);
            }
        }
        x.skipCurrentElement();
    }
    for (const auto& [focal, real] : realFocals)
        for (Distortion& d : got.distortion)
            if (d.focal == focal && real > 0) {
                d.realFocal = real;
                d.realFocalMeasured = true;
            }
    Calibration& c = calibrationFor(lens, crop, aspect);
    c.distortion.insert(c.distortion.end(), got.distortion.begin(), got.distortion.end());
    c.tca.insert(c.tca.end(), got.tca.begin(), got.tca.end());
    c.vignetting.insert(c.vignetting.end(), got.vignetting.begin(), got.vignetting.end());
}

// A number as a name writes it ("2.8"), whatever the locale; its leading part as atof reads it ("5." is 5).
double number(const std::string& s) {
    bool ok = false;
    QString t = QString::fromStdString(s);
    while (!t.isEmpty()) {
        const double v = t.toDouble(&ok);
        if (ok) return v;
        t.chop(1);
    }
    return 0;
}

// lensfun's guesses at a lens's focal lengths and widest aperture from its name ("24-70mm f/2.8", "1:2.8 50mm",
// "2.8/50"), where the database does not give them; not for adapters and converters.
void guessFromName(Lens& l, double& minF, double& maxF, double& minA) {
    static const std::regex patterns[3] = {
        std::regex("[^:]*?([0-9]+[0-9.]*)[-]?([0-9]+[0-9.]*)?(mm)[[:space:]]+(f/|f|1/|1:)?([0-9.]+)(-[0-9.]+)?.*"),
        std::regex(".*?1:([0-9.]+)[-]?([0-9.]+)?[[:space:]]+([0-9.]+)[-]?([0-9.]+)?(mm)?.*"),
        std::regex(".*?([0-9.]+)[-]?([0-9.]+)?[\\s]*/[\\s]*([0-9.]+)[-]?([0-9.]+)?.*"),
    };
    static const int groups[3][3] = {{1, 2, 5}, {3, 4, 1}, {3, 4, 1}};
    static const std::regex extender(".*?[0-9](\\.[0.9]+)?x.*");
    const std::string& m = l.model;
    for (const char* w : {"adapter", "reducer", "booster", "extender", "converter", "magnifier"})
        if (m.find(w) != std::string::npos) return;
    if (std::regex_match(m, extender)) return;
    for (int i = 0; i < 3; ++i) {
        std::smatch r;
        if (!std::regex_match(m, r, patterns[i])) continue;
        if (r[groups[i][0]].matched) minF = number(r[groups[i][0]].str());
        if (r[groups[i][1]].matched) maxF = number(r[groups[i][1]].str());
        if (r[groups[i][2]].matched) minA = number(r[groups[i][2]].str());
        return;
    }
}

void parseLens(QXmlStreamReader& x, Lens& l) {
    // A calibration's sensor is the lens's, as far as it has been given (lensfun reads the file in order).
    double crop = 0, aspect = 1.5;
    while (x.readNextStartElement()) {
        const auto n = x.name();
        if (n == QLatin1String("calibration")) {
            parseCalibration(x, l, crop, aspect);
            continue;
        }
        if (n == QLatin1String("focal")) {
            const QXmlStreamAttributes a = x.attributes();
            const double value = attr(a, "value");
            l.minFocal = value > 0 ? value : attr(a, "min");
            l.maxFocal = value > 0 ? value : attr(a, "max", l.minFocal);
            x.skipCurrentElement();
            continue;
        }
        if (n == QLatin1String("aperture")) {
            l.minAperture = attr(x.attributes(), "min");
            x.skipCurrentElement();
            continue;
        }
        const bool translated = !x.attributes().value(QLatin1String("lang")).isEmpty();
        const QString text = x.readElementText().trimmed();
        if (n == QLatin1String("maker")) {
            if (!translated) l.maker = text.toStdString();
            addName(l.names, text.toStdString());
        } else if (n == QLatin1String("model")) {
            if (!translated) l.model = text.toStdString();
            addName(l.names, text.toStdString());
        } else if (n == QLatin1String("mount")) {
            l.mounts.push_back(text.toStdString());
        } else if (n == QLatin1String("type")) {
            l.type = projectionFromName(text);
        } else if (n == QLatin1String("cropfactor")) {
            crop = l.cropFactor = text.toDouble();
        } else if (n == QLatin1String("aspect-ratio")) {
            aspect = l.aspectRatio = ratio(text);
        }
    }
    // lensfun keeps the first calibration on the lens's own sensor.
    if (!l.calibrations.empty()) {
        if (l.cropFactor > 0) l.calibrations[0].cropFactor = l.cropFactor;
        l.calibrations[0].aspectRatio = l.aspectRatio;
        if (l.cropFactor <= 0) l.cropFactor = l.calibrations[0].cropFactor;
    }
    if (l.minFocal <= 0 || l.minAperture <= 0) {
        // As lensfun: the name's, widened to the focal lengths and apertures measured.
        double lo = DBL_MAX, hi = 0, aperture = DBL_MAX;
        guessFromName(l, lo, hi, aperture);
        auto focal = [&](double f) { lo = std::min(lo, f), hi = std::max(hi, f); };
        for (const Calibration& c : l.calibrations) {
            for (const Distortion& d : c.distortion) focal(d.focal);
            for (const Tca& t : c.tca) focal(t.focal);
            for (const Vignetting& v : c.vignetting) focal(v.focal), aperture = std::min(aperture, v.aperture);
        }
        if (l.minFocal <= 0 && lo != DBL_MAX) l.minFocal = lo;
        if (l.maxFocal <= 0 && hi > 0) l.maxFocal = hi;
        if (l.minAperture <= 0 && aperture != DBL_MAX) l.minAperture = aperture;
    }
    if (l.maxFocal < l.minFocal) l.maxFocal = l.minFocal;
}

void parseMount(QXmlStreamReader& x, Mount& m) {
    while (x.readNextStartElement()) {
        const auto n = x.name();
        const std::string text = x.readElementText().trimmed().toStdString();
        if (n == QLatin1String("name")) m.name = text;
        else if (n == QLatin1String("compat")) m.compat.push_back(text);
    }
}

// The calibration set lensfun takes for a picture of crop factor `crop`: among those with what `has` asks for, the
// one whose sensor is the smallest not much smaller than the picture's.
template <class Has>
const Calibration* pickCalibration(const Lens& lens, double crop, Has has) {
    const Calibration* best = nullptr;
    double bestRatio = 1e6;
    for (const Calibration& c : lens.calibrations) {
        if (!has(c)) continue;
        if (crop <= 0 || c.cropFactor <= 0) return &c;
        const double r = crop / c.cropFactor;
        if (r >= 0.96 && r < bestRatio) {
            bestRatio = r;
            best = &c;
        }
    }
    return best;
}

// lensfun's interpolation between calibrated focal lengths: a Hermite spline through the two either side (and the
// ones beyond them for its slopes); FLT_MAX marks one missing.
double hermite(double y1, double y2, double y3, double y4, double t) {
    const double tg2 = y1 == DBL_MAX ? y3 - y2 : (y3 - y1) * 0.5;
    const double tg3 = y4 == DBL_MAX ? y3 - y2 : (y4 - y2) * 0.5;
    const double t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * y2 + (t3 - 2 * t2 + t) * tg2 + (-2 * t3 + 3 * t2) * y3 + (t3 - t2) * tg3;
}

// The (up to) two calibrations either side of `focal`: [0] the second below, [1] the nearest below, [2] the nearest
// above, [3] the second above. Null when there is none; `exact` when one is at `focal`.
template <class T>
std::array<const T*, 4> neighbours(const std::vector<T>& all, double focal, const T*& exact) {
    std::array<const T*, 4> s{nullptr, nullptr, nullptr, nullptr};
    exact = nullptr;
    for (const T& c : all) {
        if (c.focal == focal) {
            exact = &c;
            return s;
        }
        if (c.focal < focal) {
            if (!s[1] || c.focal > s[1]->focal) {
                s[0] = s[1];
                s[1] = &c;
            } else if (!s[0] || c.focal > s[0]->focal) {
                s[0] = &c;
            }
        } else {
            if (!s[2] || c.focal < s[2]->focal) {
                s[3] = s[2];
                s[2] = &c;
            } else if (!s[3] || c.focal < s[3]->focal) {
                s[3] = &c;
            }
        }
    }
    return s;
}

}  // namespace

bool Lens::calibrated() const {
    for (const Calibration& c : calibrations)
        if (!c.distortion.empty() || !c.tca.empty() || !c.vignetting.empty()) return true;
    return false;
}

bool Database::addXml(const std::string& xml, std::string* error) {
    QXmlStreamReader x(QByteArray::fromStdString(xml));
    if (!x.readNextStartElement() || x.name() != QLatin1String("lensdatabase")) {
        if (error) *error = "Not a lens database";
        return false;
    }
    const int version = x.attributes().value(QLatin1String("version")).toInt();
    if (version > 2) {
        if (error) *error = "A lens database of version " + std::to_string(version) + ", newer than this Montage reads";
        return false;
    }
    std::vector<Camera> cameras;
    std::vector<Lens> lenses;
    std::vector<Mount> mounts;
    while (x.readNextStartElement()) {
        if (x.name() == QLatin1String("camera")) {
            Camera c;
            parseCamera(x, c);
            if (!c.maker.empty() && !c.model.empty()) cameras.push_back(std::move(c));
        } else if (x.name() == QLatin1String("lens")) {
            Lens l;
            parseLens(x, l);
            if (!l.model.empty()) lenses.push_back(std::move(l));
        } else if (x.name() == QLatin1String("mount")) {
            Mount m;
            parseMount(x, m);
            if (!m.name.empty()) mounts.push_back(std::move(m));
        } else {
            x.skipCurrentElement();
        }
    }
    if (x.hasError()) {
        if (error) *error = x.errorString().toStdString() + " at line " + std::to_string(x.lineNumber());
        return false;
    }
    cameras_.insert(cameras_.end(), cameras.begin(), cameras.end());
    lenses_.insert(lenses_.end(), lenses.begin(), lenses.end());
    for (Mount& m : mounts) {
        auto it = std::find_if(mounts_.begin(), mounts_.end(), [&](const Mount& o) { return o.name == m.name; });
        if (it == mounts_.end()) mounts_.push_back(std::move(m));
        else
            for (const std::string& c : m.compat) addName(it->compat, c);
    }
    return true;
}

int Database::addDirectory(const std::string& dir) {
    int n = 0;
    const QDir d(QString::fromStdString(dir));
    for (const QString& name : d.entryList({"*.xml"}, QDir::Files, QDir::Name)) {
        QFile f(d.filePath(name));
        if (f.open(QIODevice::ReadOnly) && addXml(f.readAll().toStdString())) ++n;
    }
    return n;
}

const Camera* Database::findCamera(const std::string& device) const {
    const std::vector<std::string> have = words(device);
    if (have.empty()) return nullptr;
    const std::set<std::string> haveSet(have.begin(), have.end());
    const Camera* best = nullptr;
    size_t bestScore = 0;
    for (const Camera& c : cameras_) {
        // A model name's words must all be there, one at least not the maker's (an unknown model of a known maker is
        // no camera); the maker's count when they are.
        std::set<std::string> makerWords;
        for (const std::string& maker : c.makers)
            for (const std::string& w : words(maker)) makerWords.insert(w);
        size_t score = 0;
        for (const std::string& model : c.names) {
            if (std::find(c.makers.begin(), c.makers.end(), model) != c.makers.end()) continue;
            const std::vector<std::string> m = words(model);
            if (m.empty() || !std::all_of(m.begin(), m.end(), [&](const std::string& w) { return haveSet.count(w); }) ||
                std::all_of(m.begin(), m.end(), [&](const std::string& w) { return makerWords.count(w); }))
                continue;
            size_t s = m.size() * 2;
            for (const std::string& w : makerWords) s += haveSet.count(w);
            score = std::max(score, s);
        }
        if (score > bestScore) {
            bestScore = score;
            best = &c;
        }
    }
    return best;
}

std::vector<const Lens*> Database::lensesFor(const Camera* camera) const {
    std::vector<const Lens*> own, adapted;
    std::set<std::string> takes;
    if (camera)
        for (const Mount& m : mounts_)
            if (m.name == camera->mount) takes.insert(m.compat.begin(), m.compat.end());
    // A lens measured on a smaller sensor than the camera's cannot correct its pictures (lensfun's rule); of lenses
    // listed more than once (measured on different sensors) the one measured nearest the camera's sensor, or on the
    // largest when there is no camera, which suits every picture.
    std::map<std::string, const Lens*> chosen;
    for (const Lens& l : lenses_) {
        if (!l.calibrated()) continue;
        if (camera && camera->cropFactor > 0 && l.cropFactor > camera->cropFactor / 0.96) continue;
        const std::string key = lower(lensName(l));
        auto [it, added] = chosen.emplace(key, &l);
        if (!added && (camera && camera->cropFactor > 0 ? l.cropFactor > it->second->cropFactor
                                                        : l.cropFactor < it->second->cropFactor))
            it->second = &l;
    }
    for (const Lens& l : lenses_) {
        if (!l.calibrated() || chosen[lower(lensName(l))] != &l) continue;
        if (!camera) {
            own.push_back(&l);
            continue;
        }
        if (std::find(l.mounts.begin(), l.mounts.end(), camera->mount) != l.mounts.end()) own.push_back(&l);
        else if (std::any_of(l.mounts.begin(), l.mounts.end(), [&](const std::string& m) { return takes.count(m); }))
            adapted.push_back(&l);
    }
    auto byName = [](const Lens* a, const Lens* b) { return lower(lensName(*a)) < lower(lensName(*b)); };
    std::stable_sort(own.begin(), own.end(), byName);
    std::stable_sort(adapted.begin(), adapted.end(), byName);
    own.insert(own.end(), adapted.begin(), adapted.end());
    return own;
}

const Lens* Database::fixedLens(const Camera* camera) const {
    if (!camera || std::any_of(mounts_.begin(), mounts_.end(), [&](const Mount& m) { return m.name == camera->mount; }))
        return nullptr;
    const Lens* found = nullptr;
    for (const Lens* l : lensesFor(camera))
        if (std::find(l->mounts.begin(), l->mounts.end(), camera->mount) != l->mounts.end()) {
            if (found) return nullptr;
            found = l;
        }
    return found;
}

const Camera* Database::camera(const std::string& name) const {
    for (const Camera& c : cameras_)
        if (cameraName(c) == name) return &c;
    for (const Camera& c : cameras_)
        if (lower(cameraName(c)) == lower(name)) return &c;
    return findCamera(name);
}

const Lens* Database::lens(const std::string& name, const Camera* on) const {
    const std::vector<const Lens*> all = lensesFor(on);
    for (const Lens* l : all)
        if (lensName(*l) == name) return l;
    for (const Lens* l : all)
        if (lower(lensName(*l)) == lower(name)) return l;
    // Every word asked for beginning a word of its maker and model names ("24-70" finds "24-70mm"); the shortest name
    // wins.
    const std::vector<std::string> want = words(name);
    if (want.empty()) return nullptr;
    const Lens* best = nullptr;
    for (const Lens* l : all) {
        std::vector<std::string> have;
        for (const std::string& n : l->names)
            for (const std::string& w : words(n)) have.push_back(w);
        auto found = [&](const std::string& w) {
            return std::any_of(have.begin(), have.end(), [&](const std::string& h) { return h.rfind(w, 0) == 0; });
        };
        if (std::all_of(want.begin(), want.end(), found) &&
            (!best || lensName(*l).size() < lensName(*best).size()))
            best = l;
    }
    return best;
}

std::string cameraName(const Camera& c) {
    if (lower(c.model).rfind(lower(c.maker), 0) == 0) return c.model;
    return c.maker + " " + c.model;
}

std::string lensName(const Lens& l) {
    if (l.maker.empty() || lower(l.model).rfind(lower(l.maker), 0) == 0) return l.model;
    return l.maker + " " + l.model;
}

std::string projectionName(Projection p) {
    switch (p) {
        case Projection::Rectilinear: return "Rectilinear";
        case Projection::Fisheye: return "Fisheye";
        case Projection::Panoramic: return "Panoramic";
        case Projection::Equirectangular: return "Equirectangular";
        case Projection::Orthographic: return "Orthographic fisheye";
        case Projection::Stereographic: return "Stereographic fisheye";
        case Projection::Equisolid: return "Equisolid fisheye";
        case Projection::Thoby: return "Thoby fisheye";
        case Projection::Unknown: break;
    }
    return "Unknown";
}

std::string lensXml(const Lens& l) {
    QByteArray out;
    QXmlStreamWriter w(&out);
    auto num = [](double v) { return QString::number(v, 'g', 17); };
    w.writeStartElement("lensdatabase");
    w.writeAttribute("version", "2");
    w.writeStartElement("lens");
    w.writeTextElement("maker", QString::fromStdString(l.maker));
    w.writeTextElement("model", QString::fromStdString(l.model));
    for (const std::string& n : l.names)
        if (n != l.maker && n != l.model) {
            // Other names keep the lens findable by them.
            w.writeStartElement("model");
            w.writeAttribute("lang", "x");
            w.writeCharacters(QString::fromStdString(n));
            w.writeEndElement();
        }
    for (const std::string& m : l.mounts) w.writeTextElement("mount", QString::fromStdString(m));
    w.writeTextElement("type", projectionKey(l.type));
    w.writeTextElement("cropfactor", num(l.cropFactor));
    w.writeTextElement("aspect-ratio", num(l.aspectRatio));
    if (l.minFocal > 0) {
        w.writeEmptyElement("focal");
        w.writeAttribute("min", num(l.minFocal));
        w.writeAttribute("max", num(l.maxFocal));
    }
    if (l.minAperture > 0) {
        w.writeEmptyElement("aperture");
        w.writeAttribute("min", num(l.minAperture));
    }
    for (const Calibration& c : l.calibrations) {
        w.writeStartElement("calibration");
        w.writeAttribute("cropfactor", num(c.cropFactor));
        w.writeAttribute("aspect-ratio", num(c.aspectRatio));
        for (const Distortion& d : c.distortion) {
            w.writeEmptyElement("distortion");
            const bool pt = d.model == Distortion::PtLens;
            w.writeAttribute("model", d.model == Distortion::Poly3 ? "poly3" : d.model == Distortion::Poly5 ? "poly5" : "ptlens");
            w.writeAttribute("focal", num(d.focal));
            if (d.realFocalMeasured) w.writeAttribute("real-focal", num(d.realFocal));
            w.writeAttribute(pt ? "a" : "k1", num(d.terms[0]));
            if (d.model != Distortion::Poly3) w.writeAttribute(pt ? "b" : "k2", num(d.terms[1]));
            if (pt) w.writeAttribute("c", num(d.terms[2]));
        }
        for (const Tca& t : c.tca) {
            w.writeEmptyElement("tca");
            w.writeAttribute("model", t.model == Tca::Linear ? "linear" : "poly3");
            w.writeAttribute("focal", num(t.focal));
            if (t.model == Tca::Linear) {
                w.writeAttribute("kr", num(t.terms[0]));
                w.writeAttribute("kb", num(t.terms[1]));
            } else {
                const char* keys[6] = {"vr", "vb", "cr", "cb", "br", "bb"};
                for (int i = 0; i < 6; ++i) w.writeAttribute(keys[i], num(t.terms[i]));
            }
        }
        for (const Vignetting& v : c.vignetting) {
            w.writeEmptyElement("vignetting");
            w.writeAttribute("model", "pa");
            w.writeAttribute("focal", num(v.focal));
            w.writeAttribute("aperture", num(v.aperture));
            w.writeAttribute("distance", num(v.distance));
            w.writeAttribute("k1", num(v.k[0]));
            w.writeAttribute("k2", num(v.k[1]));
            w.writeAttribute("k3", num(v.k[2]));
        }
        w.writeEndElement();
    }
    w.writeEndElement();
    w.writeEndElement();
    return out.toStdString();
}

std::shared_ptr<const Lens> parseLens(const std::string& xml) {
    Database db;
    if (!db.addXml(xml) || db.lenses().empty()) return nullptr;
    return std::make_shared<Lens>(db.lenses().front());
}

bool interpolateDistortion(const Lens& lens, double crop, double focal, Distortion& out, Calibration* from) {
    const Calibration* c = pickCalibration(lens, crop, [](const Calibration& c) { return !c.distortion.empty(); });
    if (!c) return false;
    if (from) *from = *c;
    // Only the first model the lens gives counts.
    const Distortion::Model model = c->distortion.front().model;
    std::vector<Distortion> same;
    for (const Distortion& d : c->distortion)
        if (d.model == model) same.push_back(d);
    const Distortion* exact = nullptr;
    const auto s = neighbours(same, focal, exact);
    if (exact) {
        out = *exact;
        return true;
    }
    if (!s[1] || !s[2]) {
        out = s[1] ? *s[1] : *s[2];
        return true;
    }
    out = Distortion{};
    out.model = model;
    out.focal = focal;
    const double t = (focal - s[1]->focal) / (s[2]->focal - s[1]->focal);
    out.realFocal = hermite(s[0] ? s[0]->realFocal : DBL_MAX, s[1]->realFocal, s[2]->realFocal,
                            s[3] ? s[3]->realFocal : DBL_MAX, t);
    out.realFocalMeasured = s[1]->realFocalMeasured && s[2]->realFocalMeasured;
    // The terms times the focal length are what change smoothly.
    for (int i = 0; i < 3; ++i)
        out.terms[i] = hermite(s[0] ? s[0]->terms[i] * s[0]->focal : DBL_MAX, s[1]->terms[i] * s[1]->focal,
                               s[2]->terms[i] * s[2]->focal, s[3] ? s[3]->terms[i] * s[3]->focal : DBL_MAX, t) /
                       focal;
    return true;
}

bool interpolateTca(const Lens& lens, double crop, double focal, Tca& out) {
    const Calibration* c = pickCalibration(lens, crop, [](const Calibration& c) { return !c.tca.empty(); });
    if (!c) return false;
    const Tca::Model model = c->tca.front().model;
    std::vector<Tca> same;
    for (const Tca& t : c->tca)
        if (t.model == model) same.push_back(t);
    const Tca* exact = nullptr;
    const auto s = neighbours(same, focal, exact);
    if (exact) {
        out = *exact;
        return true;
    }
    if (!s[1] || !s[2]) {
        out = s[1] ? *s[1] : *s[2];
        return true;
    }
    out = Tca{};
    out.model = model;
    out.focal = focal;
    const double t = (focal - s[1]->focal) / (s[2]->focal - s[1]->focal);
    for (int i = 0; i < 6; ++i) {
        // The linear terms (kr, kb, vr, vb) as they are; the others times the focal length.
        auto f = [&](const Tca* c) { return i < 2 ? 1.0 : c->focal; };
        out.terms[i] = hermite(s[0] ? s[0]->terms[i] * f(s[0]) : DBL_MAX, s[1]->terms[i] * f(s[1]),
                               s[2]->terms[i] * f(s[2]), s[3] ? s[3]->terms[i] * f(s[3]) : DBL_MAX, t) /
                       (i < 2 ? 1.0 : focal);
    }
    return true;
}

bool interpolateVignetting(const Lens& lens, double crop, double focal, double aperture, double distance,
                           Vignetting& out) {
    if (aperture <= 0 || distance <= 0) return false;
    const Calibration* c = pickCalibration(lens, crop, [](const Calibration& c) { return !c.vignetting.empty(); });
    if (!c) return false;
    // Each measurement weighted by its inverse distance (to the power 3.5) in focal length across the lens's range,
    // 4 / aperture and 0.1 / distance; none when the nearest is further than 1.
    auto dist = [&](const Vignetting& v) {
        double f1 = focal - lens.minFocal, f2 = v.focal - lens.minFocal;
        const double df = lens.maxFocal - lens.minFocal;
        if (df != 0) f1 /= df, f2 /= df;
        const double a = 4.0 / v.aperture - 4.0 / aperture, d = 0.1 / v.distance - 0.1 / distance;
        return std::sqrt((f2 - f1) * (f2 - f1) + a * a + d * d);
    };
    out = Vignetting{};
    out.focal = focal;
    out.aperture = aperture;
    out.distance = distance;
    double total = 0, nearest = DBL_MAX;
    for (const Vignetting& v : c->vignetting) {
        const double d = dist(v);
        if (d < 0.0001) {
            out = v;
            return true;
        }
        nearest = std::min(nearest, d);
        const double w = 1.0 / std::pow(d, 3.5);
        for (int i = 0; i < 3; ++i) out.k[i] += w * v.k[i];
        total += w;
    }
    if (nearest > 1 || total <= 0) return false;
    for (double& k : out.k) k /= total;
    return true;
}

Correction::Correction(const Lens& lens, const Settings& s, int width, int height) {
    width = std::max(width, 1);
    height = std::max(height, 1);
    cx_ = width / 2.0;
    cy_ = height / 2.0;
    // Sizes as lensfun measures them, between the outer pixels' centres.
    const double w = std::max(width - 1, 1), h = std::max(height - 1, 1);
    // The lens's own sensor (its first calibration's): what the units are measured on.
    const double calCrop = lens.cropFactor > 0 ? lens.cropFactor : 1;
    const double calAspect = lens.aspectRatio > 0 ? lens.aspectRatio : 1.5;
    const double calDiagonal = std::sqrt(calAspect * calAspect + 1);
    // The recording's crop factor: the camera's sensor (taken to have the calibration's shape) with the recording's
    // frame as large as it fits on it, then any further crop the recording makes.
    const double camera = s.cameraCrop > 0 ? s.cameraCrop : calCrop;
    const double ar = std::max(w, h) / std::min(w, h);
    const double fullWidth = std::sqrt(1 + 1 / (calAspect * calAspect)) / std::sqrt(1 + 1 / (ar * ar));
    const double fullHeight = std::sqrt(1 + calAspect * calAspect) / std::sqrt(1 + ar * ar);
    crop_ = camera * (ar >= calAspect ? fullWidth : fullHeight) * std::max(0.05, s.recordingCrop);
    // Units of half the calibration sensor's shorter side, per pixel.
    unit_ = 2 * calCrop * calDiagonal / (crop_ * std::hypot(w, h));
    vigUnit_ = calDiagonal;
    const double focal = s.focal > 0 ? s.focal : lens.minFocal;

    distortion_ = s.distortion && interpolateDistortion(lens, crop_, focal, dist_);
    tca_ = s.tca && interpolateTca(lens, crop_, focal, tcaCal_);
    vignetting_ = s.vignetting && interpolateVignetting(lens, crop_, focal, s.aperture, s.distance, vig_);
    type_ = lens.type;
    const bool fisheye = type_ == Projection::Fisheye || type_ == Projection::Equisolid ||
                         type_ == Projection::Stereographic || type_ == Projection::Orthographic ||
                         type_ == Projection::Thoby;
    if (s.rectilinear && fisheye && focal > 0) {
        // The focal length the projection is drawn with: the measured one where there is one, divided by Hugin's
        // correction as lensfun does; in calibration units.
        Distortion d;
        double f = focal;
        if (interpolateDistortion(lens, crop_, focal, d)) {
            const double hugin = d.model == Distortion::PtLens ? 1 - d.terms[0] - d.terms[1] - d.terms[2]
                               : d.model == Distortion::Poly3  ? 1 - d.terms[0]
                                                               : 1;
            if (d.realFocalMeasured && hugin != 0) f = d.realFocal / hugin;
        }
        const double mmPerUnit = kHalfFullFrameDiagonal / (calDiagonal * calCrop);
        focal_ = f / mmPerUnit;
        rectilinear_ = focal_ > 0;
    }

    // The scale that fills the frame (or shows all of the picture): radial, so along each direction from the middle
    // a frame edge's point comes from inside the recorded frame (or the recorded frame's edge is reached first).
    scale_ = 1;
    if (s.fit != Settings::None && (distortion_ || tca_ || rectilinear_)) {
        const double hx = cx_ * unit_, hy = cy_ * unit_;
        // How far past the recorded frame's edge (+) or inside it (-) the frame edge's points come from, at worst
        // (fill: the most outside; whole: the most inside).
        auto worst = [&](double scale) {
            double out = s.fit == Settings::Fill ? -DBL_MAX : DBL_MAX;
            // (And the corner's own angle, where the frame reaches furthest.)
            for (int i = 0; i <= 129; ++i) {
                const double phi = i == 129 ? std::atan2(hy, hx) : kPi / 2 * i / 128;
                const double c = std::cos(phi), sn = std::sin(phi);
                const double edge = std::min(c > 1e-9 ? hx / c : DBL_MAX, sn > 1e-9 ? hy / sn : DBL_MAX);
                for (int ch = 0; ch < 3; ++ch) {
                    const double g = radius(edge / scale, ch) - edge;
                    out = s.fit == Settings::Fill ? std::max(out, g) : std::min(out, g);
                }
            }
            return out;
        };
        double lo = s.fit == Settings::Fill ? 1.0 / 16 : 0.25, hi = 16;
        // Only while the correction still moves outward with the radius: a barrel polynomial turns back further out
        // (and goes negative), where it means nothing. So the frame's corner stays inside where it first turns.
        const double corner = std::hypot(hx, hy), far = corner / lo;
        double turn = far;
        for (int ch = 0; ch < 3; ++ch) {
            const int steps = 4096;
            double was = radius(0, ch);
            for (int i = 1; i <= steps; ++i) {
                const double r = far * i / steps, now = radius(r, ch);
                if (!(now > was)) {
                    turn = std::min(turn, far * (i - 1) / steps);
                    break;
                }
                was = now;
            }
        }
        if (turn < far) lo = std::min(hi, std::max(lo, corner / std::max(turn, 1e-9)));
        if (s.fit == Settings::Fill) {
            // The smallest scale with nothing outside.
            if (worst(lo) <= 0) scale_ = lo;
            else if (worst(hi) > 0) scale_ = 1;
            else {
                for (int k = 0; k < 50; ++k) {
                    const double m = std::sqrt(lo * hi);
                    (worst(m) <= 0 ? hi : lo) = m;
                }
                scale_ = hi;
            }
        } else {
            // The largest scale with all of the recorded frame inside.
            if (worst(hi) >= 0) scale_ = hi;
            else if (worst(lo) < 0) scale_ = lo;
            else {
                for (int k = 0; k < 50; ++k) {
                    const double m = std::sqrt(lo * hi);
                    (worst(m) >= 0 ? lo : hi) = m;
                }
                scale_ = lo;
            }
        }
    }
    scale_ *= s.scale > 0 ? s.scale : 1;
}

double Correction::fisheyeRadius(double r) const {
    const double theta = std::atan(r / focal_);
    double g = theta;
    switch (type_) {
        case Projection::Equisolid: g = 2 * std::sin(theta / 2); break;
        case Projection::Stereographic: g = 2 * std::tan(theta / 2); break;
        case Projection::Orthographic: g = std::sin(theta); break;
        case Projection::Thoby: g = 1.47 * std::sin(0.713 * theta); break;
        default: break;
    }
    return focal_ * g;
}

double Correction::distort(double ru) const {
    const double* k = dist_.terms;
    const double r2 = ru * ru;
    switch (dist_.model) {
        case Distortion::Poly3: return ru * (1 - k[0] + k[0] * r2);
        case Distortion::Poly5: return ru * (1 + k[0] * r2 + k[1] * r2 * r2);
        case Distortion::PtLens: return ru * (k[0] * r2 * ru + k[1] * r2 + k[2] * ru + 1 - k[0] - k[1] - k[2]);
        case Distortion::None: break;
    }
    return ru;
}

double Correction::radius(double r, int channel) const {
    double rd = rectilinear_ ? fisheyeRadius(r) : r;
    if (distortion_) rd = distort(rd);
    if (tca_ && channel != 1) {
        const int i = channel == 0 ? 0 : 1;
        const double* t = tcaCal_.terms;
        rd = tcaCal_.model == Tca::Linear ? rd * t[i] : rd * (t[4 + i] * rd * rd + t[2 + i] * rd + t[i]);
    }
    return rd;
}

double Correction::sourceRadius(double r, int channel) const { return radius(r / scale_, channel); }

void Correction::source(double x, double y, double rgb[6]) const {
    const double dx = (x - cx_) * unit_, dy = (y - cy_) * unit_;
    const double r = std::hypot(dx, dy);
    for (int ch = 0; ch < 3; ++ch) {
        const double f = r > 1e-12 ? sourceRadius(r, ch) / r : 1 / scale_;
        rgb[ch * 2] = cx_ + dx * f / unit_;
        rgb[ch * 2 + 1] = cy_ + dy * f / unit_;
    }
}

double Correction::falloff(double x, double y) const {
    if (!vignetting_) return 1;
    const double r = std::hypot(x - cx_, y - cy_) * unit_ / vigUnit_, r2 = r * r;
    const double c = 1 + vig_.k[0] * r2 + vig_.k[1] * r2 * r2 + vig_.k[2] * r2 * r2 * r2;
    return std::max(c, 0.05);
}

}  // namespace montage::lens
