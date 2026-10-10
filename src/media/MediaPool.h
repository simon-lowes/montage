// Montage — shared decoder pool and caches (frames, audio, waveform peaks).
#pragma once

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "Decoder.h"

namespace montage {

// The pool's name for some of a file's audio channels (Clip::channels): audio(), audioIfReady() and the peaks take it
// in place of the path, decoding just those channels. With none it is the path itself.
std::string audioKey(const std::string& path, const std::vector<int>& channels);
// The file an audio key names, and (if asked) its channels.
std::string audioKeyFile(const std::string& key, std::vector<int>* channels = nullptr);
// The pool's name for a file's ambisonic field: audio() gives its four channels (media/Decoder.h decodeAmbisonic).
std::string ambisonicAudioKey(const std::string& path);
bool isAmbisonicAudioKey(const std::string& key);

class MediaPool {
public:
    static MediaPool& instance();

    // Decoded video frame at media time t, scaled to w x h (0 = native).
    Frame16Ptr videoFrame(const std::string& path, double t, int w = 0, int h = 0, bool highQuality = false);
    // Native display size of the video (0,0 if it cannot be opened).
    bool videoSize(const std::string& path, int& w, int& h);

    // Fully decoded stereo audio at `sampleRate` (blocking on first use), of a file or an audioKey.
    AudioBufferPtr audio(const std::string& path, int sampleRate);
    // Returns cached audio or nullptr without decoding.
    AudioBufferPtr audioIfReady(const std::string& path, int sampleRate);
    PeaksPtr peaks(const std::string& path, int sampleRate = 48000);
    PeaksPtr peaksIfReady(const std::string& path);

    void setFrameCacheBudget(size_t bytes);
    size_t frameCacheBytes() const;
    void clear();
    // Drops everything cached for one file (decoded frames, idle decoders, audio and peaks), after it changed on disk.
    void forget(const std::string& path);
    // The files idle decoders may stay open on (the open project's media). Any other file's decoders are closed now
    // if idle, or as soon as they are released, so a closed project's files are let go even while background jobs
    // finish (on Windows an open file stops its folder being moved or renamed). nullopt (the start) lifts the limit.
    void setOpenFiles(std::optional<std::set<std::string>> paths);
    size_t openDecoders() const;  // decoders open now, busy or idle
    // Called (from any thread) when audio/peaks for a path become available.
    void setReadyCallback(std::function<void(const std::string&)> cb);

private:
    MediaPool() = default;
    struct Slot {
        std::unique_ptr<VideoDecoder> dec;
        bool busy = false;
        uint64_t lastUsed = 0;
    };
    // Closes the least recently used idle decoder (any file). Caller holds m_.
    bool evictOne();
    static constexpr size_t kMaxDecoders = 16;  // open files / FFmpeg thread pools across the app
    uint64_t useClock_ = 0;
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
    std::optional<std::set<std::string>> openFiles_;  // setOpenFiles; nullopt = no limit
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
