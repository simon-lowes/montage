#include "media/LtcTimecode.h"

#include <algorithm>
#include <cmath>
#include <fstream>

#include "audio/Ltc.h"
#include "core/AudioChannels.h"
#include "core/EditOps.h"
#include "media/Decoder.h"

namespace montage {

namespace {

int baseOf(Rational fps) {
    const double r = fps.toDouble();
    return r < 24.5 ? 24 : r < 27.5 ? 25 : 30;
}

}  // namespace

std::string LtcReading::startLabel() const {
    if (start < 0 || !fps.valid()) return {};
    return ltcLabel(ltcFrameAt(std::llround(start * fps.toDouble()), baseOf(fps), dropFrame));
}

bool findLtc(const std::vector<float>& samples, int sampleRate, LtcReading& out) {
    LtcDecoder decoder(sampleRate);
    decoder.write(samples.data(), samples.size());
    const std::vector<LtcFrame> words = decoder.take();
    if (words.size() < 8) return false;
    // The rate's whole frames a second: from how long a word lasts, raised if a higher frame number is seen.
    std::vector<double> lengths;
    for (const LtcFrame& w : words) lengths.push_back(w.end - w.start);
    std::nth_element(lengths.begin(), lengths.begin() + std::ptrdiff_t(lengths.size() / 2), lengths.end());
    const double perWord = lengths[lengths.size() / 2];
    const double wordsPerSecond = sampleRate / perWord;
    int base = wordsPerSecond < 24.5 ? 24 : wordsPerSecond < 27.5 ? 25 : 30;
    int highest = 0, drop = 0;
    for (const LtcFrame& w : words) highest = std::max(highest, w.frames), drop += w.dropFrame;
    if (highest >= base) base = highest >= 25 ? 30 : 25;
    const bool dropFrame = base == 30 && drop * 2 > int(words.size());
    const int64_t day = ltcFramesPerDay(base, dropFrame);
    // Runs of words that follow on from each other: as many frames on as word lengths later (a word lost here and
    // there allowed). The longest is the timecode.
    struct Run {
        std::vector<size_t> words;
        std::vector<int64_t> numbers;  // carried on past midnight
    };
    std::vector<Run> runs;
    for (size_t i = 0; i < words.size(); ++i) {
        LtcFrame w = words[i];
        w.dropFrame = dropFrame;
        const int64_t n = ltcFrameNumber(w, base);
        bool placed = false;
        for (Run& r : runs) {
            const LtcFrame& last = words[r.words.back()];
            const int64_t step = ((n - r.numbers.back() % day) % day + day) % day;
            const double later = (w.start - last.start) / perWord;
            if (step >= 1 && std::fabs(double(step) - later) < 0.3) {
                r.words.push_back(i);
                r.numbers.push_back(r.numbers.back() + step);
                placed = true;
                break;
            }
        }
        if (!placed) runs.push_back({{i}, {n}});
    }
    const Run& best = *std::max_element(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.words.size() < b.words.size(); });
    if (best.words.size() < 8) return false;
    // Frame number against time, fitted: the frame number at the first sample, and the rate the words came at.
    double mx = 0, my = 0;
    const double k = double(best.words.size());
    for (size_t j = 0; j < best.words.size(); ++j) mx += words[best.words[j]].start, my += double(best.numbers[j] - best.numbers[0]);
    mx /= k, my /= k;
    double sxx = 0, sxy = 0;
    for (size_t j = 0; j < best.words.size(); ++j) {
        const double dx = words[best.words[j]].start - mx;
        sxx += dx * dx;
        sxy += dx * (double(best.numbers[j] - best.numbers[0]) - my);
    }
    const double slope = sxx > 0 ? sxy / sxx : 1 / perWord;  // frames a sample
    const double atZero = double(best.numbers[0]) + my - slope * mx;
    const double measured = slope * sampleRate;
    const double span = (words[best.words.back()].start - words[best.words.front()].start) / sampleRate;
    Rational fps{base, 1};
    if (base != 25) {
        const Rational pulled{base * 1000, 1001};
        // Drop frame is only ever at 29.97; otherwise the rate tells a pulled-down timecode from a whole one.
        if (dropFrame || (span >= 1.5 && std::fabs(measured - pulled.toDouble()) < std::fabs(measured - base))) fps = pulled;
    }
    LtcReading r;
    r.fps = fps;
    r.dropFrame = dropFrame;
    const double daySeconds = double(day) / fps.toDouble();
    r.start = std::fmod(atZero / fps.toDouble(), daySeconds);
    if (r.start < 0) r.start += daySeconds;
    const LtcFrame& first = words[best.words.front()];
    r.label = ltcLabel(first);
    r.at = first.start / sampleRate;
    r.words = int(best.words.size());
    r.speed = span >= 1.5 ? measured / fps.toDouble() : 1;
    r.userBits = first.userBits;
    out = r;
    return true;
}

bool readLtc(const std::string& path, LtcReading& out, std::string* error, double seconds, const std::atomic<bool>* cancel) {
    constexpr int kRate = 48000;
    std::vector<std::vector<float>> channels;
    if (!decodeSourceChannels(path, kRate, seconds, channels, error, cancel)) return false;
    if (channels.empty()) {
        if (error) *error = "The file has no sound";
        return false;
    }
    LtcReading best;
    for (size_t c = 0; c < channels.size(); ++c) {
        if (cancel && cancel->load()) return false;
        LtcReading r;
        if (findLtc(channels[c], kRate, r) && r.words > best.words) {
            best = r;
            best.channel = int(c);
        }
    }
    if (best.channel < 0) {
        if (error) *error = "No channel carries LTC in its first " + std::to_string(int(std::lround(seconds))) + " s";
        return false;
    }
    out = best;
    return true;
}

bool usesLtcTimecode(const MediaItem& m) { return m.ltcTimecode >= 0 && m.timecode == m.ltcTimecode; }

void useLtcTimecode(Project& p, Id media, const LtcReading& r) {
    MediaItem* m = p.findMedia(media);
    if (m && m->subclipOf) m = p.findMedia(m->subclipOf);
    if (!m || r.start < 0) return;
    const std::string rate = std::to_string(r.fps.num) + "/" + std::to_string(r.fps.den) + (r.dropFrame ? " DF" : "");
    std::vector<Id> ids{m->id};
    for (MediaItem& x : p.media) {
        if (x.id != m->id && x.subclipOf != m->id) continue;
        const double in = x.subclipOf ? x.subclipIn : 0;
        if (!usesLtcTimecode(x)) x.fileTimecode = x.timecode;
        x.ltcTimecode = r.start + in;
        x.timecode = x.ltcTimecode;
        x.ltcChannel = r.channel;
        x.metadata["timecode_rate"] = rate;
        if (x.subclipOf) ids.push_back(x.id);
    }
    // Clips stop playing the LTC channel, where they did.
    const std::vector<int> mixed = mixedChannels(*m);
    if (sourceChannelCount(*m) < 2 || r.channel < 0) return;
    for (Sequence& s : p.sequences)
        for (Track& t : s.audioTracks)
            for (Clip& c : t.clips) {
                if (std::find(ids.begin(), ids.end(), c.mediaId) == ids.end()) continue;
                std::vector<int> playing = c.channels.empty() ? mixed : c.channels;
                if (std::find(playing.begin(), playing.end(), r.channel) == playing.end()) continue;
                std::erase(playing, r.channel);
                if (playing.empty()) c.enabled = false;
                else c.channels = playing;
            }
}

bool useFileTimecode(Project& p, Id media) {
    MediaItem* m = p.findMedia(media);
    if (m && m->subclipOf) m = p.findMedia(m->subclipOf);
    if (!m || !usesLtcTimecode(*m)) return false;
    for (MediaItem& x : p.media)
        if ((x.id == m->id || x.subclipOf == m->id) && usesLtcTimecode(x)) {
            x.timecode = x.fileTimecode;
            x.fileTimecode = -1;
        }
    return true;
}

bool writeLtcWav(const std::string& path, int64_t startFrame, int64_t frames, Rational fps, bool dropFrame, int sampleRate,
                 std::string* error) {
    const std::vector<float> ltc = encodeLtc(startFrame, frames, fps, dropFrame, sampleRate, 0.25f);
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };  // little-endian hosts only
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = uint32_t(ltc.size() * 2);
    f.write("RIFF", 4);
    u32(36 + bytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);  // PCM
    u16(1);  // mono
    u32(uint32_t(sampleRate));
    u32(uint32_t(sampleRate) * 2);
    u16(2);
    u16(16);
    f.write("data", 4);
    u32(bytes);
    std::vector<int16_t> pcm(ltc.size());
    for (size_t i = 0; i < ltc.size(); ++i) pcm[i] = int16_t(std::lround(std::clamp(ltc[i], -1.0f, 1.0f) * 32767));
    f.write(reinterpret_cast<const char*>(pcm.data()), std::streamsize(pcm.size() * 2));
    if (!f) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    return true;
}

bool ltcRateFor(Rational fps, Rational& ltc, int& framesPerWord) {
    if (!fps.valid()) return false;
    for (int per : {1, 2}) {
        const double r = fps.toDouble() / per;
        for (const Rational c : {Rational{24000, 1001}, Rational{24, 1}, Rational{25, 1}, Rational{30000, 1001}, Rational{30, 1}})
            if (std::fabs(r - c.toDouble()) < 1e-3) {
                ltc = c;
                framesPerWord = per;
                return true;
            }
    }
    return false;
}

bool writeSequenceLtc(const std::string& path, Rational fps, FrameTime from, FrameTime to, int sampleRate, std::string* error) {
    Rational ltc;
    int per = 1;
    if (!ltcRateFor(fps, ltc, per)) {
        if (error) *error = "LTC runs at 23.976, 24, 25, 29.97 or 30 frames a second (or twice that, a word every two frames)";
        return false;
    }
    if (to <= from) {
        if (error) *error = "Nothing to write";
        return false;
    }
    const int64_t first = from / per, last = (to + per - 1) / per;
    return writeLtcWav(path, first, last - first, ltc, ltc.den != 1 && baseOf(ltc) == 30, sampleRate, error);
}

Id placeLtcTrack(Project& p, Sequence& s, Id media, FrameTime at) {
    const TrackRef track = edit::addTrack(p, s, TrackKind::Audio);
    Track* t = trackAt(s, track);
    if (!t) return 0;
    t->name = "LTC";
    t->muted = true;
    if (!edit::placeMedia(p, s, media, at, 0, -1, {TrackKind::Video, 0}, track, false, false, true).ok) return 0;
    for (Clip& c : trackAt(s, track)->clips)
        if (c.mediaId == media && c.start == at) {
            c.role = "Timecode";
            return c.id;
        }
    return 0;
}

}  // namespace montage
