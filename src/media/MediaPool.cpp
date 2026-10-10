#include "MediaPool.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <utility>

#include "core/Interpretation.h"

namespace montage {

namespace {
constexpr char kChannelMark = '\x1f';  // never in a file name
}  // namespace

std::string audioKey(const std::string& path, const std::vector<int>& channels) {
    if (channels.empty()) return path;
    std::string key = path + kChannelMark;
    for (size_t i = 0; i < channels.size(); ++i) key += (i ? "," : "") + std::to_string(channels[i]);
    return key;
}

std::string ambisonicAudioKey(const std::string& path) { return path + kChannelMark + "ambix"; }

bool isAmbisonicAudioKey(const std::string& key) {
    const size_t mark = key.find(kChannelMark);
    return mark != std::string::npos && key.compare(mark + 1, std::string::npos, "ambix") == 0;
}

std::string audioKeyFile(const std::string& key, std::vector<int>* channels) {
    const size_t mark = key.find(kChannelMark);
    if (channels) {
        channels->clear();
        if (mark != std::string::npos && !isAmbisonicAudioKey(key)) {
            size_t from = mark + 1;
            while (from < key.size()) {
                size_t comma = key.find(',', from);
                if (comma == std::string::npos) comma = key.size();
                channels->push_back(std::atoi(key.substr(from, comma - from).c_str()));
                from = comma + 1;
            }
        }
    }
    return mark == std::string::npos ? key : key.substr(0, mark);
}

MediaPool& MediaPool::instance() {
    static MediaPool pool;
    return pool;
}

VideoDecoder* MediaPool::acquire(const std::string& path, double t) {
    std::unique_lock lock(m_);
    auto& slots = decoders_[path];
    // Prefer an idle decoder positioned just before t (cheap sequential decode).
    Slot* best = nullptr;
    double bestScore = 1e18;
    for (auto& s : slots) {
        if (s.busy) continue;
        double pos = s.dec->position();
        double score = (pos >= 0 && pos <= t + 0.05 && t - pos < 2.0) ? (t - pos) : 1e9 + std::fabs(t - pos);
        if (score < bestScore) {
            bestScore = score;
            best = &s;
        }
    }
    if (best && (bestScore < 1e9 || slots.size() >= 4)) {
        best->busy = true;
        best->lastUsed = ++useClock_;
        return best->dec.get();
    }
    if (slots.size() >= 6) {
        if (best) {
            best->busy = true;
            return best->dec.get();
        }
        return nullptr;  // all busy
    }
    size_t open = 0;
    for (const auto& [p, list] : decoders_) open += list.size();
    if (open >= kMaxDecoders) evictOne();
    lock.unlock();
    auto dec = std::make_unique<VideoDecoder>();
    if (!dec->open(path)) return nullptr;
    lock.lock();
    auto& slots2 = decoders_[path];
    slots2.push_back(Slot{std::move(dec), true, ++useClock_});
    return slots2.back().dec.get();
}

bool MediaPool::evictOne() {
    std::vector<Slot>* bestList = nullptr;
    size_t bestIndex = 0;
    uint64_t oldest = UINT64_MAX;
    for (auto& [p, list] : decoders_)
        for (size_t i = 0; i < list.size(); ++i)
            if (!list[i].busy && list[i].lastUsed < oldest) {
                oldest = list[i].lastUsed;
                bestList = &list;
                bestIndex = i;
            }
    if (!bestList) return false;
    bestList->erase(bestList->begin() + long(bestIndex));
    for (auto it = decoders_.begin(); it != decoders_.end();) it = it->second.empty() ? decoders_.erase(it) : std::next(it);
    return true;
}

void MediaPool::release(VideoDecoder* d) {
    std::lock_guard lock(m_);
    auto it = decoders_.find(d->path());
    if (it == decoders_.end()) return;
    if (openFiles_ && !openFiles_->count(it->first)) {
        // Not one of the open project's files: closed rather than kept.
        std::erase_if(it->second, [d](const Slot& s) { return s.dec.get() == d; });
        if (it->second.empty()) decoders_.erase(it);
        return;
    }
    for (auto& s : it->second)
        if (s.dec.get() == d) {
            s.busy = false;
            s.lastUsed = ++useClock_;
        }
}

size_t MediaPool::openDecoders() const {
    std::lock_guard lock(m_);
    size_t n = 0;
    for (const auto& [path, slots] : decoders_) n += slots.size();
    return n;
}

void MediaPool::setOpenFiles(std::optional<std::set<std::string>> paths) {
    std::lock_guard lock(m_);
    openFiles_ = std::move(paths);
    if (!openFiles_) return;
    for (auto it = decoders_.begin(); it != decoders_.end();) {
        if (!openFiles_->count(it->first)) std::erase_if(it->second, [](const Slot& s) { return !s.busy; });
        it = it->second.empty() ? decoders_.erase(it) : std::next(it);
    }
}

Frame16Ptr MediaPool::videoFrame(const std::string& path, double t, int w, int h, bool highQuality) {
    FrameKey key{path, int64_t(std::llround(t * 1e6)), w, h, highQuality};
    {
        std::lock_guard lock(m_);
        auto it = index_.find(key);
        if (it != index_.end()) {
            lru_.splice(lru_.begin(), lru_, it->second);
            return it->second->second;
        }
    }
    VideoDecoder* dec = acquire(path, t);
    if (!dec) return nullptr;
    Frame16Ptr f = dec->frameAt(t, w, h, highQuality);
    release(dec);
    if (!f) return nullptr;
    std::lock_guard lock(m_);
    if (index_.count(key)) return f;
    lru_.emplace_front(key, f);
    index_[key] = lru_.begin();
    cacheBytes_ += f->bytes();
    while (cacheBytes_ > budget_ && lru_.size() > 1) {
        auto& back = lru_.back();
        cacheBytes_ -= back.second->bytes();
        index_.erase(back.first);
        lru_.pop_back();
    }
    return f;
}

bool MediaPool::videoSize(const std::string& path, int& w, int& h) {
    VideoDecoder* dec = acquire(path, 0);
    if (!dec) {
        w = h = 0;
        return false;
    }
    w = dec->displayWidth();
    h = dec->displayHeight();
    release(dec);
    return true;
}

AudioBufferPtr MediaPool::audioIfReady(const std::string& path, int sampleRate) {
    std::lock_guard lock(audioM_);
    auto it = audio_.find({path, sampleRate});
    return it == audio_.end() ? nullptr : it->second;
}

AudioBufferPtr MediaPool::audio(const std::string& path, int sampleRate) {
    std::shared_ptr<std::mutex> decodeLock;
    {
        std::lock_guard lock(audioM_);
        auto it = audio_.find({path, sampleRate});
        if (it != audio_.end()) return it->second;
        auto& l = audioLocks_[{path, sampleRate}];
        if (!l) l = std::make_shared<std::mutex>();
        decodeLock = l;
    }
    std::lock_guard dl(*decodeLock);  // one decode per (path, rate)
    {
        std::lock_guard lock(audioM_);
        auto it = audio_.find({path, sampleRate});
        if (it != audio_.end()) return it->second;
    }
    std::vector<int> channels;
    const std::string file = audioKeyFile(path, &channels);
    AudioBufferPtr buf = isAmbisonicAudioKey(path) ? decodeAmbisonic(file, sampleRate) : decodeAudio(file, sampleRate, nullptr, nullptr, channels);
    if (!buf) buf = std::make_shared<AudioBuffer>();  // remember failures as silence
    PeaksPtr pk = computePeaks(*buf);
    std::function<void(const std::string&)> cb;
    {
        std::lock_guard lock(audioM_);
        audio_[{path, sampleRate}] = buf;
        if (!peaks_.count(path)) peaks_[path] = pk;
        cb = readyCb_;
    }
    if (cb) cb(path);
    return buf;
}

PeaksPtr MediaPool::peaks(const std::string& path, int sampleRate) {
    if (PeaksPtr p = peaksIfReady(path)) return p;
    audio(path, sampleRate);
    return peaksIfReady(path);
}

PeaksPtr MediaPool::peaksIfReady(const std::string& path) {
    std::lock_guard lock(audioM_);
    auto it = peaks_.find(path);
    return it == peaks_.end() ? nullptr : it->second;
}

void MediaPool::setFrameCacheBudget(size_t bytes) {
    std::lock_guard lock(m_);
    budget_ = bytes;
}

size_t MediaPool::frameCacheBytes() const {
    std::lock_guard lock(m_);
    return cacheBytes_;
}

void MediaPool::clear() {
    {
        std::lock_guard lock(m_);
        lru_.clear();
        index_.clear();
        cacheBytes_ = 0;
        for (auto it = decoders_.begin(); it != decoders_.end();) {
            auto& slots = it->second;
            slots.erase(std::remove_if(slots.begin(), slots.end(), [](const Slot& s) { return !s.busy; }), slots.end());
            it = slots.empty() ? decoders_.erase(it) : std::next(it);
        }
    }
    std::lock_guard lock(audioM_);
    audio_.clear();
    peaks_.clear();
}

void MediaPool::forget(const std::string& path) {
    // The file however it is read (each eye of a stereo file, every interpretation of it).
    const std::string file = uninterpretedPath(path);
    auto same = [&](const std::string& key) { return key == path || uninterpretedPath(key) == file; };
    {
        std::lock_guard lock(m_);
        for (auto it = lru_.begin(); it != lru_.end();) {
            if (same(it->first.path)) {
                cacheBytes_ -= it->second->bytes();
                index_.erase(it->first);
                it = lru_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto d = decoders_.begin(); d != decoders_.end();) {
            if (!same(d->first)) {
                ++d;
                continue;
            }
            auto& slots = d->second;
            slots.erase(std::remove_if(slots.begin(), slots.end(), [](const Slot& s) { return !s.busy; }), slots.end());
            d = slots.empty() ? decoders_.erase(d) : std::next(d);
        }
    }
    std::lock_guard lock(audioM_);
    for (auto it = audio_.begin(); it != audio_.end();) it = same(audioKeyFile(it->first.first)) ? audio_.erase(it) : std::next(it);
    for (auto it = peaks_.begin(); it != peaks_.end();) it = same(audioKeyFile(it->first)) ? peaks_.erase(it) : std::next(it);
}

void MediaPool::setReadyCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard lock(audioM_);
    readyCb_ = std::move(cb);
}

}  // namespace montage
