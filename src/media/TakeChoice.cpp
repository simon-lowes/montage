// Montage — choosing between takes (media/TakeChoice.h).
#include "TakeChoice.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "media/Decoder.h"
#include "media/MediaPool.h"

namespace montage {

namespace {

// What a take says over media seconds [a, b) and its sound there.
void measure(const Project& p, Id media, double a, double b, TakeCandidate& t) {
    const MediaItem* m = p.findMedia(media);
    if (!m) return;
    if (m->transcript && !m->transcript->empty()) {
        t.transcribed = true;
        for (const TranscriptSegment& seg : m->transcript->segments)
            for (const TranscriptWord& w : seg.words) {
                const double mid = (w.start + w.end) / 2;
                if (mid >= a && mid < b) t.said += (t.said.empty() ? "" : " ") + w.text;
            }
    }
    constexpr int kRate = 48000;
    const AudioBufferPtr sound = MediaPool::instance().audio(m->path, kRate);
    if (!sound || sound->channels <= 0) return;
    const int ch = sound->channels;
    const int64_t first = std::clamp<int64_t>(int64_t(std::floor(a * kRate)), 0, sound->frames());
    const int64_t last = std::clamp<int64_t>(int64_t(std::ceil(b * kRate)), first, sound->frames());
    double sum = 0, peak = 0;
    int64_t clipped = 0;
    for (int64_t i = first; i < last; ++i) {
        double frame = 0;
        bool full = false;
        for (int c = 0; c < ch; ++c) {
            const double v = std::fabs(sound->samples[size_t(i * ch + c)]);
            frame = std::max(frame, v);
            full = full || v >= 0.999;
        }
        sum += frame * frame;
        peak = std::max(peak, frame);
        clipped += full;
    }
    if (last > first) {
        t.peakDb = 20 * std::log10(std::max(peak, 1e-10));
        t.rmsDb = 10 * std::log10(std::max(sum / double(last - first), 1e-20));
        t.clippedSeconds = double(clipped) / kRate;
    }
}

}  // namespace

std::vector<TakeCandidate> takeCandidates(const Project& p, const Sequence& s, TrackRef track, FrameTime from, FrameTime to) {
    std::vector<TakeCandidate> out;
    const Track* t = trackAt(s, track);
    if (!t || to <= from) return out;
    const double fps = s.fpsValue();
    for (const TakeLane& l : t->lanes) {
        const MediaItem* m = p.findMedia(l.mediaId);
        if (!m || l.start > from || l.start + mediaFrames(*m, s) < to) continue;
        TakeCandidate c;
        c.lane = l.id;
        c.name = l.name;
        c.rating = l.rating;
        measure(p, l.mediaId, double(from - l.start) / fps, double(to - l.start) / fps, c);
        out.push_back(std::move(c));
    }
    for (const Clip& cl : t->clips) {
        if (cl.takes.empty() || cl.start > from || cl.end() < to) continue;
        for (size_t k = 0; k < cl.takes.size(); ++k) {
            const Take& tk = cl.takes[k];
            TakeCandidate c;
            c.clip = cl.id;
            c.take = int(k);
            const MediaItem* m = p.findMedia(tk.mediaId);
            c.name = int(k) == cl.take ? cl.name : !tk.name.empty() ? tk.name : m ? m->name : std::string();
            // (Its in-point: the clip's, moved by the take's offset; at the clip's speed.)
            const double in = cl.sourceIn + tk.offset;
            measure(p, tk.mediaId, (in + double(from - cl.start) * cl.speed) / fps, (in + double(to - cl.start) * cl.speed) / fps, c);
            out.push_back(std::move(c));
        }
    }
    return out;
}

TakeChoice chooseTake(const Project& p, const Sequence& s, TrackRef track, FrameTime from, FrameTime to, const std::string& line) {
    TakeChoice r;
    r.takes = takeCandidates(p, s, track, from, to);
    r.best = suggestTake(r.takes, line, &r.why);
    return r;
}

namespace {

bool writeFloatWav(const std::string& path, const float* interleaved, int64_t frames, int rate, int channels) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const uint32_t bytes = uint32_t(frames * channels * 4);
    std::fwrite("RIFF", 1, 4, f), u32(36 + bytes), std::fwrite("WAVEfmt ", 1, 8, f), u32(16), u16(3), u16(uint16_t(channels));
    u32(uint32_t(rate)), u32(uint32_t(rate * channels * 4)), u16(uint16_t(channels * 4)), u16(32), std::fwrite("data", 1, 4, f), u32(bytes);
    const bool ok = std::fwrite(interleaved, 4, size_t(frames * channels), f) == size_t(frames * channels);
    return std::fclose(f) == 0 && ok;
}

}  // namespace

std::vector<std::string> splitLoopRecording(const std::string& path, double preroll, double length, int sampleRate,
                                            const std::string& folder, const std::string& stem, std::vector<bool>* complete,
                                            std::string* error) {
    std::vector<std::string> out;
    if (complete) complete->clear();
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return std::vector<std::string>{};
    };
    if (!(length > 0)) return fail("The loop needs an In and an Out");
    std::string err;
    const AudioBufferPtr sound = decodeAudio(path, sampleRate, &err);
    if (!sound) return fail(err.empty() ? "Cannot read " + path : err);
    const std::vector<LoopPass> passes =
        loopPasses(sound->frames(), std::llround(preroll * sampleRate), length * sampleRate, sampleRate);
    if (std::none_of(passes.begin(), passes.end(), [](const LoopPass& p) { return p.complete; }))
        return fail("The recording ends before a whole pass from In to Out");
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(folder), ec);
    for (size_t k = 0; k < passes.size(); ++k) {
        std::string file;
        for (int n = int(k) + 1;; ++n) {  // (a name not taken)
            file = folder + "/" + stem + std::to_string(n) + ".wav";
            if (!std::filesystem::exists(std::filesystem::u8path(file), ec) || n > int(k) + 1000) break;
        }
        if (!writeFloatWav(file, sound->samples.data() + passes[k].first * sound->channels, passes[k].count, sampleRate, sound->channels))
            return fail("Cannot write " + file);
        out.push_back(file);
        if (complete) complete->push_back(passes[k].complete);
    }
    return out;
}

}  // namespace montage
