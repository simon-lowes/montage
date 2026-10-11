#include "Inference.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <atomic>
#include <cstdlib>
#include <mutex>

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
std::atomic<int> gFallbacks{0};
std::mutex gReasonMutex;
std::string gReason;

#ifdef MONTAGE_WITH_ONNXRUNTIME
Ort::Env& sharedEnv() {
    static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "montage");
    return env;
}
#endif

}  // namespace

void setInferenceMode(InferenceMode m) { gMode = int(m); }

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
    for (InferenceMode m : {InferenceMode::Auto, InferenceMode::Efficient, InferenceMode::Cpu})
        if (name == inferenceModeName(m)) {
            out = m;
            return true;
        }
    return false;
}

std::vector<InferenceDevice> inferenceDevices() {
    std::vector<InferenceDevice> out;
#ifdef MONTAGE_WITH_ONNXRUNTIME
    if (!ortUsable(nullptr)) return out;
    try {
#if ORT_API_VERSION >= 23
        for (const Ort::ConstEpDevice& d : sharedEnv().GetEpDevices()) {
            const Ort::ConstHardwareDevice hw = d.Device();
            const OrtHardwareDeviceType t = hw.Type();
            out.push_back({d.EpName() ? d.EpName() : "", hw.Vendor() ? hw.Vendor() : "",
                           t == OrtHardwareDeviceType_GPU ? "GPU" : t == OrtHardwareDeviceType_NPU ? "NPU" : "CPU"});
        }
#endif
        if (out.empty())
            for (const std::string& p : Ort::GetAvailableProviders()) out.push_back({p, "", p == "CPUExecutionProvider" ? "CPU" : "GPU"});
    } catch (const std::exception&) {
    }
#endif
    return out;
}

int registerInferenceLibraries(const std::string& folder) {
    int n = 0;
#if defined(MONTAGE_WITH_ONNXRUNTIME) && ORT_API_VERSION >= 23
    if (!ortUsable(nullptr) || folder.empty()) return 0;
    const QDir dir(QString::fromStdString(folder));
    for (const QFileInfo& f : dir.entryInfoList(QDir::Files)) {
        if (!QLibrary::isLibrary(f.fileName())) continue;
        try {
#ifdef _WIN32
            const std::wstring path = f.absoluteFilePath().toStdWString();
#else
            const std::string path = f.absoluteFilePath().toStdString();
#endif
            sharedEnv().RegisterExecutionProviderLibrary(f.completeBaseName().toStdString().c_str(), path);
            ++n;
        } catch (const std::exception&) {
            // (Not a provider, or one this runtime cannot load: left out.)
        }
    }
#else
    (void)folder;
#endif
    return n;
}

bool inferenceCheck(std::string* error) {
#ifdef MONTAGE_WITH_ONNXRUNTIME
    if (!ortUsable(error)) return false;
    // y = Identity(x) over two floats, as ONNX's protocol buffer (IR 8, opset 13).
    static const unsigned char kModel[] = {0x08, 0x08, 0x3a, 0x37, 0x0a, 0x10, 0x0a, 0x01, 0x78, 0x12, 0x01, 0x79, 0x22, 0x08, 0x49, 0x64, 0x65,
                                           0x6e, 0x74, 0x69, 0x74, 0x79, 0x12, 0x01, 0x67, 0x5a, 0x0f, 0x0a, 0x01, 0x78, 0x12, 0x0a, 0x0a, 0x08,
                                           0x08, 0x01, 0x12, 0x04, 0x0a, 0x02, 0x08, 0x02, 0x62, 0x0f, 0x0a, 0x01, 0x79, 0x12, 0x0a, 0x0a, 0x08,
                                           0x08, 0x01, 0x12, 0x04, 0x0a, 0x02, 0x08, 0x02, 0x42, 0x04, 0x0a, 0x00, 0x10, 0x0d};
    const QString file = QDir::temp().filePath(QStringLiteral("montage-inference-check-%1.onnx").arg(QCoreApplication::applicationPid()));
    {
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly) || f.write(reinterpret_cast<const char*>(kModel), sizeof kModel) != qint64(sizeof kModel)) {
            if (error) *error = "Cannot write a test model";
            return false;
        }
    }
    bool ok = false;
    try {
        Ort::Env& env = sharedEnv();
        Ort::SessionOptions so;
#ifdef _WIN32
        const std::wstring path = file.toStdWString();
#else
        const std::string path = file.toStdString();
#endif
        auto session = openModel(env, path.c_str(), so);
        float in[2] = {0.25f, -3.5f};
        const int64_t shape[1] = {2};
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value x = Ort::Value::CreateTensor<float>(mem, in, 2, shape, 1);
        const char* inputs[] = {"x"};
        const char* outputs[] = {"y"};
        auto out = session->Run(Ort::RunOptions{nullptr}, inputs, &x, 1, outputs, 1);
        const float* y = out.front().GetTensorData<float>();
        ok = y[0] == in[0] && y[1] == in[1];
        if (!ok && error) *error = "The test model gave the wrong answer";
    } catch (const std::exception& e) {
        if (error) *error = e.what();
    }
    QFile::remove(file);
    return ok;
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
std::unique_ptr<Ort::Session> openModel(Ort::Env& env, const ORTCHAR_T* path, const Ort::SessionOptions& options) {
    sharedEnv();  // (the process's environment, with any provider libraries, made before the model's)
    const InferenceMode mode = inferenceMode();
#if ORT_API_VERSION >= 23
    if (mode != InferenceMode::Cpu) {
        try {
            Ort::SessionOptions accelerated = options.Clone();
            accelerated.SetEpSelectionPolicy(mode == InferenceMode::Efficient ? OrtExecutionProviderDevicePolicy_PREFER_NPU
                                                                              : OrtExecutionProviderDevicePolicy_PREFER_GPU);
            return std::make_unique<Ort::Session>(env, path, accelerated);
        } catch (const std::exception& e) {
            ++gFallbacks;
            std::lock_guard lock(gReasonMutex);
            gReason = e.what();
        }
    }
#endif
    return std::make_unique<Ort::Session>(env, path, options);
}
#endif

}  // namespace montage
