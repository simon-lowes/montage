// Montage — shared decoder pool and caches (frames, audio, waveform peaks).
#pragma once

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "Decoder.h"

namespace montage {

class MediaPool {
public:
    static MediaPool& instance();

    // Decoded video frame at media time t, scaled to w x h (0 = native).
    Frame16Ptr videoFrame(const std::string& path, double t, int w = 0, int h = 0, bool highQuality = false);
    // Native display size of the video (0,0 if it cannot be opened).
    bool videoSize(const std::string& path, int& w, int& h);

    // Fully decoded stereo audio at `sampleRate` (blocking on first use).
    AudioBufferPtr audio(const std::string& path, int sampleRate);
    // Returns cached audio or nullptr without decoding.
    AudioBufferPtr audioIfReady(const std::string& path, int sampleRate);
    PeaksPtr peaks(const std::string& path, int sampleRate = 48000);
    PeaksPtr peaksIfReady(const std::string& path);

    void setFrameCacheBudget(size_t bytes);
    size_t frameCacheBytes() const;
    void clear();
    // Called (from any thread) when audio/peaks for a path become available.
    void setReadyCallback(std::function<void(const std::string&)> cb);

private:
    MediaPool() = default;
    struct Slot {
        std::unique_ptr<VideoDecoder> dec;
        bool busy = false;
    };
    VideoDecoder* acquire(const std::string& path, double t);
    void release(VideoDecoder* d);

    struct FrameKey {
        std::string path;
        int64_t micros;
        int w, h;
        bool hq;
        bool operator==(const FrameKey& o) const {
            return micros == o.micros && w == o.w && h == o.h && hq == o.hq && path == o.path;
        }
    };
    struct FrameKeyHash {
        size_t operator()(const FrameKey& k) const {
            size_t h = std::hash<std::string>()(k.path);
            h ^= std::hash<int64_t>()(k.micros) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h ^= size_t(k.w) * 31 + size_t(k.h) * 131 + (k.hq ? 7 : 0);
            return h;
        }
    };

    mutable std::mutex m_;
    std::map<std::string, std::vector<Slot>> decoders_;
    std::list<std::pair<FrameKey, Frame16Ptr>> lru_;
    std::unordered_map<FrameKey, decltype(lru_)::iterator, FrameKeyHash> index_;
    size_t cacheBytes_ = 0;
    size_t budget_ = size_t(1) << 30;  // 1 GiB

    std::mutex audioM_;
    std::map<std::pair<std::string, int>, AudioBufferPtr> audio_;
    std::map<std::string, PeaksPtr> peaks_;
    std::map<std::pair<std::string, int>, std::shared_ptr<std::mutex>> audioLocks_;
    std::function<void(const std::string&)> readyCb_;
};

}  // namespace montage
