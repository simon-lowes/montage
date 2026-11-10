// Montage — sound-effect sampler (see Sampler.h).
#include "Sampler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>

#include "ElasticAudio.h"
#include "core/TakeLanes.h"
#include "media/MediaPool.h"

namespace montage::sampler {

namespace {

constexpr size_t kMaxVoices = 64;

uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

double unit(uint32_t& state) {  // [0, 1)
    state = mix32(state + 0x9e3779b9U);
    return double(state >> 8) / double(1u << 24);
}

Track* samplerTrack(Sequence& s, TrackRef track, std::string& why) {
    Track* t = trackAt(s, track);
    if (!t || track.kind != TrackKind::Audio) {
        why = "The sampler is on audio tracks";
        return nullptr;
    }
    return t;
}

}  // namespace

uint32_t noteSeed(uint32_t passSeed, size_t index) { return mix32(passSeed * 0x9e3779b9U ^ mix32(uint32_t(index) + 0x85ebca6bU)); }

int curveVelocity(const SamplerSlot& slot, int velocity) {
    const int v = std::clamp(velocity, 1, 127);
    switch (slot.velocityCurve) {
        case 1: return std::clamp(int(std::lround(127 * std::pow(v / 127.0, 0.5))), 1, 127);  // soft
        case 2: return std::clamp(int(std::lround(127 * std::pow(v / 127.0, 2.0))), 1, 127);  // hard
        case 3: {
            // Drawn: straight between its points (in order of the velocity in), level before the first and after the last;
            // with none, as played.
            // (no copy made: this is played on the audio thread)
            const std::pair<int, int>* lo = nullptr;
            const std::pair<int, int>* hi = nullptr;
            for (const auto& pt : slot.velocityPoints) {
                if (pt.first <= v && (!lo || pt.first > lo->first)) lo = &pt;
                if (pt.first >= v && (!hi || pt.first < hi->first)) hi = &pt;
            }
            if (!lo && !hi) return v;
            int out = 0;
            if (!lo) out = hi->second;
            else if (!hi || hi->first == lo->first) out = lo->second;
            else out = int(std::lround(lo->second + (hi->second - lo->second) * double(v - lo->first) / double(hi->first - lo->first)));
            return std::clamp(out, 1, 127);
        }
        default: return v;
    }
}

namespace {

std::atomic<double> gThrottle{0};
std::atomic<int64_t> gUnderruns{0};
std::atomic<uint64_t> gCacheSerial{0};

// A cache file made for one kit (a sound given by a loader): removed with its last user.
struct OwnedDiskSound : DiskSound {
    ~OwnedDiskSound() {
        std::error_code ec;
        std::filesystem::remove(file, ec);
    }
};

std::filesystem::path cacheFolder(const StreamOptions& o) {
    std::error_code ec;
    std::filesystem::path dir = o.cacheDir.empty() ? std::filesystem::temp_directory_path(ec) / "montage-sampler" : std::filesystem::path(o.cacheDir);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

bool writeFloats(std::FILE* f, const float* x, size_t n) { return std::fwrite(x, sizeof(float), n, f) == n; }

bool seekFrame(std::FILE* f, int64_t frame) {  // (64-bit offsets everywhere: long is 32 bits on Windows)
#ifdef _WIN32
    return _fseeki64(f, frame * 8, SEEK_SET) == 0;
#else
    return fseeko(f, off_t(frame * 8), SEEK_SET) == 0;
#endif
}

// The frames from `from` (of the file) read back from a cache file into memory: a streamed sound's head.
AudioBufferPtr readHead(const DiskSound& d, int64_t from, int64_t frames, int rate) {
    auto head = std::make_shared<AudioBuffer>();
    head->sampleRate = rate;
    head->channels = 2;
    head->samples.assign(size_t(std::max<int64_t>(0, frames)) * 2, 0.0f);
    std::FILE* f = std::fopen(d.file.c_str(), "rb");
    if (!f) return nullptr;
    const bool ok = seekFrame(f, from) &&
                    std::fread(head->samples.data(), sizeof(float), head->samples.size(), f) == head->samples.size();
    std::fclose(f);
    return ok ? head : nullptr;
}

// A long file decoded in blocks straight into its cache file (kept to be used again while the file is unchanged).
std::shared_ptr<const DiskSound> decodeToDisk(const std::string& path, int rate, const StreamOptions& o, std::string* why) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        if (why) *why = "cannot read " + path;
        return nullptr;
    }
    const auto stamp = std::filesystem::last_write_time(path, ec).time_since_epoch().count();
    const std::string id = path + "|" + std::to_string(size) + "|" + std::to_string(stamp) + "|" + std::to_string(rate);
    const std::filesystem::path file = cacheFolder(o) / ("sound-" + std::to_string(std::hash<std::string>{}(id)) + ".f32");
    auto d = std::make_shared<DiskSound>();
    d->file = file.string();
    if (const auto have = std::filesystem::file_size(file, ec); !ec && have > 0 && have % 8 == 0) {
        d->frames = int64_t(have / 8);
        return d;
    }
    const std::string part = d->file + ".part" + std::to_string(gCacheSerial++);
    std::FILE* f = std::fopen(part.c_str(), "wb");
    if (!f) {
        if (why) *why = "cannot write " + part;
        return nullptr;
    }
    int64_t frames = 0;
    bool wrote = true;
    const bool ok = decodeAudioStereoBlocks(path, rate, 65536, [&](const std::vector<float>& stereo) {
        wrote = wrote && writeFloats(f, stereo.data(), stereo.size());
        frames += int64_t(stereo.size() / 2);
        return wrote;
    }, why);
    std::fclose(f);
    if (!ok || !wrote || frames <= 0) {
        std::filesystem::remove(part, ec);
        return nullptr;
    }
    std::filesystem::rename(part, file, ec);
    if (ec) {
        std::filesystem::remove(part, ec);
        return nullptr;
    }
    d->frames = frames;
    return d;
}

// A sound a loader gave, written to a cache file of the kit's own.
std::shared_ptr<const DiskSound> writeToDisk(const AudioBuffer& stereo, const StreamOptions& o) {
    auto d = std::make_shared<OwnedDiskSound>();
    d->file = (cacheFolder(o) / ("kit-" + std::to_string(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())) + "-" +
                                 std::to_string(gCacheSerial++) + ".f32"))
                  .string();
    std::FILE* f = std::fopen(d->file.c_str(), "wb");
    if (!f) return nullptr;
    const bool ok = writeFloats(f, stereo.samples.data(), stereo.samples.size());
    std::fclose(f);
    if (!ok) return nullptr;
    d->frames = stereo.frames();
    return d;
}

}  // namespace

void setDiskThrottle(double secondsPerMegabyte) { gThrottle = std::max(0.0, secondsPerMegabyte); }
int64_t streamUnderruns() { return gUnderruns.load(); }

KitPtr kitFor(const KitPtr& kit, Id output) {
    if (!kit) return nullptr;
    auto part = std::make_shared<Kit>();
    part->sampleRate = kit->sampleRate;
    for (const Kit::Slot& sl : kit->pads)
        if (sl.settings.output == output && sl.settings.key >= 0 && sl.settings.key < 128) {
            part->pads.push_back(sl);
            part->byKey[size_t(sl.settings.key)] = int(part->pads.size());
        }
    return part;
}

KitPtr loadKit(const SamplerSettings& settings, int sampleRate, std::vector<std::string>* missing, const Loader& load,
               const StreamOptions& stream) {
    auto kit = std::make_shared<Kit>();
    kit->sampleRate = sampleRate;
    const int64_t streamOver = stream.longerThan > 0 ? int64_t(stream.longerThan * sampleRate) : -1;
    const int64_t headFrames = std::max<int64_t>(1024, int64_t(stream.headSeconds * sampleRate));
    for (const SamplerSlot& sl : settings.pads) {
        if (sl.key < 0 || sl.key > 127 || kit->byKey[size_t(sl.key)]) continue;
        Kit::Slot slot;
        slot.settings = sl;
        for (const SamplerSound& sd : sl.sounds) {
            std::string why;
            // A long file is read in blocks into its cache file, never held whole.
            if (!load && streamOver > 0) {
                MediaItem probed;
                if (probeMedia(sd.path, probed, &why) && probed.duration * sampleRate > double(streamOver)) {
                    if (auto disk = decodeToDisk(sd.path, sampleRate, stream, &why)) {
                        Kit::Sound s;
                        s.disk = disk;
                        s.start = std::clamp<int64_t>(std::llround(sd.start * sampleRate), 0, disk->frames);
                        s.end = sd.end > sd.start ? std::clamp<int64_t>(std::llround(sd.end * sampleRate), s.start, disk->frames) : disk->frames;
                        s.velLow = sd.velLow, s.velHigh = sd.velHigh;
                        s.head = std::min(headFrames, s.end - s.start);
                        s.buffer = readHead(*disk, s.start, s.head, sampleRate);
                        if (s.buffer && s.end > s.start) slot.sounds.push_back(std::move(s));
                        else if (missing && std::find(missing->begin(), missing->end(), sd.path) == missing->end()) missing->push_back(sd.path);
                        continue;
                    }
                }
            }
            AudioBufferPtr buf = load ? load(sd.path, sampleRate, &why) : MediaPool::instance().audio(sd.path, sampleRate);
            if (!buf || buf->frames() <= 0 || buf->channels < 1) {
                if (missing && std::find(missing->begin(), missing->end(), sd.path) == missing->end()) missing->push_back(sd.path);
                continue;
            }
            if (buf->channels != 2) {  // (the library gives stereo; anything else is folded to it)
                auto st = std::make_shared<AudioBuffer>();
                st->sampleRate = buf->sampleRate;
                st->channels = 2;
                const int64_t n = buf->frames();
                st->samples.resize(size_t(n) * 2);
                for (int64_t i = 0; i < n; ++i) {
                    const float l = buf->samples[size_t(i) * size_t(buf->channels)];
                    const float r = buf->channels > 1 ? buf->samples[size_t(i) * size_t(buf->channels) + 1] : l;
                    st->samples[size_t(i) * 2] = l, st->samples[size_t(i) * 2 + 1] = r;
                }
                buf = st;
            }
            Kit::Sound s;
            s.buffer = buf;
            const int64_t n = buf->frames();
            s.start = std::clamp<int64_t>(std::llround(sd.start * sampleRate), 0, n);
            s.end = sd.end > sd.start ? std::clamp<int64_t>(std::llround(sd.end * sampleRate), s.start, n) : n;
            s.velLow = sd.velLow, s.velHigh = sd.velHigh;
            // A long sound a loader gave (already in memory): streamed all the same, its memory let go.
            if (streamOver > 0 && s.end - s.start > streamOver)
                if (auto disk = writeToDisk(*buf, stream)) {
                    s.disk = disk;
                    s.head = std::min(headFrames, s.end - s.start);
                    s.buffer = readHead(*disk, s.start, s.head, sampleRate);
                    if (!s.buffer) continue;
                }
            if (s.end > s.start) slot.sounds.push_back(std::move(s));
        }
        kit->pads.push_back(std::move(slot));
        kit->byKey[size_t(sl.key)] = int(kit->pads.size());
    }
    return kit;
}

// ---- Engine -----------------------------------------------------------------------------------------------------

// (room made here, outside the audio thread, so render() never allocates)
constexpr size_t kRobinCapacity = 1024, kEventCapacity = 4096;
constexpr size_t kWindows = 32;  // voices of streamed sounds at once (more play their heads, then silence)

// A streamed voice's frames past its head: read ahead by the disk thread into a ring the voice reads behind it.
struct Engine::Window {
    std::atomic<int> state{0};  // 0 free, 1 playing (the disk thread fills it), 2 let go (the disk thread frees it)
    std::shared_ptr<const DiskSound> disk;  // set by the audio thread before 1, let go of by the disk thread
    int64_t fileStart = 0, length = 0;      // the sound's first frame in the file, and its frames
    int64_t capacity = 0;
    std::vector<float> buf;
    std::atomic<int64_t> filled{0};  // the sound's frames (from its start) read so far
    std::atomic<int64_t> want{0};    // where the voice is
};

struct Engine::Disk {
    std::vector<std::unique_ptr<Window>> windows;
    std::atomic<bool> running{true};
    std::mutex m;
    std::condition_variable wake;
    std::thread thread;
    explicit Disk(int64_t capacity) {
        for (size_t i = 0; i < kWindows; ++i) {
            auto w = std::make_unique<Window>();
            w->capacity = capacity;
            w->buf.assign(size_t(capacity) * 2, 0.0f);
            windows.push_back(std::move(w));
        }
        thread = std::thread([this] { run(); });
    }
    ~Disk() {
        running = false;
        wake.notify_all();
        thread.join();
    }
    void run() {
        std::map<std::string, std::FILE*> files;
        std::vector<float> chunk;
        while (running) {
            bool worked = false;
            for (const auto& wp : windows) {
                Window& w = *wp;
                const int st = w.state.load(std::memory_order_acquire);
                if (st == 2) {
                    w.disk.reset();
                    w.state.store(0, std::memory_order_release);
                    continue;
                }
                if (st != 1 || !w.disk) continue;
                const int64_t f = w.filled.load(std::memory_order_relaxed);  // (only this thread moves it)
                const int64_t ahead = f - w.want.load(std::memory_order_acquire);
                const int64_t limit = w.capacity * 3 / 4;
                if (f >= w.length || ahead >= limit) continue;
                const int64_t n = std::min({int64_t(4096), w.capacity / 4, w.length - f, limit - ahead});
                if (n <= 0) continue;
                std::FILE*& file = files[w.disk->file];
                if (!file) file = std::fopen(w.disk->file.c_str(), "rb");
                chunk.assign(size_t(n) * 2, 0.0f);
                if (file && seekFrame(file, w.fileStart + f)) {
                    const size_t got = std::fread(chunk.data(), sizeof(float), chunk.size(), file);
                    if (got < chunk.size()) std::fill(chunk.begin() + std::ptrdiff_t(got), chunk.end(), 0.0f);
                }
                if (const double t = gThrottle.load(); t > 0)  // (a slow disk, for the tests)
                    std::this_thread::sleep_for(std::chrono::duration<double>(t * double(chunk.size() * sizeof(float)) / 1e6));
                for (int64_t i = 0; i < n; ++i) {
                    const size_t k = size_t((f + i) % w.capacity) * 2;
                    w.buf[k] = chunk[size_t(i) * 2];
                    w.buf[k + 1] = chunk[size_t(i) * 2 + 1];
                }
                w.filled.store(f + n, std::memory_order_release);
                worked = true;
            }
            if (!worked) {
                std::unique_lock<std::mutex> lock(m);
                wake.wait_for(lock, std::chrono::milliseconds(2));
            }
        }
        for (auto& [name, file] : files)
            if (file) std::fclose(file);
    }
};

Engine::Engine(int sampleRate) : rate_(std::max(1, sampleRate)), chokeLen_(std::max<int64_t>(1, rate_ / 1000)) {
    voices_.reserve(kMaxVoices);
    robinKeys_.reserve(kRobinCapacity);
    robinTurns_.reserve(kRobinCapacity);
    queued_.reserve(kEventCapacity);
    due_.reserve(kEventCapacity);
    retired_.reserve(4);
}

Engine::~Engine() {
    for (Voice& v : voices_) letGo(v);
}

void Engine::letGo(Voice& v) {
    if (v.window) v.window->state.store(2, std::memory_order_release);
    v.window = nullptr;
}

void Engine::setKit(KitPtr kit) {
    KitPtr pending;  // (the one replaced, freed after the lock)
    std::vector<KitPtr> freed;
    // A kit with streamed sounds: the disk thread and its windows (made once, before the audio thread can see the kit).
    if (kit && !disk_)
        for (const Kit::Slot& sl : kit->pads)
            if (std::any_of(sl.sounds.begin(), sl.sounds.end(), [](const Kit::Sound& sd) { return sd.disk != nullptr; })) {
                std::lock_guard<std::mutex> lock(kitMutex_);
                disk_ = std::make_shared<Disk>(std::max<int64_t>(8192, rate_));
                break;
            }
    {
        std::lock_guard<std::mutex> lock(kitMutex_);
        pending = std::move(pendingKit_);
        pendingKit_ = std::move(kit);
        // Kits the audio thread has let go of and no voice still plays.
        for (auto it = retired_.begin(); it != retired_.end();)
            if (it->use_count() == 1) freed.push_back(std::move(*it)), it = retired_.erase(it);
            else ++it;
        retired_.reserve(retired_.size() + 2);  // (room for the audio thread to retire the current one)
    }
}

KitPtr Engine::kit() const {
    std::lock_guard<std::mutex> lock(kitMutex_);
    return pendingKit_;
}

void Engine::noteOn(int key, int velocity, int64_t at, uint32_t seed, uint64_t id) {
    if (key < 0 || key > 127) return;
    std::lock_guard<std::mutex> lock(eventMutex_);
    // (the lists hold kEventCapacity, so the audio thread never makes them bigger: past that a note is not played,
    // with room kept for the notes' ends)
    if (queued_.size() + 256 >= kEventCapacity) return;
    queued_.push_back({true, key, std::clamp(velocity, 1, 127), at, seed, order_++, id});
}

void Engine::noteOff(int key, int64_t at, uint64_t id) {
    if (key < 0 || key > 127) return;
    std::lock_guard<std::mutex> lock(eventMutex_);
    if (queued_.size() >= kEventCapacity) {
        // (full: a note's end is never dropped; the latest of its starts still waiting goes instead, or else the latest
        // start waiting)
        auto on = std::find_if(queued_.rbegin(), queued_.rend(), [&](const Event& e) { return e.on && e.key == key; });
        if (on == queued_.rend()) on = std::find_if(queued_.rbegin(), queued_.rend(), [](const Event& e) { return e.on; });
        if (on == queued_.rend()) return;  // (all ends: this key's is among them or the note has gone)
        queued_.erase(std::next(on).base());
    }
    queued_.push_back({false, key, 0, at, 0, order_++, id});
}

void Engine::allNotesOff() {
    std::lock_guard<std::mutex> lock(eventMutex_);
    if (queued_.size() + 128 > kEventCapacity)  // (full: the notes waiting are let go, as all of them end anyway)
        queued_.erase(std::remove_if(queued_.begin(), queued_.end(), [](const Event& e) { return e.on; }), queued_.end());
    if (queued_.size() + 128 > kEventCapacity) queued_.clear();
    for (int k = 0; k < 128; ++k) queued_.push_back({false, k, 0, -1, 0, order_++, 0});
}

void Engine::reset() {
    std::lock_guard<std::mutex> lock(eventMutex_);
    queued_.clear();
    resetPending_ = true;
}

void Engine::startVoice(const Event& e, const KitPtr& kit) {
    const Kit::Slot* slot = kit ? kit->slot(e.key) : nullptr;
    if (!slot || slot->sounds.empty()) return;
    // The force played, through the slot's curve: what picks the layer and sets the level.
    const int velocity = curveVelocity(slot->settings, e.velocity);
    // (counted rather than listed: nothing allocated on the audio thread)
    auto answers = [&](const Kit::Sound& sd) { return velocity >= sd.velLow && velocity <= sd.velHigh; };
    size_t layerSize = 0;
    int firstSound = -1;
    for (size_t i = 0; i < slot->sounds.size(); ++i)
        if (answers(slot->sounds[i])) {
            if (firstSound < 0) firstSound = int(i);
            ++layerSize;
        }
    if (!layerSize) return;
    // Round robin among the sounds answering this velocity.
    const std::pair<int, int> robin{e.key, firstSound};
    size_t r = 0;
    while (r < robinKeys_.size() && robinKeys_[r] != robin) ++r;
    uint32_t turn = 0;
    if (r < robinKeys_.size()) turn = robinTurns_[r]++;
    else if (robinKeys_.size() < kRobinCapacity) robinKeys_.push_back(robin), robinTurns_.push_back(1);
    size_t pick = turn % layerSize, chosen = 0;
    for (size_t i = 0; i < slot->sounds.size(); ++i)
        if (answers(slot->sounds[i]) && pick-- == 0) {
            chosen = i;
            break;
        }
    const Kit::Sound& sound = slot->sounds[chosen];
    const SamplerSlot& st = slot->settings;
    // Choke: every voice of the group fades out within a millisecond.
    if (st.choke)
        for (Voice& v : voices_)
            if (v.choke == st.choke && v.choked < 0) v.choked = 0;
    uint32_t rnd = e.seed;
    const double pitch = st.pitchVariation > 0 ? (unit(rnd) * 2 - 1) * st.pitchVariation : 0;
    const double level = st.levelVariation > 0 ? (unit(rnd) * 2 - 1) * st.levelVariation : 0;
    Voice v;
    v.kit = kit;
    v.sound = &sound;
    v.key = e.key;
    v.id = e.id;
    v.choke = st.choke;
    v.oneShot = st.oneShot;
    v.step = pitch != 0 ? std::pow(2.0, pitch / 12.0) : 1.0;
    v.gain = float(std::pow(10.0, (st.gainDb + level) / 20.0) * velocity / 127.0);
    v.attackLen = std::llround(st.attack * rate_);
    v.releaseLen = std::max<int64_t>(1, std::llround(st.release * rate_));
    v.serial = serial_++;
    if (voices_.size() >= kMaxVoices) {  // (the oldest gives way)
        auto oldest = std::min_element(voices_.begin(), voices_.end(), [](const Voice& a, const Voice& b) { return a.serial < b.serial; });
        letGo(*oldest);
        voices_.erase(oldest);
    }
    // A streamed sound: a window the disk thread fills from where its head ends (none free: the head alone plays).
    if (sound.disk && disk_)
        for (const auto& w : disk_->windows)
            if (w->state.load(std::memory_order_acquire) == 0) {
                w->disk = sound.disk;  // (the window held none: nothing is freed here)
                w->fileStart = sound.start;
                w->length = sound.end - sound.start;
                w->filled.store(sound.head, std::memory_order_relaxed);
                w->want.store(0, std::memory_order_relaxed);
                w->state.store(1, std::memory_order_release);
                v.window = w.get();
                break;
            }
    voices_.push_back(std::move(v));
}

void Engine::release(int key, uint64_t id) {
    for (Voice& v : voices_)
        if (v.key == key && (!id || v.id == id) && !v.oneShot && v.released < 0) v.released = 0;
}

void Engine::renderVoices(float* out, int frames) {
    static const float silence[2] = {0.0f, 0.0f};
    for (Voice& v : voices_) {
        const Kit::Sound& s = *v.sound;
        const float* src = s.buffer->samples.data();
        const int64_t len = s.end - s.start;
        const bool streamed = s.disk != nullptr;
        Window* win = v.window;
        int64_t told = int64_t(v.pos);  // (where the disk thread was last told the voice is)
        if (win) win->want.store(told, std::memory_order_release);
        // A frame of the sound (from its start): in memory, or for a streamed one past its head, from its window.
        auto frame = [&](int64_t rel) -> const float* {
            if (!streamed) return src + size_t(s.start + rel) * 2;
            if (rel < s.head) return src + size_t(rel) * 2;
            if (!win) return silence;
            // (told again as it goes in a long block, so the disk thread reads on past where the block began)
            if (rel - told >= win->capacity / 4) win->want.store(told = rel, std::memory_order_release);
            int64_t f = win->filled.load(std::memory_order_acquire);
            if (rel >= f && blocking_) {
                // (offline: waited for, up to half a minute for a very slow disk)
                win->want.store(told = rel, std::memory_order_release);
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                while ((f = win->filled.load(std::memory_order_acquire)) <= rel && std::chrono::steady_clock::now() < until)
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            if (rel < f && rel >= f - win->capacity) return win->buf.data() + size_t(rel % win->capacity) * 2;
            ++gUnderruns;
            return silence;
        };
        for (int i = 0; i < frames; ++i) {
            if (v.pos >= double(len)) break;
            float env = 1;
            if (v.attackLen > 0 && v.age < v.attackLen) env = float(double(v.age) / double(v.attackLen));
            if (v.released >= 0) {
                if (v.released == 0) v.releaseFrom = env;
                if (v.released >= v.releaseLen) {
                    v.pos = double(len);
                    break;
                }
                env = v.releaseFrom * float(1.0 - double(v.released) / double(v.releaseLen));
                ++v.released;
            }
            if (v.choked >= 0) {
                if (v.choked >= chokeLen_) {
                    v.pos = double(len);
                    break;
                }
                env *= float(1.0 - double(v.choked + 1) / double(chokeLen_ + 1));
                ++v.choked;
            }
            float l, r;
            const int64_t at = int64_t(v.pos);
            const double frac = v.pos - double(at);
            const float* a = frame(at);
            if (frac == 0 || at + 1 >= len) {
                l = a[0], r = a[1];
            } else {
                const float* b = frame(at + 1);
                l = float(a[0] + (b[0] - a[0]) * frac);
                r = float(a[1] + (b[1] - a[1]) * frac);
            }
            const float g = v.gain * env;
            out[size_t(i) * 2] += l * g;
            out[size_t(i) * 2 + 1] += r * g;
            v.pos += v.step;
            ++v.age;
        }
    }
    for (Voice& v : voices_)
        if (v.pos >= double(v.sound->end - v.sound->start)) letGo(v);
    std::erase_if(voices_, [](const Voice& v) { return v.pos >= double(v.sound->end - v.sound->start); });
}

void Engine::render(int64_t start, int frames, float* out) {
    if (frames <= 0) return;
    if (std::unique_lock<std::mutex> lock(kitMutex_, std::try_to_lock);
        lock.owns_lock() && kit_ != pendingKit_ && (!kit_ || retired_.size() < retired_.capacity())) {
        // (the kit let go of is kept for setKit to free, in the room made for it there)
        if (kit_) retired_.push_back(std::move(kit_));
        kit_ = pendingKit_;
    }
    if (std::unique_lock<std::mutex> lock(eventMutex_, std::try_to_lock); lock.owns_lock()) {
        if (resetPending_) {
            for (Voice& v : voices_) letGo(v);
            voices_.clear(), due_.clear(), robinKeys_.clear(), robinTurns_.clear();
            resetPending_ = false;
        }
        if (due_.empty()) std::swap(due_, queued_);  // (buffers traded, not copied)
        else {
            // (as many as the list holds without growing: the rest wait for the next block)
            const size_t n = std::min(queued_.size(), due_.size() < kEventCapacity ? kEventCapacity - due_.size() : size_t(0));
            due_.insert(due_.end(), queued_.begin(), queued_.begin() + std::ptrdiff_t(n));
            queued_.erase(queued_.begin(), queued_.begin() + std::ptrdiff_t(n));
        }
    }
    if (due_.empty() && voices_.empty()) return;  // (silent: nothing to do)
    // In time order (as played, for notes at the same sample; `order` makes it total, so no stable sort's buffer).
    std::sort(due_.begin(), due_.end(), [&](const Event& a, const Event& b) {
        const int64_t ta = a.at < 0 ? start : std::max(a.at, start), tb = b.at < 0 ? start : std::max(b.at, start);
        return ta != tb ? ta < tb : a.order < b.order;
    });
    const int64_t end = start + frames;
    auto dueAt = [&](const Event& e) { return e.at < 0 ? start : std::max(e.at, start); };
    int done = 0;
    size_t used = 0;
    while (done < frames) {
        while (used < due_.size() && dueAt(due_[used]) <= start + done) {
            const Event& e = due_[used++];
            if (e.on) startVoice(e, kit_);
            else release(e.key, e.id);
        }
        const int64_t next = used < due_.size() ? std::min(dueAt(due_[used]), end) : end;
        renderVoices(out + size_t(done) * 2, int(next - start - done));
        done = int(next - start);
    }
    // Events left for a later block (all of them were due at or after its end).
    due_.erase(due_.begin(), due_.begin() + std::ptrdiff_t(used));
}

// ---- Passes -----------------------------------------------------------------------------------------------------

std::vector<float> renderNotes(const KitPtr& kit, const std::vector<SamplerNote>& notes, uint32_t seed, int64_t start, int64_t frames) {
    std::vector<float> out;
    if (!kit) return out;
    Engine e(kit->sampleRate);
    e.setBlocking(true);  // (streamed sounds read whole, however slow the disk)
    e.setKit(kit);
    int64_t last = start;
    for (size_t i = 0; i < notes.size(); ++i) {
        const SamplerNote& n = notes[i];
        e.noteOn(n.key, n.velocity, n.at, noteSeed(seed, i), i + 1);
        e.noteOff(n.key, n.at + std::max<int64_t>(1, n.length), i + 1);
        last = std::max(last, n.at + std::max<int64_t>(1, n.length));
    }
    const int block = 1024;
    const int64_t limit = frames >= 0 ? frames : last - start + int64_t(kit->sampleRate) * 600;
    for (int64_t at = 0; at < limit;) {
        const int n = int(std::min<int64_t>(block, limit - at));
        out.resize(size_t(at + n) * 2, 0.0f);
        e.render(start + at, n, out.data() + size_t(at) * 2);
        at += n;
        if (frames < 0 && start + at >= last && e.activeVoices() == 0) break;
    }
    if (frames < 0) {  // (trailing silence of the last block left out)
        size_t keep = out.size();
        while (keep >= 2 && out[keep - 1] == 0.0f && out[keep - 2] == 0.0f && int64_t(keep / 2) > last - start) keep -= 2;
        out.resize(keep);
    }
    return out;
}

bool writeWav(const std::string& path, const std::vector<float>& stereo, int sampleRate, std::string* error) {
    if (stereo.size() * 4 > size_t(UINT32_MAX) - 36) {  // (a WAV file's sizes are 32-bit: about 3 hours here)
        if (error) *error = "The pass is too long for a WAV file";
        return false;
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = uint32_t(stereo.size() * 4);
    f.write("RIFF", 4), u32(36 + bytes), f.write("WAVE", 4);
    f.write("fmt ", 4), u32(16), u16(3), u16(2), u32(uint32_t(sampleRate)), u32(uint32_t(sampleRate) * 8), u16(8), u16(32);
    f.write("data", 4), u32(bytes);
    f.write(reinterpret_cast<const char*>(stereo.data()), std::streamsize(bytes));  // (little-endian hosts)
    f.close();
    if (!f) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    return true;
}

std::vector<std::pair<double, double>> splitByOnsets(const AudioBuffer& recording, double sensitivity, double minGap) {
    std::vector<std::pair<double, double>> out;
    const std::vector<double> onsets = detectOnsets(recording, sensitivity, minGap);
    const double length = double(recording.frames()) / std::max(1, recording.sampleRate);
    for (size_t i = 0; i < onsets.size(); ++i) {
        const double end = i + 1 < onsets.size() ? onsets[i + 1] : length;
        if (end > onsets[i]) out.push_back({onsets[i], end});
    }
    return out;
}

// ---- Edits ------------------------------------------------------------------------------------------------------

edit::Result setSlot(Sequence& s, TrackRef track, const SamplerSlot& slot) {
    std::string why;
    Track* t = samplerTrack(s, track, why);
    if (!t) return edit::Result::fail(why);
    if (slot.key < 0 || slot.key > 127) return edit::Result::fail("A slot's key is 0 to 127");
    auto& pads = t->sampler.pads;
    auto it = std::find_if(pads.begin(), pads.end(), [&](const SamplerSlot& e) { return e.key == slot.key; });
    if (it != pads.end()) *it = slot;
    else {
        pads.push_back(slot);
        std::sort(pads.begin(), pads.end(), [](const SamplerSlot& a, const SamplerSlot& b) { return a.key < b.key; });
    }
    return {};
}

edit::Result removeSlot(Sequence& s, TrackRef track, int key) {
    std::string why;
    Track* t = samplerTrack(s, track, why);
    if (!t) return edit::Result::fail(why);
    const size_t before = t->sampler.pads.size();
    std::erase_if(t->sampler.pads, [&](const SamplerSlot& e) { return e.key == key; });
    return t->sampler.pads.size() < before ? edit::Result{} : edit::Result::fail("No slot on that key");
}

edit::Result addSlices(Sequence& s, TrackRef track, const std::string& path, const std::vector<std::pair<double, double>>& slices,
                       int firstKey, const std::string& name) {
    if (slices.empty()) return edit::Result::fail("No hits found to put on keys");
    if (firstKey < 0 || firstKey + int(slices.size()) - 1 > 127)
        return edit::Result::fail("The hits do not fit on the keys from " + std::to_string(firstKey) + " (" + std::to_string(slices.size()) + " of them)");
    for (size_t i = 0; i < slices.size(); ++i) {
        SamplerSlot sl;
        sl.key = firstKey + int(i);
        sl.name = (name.empty() ? std::string("Hit") : name) + " " + std::to_string(i + 1);
        sl.sounds.push_back({path, slices[i].first, slices[i].second, 1, 127});
        // (a slot already on the key keeps its playing settings)
        if (const Track* t = trackAt(s, track))
            for (const SamplerSlot& old : t->sampler.pads)
                if (old.key == sl.key) {
                    const std::vector<SamplerSound> sounds = sl.sounds;
                    const std::string nm = sl.name;
                    sl = old, sl.sounds = sounds, sl.name = nm;
                }
        edit::Result r = setSlot(s, track, sl);
        if (!r.ok) return r;
    }
    return {};
}

namespace {

double projectDuration(const Project& p, Id media) {
    const MediaItem* m = p.findMedia(media);
    return m ? m->duration : 0;
}

// The pass's sound from timeline frame `from`: written to `file` and added to the project.
Id passMedia(Project& p, const Sequence& s, const KitPtr& kit, const std::vector<SamplerNote>& notes, uint32_t seed, FrameTime from,
             const std::string& file, int64_t& frames, std::string& why) {
    const double fps = s.fpsValue();
    const int64_t startSample = std::llround(double(from) * kit->sampleRate / fps);
    std::vector<float> audio = renderNotes(kit, notes, seed, startSample);
    // (silence to the end of the frame the sound ends in, so a clip of whole frames holds all of it)
    if (!audio.empty()) {
        const int64_t endSample = startSample + int64_t(audio.size() / 2);
        FrameTime endFrame = FrameTime(std::ceil(double(endSample) * fps / kit->sampleRate));
        while (std::llround(double(endFrame) * kit->sampleRate / fps) < endSample) ++endFrame;
        audio.resize(size_t(std::llround(double(endFrame) * kit->sampleRate / fps) - startSample) * 2, 0.0f);
    }
    frames = int64_t(audio.size() / 2);
    if (frames <= 0) {
        why = "The pass made no sound (no slots on its keys)";
        return 0;
    }
    if (!writeWav(file, audio, kit->sampleRate, &why)) return 0;
    MediaItem m;
    m.id = p.newId();
    if (!probeMedia(file, m, &why)) return 0;
    p.media.push_back(m);
    return m.id;
}

}  // namespace

namespace {

// The audio track with this id.
std::optional<TrackRef> audioTrackRef(const Sequence& s, Id id) {
    for (size_t i = 0; i < s.audioTracks.size(); ++i)
        if (s.audioTracks[i].id == id) return TrackRef{TrackKind::Audio, int(i)};
    return std::nullopt;
}

// The frame a note at sample `at` falls in (never one whose first sample comes after it).
FrameTime frameOfSample(const Sequence& s, int64_t at) {
    const double fps = s.fpsValue();
    FrameTime f = FrameTime(std::floor(double(std::max<int64_t>(0, at)) * fps / s.sampleRate));
    while (f > 0 && std::llround(double(f) * s.sampleRate / fps) > at) --f;
    return f;
}

// "pass.wav" for track 12: "pass-12.wav".
std::string partFile(const std::string& file, Id track) {
    const size_t dot = file.find_last_of('.'), slash = file.find_last_of("/\\");
    const size_t at = dot != std::string::npos && (slash == std::string::npos || dot > slash) ? dot : file.size();
    return file.substr(0, at) + "-" + std::to_string(track) + file.substr(at);
}

// One part of a pass (the slots of `kit` heard on `dst`) onto a new lane of `dst` and its comp from the first note.
edit::Result placePart(Project& p, Sequence& s, TrackRef dst, const KitPtr& kit, const std::vector<SamplerNote>& notes, uint32_t seed,
                       const std::string& file, FrameTime first, FrameTime noteFrame, const std::string& name, bool clipPerHit) {
    std::string why;
    int64_t frames = 0;
    const Id media = passMedia(p, s, kit, notes, seed, first, file, frames, why);
    if (!media) return edit::Result::fail(why);
    edit::Result r = edit::addTakeLane(p, s, dst, media, first, name);
    if (!r.ok) return r;
    const Id lane = r.created.front();
    const double fps = s.fpsValue();
    const FrameTime to = first + FrameTime(std::llround(double(frames) * fps / s.sampleRate));
    // (a part sent to another track is comped from its own first note there)
    FrameTime own = -1;
    for (const SamplerNote& n : notes)
        if (kit->slot(n.key) && (own < 0 || frameOfSample(s, n.at) < own)) own = frameOfSample(s, n.at);
    noteFrame = std::max(noteFrame, own);
    // The comp takes it from its first note on (what the track had before the first note stays).
    if (to > noteFrame) {
        edit::Result c = edit::compTake(p, s, dst, lane, noteFrame, to);
        if (!c.ok) return c;
        r.created.insert(r.created.end(), c.created.begin(), c.created.end());
        // One clip a hit: the comp cut where each later note falls (notes in the same frame make one).
        if (clipPerHit) {
            std::vector<FrameTime> cuts;
            for (const SamplerNote& n : notes)
                if (kit->slot(n.key)) cuts.push_back(frameOfSample(s, n.at));
            std::sort(cuts.begin(), cuts.end());
            cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
            for (FrameTime f : cuts)
                if (f > noteFrame && f < to) {
                    edit::Result cut = edit::razor(p, s, dst, f);
                    if (!cut.ok) return cut;
                }
        }
    }
    return r;
}

}  // namespace

edit::Result addPass(Project& p, Sequence& s, TrackRef track, const KitPtr& kit, std::vector<SamplerNote> notes, uint32_t seed,
                     const std::string& file, FrameTime from, const std::string& name) {
    std::string why;
    if (!samplerTrack(s, track, why)) return edit::Result::fail(why);
    if (!kit) return edit::Result::fail("No sounds loaded");
    if (notes.empty()) return edit::Result::fail("No notes were played");
    if (kit->sampleRate != s.sampleRate) return edit::Result::fail("The sounds were loaded at another rate than the sequence's");
    // (kept in the order played: it numbers their variation and turns)
    const int64_t earliest = std::min_element(notes.begin(), notes.end(), [](const SamplerNote& a, const SamplerNote& b) { return a.at < b.at; })->at;
    const double fps = s.fpsValue();
    const FrameTime noteFrame = frameOfSample(s, earliest);
    // (from where recording started, at most two seconds before the first note: a pass started long before its
    // first hit is not rendered as minutes of silence)
    const FrameTime lead = FrameTime(std::ceil(2 * fps));
    const FrameTime first = from >= 0 ? std::max(std::min(noteFrame, from), std::max<FrameTime>(0, noteFrame - lead)) : noteFrame;
    const Id ownId = trackAt(s, track)->id;
    const bool clipPerHit = trackAt(s, track)->sampler.clipPerHit;
    // The tracks the slots send to (other than this one), in the slots' order; a slot sent to a track that is not
    // there (deleted) plays on this one.
    std::vector<Id> others;
    for (const Kit::Slot& sl : kit->pads)
        if (sl.settings.output && sl.settings.output != ownId && audioTrackRef(s, sl.settings.output) &&
            std::find(others.begin(), others.end(), sl.settings.output) == others.end())
            others.push_back(sl.settings.output);
    KitPtr own = kit;
    if (!others.empty()) {
        auto mine = std::make_shared<Kit>(*kit);
        for (Kit::Slot& sl : mine->pads)
            if (std::find(others.begin(), others.end(), sl.settings.output) == others.end()) sl.settings.output = 0;
        own = kitFor(mine, 0);
        const KitPtr whole = mine;
        edit::Result all;
        bool any = false;
        auto playsOn = [&](const KitPtr& k) {
            return std::any_of(notes.begin(), notes.end(), [&](const SamplerNote& n) { return k->slot(n.key) != nullptr; });
        };
        if (playsOn(own)) {
            edit::Result r = placePart(p, s, track, own, notes, seed, file, first, noteFrame, name, clipPerHit);
            if (!r.ok) return r;
            trackAt(s, track)->sampler.passes.push_back({r.created.front(), seed, notes, 0});
            all.created.insert(all.created.end(), r.created.begin(), r.created.end());
            any = true;
        }
        for (Id o : others) {
            const KitPtr part = kitFor(whole, o);
            if (!playsOn(part)) continue;
            const TrackRef dst = *audioTrackRef(s, o);
            edit::Result r = placePart(p, s, dst, part, notes, seed, partFile(file, o), first, noteFrame, name, clipPerHit);
            if (!r.ok) return r;
            trackAt(s, track)->sampler.passes.push_back({r.created.front(), seed, notes, o});
            all.created.insert(all.created.end(), r.created.begin(), r.created.end());
            any = true;
        }
        if (!any) return edit::Result::fail("The pass made no sound (no slots on its keys)");
        return all;
    }
    edit::Result r = placePart(p, s, track, own, notes, seed, file, first, noteFrame, name, clipPerHit);
    if (!r.ok) return r;
    trackAt(s, track)->sampler.passes.push_back({r.created.front(), seed, std::move(notes), 0});
    return r;
}

edit::Result revoicePass(Project& p, Sequence& s, TrackRef track, Id lane, const KitPtr& kit, const std::string& file) {
    std::string why;
    Track* t = samplerTrack(s, track, why);
    if (!t) return edit::Result::fail(why);
    if (!kit) return edit::Result::fail("No sounds loaded");
    auto pass = std::find_if(t->sampler.passes.begin(), t->sampler.passes.end(), [&](const SamplerPass& ps) { return ps.lane == lane; });
    if (pass == t->sampler.passes.end()) return edit::Result::fail("That lane is not a sampler pass");
    const SamplerPass ps = *pass;
    const Id ownId = t->id;
    // The part of a pass sent to another track: its lane is there, voiced by the slots sent to it.
    TrackRef where = track;
    if (ps.track) {
        const auto ref = audioTrackRef(s, ps.track);
        if (!ref) return edit::Result::fail("The track that pass was sent to is not there");
        where = *ref;
    }
    KitPtr part = kit;
    if (ps.track || std::any_of(kit->pads.begin(), kit->pads.end(), [&](const Kit::Slot& sl) { return sl.settings.output != 0; })) {
        auto mine = std::make_shared<Kit>(*kit);
        for (Kit::Slot& sl : mine->pads)
            if (sl.settings.output == ownId || (sl.settings.output && !audioTrackRef(s, sl.settings.output))) sl.settings.output = 0;
        part = kitFor(mine, ps.track);
    }
    t = trackAt(s, where);
    auto ln = std::find_if(t->lanes.begin(), t->lanes.end(), [&](const TakeLane& l) { return l.id == lane; });
    if (ln == t->lanes.end()) return edit::Result::fail("That lane is not a sampler pass");
    int64_t frames = 0;
    const FrameTime from = ln->start;
    const Id old = ln->mediaId;
    const Id media = passMedia(p, s, part, ps.notes, ps.seed, from, file, frames, why);
    if (!media) return edit::Result::fail(why);
    t = trackAt(s, where);  // (the project's media grew, not the track; looked up again all the same)
    ln = std::find_if(t->lanes.begin(), t->lanes.end(), [&](const TakeLane& l) { return l.id == lane; });
    ln->mediaId = media;
    // The comp's clips cut from this lane take the new sound at the same places; one that ran to the old sound's end
    // runs to the new one's (as far as the next clip).
    const FrameTime oldEnd = from + FrameTime(std::llround(projectDuration(p, old) * s.fpsValue()));
    const FrameTime newEnd = from + FrameTime(std::llround(double(frames) * s.fpsValue() / s.sampleRate));
    auto remap = [&](std::vector<Clip>& clips) {
        for (size_t i = 0; i < clips.size(); ++i) {  // (clips are kept sorted by start)
            Clip& c = clips[i];
            if (c.mediaId != old) continue;
            c.mediaId = media;
            if (c.end() == oldEnd && newEnd > oldEnd) {
                const FrameTime limit = i + 1 < clips.size() ? clips[i + 1].start : newEnd;
                c.duration = std::max<FrameTime>(1, std::min(newEnd, limit) - c.start);
            } else if (c.end() > newEnd && newEnd > c.start) {
                c.duration = newEnd - c.start;  // (never past the new sound's end)
            }
        }
    };
    remap(t->clips);
    for (TrackVersion& v : t->versions) remap(v.clips);
    edit::Result r;
    r.created.push_back(lane);
    return r;
}

}  // namespace montage::sampler
