#include "Inference.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QSettings>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <set>

#include "ModelFiles.h"

#ifdef MONTAGE_WITH_ONNXRUNTIME
#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/onnxruntime_cxx_api.h>
#endif
#include "OrtSupport.h"
#endif

namespace montage {

namespace {

std::atomic<int> gMode{int(InferenceMode::Auto)};
std::atomic<int> gGeneration{0};  // raised each time the mode changes, so models open again for it
std::atomic<int> gFallbacks{0};
std::mutex gReasonMutex;
std::string gReason, gLibraryError;

void noteFallback(const std::string& why) {
    ++gFallbacks;
    std::lock_guard lock(gReasonMutex);
    gReason = why;
}

#ifdef MONTAGE_WITH_ONNXRUNTIME
Ort::Env& sharedEnv() {
    // (ONNX Runtime has one environment per process: every Ort::Env is it. This one keeps it, and the provider
    // libraries registered with it, alive while models come and go.)
    static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "montage");
    return env;
}
#endif

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

void setInferenceMode(InferenceMode m) {
    if (gMode.exchange(int(m)) != int(m)) ++gGeneration;
}

bool inferenceModeOverridden() {
    InferenceMode m;
    const char* env = std::getenv("MONTAGE_INFERENCE");
    return env && parseInferenceMode(env, m);
}

InferenceMode inferenceMode() {
    if (const char* env = std::getenv("MONTAGE_INFERENCE")) {
        InferenceMode m;
        if (parseInferenceMode(env, m)) return m;
    }
    return InferenceMode(gMode.load());
}

const char* inferenceModeName(InferenceMode m) {
    switch (m) {
        case InferenceMode::Efficient: return "efficient";
        case InferenceMode::Cpu: return "cpu";
        default: return "auto";
    }
}

bool parseInferenceMode(const std::string& name, InferenceMode& out) {
    const std::string n = lower(name);
    for (InferenceMode m : {InferenceMode::Auto, InferenceMode::Efficient, InferenceMode::Cpu})
        if (n == inferenceModeName(m)) {
            out = m;
            return true;
        }
    return false;
}

int initInference() {
    static std::once_flag once;
    static int registered = 0;
    std::call_once(once, [] {
        const QString exe = QCoreApplication::instance() ? QCoreApplication::applicationDirPath() : QString();
        if (!exe.isEmpty()) registered += registerInferenceLibraries((exe + QStringLiteral("/execution-providers")).toStdString());
        registered += registerInferenceLibraries(modelsDirectory() + "/execution-providers");
        InferenceMode saved = InferenceMode::Auto;
        const QSettings settings(QStringLiteral("Montage"), QStringLiteral("Montage"));
        if (parseInferenceMode(settings.value(QStringLiteral("ai/inference"), QStringLiteral("auto")).toString().toStdString(), saved))
            setInferenceMode(saved);
    });
    return registered;
}

std::vector<InferenceDevice> inferenceDevices() {
    std::vector<InferenceDevice> out;
#ifdef MONTAGE_WITH_ONNXRUNTIME
    if (!ortUsable(nullptr)) return out;
    try {
#if ORT_API_VERSION >= 22
        for (const Ort::ConstEpDevice& d : sharedEnv().GetEpDevices()) {
            const Ort::ConstHardwareDevice hw = d.Device();
            const OrtHardwareDeviceType t = hw.Type();
            out.push_back({d.EpName() ? d.EpName() : "", hw.Vendor() ? hw.Vendor() : "",
                           t == OrtHardwareDeviceType_GPU ? "GPU" : t == OrtHardwareDeviceType_NPU ? "NPU" : "CPU"});
        }
#endif
        // (An older runtime chooses no device: models run on the processor.)
        if (out.empty()) out.push_back({"CPUExecutionProvider", "", "CPU"});
    } catch (const std::exception&) {
    }
#endif
    return out;
}

int registerInferenceLibraries(const std::string& folder) {
    int n = 0;
#if defined(MONTAGE_WITH_ONNXRUNTIME) && ORT_API_VERSION >= 22
    if (!ortUsable(nullptr) || folder.empty()) return 0;
    static std::mutex m;
    static std::set<QString> files, names;  // (taken already: a library is loaded once, under one name)
    std::lock_guard lock(m);
    const QDir dir(QString::fromStdString(folder));
    for (const QFileInfo& f : dir.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name)) {
        if (!QLibrary::isLibrary(f.fileName())) continue;
        const QString file = f.canonicalFilePath(), name = f.completeBaseName();
        if (files.count(file) || names.count(name)) continue;
        try {
#ifdef _WIN32
            const std::wstring path = QDir::toNativeSeparators(file).toStdWString();
#else
            const std::string path = file.toStdString();
#endif
            sharedEnv().RegisterExecutionProviderLibrary(name.toStdString().c_str(), path);
            files.insert(file);
            names.insert(name);
            ++n;
        } catch (const std::exception& e) {
            // (Not a provider, or one this runtime cannot load: left out, and why kept.)
            std::lock_guard reason(gReasonMutex);
            gLibraryError = f.fileName().toStdString() + ": " + e.what();
        }
    }
#else
    (void)folder;
#endif
    return n;
}

std::string lastInferenceLibraryError() {
    std::lock_guard lock(gReasonMutex);
    return gLibraryError;
}

bool inferenceCheck(std::string* error) {
#ifdef MONTAGE_WITH_ONNXRUNTIME
    if (!ortUsable(error)) return false;
    // y = Identity(x) over two floats, as ONNX's protocol buffer (IR 8, opset 13), opened from memory.
    static const unsigned char kModel[] = {0x08, 0x08, 0x3a, 0x37, 0x0a, 0x10, 0x0a, 0x01, 0x78, 0x12, 0x01, 0x79, 0x22, 0x08, 0x49, 0x64, 0x65,
                                           0x6e, 0x74, 0x69, 0x74, 0x79, 0x12, 0x01, 0x67, 0x5a, 0x0f, 0x0a, 0x01, 0x78, 0x12, 0x0a, 0x0a, 0x08,
                                           0x08, 0x01, 0x12, 0x04, 0x0a, 0x02, 0x08, 0x02, 0x62, 0x0f, 0x0a, 0x01, 0x79, 0x12, 0x0a, 0x0a, 0x08,
                                           0x08, 0x01, 0x12, 0x04, 0x0a, 0x02, 0x08, 0x02, 0x42, 0x04, 0x0a, 0x00, 0x10, 0x0d};
    try {
        OrtModel model(sharedEnv(), std::vector<unsigned char>(std::begin(kModel), std::end(kModel)), Ort::SessionOptions{}, ModelRuns::Anywhere);
        float in[2] = {0.25f, -3.5f};
        const int64_t shape[1] = {2};
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value x = Ort::Value::CreateTensor<float>(mem, in, 2, shape, 1);
        const char* inputs[] = {"x"};
        const char* outputs[] = {"y"};
        auto out = model.Run(Ort::RunOptions{nullptr}, inputs, &x, 1, outputs, 1);
        const float* y = out.front().GetTensorData<float>();
        if (y[0] == in[0] && y[1] == in[1]) return true;
        if (error) *error = "The test model gave the wrong answer";
    } catch (const std::exception& e) {
        if (error) *error = e.what();
    }
    return false;
#else
    if (error) *error = "This build has no AI runtime";
    return false;
#endif
}

int inferenceFallbacks() { return gFallbacks; }

std::string lastInferenceFallback() {
    std::lock_guard lock(gReasonMutex);
    return gReason;
}

#ifdef MONTAGE_WITH_ONNXRUNTIME
OrtModel::OrtModel(Ort::Env& env, const ORTCHAR_T* path, const Ort::SessionOptions& options, ModelRuns where)
    : env_(env), path_(path), options_(options.Clone()), where_(where) {
    session_ = open(true);
}

OrtModel::OrtModel(Ort::Env& env, std::vector<unsigned char> bytes, const Ort::SessionOptions& options, ModelRuns where)
    : env_(env), bytes_(std::move(bytes)), options_(options.Clone()), where_(where) {
    session_ = open(true);
}

std::shared_ptr<Ort::Session> OrtModel::open(bool device) {
    sharedEnv();  // (the process's environment, with any provider libraries)
    generation_ = gGeneration;
    auto make = [&](const Ort::SessionOptions& o) {
        return bytes_.empty() ? std::make_shared<Ort::Session>(env_, path_.c_str(), o)
                              : std::make_shared<Ort::Session>(env_, bytes_.data(), bytes_.size(), o);
    };
    const InferenceMode mode = inferenceMode();
#if ORT_API_VERSION >= 22
    // (only with a GPU or NPU to choose: else the policy picks the processor, and runs would queue for nothing)
    auto accelerator = [] {
        try {
            for (const Ort::ConstEpDevice& d : sharedEnv().GetEpDevices())
                if (d.Device().Type() != OrtHardwareDeviceType_CPU) return true;
        } catch (const std::exception&) {
        }
        return false;
    };
    if (device && where_ == ModelRuns::Anywhere && mode != InferenceMode::Cpu && accelerator()) {
        std::string why;
        try {
            Ort::SessionOptions o = options_.Clone();
            o.SetEpSelectionPolicy(mode == InferenceMode::Efficient ? OrtExecutionProviderDevicePolicy_PREFER_NPU
                                                                    : OrtExecutionProviderDevicePolicy_PREFER_GPU);
            auto s = make(o);
            accelerated_ = true;
            return s;
        } catch (const std::exception& e) {
            why = e.what();
        }
        // The processor, as the loader asked; a fallback only when it opens there (a missing or broken file is no
        // device's fault).
        auto s = make(options_);
        noteFallback(why);
        accelerated_ = false;
        return s;
    }
#else
    (void)device;
    (void)mode;
#endif
    accelerated_ = false;
    return make(options_);
}

std::vector<Ort::Value> OrtModel::Run(const Ort::RunOptions& run, const char* const* inputNames, const Ort::Value* inputs, size_t inputCount,
                                      const char* const* outputNames, size_t outputCount) {
    std::unique_lock lock(mutex_);
    if (generation_ != gGeneration) session_ = open(true);  // (the mode changed since it opened)
    std::shared_ptr<Ort::Session> s = session_;
    if (!accelerated_) {
        lock.unlock();
        return s->Run(run, inputNames, inputs, inputCount, outputNames, outputCount);
    }
    try {
        return s->Run(run, inputNames, inputs, inputCount, outputNames, outputCount);
    } catch (const std::exception& e) {
        // The device failed while running it: the processor runs it again, and from now on. (If the processor fails
        // too, the input was at fault, not the device: that error stands and the model stays where it was.)
        const std::string why = e.what();
        std::vector<Ort::Value> out;
        std::shared_ptr<Ort::Session> cpu;
        try {
            cpu = open(false);
            out = cpu->Run(run, inputNames, inputs, inputCount, outputNames, outputCount);
        } catch (...) {
            accelerated_ = true;  // (still the device's session)
            throw;
        }
        session_ = cpu;
        noteFallback(why);
        return out;
    }
}

size_t OrtModel::GetInputCount() const {
    std::lock_guard lock(mutex_);
    return session_->GetInputCount();
}

size_t OrtModel::GetOutputCount() const {
    std::lock_guard lock(mutex_);
    return session_->GetOutputCount();
}

Ort::AllocatedStringPtr OrtModel::GetInputNameAllocated(size_t i, OrtAllocator* allocator) const {
    std::lock_guard lock(mutex_);
    return session_->GetInputNameAllocated(i, allocator);
}

Ort::AllocatedStringPtr OrtModel::GetOutputNameAllocated(size_t i, OrtAllocator* allocator) const {
    std::lock_guard lock(mutex_);
    return session_->GetOutputNameAllocated(i, allocator);
}

bool OrtModel::accelerated() const {
    std::lock_guard lock(mutex_);
    return accelerated_;
}

std::unique_ptr<OrtModel> openModel(Ort::Env& env, const ORTCHAR_T* path, const Ort::SessionOptions& options, ModelRuns where) {
    return std::make_unique<OrtModel>(env, path, options, where);
}
#endif

}  // namespace montage
