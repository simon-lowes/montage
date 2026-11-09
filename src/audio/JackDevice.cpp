// Montage — JACK through its C API, the library loaded when Montage runs (never linked at build time): PipeWire's
// JACK library on Linux, or a system libjack. Only the API's functions are declared here, as the program calls them.

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "AudioDeviceInternal.h"
#include "DynLib.h"

namespace montage::audiodev {

namespace {

// The JACK C API as called here (jack/jack.h, jack/types.h).
using jack_nframes_t = uint32_t;
struct jack_client_t;
struct jack_port_t;
struct jack_latency_range_t {
    jack_nframes_t min, max;
};
enum : unsigned long { JackPortIsInput = 0x1, JackPortIsOutput = 0x2, JackPortIsPhysical = 0x4 };
enum : int { JackNoStartServer = 0x01 };
enum : int { JackCaptureLatency = 0, JackPlaybackLatency = 1 };
constexpr const char* kAudioType = "32 bit float mono audio";

struct JackApi {
    plugins::DynLib lib;
    std::string path;
    jack_client_t* (*client_open)(const char*, int, int*, ...) = nullptr;
    int (*client_close)(jack_client_t*) = nullptr;
    jack_nframes_t (*get_sample_rate)(jack_client_t*) = nullptr;
    jack_nframes_t (*get_buffer_size)(jack_client_t*) = nullptr;
    int (*set_buffer_size)(jack_client_t*, jack_nframes_t) = nullptr;
    int (*set_process_callback)(jack_client_t*, int (*)(jack_nframes_t, void*), void*) = nullptr;
    int (*set_xrun_callback)(jack_client_t*, int (*)(void*), void*) = nullptr;
    int (*set_sample_rate_callback)(jack_client_t*, int (*)(jack_nframes_t, void*), void*) = nullptr;
    int (*set_buffer_size_callback)(jack_client_t*, int (*)(jack_nframes_t, void*), void*) = nullptr;
    void (*on_shutdown)(jack_client_t*, void (*)(void*), void*) = nullptr;
    jack_port_t* (*port_register)(jack_client_t*, const char*, const char*, unsigned long, unsigned long) = nullptr;
    void* (*port_get_buffer)(jack_port_t*, jack_nframes_t) = nullptr;
    const char* (*port_name)(const jack_port_t*) = nullptr;
    int (*port_rename)(jack_client_t*, jack_port_t*, const char*) = nullptr;  // (newer libraries)
    int (*port_set_name)(jack_port_t*, const char*) = nullptr;               // (older ones)
    void (*port_get_latency_range)(jack_port_t*, int, jack_latency_range_t*) = nullptr;
    int (*activate)(jack_client_t*) = nullptr;
    int (*deactivate)(jack_client_t*) = nullptr;
    const char** (*get_ports)(jack_client_t*, const char*, const char*, unsigned long) = nullptr;
    void (*free_)(void*) = nullptr;
    int (*connect)(jack_client_t*, const char*, const char*) = nullptr;
    float (*cpu_load)(jack_client_t*) = nullptr;

    template <typename F>
    bool get(F& f, const char* name) {
        f = reinterpret_cast<F>(lib.symbol(name));
        return f != nullptr;
    }
    bool load(const std::string& file) {
        std::string err;
        if (!lib.open(file, &err)) return false;
        path = file;
        bool ok = get(client_open, "jack_client_open") && get(client_close, "jack_client_close") && get(get_sample_rate, "jack_get_sample_rate") &&
                  get(get_buffer_size, "jack_get_buffer_size") && get(set_process_callback, "jack_set_process_callback") &&
                  get(on_shutdown, "jack_on_shutdown") && get(port_register, "jack_port_register") && get(port_get_buffer, "jack_port_get_buffer") &&
                  get(port_name, "jack_port_name") && get(activate, "jack_activate") && get(deactivate, "jack_deactivate") &&
                  get(get_ports, "jack_get_ports") && get(free_, "jack_free") && get(connect, "jack_connect");
        get(set_buffer_size, "jack_set_buffer_size");
        get(set_xrun_callback, "jack_set_xrun_callback");
        get(set_sample_rate_callback, "jack_set_sample_rate_callback");
        get(set_buffer_size_callback, "jack_set_buffer_size_callback");
        get(port_rename, "jack_port_rename");
        get(port_set_name, "jack_port_set_name");
        get(port_get_latency_range, "jack_port_get_latency_range");
        get(cpu_load, "jack_cpu_load");
        // (the library's own messages, "cannot connect to server" each time one is looked for, kept off the console:
        // what went wrong is in the engine's status)
        void (*setError)(void (*)(const char*)) = nullptr;
        void (*setInfo)(void (*)(const char*)) = nullptr;
        if (ok && std::getenv("MONTAGE_JACK_MESSAGES") == nullptr) {
            if (get(setError, "jack_set_error_function")) setError([](const char*) {});
            if (get(setInfo, "jack_set_info_function")) setInfo([](const char*) {});
        }
        return ok;
    }
};

std::vector<std::string> jackCandidates() {
    std::vector<std::string> out;
    if (const char* e = std::getenv("MONTAGE_JACK_LIBRARY"); e && *e) out.emplace_back(e);
#if defined(_WIN32)
    out.insert(out.end(), {"libjack64.dll", "libjack.dll"});
#elif defined(__APPLE__)
    out.insert(out.end(), {"/opt/homebrew/lib/libjack.0.dylib", "/usr/local/lib/libjack.0.dylib", "libjack.0.dylib"});
#else
    // PipeWire's JACK library first (MIT), then whichever libjack the system has.
    for (const char* dir : {"/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu", "/usr/lib64", "/usr/lib"})
        out.push_back(std::string(dir) + "/pipewire-0.3/jack/libjack.so.0");
    out.emplace_back("libjack.so.0");
#endif
    return out;
}

// The libraries that load, each loaded once and kept (a JACK library is not unloaded while Montage runs: its threads
// may outlive a client, and the engine, itself kept to the end, closes its client after other statics are gone).
std::mutex gJackM;
std::vector<std::unique_ptr<JackApi>>& loadedJacks() {
    static auto* libs = new std::vector<std::unique_ptr<JackApi>>;  // (never destroyed)
    static bool tried = false;
    if (!tried) {
        tried = true;
        for (const std::string& c : jackCandidates()) {
            auto api = std::make_unique<JackApi>();
            if (api->load(c)) libs->push_back(std::move(api));
        }
    }
    return *libs;
}

// A client's name as a pattern for jack_get_ports (an extended regular expression): its own characters only.
std::string literalPattern(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (std::string_view(".[]{}()\\*+?^$|").find(c) != std::string_view::npos) out += '\\';
        out += c;
    }
    return out;
}

// A client on the first library whose server answers (PipeWire's library talks only to PipeWire, a system libjack to
// its own server).
jack_client_t* openClient(const char* name, JackApi** used, std::string* error) {
    std::lock_guard<std::mutex> lock(gJackM);
    auto& libs = loadedJacks();
    if (libs.empty()) {
        if (error) *error = "No JACK library was found (PipeWire's or a system libjack)";
        return nullptr;
    }
    for (auto& api : libs) {
        int status = 0;
        if (jack_client_t* c = api->client_open(name, JackNoStartServer, &status)) {
            *used = api.get();
            return c;
        }
    }
    if (error) *error = "No JACK server is running (nor PipeWire with its JACK library)";
    return nullptr;
}

class JackStream : public Stream {
public:
    JackStream(JackApi* api, jack_client_t* client, StreamCallbacks cb) : api_(api), client_(client), cb_(std::move(cb)) {}
    ~JackStream() override {
        if (active_) api_->deactivate(client_);
        api_->client_close(client_);
    }

    bool start(const DeviceSettings& s, int inputs, int outputs, const std::vector<std::string>& names, std::string* error) {
        rate_ = int(api_->get_sample_rate(client_));
        if (api_->set_buffer_size && s.bufferFrames > 0 && int(api_->get_buffer_size(client_)) != s.bufferFrames)
            api_->set_buffer_size(client_, jack_nframes_t(clampBuffer(s.bufferFrames)));  // (asked of the server, for every client)
        buffer_ = int(api_->get_buffer_size(client_));
        for (int k = 0; k < outputs; ++k) {
            const std::string n = k < int(names.size()) && !names[size_t(k)].empty() ? names[size_t(k)] : "Out " + std::to_string(k + 1);
            jack_port_t* p = api_->port_register(client_, n.c_str(), kAudioType, JackPortIsOutput, 0);
            if (!p) p = api_->port_register(client_, ("Out " + std::to_string(k + 1)).c_str(), kAudioType, JackPortIsOutput, 0);
            if (!p) {
                if (error) *error = "JACK refused the output port " + n;
                return false;
            }
            outs_.push_back(p);
        }
        for (int k = 0; k < inputs; ++k) {
            const std::string n = inputName(s, k);
            jack_port_t* p = api_->port_register(client_, n.c_str(), kAudioType, JackPortIsInput, 0);
            if (!p) p = api_->port_register(client_, ("In " + std::to_string(k + 1)).c_str(), kAudioType, JackPortIsInput, 0);
            if (!p) {
                if (error) *error = "JACK refused the input port " + n;
                return false;
            }
            ins_.push_back(p);
        }
        sized(buffer_);
        api_->set_process_callback(client_, &JackStream::processCb, this);
        if (api_->set_xrun_callback) api_->set_xrun_callback(client_, &JackStream::xrunCb, this);
        if (api_->set_sample_rate_callback) api_->set_sample_rate_callback(client_, &JackStream::rateCb, this);
        if (api_->set_buffer_size_callback) api_->set_buffer_size_callback(client_, &JackStream::bufferCb, this);
        api_->on_shutdown(client_, &JackStream::shutdownCb, this);
        if (api_->activate(client_) != 0) {
            if (error) *error = "JACK would not start Montage's client";
            return false;
        }
        active_ = true;
        if (s.autoConnect) joinPhysical(s);
        return true;
    }

    int sampleRate() const override { return rate_.load(); }
    int bufferFrames() const override { return buffer_.load(); }
    int inputs() const override { return int(ins_.size()); }
    int outputs() const override { return int(outs_.size()); }
    Latency latency() const override {
        // The ports' own latencies as the graph has them: the most from our input back to a capture port, and from our
        // output on to a playback port.
        Latency l;
        if (!api_->port_get_latency_range) return l;
        for (jack_port_t* p : ins_) {
            jack_latency_range_t r{0, 0};
            api_->port_get_latency_range(p, JackCaptureLatency, &r);
            l.input = std::max(l.input, int(r.max));
        }
        for (jack_port_t* p : outs_) {
            jack_latency_range_t r{0, 0};
            api_->port_get_latency_range(p, JackPlaybackLatency, &r);
            l.output = std::max(l.output, int(r.max));
        }
        return l;
    }
    int64_t xruns() const override { return xruns_.load(); }
    std::string device() const override { return "JACK (" + api_->path + ")"; }
    std::vector<std::string> ports() const override {
        std::vector<std::string> out;
        for (jack_port_t* p : outs_) out.emplace_back(api_->port_name(p));
        for (jack_port_t* p : ins_) out.emplace_back(api_->port_name(p));
        return out;
    }
    bool renameOutputs(const std::vector<std::string>& names) override {
        bool ok = true;
        for (size_t k = 0; k < outs_.size(); ++k) {
            const std::string n = k < names.size() && !names[k].empty() ? names[k] : "Out " + std::to_string(k + 1);
            if (api_->port_rename) ok = api_->port_rename(client_, outs_[k], n.c_str()) == 0 && ok;
            else if (api_->port_set_name) ok = api_->port_set_name(outs_[k], n.c_str()) == 0 && ok;
            else ok = false;
        }
        return ok;
    }
    bool connect(const std::string& from, const std::string& to) override { return api_->connect(client_, from.c_str(), to.c_str()) == 0; }
    std::string clock() const override { return "JACK server"; }
    double load() const override { return api_->cpu_load ? double(api_->cpu_load(client_)) / 100.0 : -1; }

private:
    void sized(int frames) {
        frames = std::max(frames, 1);
        in_.assign(size_t(frames) * std::max<size_t>(1, ins_.size()), 0.0f);
        out_.assign(size_t(frames) * std::max<size_t>(1, outs_.size()), 0.0f);
        capacity_ = frames;
    }
    // Our ports to the physical ones in order (those of the client the settings name, else the first ones found).
    void joinPhysical(const DeviceSettings& s) {
        auto join = [&](const std::string& client, unsigned long flags, const std::vector<jack_port_t*>& ours, bool ourIsSource) {
            const std::string pattern = client.empty() ? std::string() : "^" + literalPattern(client) + ":";
            const char** phys = api_->get_ports(client_, pattern.empty() ? nullptr : pattern.c_str(), kAudioType, JackPortIsPhysical | flags);
            if (!phys) return;
            for (size_t k = 0; k < ours.size() && phys[k]; ++k) {
                const char* mine = api_->port_name(ours[k]);
                if (ourIsSource) api_->connect(client_, mine, phys[k]);
                else api_->connect(client_, phys[k], mine);
            }
            api_->free_(phys);
        };
        join(s.output, JackPortIsInput, outs_, true);
        join(s.input, JackPortIsOutput, ins_, false);
    }

    static int processCb(jack_nframes_t n, void* arg) {
        auto* self = static_cast<JackStream*>(arg);
        const int frames = int(n);
        if (frames > self->capacity_) return 0;  // (the buffer callback resizes before such a period)
        const int ni = int(self->ins_.size()), no = int(self->outs_.size());
        for (int c = 0; c < ni; ++c) {
            const float* src = static_cast<const float*>(self->api_->port_get_buffer(self->ins_[size_t(c)], n));
            for (int i = 0; i < frames; ++i) self->in_[size_t(i) * size_t(ni) + size_t(c)] = src[i];
        }
        if (self->cb_.process) self->cb_.process(self->in_.data(), ni, self->out_.data(), no, frames);
        for (int c = 0; c < no; ++c) {
            float* dst = static_cast<float*>(self->api_->port_get_buffer(self->outs_[size_t(c)], n));
            for (int i = 0; i < frames; ++i) dst[i] = self->out_[size_t(i) * size_t(no) + size_t(c)];
        }
        return 0;
    }
    static int xrunCb(void* arg) {
        ++static_cast<JackStream*>(arg)->xruns_;
        return 0;
    }
    static int rateCb(jack_nframes_t r, void* arg) {
        auto* self = static_cast<JackStream*>(arg);
        if (int(r) == self->rate_.load()) return 0;
        self->rate_ = int(r);
        if (self->cb_.rateChanged) self->cb_.rateChanged(int(r));
        return 0;
    }
    static int bufferCb(jack_nframes_t n, void* arg) {
        auto* self = static_cast<JackStream*>(arg);  // (JACK calls it with the process callback stopped)
        self->buffer_ = int(n);
        if (int(n) > self->capacity_) self->sized(int(n));
        return 0;
    }
    static void shutdownCb(void* arg) {
        auto* self = static_cast<JackStream*>(arg);
        if (self->cb_.lost) self->cb_.lost("The JACK server stopped");
    }

    JackApi* api_;
    jack_client_t* client_;
    StreamCallbacks cb_;
    bool active_ = false;
    std::atomic<int> rate_{0}, buffer_{0};
    std::atomic<int64_t> xruns_{0};
    std::vector<jack_port_t*> outs_, ins_;
    std::vector<float> in_, out_;
    int capacity_ = 0;
};

}  // namespace

std::string jackLibrary() {
    std::lock_guard<std::mutex> lock(gJackM);
    auto& libs = loadedJacks();
    return libs.empty() ? std::string() : libs.front()->path;
}

namespace detail {

bool jackLoadable() {
    std::lock_guard<std::mutex> lock(gJackM);
    return !loadedJacks().empty();
}

std::vector<DeviceInfo> jackDevices(std::string* error) {
    JackApi* api = nullptr;
    jack_client_t* c = openClient("montage-list", &api, error);
    if (!c) return {};
    // The clients with physical ports, each once ("system" first).
    std::vector<DeviceInfo> out;
    auto add = [&](unsigned long flags, bool capture) {
        const char** ports = api->get_ports(c, nullptr, kAudioType, JackPortIsPhysical | flags);
        if (!ports) return;
        for (size_t k = 0; ports[k]; ++k) {
            const std::string full = ports[k];
            const std::string client = full.substr(0, full.find(':'));
            auto it = std::find_if(out.begin(), out.end(), [&](const DeviceInfo& d) { return d.id == client; });
            if (it == out.end()) {
                out.push_back({client, client, 0, 0, double(api->get_sample_rate(c))});
                it = out.end() - 1;
            }
            (capture ? it->inputs : it->outputs)++;
        }
        api->free_(ports);
    };
    add(JackPortIsInput, false);  // (a physical input port takes our output: a playback port)
    add(JackPortIsOutput, true);
    std::stable_partition(out.begin(), out.end(), [](const DeviceInfo& d) { return d.id == "system"; });
    api->client_close(c);
    return out;
}

std::unique_ptr<Stream> openJack(const DeviceSettings& s, int inputs, int outputs, const std::vector<std::string>& outputNames,
                                 StreamCallbacks callbacks, std::string* error) {
    JackApi* api = nullptr;
    jack_client_t* c = openClient("montage", &api, error);
    if (!c) return nullptr;
    auto st = std::make_unique<JackStream>(api, c, std::move(callbacks));
    if (!st->start(s, inputs, outputs, outputNames, error)) return nullptr;
    return st;
}

}  // namespace detail

}  // namespace montage::audiodev
