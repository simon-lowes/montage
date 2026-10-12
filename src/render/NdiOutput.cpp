// Montage — reference output over NDI: the program as an NDI source on the network ("Montage"), 16-bit P216 or 8-bit
// UYVY with its colour metadata, embedded audio as planar float, timecode. The NDI Runtime (Vizrt's, free to install,
// not shipped) is loaded when it is first used: NDI_RUNTIME_DIR_V6's (as the SDK says to look), else the system's.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "Processing.NDI.Lib.h"
#include "ReferenceOutput.h"
#include "audio/DynLib.h"

namespace montage::output {

namespace {

struct NdiApi {
    plugins::DynLib lib;
    bool ok = false;
    std::string error;
    bool (*initialize)() = nullptr;
    const char* (*version)() = nullptr;
    NDIlib_send_instance_t (*sendCreate)(const NDIlib_send_create_t*) = nullptr;
    void (*sendDestroy)(NDIlib_send_instance_t) = nullptr;
    void (*sendVideoAsync)(NDIlib_send_instance_t, const NDIlib_video_frame_v2_t*) = nullptr;
    void (*sendAudio)(NDIlib_send_instance_t, const NDIlib_audio_frame_v2_t*) = nullptr;
    int (*connections)(NDIlib_send_instance_t, uint32_t) = nullptr;
};

std::vector<std::string> libraryCandidates() {
#if defined(_WIN32)
    const char* names[] = {"Processing.NDI.Lib.x64.dll"};
#elif defined(__APPLE__)
    const char* names[] = {"libndi.dylib"};
#else
    const char* names[] = {"libndi.so.6", "libndi.so.5", "libndi.so"};
#endif
    std::vector<std::string> out;
    for (const char* env : {"NDI_RUNTIME_DIR_V6", "NDI_RUNTIME_DIR_V5"}) {
        const char* dir = std::getenv(env);
        if (dir && *dir)
            for (const char* n : names) out.push_back(std::string(dir) + "/" + n);
    }
#if defined(__APPLE__)
    out.push_back("/usr/local/lib/libndi.dylib");
    out.push_back("/Library/NDI SDK for Apple/lib/macOS/libndi.dylib");
#endif
    for (const char* n : names) out.push_back(n);  // (the system's search path)
    return out;
}

NdiApi& api() {
    static NdiApi a;
    static std::once_flag once;
    std::call_once(once, [] {
        std::string last;
        for (const std::string& path : libraryCandidates())
            if (a.lib.open(path, &last)) break;
        if (!a.lib.isOpen()) {
            a.error = "The NDI Runtime is not installed (ndi.link/NDIRedistV6)";
            return;
        }
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(a.lib.symbol(name));
            return fn != nullptr;
        };
        a.ok = get(a.initialize, "NDIlib_initialize") && get(a.version, "NDIlib_version") &&
               get(a.sendCreate, "NDIlib_send_create") && get(a.sendDestroy, "NDIlib_send_destroy") &&
               get(a.sendVideoAsync, "NDIlib_send_send_video_async_v2") && get(a.sendAudio, "NDIlib_send_send_audio_v2") &&
               get(a.connections, "NDIlib_send_get_no_connections");
        if (!a.ok) a.error = "The NDI Runtime installed is too old (NDI 5 or later needed)";
        else if (!a.initialize()) a.ok = false, a.error = "NDI does not run on this processor (it needs SSE4.2)";
    });
    return a;
}

class NdiBackend : public Backend {
public:
    ~NdiBackend() override { close(); }
    std::string name() const override { return "NDI"; }
    bool available(std::string* why) override {
        if (!api().ok && why) *why = api().error;
        return api().ok;
    }
    std::vector<Device> devices() override {
        if (!api().ok) return {};
        Device d;
        d.id = "Montage";
        d.name = "NDI source \"Montage\"";
        d.formats = {PixelFormat::P216, PixelFormat::Uyvy8};
        d.hdr = true;  // (its colour metadata; NDI has no mastering display metadata)
        d.audioChannels = {2, 8, 16};
        d.audioRates.clear();
        return {d};
    }
    bool open(const Device& device, const Mode&, const Signal& signal, int, int, std::string* error) override {
        close();
        if (!api().ok) {
            if (error) *error = api().error;
            return false;
        }
        NDIlib_send_create_t create;
        create.p_ndi_name = device.id.empty() ? "Montage" : device.id.c_str();
        create.p_groups = nullptr;
        create.clock_video = true;  // (the runtime paces the frames sent)
        create.clock_audio = false;
        std::lock_guard lock(m_);
        sender_ = api().sendCreate(&create);
        if (!sender_) {
            if (error) *error = "NDI could not make the source";
            return false;
        }
        metadata_ = ndiColorInfo(signal);
        return true;
    }
    bool send(const VideoFrame& f, const Audio& a) override {
        NDIlib_send_instance_t sender = sender_;
        if (!sender) return false;
        // 100 ns units, from the frame's place in the sequence.
        const int64_t timecode = f.fps.num > 0 ? f.frame * int64_t(10000000) * f.fps.den / f.fps.num : 0;
        if (a.frames > 0 && a.channels > 0) {
            planar_.assign(size_t(a.frames) * size_t(a.channels), 0.0f);
            if (a.samples)
                for (int c = 0; c < a.channels; ++c)
                    for (int i = 0; i < a.frames; ++i) planar_[size_t(c) * size_t(a.frames) + size_t(i)] = a.samples[size_t(i) * size_t(a.channels) + size_t(c)];
            NDIlib_audio_frame_v2_t af;
            af.sample_rate = a.rate;
            af.no_channels = a.channels;
            af.no_samples = a.frames;
            af.timecode = timecode;
            af.p_data = planar_.data();
            af.channel_stride_in_bytes = int(sizeof(float)) * a.frames;
            af.p_metadata = nullptr;
            af.timestamp = 0;
            api().sendAudio(sender, &af);
        }
        // The runtime reads a frame sent asynchronously until the next is sent: two buffers in turn.
        std::vector<uint8_t>& buf = buffers_[next_];
        next_ ^= 1;
        buf.assign(f.data, f.data + frameBytes(f.format, f.width, f.height));
        NDIlib_video_frame_v2_t vf;
        vf.xres = f.width;
        vf.yres = f.height;
        vf.FourCC = f.format == PixelFormat::P216 ? NDIlib_FourCC_type_P216 : NDIlib_FourCC_type_UYVY;
        vf.frame_rate_N = f.fps.num;
        vf.frame_rate_D = f.fps.den;
        vf.picture_aspect_ratio = float(f.width) / float(std::max(1, f.height));
        vf.frame_format_type = NDIlib_frame_format_type_progressive;
        vf.timecode = timecode;
        vf.p_data = buf.data();
        vf.line_stride_in_bytes = int(f.stride);
        vf.p_metadata = metadata_.c_str();
        vf.timestamp = 0;
        api().sendVideoAsync(sender, &vf);  // (returns when it is time for the next frame)
        return true;
    }
    void close() override {
        std::lock_guard lock(m_);
        if (!sender_) return;
        api().sendVideoAsync(sender_, nullptr);  // (lets go of the last frame)
        api().sendDestroy(sender_);
        sender_ = nullptr;
    }
    int receivers() override {
        std::lock_guard lock(m_);
        return sender_ ? api().connections(sender_, 0) : -1;
    }

private:
    std::mutex m_;
    NDIlib_send_instance_t sender_ = nullptr;
    std::string metadata_;
    std::vector<uint8_t> buffers_[2];
    int next_ = 0;
    std::vector<float> planar_;
};

}  // namespace

std::shared_ptr<Backend> makeNdiBackend() { return std::make_shared<NdiBackend>(); }

}  // namespace montage::output
