#include "Sofa.h"

#include <QFile>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>

#include "core/Zip.h"

#ifdef MONTAGE_WITH_MYSOFA
#include <mysofa.h>
#endif

namespace montage {

bool sofaSupported() {
#ifdef MONTAGE_WITH_MYSOFA
    return true;
#else
    return false;
#endif
}

namespace {

// A zip's SOFA file: the only one, or the 48 kHz one.
bool sofaFromZip(const std::string& path, std::string& out, std::string* error) {
    ZipReader zip;
    if (!zip.openFile(path)) {
        if (error) *error = "Not a zip archive";
        return false;
    }
    const ZipReader::Entry* pick = nullptr;
    for (const auto& e : zip.entries()) {
        std::string name = e.name;
        for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (name.size() < 5 || name.compare(name.size() - 5, 5, ".sofa") != 0) continue;
        if (!pick || name.find("48k") != std::string::npos) pick = &e;
    }
    if (!pick) {
        if (error) *error = "The archive holds no SOFA file";
        return false;
    }
    return zip.read(*pick, out, error);
}

}  // namespace

std::shared_ptr<const Hrirs> readSofa(const std::string& path, double sampleRate, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return std::shared_ptr<const Hrirs>();
    };
#ifndef MONTAGE_WITH_MYSOFA
    (void)path, (void)sampleRate;
    return fail("This build of Montage cannot read SOFA files (it was built without libmysofa)");
#else
    std::string bytes;
    std::string lower = path;
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".zip") == 0) {
        std::string why;
        if (!sofaFromZip(path, bytes, &why)) return fail(why);
    } else {
        QFile f(QString::fromStdString(path));
        if (!f.open(QIODevice::ReadOnly)) return fail("Cannot open " + path);
        bytes = f.readAll().toStdString();
    }
    int err = 0;
    MYSOFA_HRTF* h = mysofa_load_data(bytes.data(), bytes.size(), &err);
    if (!h) return fail("Not a SOFA file libmysofa can read (error " + std::to_string(err) + ")");
    struct Free {
        MYSOFA_HRTF* h;
        ~Free() { mysofa_free(h); }
    } guard{h};
    if ((err = mysofa_check(h)) != MYSOFA_OK)
        return fail("Not head-related impulse responses (SimpleFreeFieldHRIR) libmysofa accepts (error " + std::to_string(err) + ")");
    if (h->R != 2 || h->M < 1 || h->N < 1) return fail("Not two ears' impulse responses");
    const double sr = sampleRate > 0 ? sampleRate : 48000;
    if (h->DataSamplingRate.elements < 1 || std::fabs(h->DataSamplingRate.values[0] - sr) > 0.5)
        if ((err = mysofa_resample(h, float(sr))) != MYSOFA_OK) return fail("Cannot resample the responses (error " + std::to_string(err) + ")");
    // As mysofa_open does: the level that of the front, and the positions as unit vectors (X ahead, Y left, Z up).
    mysofa_loudness(h);
    mysofa_tocartesian(h);
    auto out = std::make_shared<Hrirs>();
    const char* title = mysofa_getAttribute(h->attributes, const_cast<char*>("Title"));
    const char* listener = mysofa_getAttribute(h->attributes, const_cast<char*>("ListenerShortName"));
    out->name = title && *title ? title : listener && *listener ? listener : "SOFA";
    out->sampleRate = sr;
    const int n = int(h->N);
    // Delays (one per ear, or per measurement and ear) become leading zeros.
    auto delayOf = [&](unsigned m, int ear) {
        const unsigned e = h->DataDelay.elements;
        if (e == 2) return h->DataDelay.values[ear];
        if (e == 2 * h->M) return h->DataDelay.values[m * 2 + unsigned(ear)];
        return 0.0f;
    };
    int lead = 0;
    for (unsigned m = 0; m < h->M; ++m)
        for (int e = 0; e < 2; ++e) lead = std::max(lead, int(std::lround(std::max(0.0f, delayOf(m, e)))));
    const int cap = int(std::lround(512 * sr / 48000));
    out->length = std::min(n + lead, cap);
    for (const auto& d : binauralDirections()) {
        const double az = -d[0] * M_PI / 180, el = d[1] * M_PI / 180;
        const double want[3] = {std::cos(az) * std::cos(el), std::sin(az) * std::cos(el), std::sin(el)};
        unsigned best = 0;
        double bestDot = -2;
        for (unsigned m = 0; m < h->M; ++m) {
            const float* p = &h->SourcePosition.values[m * 3];
            const double len = std::sqrt(double(p[0]) * p[0] + double(p[1]) * p[1] + double(p[2]) * p[2]);
            if (len <= 0) continue;
            const double dot = (p[0] * want[0] + p[1] * want[1] + p[2] * want[2]) / len;
            if (dot > bestDot) bestDot = dot, best = m;
        }
        out->directions.push_back(d);
        for (int e = 0; e < 2; ++e) {
            std::vector<float>& ir = e == 0 ? out->left : out->right;
            const size_t at = ir.size();
            ir.resize(at + size_t(out->length), 0.0f);
            const int shift = int(std::lround(std::max(0.0f, delayOf(best, e))));
            const float* src = &h->DataIR.values[(size_t(best) * 2 + size_t(e)) * size_t(n)];
            for (int i = 0; i < n && shift + i < out->length; ++i) ir[at + size_t(shift + i)] = src[i];
            // Cut short: the last 32 taps faded out.
            if (n + shift > out->length)
                for (int i = 0; i < 32 && i < out->length; ++i) ir[at + size_t(out->length - 1 - i)] *= float(i) / 32;
        }
    }
    return out;
#endif
}

const ModelPack& sadieKu100Pack() {
    static const ModelPack pack = [] {
        ModelPack p;
        p.id = "sadie-ku100";
        p.title = "KU100 head-related impulse responses";
        p.directoryEnv = "MONTAGE_HRTF_DIR";
        p.urlEnv = "MONTAGE_HRTF_URL";
        p.files.push_back({"D1_HRIR_SOFA.zip", "https://zenodo.org/records/12092466/files/D1_HRIR_SOFA.zip?download=1",
                           "366321fa78f211bacc0ec6bea96701625b196a5db54ec16748b0eab0b9705f75", 38151804});
        return p;
    }();
    return pack;
}

namespace {

std::mutex hrtfMutex;
std::string hrtfChoice;
std::atomic<uint64_t> hrtfGeneration{1};
struct Loaded {
    std::shared_ptr<const Hrirs> hrirs;
    std::string error;
    bool done = false;  // false while being read in the background
};
std::map<double, Loaded> hrtfCache;  // for the current choice, by rate

std::shared_ptr<const Hrirs> loadHrtf(const std::string& choice, double sampleRate, std::string& why) {
    if (choice == "ku100") {
        const ModelPack& pack = sadieKu100Pack();
        if (!pack.installed()) {
            why = "SADIE II's KU100 responses are not downloaded";
            return nullptr;
        }
        return readSofa(pack.path(pack.files.front()), sampleRate, &why);
    }
    return readSofa(choice, sampleRate, &why);
}

}  // namespace

void setBinauralHrtf(const std::string& choice) {
    std::lock_guard lock(hrtfMutex);
    if (choice == hrtfChoice) return;
    hrtfChoice = choice;
    hrtfCache.clear();
    ++hrtfGeneration;
}

std::string binauralHrtf() {
    std::lock_guard lock(hrtfMutex);
    return hrtfChoice;
}

uint64_t binauralHrtfGeneration() { return hrtfGeneration.load(); }

std::shared_ptr<const Hrirs> binauralHrirs(double sampleRate, bool wait, std::string* error) {
    std::unique_lock lock(hrtfMutex);
    if (hrtfChoice.empty()) return nullptr;
    const std::string choice = hrtfChoice;
    if (auto it = hrtfCache.find(sampleRate); it != hrtfCache.end() && (it->second.done || !wait)) {
        if (error) *error = it->second.error;
        return it->second.hrirs;
    }
    if (!wait) {
        // Read in the background (a measured set takes a moment); the spherical head meanwhile, and renderers made
        // again (a new generation) once it is in.
        hrtfCache[sampleRate] = Loaded{};
        std::thread([choice, sampleRate] {
            std::string why;
            auto h = loadHrtf(choice, sampleRate, why);
            std::lock_guard lock(hrtfMutex);
            if (choice != hrtfChoice) return;
            hrtfCache[sampleRate] = Loaded{h, why, true};
            ++hrtfGeneration;
        }).detach();
        return nullptr;
    }
    lock.unlock();
    std::string why;
    auto h = loadHrtf(choice, sampleRate, why);
    lock.lock();
    if (choice == hrtfChoice && !hrtfCache[sampleRate].done) {
        hrtfCache[sampleRate] = Loaded{h, why, true};
        ++hrtfGeneration;  // (renderers made while it was read in the background)
    }
    if (error) *error = why;
    return h;
}

}  // namespace montage
