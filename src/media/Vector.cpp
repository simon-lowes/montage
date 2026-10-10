#include "Vector.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

#ifdef MONTAGE_WITH_THORVG
#include <thorvg.h>
#endif

namespace montage {

namespace {

std::string lowerExtension(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

// A Lottie file starts with its frame rate and in and out points, before any assets.
bool looksLikeLottie(const std::string& path) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = f.read(8192);
    return head.trimmed().startsWith('{') && head.contains("\"fr\"") && head.contains("\"ip\"") && head.contains("\"op\"");
}

}  // namespace

#ifdef MONTAGE_WITH_THORVG

class VectorDocument {
public:
    std::mutex m;
    std::string data, mime, dir;
    VectorInfo info;
    // The renderer for the size last asked for.
    std::unique_ptr<tvg::SwCanvas> canvas;
    std::unique_ptr<tvg::Animation> animation;
    std::vector<uint32_t> buffer;
    int cw = 0, ch = 0;
    float lastFrame = -1;

    bool build(int w, int h) {
        canvas.reset();
        animation.reset(tvg::Animation::gen());
        if (!animation) return false;
        tvg::Picture* pic = animation->picture();
        if (pic->load(data.data(), uint32_t(data.size()), mime.c_str(), dir.empty() ? nullptr : dir.c_str(), true) !=
            tvg::Result::Success)
            return false;
        pic->size(float(w), float(h));
        canvas.reset(tvg::SwCanvas::gen());
        if (!canvas) return false;
        buffer.assign(size_t(w) * size_t(h), 0);
        if (canvas->target(buffer.data(), uint32_t(w), uint32_t(w), uint32_t(h), tvg::ColorSpace::ABGR8888S) != tvg::Result::Success)
            return false;
        if (canvas->add(pic) != tvg::Result::Success) return false;
        cw = w;
        ch = h;
        lastFrame = -1;
        return true;
    }
};

namespace {

void initThorvg() {
    static std::once_flag once;
    std::call_once(once, [] {
        const unsigned n = std::max(1u, std::min(4u, std::thread::hardware_concurrency() / 2));
        tvg::Initializer::init(n);
    });
}

}  // namespace

bool vectorSupport() { return true; }

bool isVectorPath(const std::string& path) {
    const std::string ext = lowerExtension(path);
    if (ext == "svg") return true;
    return ext == "json" && looksLikeLottie(path);
}

std::shared_ptr<VectorDocument> openVector(const std::string& path, VectorInfo& info, std::string* error) {
    initThorvg();
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "Cannot open " + path;
        return nullptr;
    }
    auto doc = std::make_shared<VectorDocument>();
    const QByteArray bytes = f.readAll();
    doc->data.assign(bytes.constData(), size_t(bytes.size()));
    doc->mime = lowerExtension(path) == "svg" ? "svg" : "lot";
    // Images a Lottie or SVG refers to by name are found beside it.
    doc->dir = QFileInfo(QString::fromStdString(path)).absolutePath().toStdString() + "/";
    std::unique_ptr<tvg::Animation> probe(tvg::Animation::gen());
    tvg::Picture* pic = probe ? probe->picture() : nullptr;
    if (!pic || pic->load(doc->data.data(), uint32_t(doc->data.size()), doc->mime.c_str(), doc->dir.c_str(), true) !=
                    tvg::Result::Success) {
        if (error) *error = (doc->mime == "svg" ? "Cannot read the SVG " : "Cannot read the Lottie animation ") + path;
        return nullptr;
    }
    float w = 0, h = 0;
    pic->size(&w, &h);
    VectorInfo vi;
    vi.width = std::max(1, int(std::lround(w)));
    vi.height = std::max(1, int(std::lround(h)));
    const float frames = probe->totalFrame(), seconds = probe->duration();
    if (frames > 1 && seconds > 0) {
        vi.animated = true;
        vi.duration = seconds;
        vi.fps = frames / seconds;
        // Exported rates are whole numbers or NTSC ones; frames / seconds can be off in the last digit.
        if (std::fabs(vi.fps - std::round(vi.fps)) < 1e-3) vi.fps = std::round(vi.fps);
    }
    doc->info = vi;
    info = vi;
    return doc;
}

Frame16Ptr renderVector(VectorDocument& doc, double t, int w, int h) {
    if (w <= 0) w = doc.info.width;
    if (h <= 0) h = doc.info.height;
    std::lock_guard lock(doc.m);
    if ((doc.cw != w || doc.ch != h || !doc.canvas) && !doc.build(w, h)) return nullptr;
    float frame = 0;
    if (doc.info.animated) {
        const double total = doc.animation->totalFrame();
        frame = float(std::clamp(t * doc.info.fps, 0.0, std::max(0.0, total - 1e-3)));
    }
    if (frame != doc.lastFrame) {
        doc.animation->frame(frame);  // "insufficient condition" just means it is already there
        doc.canvas->update();
        doc.lastFrame = frame;
    }
    std::fill(doc.buffer.begin(), doc.buffer.end(), 0u);
    if (doc.canvas->draw(true) != tvg::Result::Success || doc.canvas->sync() != tvg::Result::Success) return nullptr;
    auto f = std::make_shared<Frame16>();
    f->width = w;
    f->height = h;
    f->pts = doc.info.animated ? frame / doc.info.fps : 0;
    f->px.resize(size_t(w) * size_t(h) * 4);
    // ABGR8888S: un-premultiplied, red in the low byte.
    for (size_t i = 0; i < doc.buffer.size(); ++i) {
        const uint32_t v = doc.buffer[i];
        uint16_t* o = &f->px[i * 4];
        o[0] = uint16_t((v & 0xff) * 257);
        o[1] = uint16_t(((v >> 8) & 0xff) * 257);
        o[2] = uint16_t(((v >> 16) & 0xff) * 257);
        o[3] = uint16_t(((v >> 24) & 0xff) * 257);
    }
    return f;
}

#else

class VectorDocument {};

bool vectorSupport() { return false; }
bool isVectorPath(const std::string&) { return false; }
std::shared_ptr<VectorDocument> openVector(const std::string& path, VectorInfo&, std::string* error) {
    if (error) *error = "Built without ThorVG: cannot open " + path;
    return nullptr;
}
Frame16Ptr renderVector(VectorDocument&, double, int, int) { return nullptr; }

#endif

}  // namespace montage
