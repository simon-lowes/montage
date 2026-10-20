#include "DescribePicture.h"

#include <QString>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

#include "Decoder.h"
#include "SoundSearch.h"

#ifdef MONTAGE_WITH_ONNXRUNTIME
#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/onnxruntime_cxx_api.h>
#endif
#include "OrtSupport.h"
#endif

namespace montage {

const ModelPack& describeModel() {
    static const ModelPack pack = [] {
        const std::string base = "https://huggingface.co/onnx-community/Florence-2-large/resolve/b7751295927207b2a8c2f354af8baadea5b0404b/";
        ModelPack p;
        p.id = "florence-2-large";
        p.title = "picture description model";
        p.directoryEnv = "MONTAGE_DESCRIBE_MODEL";
        p.urlEnv = "MONTAGE_DESCRIBE_MODEL_URL";
        p.files = {
            // 4-bit weights with float maths throughout (no integer matrix kernels: ONNX Runtime's AMX one crashed on
            // Windows). The token embeddings are a lookup, kept in float.
            {"vision_encoder_q4.onnx", base + "onnx/vision_encoder_q4.onnx",
             "34dff602f09ee6dd50a075df84d0030400d9989bd4d603f5759e2d1c4a576408", 315744335},
            {"embed_tokens_q4.onnx", base + "onnx/embed_tokens_q4.onnx",
             "8e2d2d452503b2c75683f21e96659a13b3c3da1561ca59a5294ee35f9f49b78c", 210080062},
            {"encoder_model_q4.onnx", base + "onnx/encoder_model_q4.onnx",
             "aa3fdccdbab224adcfbf3c5df7a95ea89cdffc7e917bac9f8a7879b94560c1f0", 99451927},
            {"decoder_model_merged_q4.onnx", base + "onnx/decoder_model_merged_q4.onnx",
             "d13b29ac7da5a998c73d9d8bf9e9380425dc5d03398a82507831651e1a47a374", 165295714},
            {"vocab.json", base + "vocab.json", "ed19656ea1707df69134c4af35c8ceda2cc9860bf2c3495026153a133670ab5e", 798293},
            {"merges.txt", base + "merges.txt", "1ce1664773c50f3e0cc8842619a93edc4624525b728b188a9e0be33b7726adc5", 456318},
        };
        return p;
    }();
    return pack;
}

namespace {

// The task prompts Florence-2 was trained on.
const char* taskPrompt(PictureDetail d) {
    return d == PictureDetail::Brief ? "What does the image describe?" : "Describe in detail what is shown in the image.";
}

// `img` scaled to kDescribeInput square (box-averaged where it shrinks, bilinear where it grows), as Florence-2's
// input: RGB planes, each value (v - mean) / std with ImageNet's mean and spread.
std::vector<float> pictureInput(const Image& img) {
    constexpr int n = kDescribeInput;
    static const float mean[3] = {0.485f, 0.456f, 0.406f}, spread[3] = {0.229f, 0.224f, 0.225f};
    std::vector<float> out(size_t(3) * n * n);
    const double sx = double(img.width) / n, sy = double(img.height) / n;
    parallelRows(n, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < n; ++x) {
                double rgb[3] = {0, 0, 0};
                if (sx > 1 || sy > 1) {
                    const int ax = std::clamp(int(std::floor(x * sx)), 0, img.width - 1);
                    const int bx = std::clamp(int(std::ceil((x + 1) * sx)), ax + 1, img.width);
                    const int ay = std::clamp(int(std::floor(y * sy)), 0, img.height - 1);
                    const int by = std::clamp(int(std::ceil((y + 1) * sy)), ay + 1, img.height);
                    for (int v = ay; v < by; ++v)
                        for (int u = ax; u < bx; ++u)
                            for (int c = 0; c < 3; ++c) rgb[c] += img.at(u, v)[c];
                    for (double& c : rgb) c /= double((bx - ax) * (by - ay));
                } else {
                    const double fx = std::clamp((x + 0.5) * sx - 0.5, 0.0, img.width - 1.0);
                    const double fy = std::clamp((y + 0.5) * sy - 0.5, 0.0, img.height - 1.0);
                    const int x0 = int(fx), y0i = int(fy), x1 = std::min(x0 + 1, img.width - 1), y1i = std::min(y0i + 1, img.height - 1);
                    const double tx = fx - x0, ty = fy - y0i;
                    for (int c = 0; c < 3; ++c)
                        rgb[c] = (img.at(x0, y0i)[c] * (1 - tx) + img.at(x1, y0i)[c] * tx) * (1 - ty) +
                                 (img.at(x0, y1i)[c] * (1 - tx) + img.at(x1, y1i)[c] * tx) * ty;
                }
                for (int c = 0; c < 3; ++c)
                    out[(size_t(c) * n + y) * n + x] = (float(std::clamp(rgb[c], 0.0, 1.0)) - mean[c]) / spread[c];
            }
    });
    return out;
}

}  // namespace

#ifdef MONTAGE_WITH_ONNXRUNTIME

bool describeAvailable() { return true; }

struct PictureDescriber::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-describe"};
    std::unique_ptr<OrtModel> vision, embed, encoder, decoder;
    ClapTokenizer tokenizer;
    std::vector<std::string> decoderInputs, decoderOutputs;
    mutable std::mutex busy;
};

#else

bool describeAvailable() { return false; }
struct PictureDescriber::Impl {
    ClapTokenizer tokenizer;
};

#endif

PictureDescriber::PictureDescriber() : d_(std::make_unique<Impl>()) {}
PictureDescriber::~PictureDescriber() = default;

std::shared_ptr<PictureDescriber> PictureDescriber::load(std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<PictureDescriber>();  // kept for the session
    std::lock_guard lock(m);
    if (*cached) return *cached;
    const ModelPack& pack = describeModel();
    if (!pack.installed()) {
        if (error) *error = "The picture description model is not downloaded";
        return nullptr;
    }
    std::shared_ptr<PictureDescriber> model(new PictureDescriber);
    if (!model->d_->tokenizer.load(pack.path(pack.files[4]), pack.path(pack.files[5]))) {
        if (error) *error = "The picture description model's vocabulary could not be read";
        return nullptr;
    }
#ifdef MONTAGE_WITH_ONNXRUNTIME
    if (!ortUsable(error)) return nullptr;
    try {
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(int(std::max(1u, std::thread::hardware_concurrency())));
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        auto open = [&](const ModelFile& f, ModelRuns where) {
#ifdef _WIN32
            const std::wstring path = QString::fromStdString(pack.path(f)).toStdWString();
#else
            const std::string path = pack.path(f);
#endif
            return openModel(model->d_->env, path.c_str(), so, where);
        };
        model->d_->vision = open(pack.files[0], ModelRuns::Anywhere);
        model->d_->embed = open(pack.files[1], ModelRuns::OnProcessor);
        model->d_->encoder = open(pack.files[2], ModelRuns::Anywhere);
        model->d_->decoder = open(pack.files[3], ModelRuns::OnProcessor);  // (a word at a time)
        Ort::AllocatorWithDefaultOptions alloc;
        for (size_t i = 0; i < model->d_->decoder->GetInputCount(); ++i)
            model->d_->decoderInputs.push_back(model->d_->decoder->GetInputNameAllocated(i, alloc).get());
        for (size_t i = 0; i < model->d_->decoder->GetOutputCount(); ++i)
            model->d_->decoderOutputs.push_back(model->d_->decoder->GetOutputNameAllocated(i, alloc).get());
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The picture description model could not be loaded: ") + e.what();
        return nullptr;
    }
#else
    if (error) *error = "This build of Montage cannot describe pictures: it was built without ONNX Runtime";
    return nullptr;
#endif
    *cached = model;
    return model;
}

std::string PictureDescriber::describe(const Image& picture, PictureDetail detail, std::string* error,
                                       const std::atomic<bool>* cancel) const {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return std::string();
    };
    if (picture.empty()) return fail("There is no picture to describe");
#ifdef MONTAGE_WITH_ONNXRUNTIME
    auto cancelled = [&] { return cancel && cancel->load(); };
    std::lock_guard lock(d_->busy);
    try {
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto embed = [&](std::vector<int64_t>& ids) {
            const int64_t shape[2] = {1, int64_t(ids.size())};
            Ort::Value in = Ort::Value::CreateTensor<int64_t>(mem, ids.data(), ids.size(), shape, 2);
            const char* inName[] = {"input_ids"};
            const char* outName[] = {"inputs_embeds"};
            return std::move(d_->embed->Run(Ort::RunOptions{nullptr}, inName, &in, 1, outName, 1)[0]);
        };
        // The picture's features, then the prompt's tokens after them, through the encoder.
        std::vector<float> pixels = pictureInput(picture);
        const int64_t pshape[4] = {1, 3, kDescribeInput, kDescribeInput};
        Ort::Value pin = Ort::Value::CreateTensor<float>(mem, pixels.data(), pixels.size(), pshape, 4);
        const char* vIn[] = {"pixel_values"};
        const char* vOut[] = {"image_features"};
        auto features = d_->vision->Run(Ort::RunOptions{nullptr}, vIn, &pin, 1, vOut, 1);
        if (cancelled()) return fail("Cancelled");
        std::vector<int64_t> prompt = d_->tokenizer.encode(taskPrompt(detail));
        Ort::Value promptEmbeds = embed(prompt);
        const auto fshape = features[0].GetTensorTypeAndShapeInfo().GetShape();
        const auto eshape = promptEmbeds.GetTensorTypeAndShapeInfo().GetShape();
        if (fshape.size() != 3 || eshape.size() != 3 || fshape[2] != eshape[2]) return fail("The picture description model is not the expected one");
        const int64_t width = fshape[2], length = fshape[1] + eshape[1];
        std::vector<float> joined(size_t(length * width));
        std::copy_n(features[0].GetTensorData<float>(), size_t(fshape[1] * width), joined.begin());
        std::copy_n(promptEmbeds.GetTensorData<float>(), size_t(eshape[1] * width), joined.begin() + fshape[1] * width);
        std::vector<int64_t> mask(size_t(length), 1);
        const int64_t jshape[3] = {1, length, width}, mshape[2] = {1, length};
        Ort::Value encIn[2] = {Ort::Value::CreateTensor<int64_t>(mem, mask.data(), mask.size(), mshape, 2),
                               Ort::Value::CreateTensor<float>(mem, joined.data(), joined.size(), jshape, 3)};
        const char* eIn[] = {"attention_mask", "inputs_embeds"};
        const char* eOut[] = {"last_hidden_state"};
        auto hidden = d_->encoder->Run(Ort::RunOptions{nullptr}, eIn, encIn, 2, eOut, 1);
        if (cancelled()) return fail("Cancelled");

        // Greedy decoding with the decoder's own key-value cache: the words' cache grows each step, the picture's is
        // made at the first step and kept.
        const std::vector<std::string>& inNames = d_->decoderInputs;
        const std::vector<std::string>& outNames = d_->decoderOutputs;
        std::vector<Ort::Value> feed;
        std::vector<const char*> feedNames;
        float none = 0;
        bool useCache = false;
        int embedsAt = -1;
        for (const std::string& name : inNames) {
            feedNames.push_back(name.c_str());
            if (name == "encoder_attention_mask") {
                feed.push_back(Ort::Value::CreateTensor<int64_t>(mem, mask.data(), mask.size(), mshape, 2));
            } else if (name == "encoder_hidden_states") {
                feed.push_back(Ort::Value(nullptr));  // the encoder's output, moved in below
            } else if (name == "inputs_embeds") {
                embedsAt = int(feed.size());
                feed.push_back(Ort::Value(nullptr));
            } else if (name == "use_cache_branch") {
                const int64_t one = 1;
                feed.push_back(Ort::Value::CreateTensor<bool>(mem, &useCache, 1, &one, 1));
            } else if (name.rfind("past_key_values.", 0) == 0) {
                const int64_t empty[4] = {1, 16, 0, 64};
                feed.push_back(Ort::Value::CreateTensor<float>(mem, &none, 0, empty, 4));
            } else {
                return fail("The picture description model is not the expected one");
            }
        }
        for (size_t i = 0; i < inNames.size(); ++i)
            if (inNames[i] == "encoder_hidden_states") feed[i] = std::move(hidden[0]);
        // Where each output goes back in: present.N.x.y feeds past_key_values.N.x.y.
        std::vector<int> backTo(outNames.size(), -1);
        for (size_t o = 0; o < outNames.size(); ++o)
            if (outNames[o].rfind("present.", 0) == 0) {
                const std::string past = "past_key_values." + outNames[o].substr(8);
                for (size_t i = 0; i < inNames.size(); ++i)
                    if (inNames[i] == past) backTo[o] = int(i);
            }
        std::vector<const char*> fetchNames;
        for (const std::string& n : outNames) fetchNames.push_back(n.c_str());
        if (embedsAt < 0 || outNames.empty() || outNames[0] != "logits") return fail("The picture description model is not the expected one");

        constexpr int64_t kEnd = 2;
        std::vector<int64_t> out{2, 0}, next = out;  // decoding starts from </s> <s>
        const size_t most = detail == PictureDetail::Brief ? 64 : 256;
        while (out.size() < most + 2) {
            if (cancelled()) return fail("Cancelled");
            feed[size_t(embedsAt)] = embed(next);
            auto res = d_->decoder->Run(Ort::RunOptions{nullptr}, feedNames.data(), feed.data(), feed.size(), fetchNames.data(),
                                        fetchNames.size());
            const auto lshape = res[0].GetTensorTypeAndShapeInfo().GetShape();
            const int64_t vocab = lshape.back(), rows = lshape[1];
            const float* logits = res[0].GetTensorData<float>() + (rows - 1) * vocab;
            // The likeliest token: never <s> or <pad> again, and no three tokens said twice (a caption that starts
            // repeating itself).
            std::vector<int64_t> banned;
            for (size_t i = 2; i < out.size(); ++i)
                if (out[i - 2] == out[out.size() - 2] && out[i - 1] == out.back()) banned.push_back(out[i]);
            int64_t best = -1;
            for (int64_t t = 2; t < vocab; ++t)
                if ((best < 0 || logits[t] > logits[best]) && std::find(banned.begin(), banned.end(), t) == banned.end()) best = t;
            for (size_t o = 1; o < res.size(); ++o)
                if (backTo[o] >= 0 && (!useCache || outNames[o].find(".encoder.") == std::string::npos))
                    feed[size_t(backTo[o])] = std::move(res[o]);
            useCache = true;
            if (best < 0 || best == kEnd) break;
            out.push_back(best);
            next = {best};
        }
        QString text = QString::fromStdString(d_->tokenizer.decode(out)).simplified();
        return text.toStdString();
    } catch (const Ort::Exception& e) {
        return fail(std::string("The picture could not be described: ") + e.what());
    }
#else
    (void)detail;
    (void)cancel;
    return fail("This build of Montage cannot describe pictures");
#endif
}

std::string describeFrame(const std::string& path, double seconds, PictureDetail detail, std::string* error,
                          const std::atomic<bool>* cancel) {
    std::shared_ptr<PictureDescriber> model = PictureDescriber::load(error);
    if (!model) return {};
    VideoDecoder dec;
    if (!dec.open(path, error)) return {};
    Frame16Ptr f = dec.frameAt(std::max(0.0, seconds), kDescribeInput, kDescribeInput, true);
    if (!f) {
        if (error) *error = "Cannot read that frame of " + path;
        return {};
    }
    return model->describe(toImage(*f), detail, error, cancel);
}

}  // namespace montage
