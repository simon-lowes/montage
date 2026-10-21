#include "LanguageModel.h"

#include <QString>
#include <algorithm>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#ifdef MONTAGE_WITH_LLAMA
#include <llama.h>
#endif

namespace montage {

const ModelPack& languageModel() {
    static const ModelPack pack = [] {
        ModelPack p;
        p.id = "granite-4.0-micro";
        p.title = "language model";
        p.directoryEnv = "MONTAGE_LANGUAGE_MODEL";
        p.urlEnv = "MONTAGE_LANGUAGE_MODEL_URL";
        p.files = {
            // 4-bit (Q4_K_M), from IBM's own GGUF export.
            {"granite-4.0-micro-Q4_K_M.gguf",
             "https://huggingface.co/ibm-granite/granite-4.0-micro-GGUF/resolve/ec48475f0c811d812fbfb61975717a9c36eeb652/"
             "granite-4.0-micro-Q4_K_M.gguf",
             "97c417dcc0534b0737c74016fb2af083cb17c3b51eaac621192d23961b7024eb", 2099502528},
        };
        return p;
    }();
    return pack;
}

// Questions and answers together stay within this many tokens: room for an hour's transcript, and a context
// (its key-value store) of about a gigabyte and a half at most.
constexpr int kMostContext = 16384;

#ifdef MONTAGE_WITH_LLAMA

bool languageModelAvailable() { return true; }

struct LanguageModel::Impl {
    llama_model* model = nullptr;
    std::string path;
    mutable std::mutex busy;
    ~Impl() {
        if (model) llama_model_free(model);
    }
};

namespace {
void quietLog(ggml_log_level level, const char* text, void*) {
    if (level == GGML_LOG_LEVEL_ERROR) fputs(text, stderr);
}
}  // namespace

std::shared_ptr<LanguageModel> LanguageModel::load(const std::string& path, std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<LanguageModel>();  // the last one loaded, kept for the session
    std::lock_guard lock(m);
    std::string file = path;
    if (file.empty()) {
        const ModelPack& pack = languageModel();
        if (!pack.installed()) {
            if (error) *error = "The language model is not downloaded";
            return nullptr;
        }
        file = pack.path(pack.files[0]);
    }
    if (*cached && (*cached)->d_->path == file) return *cached;
    static std::once_flag init;
    std::call_once(init, [] {
        llama_log_set(quietLog, nullptr);
        llama_backend_init();
    });
    std::shared_ptr<LanguageModel> lm(new LanguageModel);
    llama_model_params mp = llama_model_default_params();
    // The processor alone (no layers or operations offloaded to a GPU): the same answers on every computer.
    static ggml_backend_dev_t cpuOnly[2] = {ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU), nullptr};
    mp.n_gpu_layers = 0;
    mp.devices = cpuOnly;
    lm->d_->model = llama_model_load_from_file(file.c_str(), mp);
    if (!lm->d_->model) {
        if (error) *error = "The language model could not be loaded from " + file;
        return nullptr;
    }
    lm->d_->path = file;
    *cached = lm;
    return lm;
}

int LanguageModel::tokenCount(const std::string& text) const {
    const llama_vocab* vocab = llama_model_get_vocab(d_->model);
    const int n = -llama_tokenize(vocab, text.c_str(), int32_t(text.size()), nullptr, 0, false, true);
    return std::max(0, n);
}

int LanguageModel::contextTokens() const { return std::min(kMostContext, std::max(512, int(llama_model_n_ctx_train(d_->model)))); }

std::string LanguageModel::write(const WriteRequest& r, std::string* error, const std::function<void(double)>& progress,
                                 const std::atomic<bool>* cancel) const {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return std::string();
    };
    std::lock_guard lock(d_->busy);
    const llama_vocab* vocab = llama_model_get_vocab(d_->model);
    // The question as the model was trained to read it: its chat template when it has one (the template's own
    // markers are special tokens), else the system words and the question as plain text.
    std::string text;
    bool special = false;
    if (const char* tmpl = llama_model_chat_template(d_->model, nullptr)) {
        std::vector<llama_chat_message> chat;
        if (!r.system.empty()) chat.push_back({"system", r.system.c_str()});
        chat.push_back({"user", r.prompt.c_str()});
        std::vector<char> buf(2 * (r.system.size() + r.prompt.size()) + 1024);
        int n = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, buf.data(), int32_t(buf.size()));
        if (n > int(buf.size())) {
            buf.resize(size_t(n));
            n = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, buf.data(), int32_t(buf.size()));
        }
        if (n > 0) text.assign(buf.data(), size_t(n)), special = true;
    }
    if (!special) text = (r.system.empty() ? "" : r.system + "\n\n") + r.prompt + "\n\n";
    const bool bos = !special || llama_vocab_get_add_bos(vocab);
    const int count = -llama_tokenize(vocab, text.c_str(), int32_t(text.size()), nullptr, 0, bos, special);
    std::vector<llama_token> tokens(size_t(std::max(1, count)));
    if (count <= 0 || llama_tokenize(vocab, text.c_str(), int32_t(text.size()), tokens.data(), count, bos, special) != count)
        return fail("The question could not be read");
    const int maxTokens = std::max(1, r.maxTokens);
    if (count + maxTokens > contextTokens()) return fail("The question is too long for the language model");

    // (llama.cpp throws in places, its grammar sampler among them, on a state it did not foresee: an answer that
    // fails, not a crash.)
    try {
        llama_context_params cp = llama_context_default_params();
        cp.n_ctx = uint32_t((count + maxTokens + 255) / 256 * 256);
        cp.n_batch = 512;
        cp.n_ubatch = 512;
        cp.n_threads = cp.n_threads_batch = int32_t(std::max(1u, std::thread::hardware_concurrency()));
        cp.no_perf = true;
        cp.op_offload = false;
        cp.abort_callback = [](void* c) { return c && static_cast<const std::atomic<bool>*>(c)->load(); };
        cp.abort_callback_data = const_cast<std::atomic<bool>*>(cancel);
        std::unique_ptr<llama_context, void (*)(llama_context*)> ctx(llama_init_from_model(d_->model, cp), llama_free);
        if (!ctx) return fail("The language model has no room to work (not enough memory)");
        std::unique_ptr<llama_sampler, void (*)(llama_sampler*)> sampler(llama_sampler_chain_init(llama_sampler_chain_default_params()),
                                                                        llama_sampler_free);
        if (!r.grammar.empty()) {
            llama_sampler* g = llama_sampler_init_grammar(vocab, r.grammar.c_str(), "root");
            if (!g) return fail("The answer's grammar could not be read");
            llama_sampler_chain_add(sampler.get(), g);
        }
        llama_sampler_chain_add(sampler.get(), llama_sampler_init_greedy());

        auto cancelled = [&] { return cancel && cancel->load(); };
        for (int at = 0; at < count; at += int(cp.n_batch)) {
            if (cancelled()) return fail("Cancelled");
            const int n = std::min(int(cp.n_batch), count - at);
            if (llama_decode(ctx.get(), llama_batch_get_one(tokens.data() + at, n)) != 0)
                return fail(cancelled() ? "Cancelled" : "The language model could not read the question");
            if (progress) progress(0.5 * double(at + n) / count);
        }
        std::string answer;
        char piece[256];
        for (int i = 0; i < maxTokens; ++i) {
            if (cancelled()) return fail("Cancelled");
            llama_token t = llama_sampler_sample(sampler.get(), ctx.get(), -1);
            if (llama_vocab_is_eog(vocab, t)) break;
            const int n = llama_token_to_piece(vocab, t, piece, int32_t(sizeof piece), 0, false);
            if (n > 0) answer.append(piece, size_t(n));
            if (progress) progress(0.5 + 0.5 * double(i + 1) / maxTokens);
            if (llama_decode(ctx.get(), llama_batch_get_one(&t, 1)) != 0)
                return fail(cancelled() ? "Cancelled" : "The language model stopped part way");
        }
        if (progress) progress(1.0);
        return answer;
    } catch (const std::exception& e) {
        return fail(std::string("The language model failed: ") + e.what());
    }
}

#else

bool languageModelAvailable() { return false; }
struct LanguageModel::Impl {};

std::shared_ptr<LanguageModel> LanguageModel::load(const std::string&, std::string* error) {
    if (error) *error = "This build of Montage has no language model: it was built without llama.cpp";
    return nullptr;
}
int LanguageModel::tokenCount(const std::string& text) const { return int(text.size() / 4); }
int LanguageModel::contextTokens() const { return kMostContext; }
std::string LanguageModel::write(const WriteRequest&, std::string* error, const std::function<void(double)>&,
                                 const std::atomic<bool>*) const {
    if (error) *error = "This build of Montage has no language model";
    return {};
}

#endif

LanguageModel::LanguageModel() : d_(std::make_unique<Impl>()) {}
LanguageModel::~LanguageModel() = default;

}  // namespace montage
