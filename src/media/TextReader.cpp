#include "TextReader.h"

#include <QFile>
#include <QImage>
#include <QString>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "Decoder.h"
#include "core/OnScreenText.h"

#ifdef MONTAGE_WITH_ONNXRUNTIME
#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/onnxruntime_cxx_api.h>
#endif
#include "OrtSupport.h"
#endif

namespace montage {

const ModelPack& ocrModel() {
    static const ModelPack pack = [] {
        const std::string det = "https://huggingface.co/SWHL/RapidOCR/resolve/1cfba2e90fc938db55889873735088de210cc173/PP-OCRv4/";
        const std::string rec = "https://huggingface.co/monkt/paddleocr-onnx/resolve/7b02d0a30a07ba2b92ad1ff5a8941ae2c633de65/languages/";
        ModelPack p;
        p.id = "ocr";
        p.title = "text reading model";
        p.directoryEnv = "MONTAGE_OCR_MODEL";
        p.urlEnv = "MONTAGE_OCR_MODEL_URL";
        p.files = {
            {"ch_PP-OCRv4_det_infer.onnx", det + "ch_PP-OCRv4_det_infer.onnx", "d2a7720d45a54257208b1e13e36a8479894cb74155a5efe29462512d42f49da9", 4745517},
            {"en_rec.onnx", rec + "english/rec.onnx", "4e16deb22c4da6468bdca539b2cd3c8687825538b67109177c47d359ab994cd7", 7830888},
            {"en_dict.txt", rec + "english/dict.txt", "e025a66d31f327ba0c232e03f407ae8d105e1e709e7ccb3f408aa778c24e70d6", 1416},
            {"latin_rec.onnx", rec + "latin/rec.onnx", "614ffc2d6d3902d360fad7f1b0dd455ee45e877069d14c4e51a99dc4ef144409", 7862832},
            {"latin_dict.txt", rec + "latin/dict.txt", "3c0a8a79b612653c25f765271714f71281e4e955962c153e272b7b8c1d2b13ff", 1634},
        };
        return p;
    }();
    return pack;
}

namespace {

constexpr int kRecHeight = 48;
constexpr int kDetLimit = 960;    // the detector sees the picture's long side at most this big
constexpr float kBinary = 0.3f;   // text probability counted as text
constexpr float kBoxScore = 0.6f; // a box's mean probability to keep it
constexpr double kUnclip = 1.6;   // how far a box is grown past the text's core

bool latinLanguage(const std::string& lang) {
    const std::string l = QString::fromStdString(lang).toLower().left(2).toStdString();
    return !l.empty() && l != "en";
}

QImage frameImage(const Frame16& f) {
    QImage img(f.width, f.height, QImage::Format_RGBA64);
    for (int y = 0; y < f.height; ++y)
        std::memcpy(img.scanLine(y), f.px.data() + size_t(y) * size_t(f.width) * 4, size_t(f.width) * 8);
    return img;
}

}  // namespace

#ifdef MONTAGE_WITH_ONNXRUNTIME
bool ocrAvailable() { return true; }
struct TextReader::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-ocr"};
    std::unique_ptr<Ort::Session> det, rec;
    std::vector<std::string> chars;  // the recogniser's characters: index 1 on (0 is CTC's blank), a space last
};
#else
bool ocrAvailable() { return false; }
struct TextReader::Impl {
    std::vector<std::string> chars;
};
#endif

TextReader::TextReader() : d_(std::make_unique<Impl>()) {}
TextReader::~TextReader() = default;

std::shared_ptr<TextReader> TextReader::load(const std::string& language, std::string* error) {
    static std::mutex m;
    static auto* cached = new std::map<bool, std::shared_ptr<TextReader>>();
    const bool latin = latinLanguage(language);
    std::lock_guard lock(m);
    if (auto it = cached->find(latin); it != cached->end()) return it->second;
    const ModelPack& pack = ocrModel();
    if (!pack.installed()) {
        if (error) *error = "The text reading model is not downloaded";
        return nullptr;
    }
    std::shared_ptr<TextReader> r(new TextReader);
    QFile dict(QString::fromStdString(pack.path(pack.files[latin ? 4 : 2])));
    if (!dict.open(QIODevice::ReadOnly)) {
        if (error) *error = "The text reading model's characters could not be read";
        return nullptr;
    }
    for (QByteArray line : dict.readAll().split('\n')) {
        if (line.endsWith('\r')) line.chop(1);
        r->d_->chars.push_back(line.toStdString());
    }
    while (!r->d_->chars.empty() && r->d_->chars.back().empty()) r->d_->chars.pop_back();
    r->d_->chars.push_back(" ");
#ifdef MONTAGE_WITH_ONNXRUNTIME
    if (!ortUsable(error)) return nullptr;
    try {
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(int(std::max(1u, std::thread::hardware_concurrency())));
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        auto open = [&](const ModelFile& f) {
#ifdef _WIN32
            const std::wstring path = QString::fromStdString(pack.path(f)).toStdWString();
#else
            const std::string path = pack.path(f);
#endif
            return std::make_unique<Ort::Session>(r->d_->env, path.c_str(), so);
        };
        r->d_->det = open(pack.files[0]);
        r->d_->rec = open(pack.files[latin ? 3 : 1]);
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The text reading model could not be loaded: ") + e.what();
        return nullptr;
    }
#else
    if (error) *error = "This build of Montage cannot read text in pictures (it was built without ONNX Runtime)";
    return nullptr;
#endif
    (*cached)[latin] = r;
    return r;
}

std::vector<TextLine> TextReader::read(const Frame16& frame, const TextRegion& region, std::string* error) const {
    return read(frameImage(frame), region, error);
}

std::vector<TextLine> TextReader::read(const QImage& pictureIn, const TextRegion& region, std::string* error) const {
    std::vector<TextLine> out;
#ifdef MONTAGE_WITH_ONNXRUNTIME
    if (pictureIn.isNull()) return out;
    const int PW = pictureIn.width(), PH = pictureIn.height();
    const QRect area = QRect(QPoint(int(std::floor(std::clamp(region.x0, 0.0, 1.0) * PW)), int(std::floor(std::clamp(region.y0, 0.0, 1.0) * PH))),
                             QPoint(int(std::ceil(std::clamp(region.x1, 0.0, 1.0) * PW)) - 1, int(std::ceil(std::clamp(region.y1, 0.0, 1.0) * PH)) - 1))
                           .intersected(pictureIn.rect());
    if (area.width() < 8 || area.height() < 8) return out;
    const QImage picture = pictureIn.copy(area).convertToFormat(QImage::Format_RGB888);
    const int W = picture.width(), H = picture.height();
    try {
        auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        // ---- Where the text is: a probability map at a size divisible by 32, blue-green-red, ImageNet-normalised.
        const double scale = std::min(1.0, double(kDetLimit) / std::max(W, H));
        const int dw = std::max(32, int(std::lround(W * scale / 32.0)) * 32), dh = std::max(32, int(std::lround(H * scale / 32.0)) * 32);
        const QImage small = picture.scaled(dw, dh, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        std::vector<float> in(size_t(3) * size_t(dw) * size_t(dh));
        static const float mean[3] = {0.485f, 0.456f, 0.406f}, stdev[3] = {0.229f, 0.224f, 0.225f};
        for (int y = 0; y < dh; ++y) {
            const uchar* row = small.constScanLine(y);
            for (int x = 0; x < dw; ++x)
                for (int c = 0; c < 3; ++c) {
                    const float v = row[x * 3 + (2 - c)] / 255.0f;  // B, G, R
                    in[(size_t(c) * size_t(dh) + size_t(y)) * size_t(dw) + size_t(x)] = (v - mean[c]) / stdev[c];
                }
        }
        const int64_t shape[4] = {1, 3, dh, dw};
        Ort::Value t = Ort::Value::CreateTensor<float>(mem, in.data(), in.size(), shape, 4);
        const char* detIn[] = {"x"};
        const char* detOut[] = {"sigmoid_0.tmp_0"};
        auto res = d_->det->Run(Ort::RunOptions{nullptr}, detIn, &t, 1, detOut, 1);
        const float* prob = res[0].GetTensorData<float>();
        // ---- Boxes: connected runs of text probability (4-connected), scored by their mean, grown past the core.
        std::vector<int> label(size_t(dw) * size_t(dh), 0);
        struct Box {
            double x0, y0, x1, y1;
        };
        std::vector<Box> boxes;
        std::vector<int> stack;
        int next = 0;
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x) {
                const size_t i0 = size_t(y) * size_t(dw) + size_t(x);
                if (label[i0] || prob[i0] <= kBinary) continue;
                ++next;
                int bx0 = x, bx1 = x, by0 = y, by1 = y;
                double sum = 0;
                int count = 0;
                stack.assign(1, int(i0));
                label[i0] = next;
                while (!stack.empty()) {
                    const int i = stack.back();
                    stack.pop_back();
                    const int px = i % dw, py = i / dw;
                    sum += prob[i], ++count;
                    bx0 = std::min(bx0, px), bx1 = std::max(bx1, px), by0 = std::min(by0, py), by1 = std::max(by1, py);
                    const int nb[4][2] = {{px - 1, py}, {px + 1, py}, {px, py - 1}, {px, py + 1}};
                    for (const auto& n : nb) {
                        if (n[0] < 0 || n[1] < 0 || n[0] >= dw || n[1] >= dh) continue;
                        const size_t j = size_t(n[1]) * size_t(dw) + size_t(n[0]);
                        if (!label[j] && prob[j] > kBinary) label[j] = next, stack.push_back(int(j));
                    }
                }
                const double w = bx1 - bx0 + 1, h = by1 - by0 + 1;
                if (sum / count < kBoxScore || std::min(w, h) < 3) continue;
                const double grow = w * h * kUnclip / (2 * (w + h));
                boxes.push_back({std::max(0.0, (bx0 - grow) * W / dw), std::max(0.0, (by0 - grow) * H / dh),
                                 std::min(double(W), (bx1 + 1 + grow) * W / dw), std::min(double(H), (by1 + 1 + grow) * H / dh)});
            }
        // ---- Reading each box: 48 pixels high, as wide as its shape, blue-green-red in -1..1; CTC's best path.
        struct Read {
            Box box;
            std::string text;
            float confidence;
        };
        std::vector<Read> reads;
        for (const Box& b : boxes) {
            const QRect r(int(std::floor(b.x0)), int(std::floor(b.y0)), int(std::ceil(b.x1) - std::floor(b.x0)), int(std::ceil(b.y1) - std::floor(b.y0)));
            if (r.width() < 2 || r.height() < 2) continue;
            const int rw = std::clamp(int(std::ceil(double(kRecHeight) * r.width() / r.height())), 8, kRecHeight * 40);
            const QImage crop = picture.copy(r).scaled(rw, kRecHeight, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            std::vector<float> rin(size_t(3) * kRecHeight * size_t(rw));
            for (int y = 0; y < kRecHeight; ++y) {
                const uchar* row = crop.constScanLine(y);
                for (int x = 0; x < rw; ++x)
                    for (int c = 0; c < 3; ++c)
                        rin[(size_t(c) * kRecHeight + size_t(y)) * size_t(rw) + size_t(x)] = (row[x * 3 + (2 - c)] / 255.0f - 0.5f) / 0.5f;
            }
            const int64_t rshape[4] = {1, 3, kRecHeight, rw};
            Ort::Value rt = Ort::Value::CreateTensor<float>(mem, rin.data(), rin.size(), rshape, 4);
            const char* recIn[] = {"x"};
            const char* recOut[] = {"fetch_name_0"};
            auto rr = d_->rec->Run(Ort::RunOptions{nullptr}, recIn, &rt, 1, recOut, 1);
            const auto dims = rr[0].GetTensorTypeAndShapeInfo().GetShape();
            const size_t steps = size_t(dims[1]), classes = size_t(dims[2]);
            const float* p = rr[0].GetTensorData<float>();
            std::string text;
            double conf = 0;
            int got = 0;
            size_t last = 0;
            for (size_t s = 0; s < steps; ++s) {
                const float* row = p + s * classes;
                const size_t best = size_t(std::max_element(row, row + classes) - row);
                if (best != 0 && best != last && best - 1 < d_->chars.size()) {
                    text += d_->chars[best - 1];
                    conf += row[best], ++got;
                }
                last = best;
            }
            const QString trimmed = QString::fromStdString(text).trimmed();
            if (!trimmed.isEmpty()) reads.push_back({b, trimmed.toStdString(), got ? float(conf / got) : 0.0f});
        }
        // ---- Lines: pieces side by side at the same height joined, top to bottom.
        std::sort(reads.begin(), reads.end(), [](const Read& a, const Read& b) { return a.box.y0 + a.box.y1 < b.box.y0 + b.box.y1; });
        std::vector<std::vector<Read>> lines;
        for (const Read& r : reads) {
            bool placed = false;
            for (auto& l : lines) {
                const Box& f = l.front().box;
                const double overlap = std::min(f.y1, r.box.y1) - std::max(f.y0, r.box.y0);
                if (overlap > 0.5 * std::min(f.y1 - f.y0, r.box.y1 - r.box.y0)) {
                    l.push_back(r);
                    placed = true;
                    break;
                }
            }
            if (!placed) lines.push_back({r});
        }
        for (auto& l : lines) {
            std::sort(l.begin(), l.end(), [](const Read& a, const Read& b) { return a.box.x0 < b.box.x0; });
            TextLine tl;
            tl.x0 = 1e9, tl.y0 = 1e9, tl.x1 = -1, tl.y1 = -1;
            double conf = 0;
            for (const Read& r : l) {
                tl.text += (tl.text.empty() ? "" : " ") + r.text;
                tl.x0 = std::min(tl.x0, r.box.x0), tl.y0 = std::min(tl.y0, r.box.y0), tl.x1 = std::max(tl.x1, r.box.x1), tl.y1 = std::max(tl.y1, r.box.y1);
                conf += r.confidence;
            }
            tl.confidence = float(conf / double(l.size()));
            // Back to fractions of the whole picture.
            tl.x0 = (area.x() + tl.x0) / PW, tl.x1 = (area.x() + tl.x1) / PW, tl.y0 = (area.y() + tl.y0) / PH, tl.y1 = (area.y() + tl.y1) / PH;
            out.push_back(std::move(tl));
        }
    } catch (const Ort::Exception& e) {
        if (error) *error = e.what();
        out.clear();
    }
#else
    (void)pictureIn, (void)region;
    if (error) *error = "No ONNX Runtime";
#endif
    return out;
}

std::string textOf(const std::vector<TextLine>& lines, float minConfidence) {
    std::string out;
    for (const TextLine& l : lines)
        if (l.confidence >= minConfidence) out += (out.empty() ? "" : "\n") + l.text;
    return out;
}

bool readBurnedInSubtitles(const std::string& path, double from, double to, const TextRegion& region, Rational fps,
                           std::vector<Caption>& out, const std::string& language, double perSecond,
                           const std::function<void(double)>& progress, const std::atomic<bool>* cancel, std::string* error) {
    out.clear();
    auto reader = TextReader::load(language, error);
    if (!reader) return false;
    VideoDecoder dec;
    if (!dec.open(path, error)) return false;
    const double len = dec.duration();
    from = std::clamp(from, 0.0, std::max(0.0, len));
    if (to <= from) to = len;
    const double step = 1.0 / std::clamp(perSecond, 0.5, 30.0);
    std::vector<std::pair<double, std::string>> readings;
    // At most 1280 wide: subtitles stay legible and reading stays quick.
    const int fw = std::min(1280, std::max(1, dec.displayWidth()));
    const int fh = std::max(1, int(std::lround(double(fw) * dec.displayHeight() / std::max(1, dec.displayWidth()))));
    QImage previous;
    std::string previousText;
    for (double t = from; t < to - 1e-6; t += step) {
        if (cancel && cancel->load()) {
            if (error) *error = "Cancelled";
            return false;
        }
        Frame16Ptr f = dec.frameAt(t, fw, fh);
        if (!f) {
            readings.push_back({t, ""});
            continue;
        }
        // An unchanged picture in the region reads the same: not read again.
        const QImage img = frameImage(*f);
        const QRect band(int(region.x0 * fw), int(region.y0 * fh), std::max(1, int((region.x1 - region.x0) * fw)), std::max(1, int((region.y1 - region.y0) * fh)));
        const QImage now = img.copy(band).scaled(64, 16, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_Grayscale8);
        bool same = !previous.isNull();
        for (int y = 0; same && y < now.height(); ++y)
            for (int x = 0; x < now.width(); ++x)
                if (std::abs(int(now.constScanLine(y)[x]) - int(previous.constScanLine(y)[x])) > 6) {
                    same = false;
                    break;
                }
        std::string text = previousText;
        if (!same) {
            std::string err;
            text = textOf(reader->read(img, region, &err));
            if (!err.empty()) {
                if (error) *error = err;
                return false;
            }
        }
        previous = now;
        previousText = text;
        readings.push_back({t, text});
        if (progress) progress(std::min(1.0, (t - from + step) / std::max(step, to - from)));
    }
    out = captionsFromReadings(readings, step, fps);
    return true;
}

bool readSlateFromPicture(const std::string& path, SlateInfo& out, std::string* error, double seconds, const std::atomic<bool>* cancel) {
    auto reader = TextReader::load("en", error);
    if (!reader) return false;
    VideoDecoder dec;
    if (!dec.open(path, error)) return false;
    const double end = std::min(std::max(0.5, seconds), dec.duration() > 0 ? dec.duration() : seconds);
    const int fw = std::min(1280, std::max(1, dec.displayWidth()));
    const int fh = std::max(1, int(std::lround(double(fw) * dec.displayHeight() / std::max(1, dec.displayWidth()))));
    for (double t = 0.25; t < end; t += 0.5) {
        if (cancel && cancel->load()) {
            if (error) *error = "Cancelled";
            return false;
        }
        Frame16Ptr f = dec.frameAt(t, fw, fh);
        if (!f) continue;
        std::string err;
        const std::string text = textOf(reader->read(*f, {}, &err), 0.5f);
        if (auto s = slateFromText(text)) {
            out = *s;
            out.at = t;
            return true;
        }
    }
    if (error) *error = "No slate was found in the first seconds of the picture";
    return false;
}

}  // namespace montage
