// Montage — sound-effect sampler (see Sampler.h).
#include "Sampler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

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

KitPtr loadKit(const SamplerSettings& settings, int sampleRate, std::vector<std::string>* missing, const Loader& load) {
    auto kit = std::make_shared<Kit>();
    kit->sampleRate = sampleRate;
    for (const SamplerSlot& sl : settings.pads) {
        if (sl.key < 0 || sl.key > 127 || kit->byKey[size_t(sl.key)]) continue;
        Kit::Slot slot;
        slot.settings = sl;
        for (const SamplerSound& sd : sl.sounds) {
            std::string why;
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
            if (s.end > s.start) slot.sounds.push_back(std::move(s));
        }
        kit->pads.push_back(std::move(slot));
        kit->byKey[size_t(sl.key)] = int(kit->pads.size());
    }
    return kit;
}

// ---- Engine -----------------------------------------------------------------------------------------------------

Engine::Engine(int sampleRate) : rate_(std::max(1, sampleRate)), chokeLen_(std::max<int64_t>(1, rate_ / 1000)) { voices_.reserve(kMaxVoices); }

void Engine::setKit(KitPtr kit) {
    std::lock_guard<std::mutex> lock(kitMutex_);
    pendingKit_ = std::move(kit);
}

KitPtr Engine::kit() const {
    std::lock_guard<std::mutex> lock(kitMutex_);
    return pendingKit_;
}

void Engine::noteOn(int key, int velocity, int64_t at, uint32_t seed, uint64_t id) {
    if (key < 0 || key > 127) return;
    std::lock_guard<std::mutex> lock(eventMutex_);
    queued_.push_back({true, key, std::clamp(velocity, 1, 127), at, seed, order_++, id});
}

void Engine::noteOff(int key, int64_t at, uint64_t id) {
    if (key < 0 || key > 127) return;
    std::lock_guard<std::mutex> lock(eventMutex_);
    queued_.push_back({false, key, 0, at, 0, order_++, id});
}

void Engine::allNotesOff() {
    std::lock_guard<std::mutex> lock(eventMutex_);
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
    std::vector<int> layer;
    for (size_t i = 0; i < slot->sounds.size(); ++i)
        if (e.velocity >= slot->sounds[i].velLow && e.velocity <= slot->sounds[i].velHigh) layer.push_back(int(i));
    if (layer.empty()) return;
    // Round robin among the sounds answering this velocity.
    const std::pair<int, int> robin{e.key, layer.front()};
    size_t r = 0;
    while (r < robinKeys_.size() && robinKeys_[r] != robin) ++r;
    if (r == robinKeys_.size()) robinKeys_.push_back(robin), robinTurns_.push_back(0);
    const Kit::Sound& sound = slot->sounds[size_t(layer[robinTurns_[r]++ % layer.size()])];
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
    v.gain = float(std::pow(10.0, (st.gainDb + level) / 20.0) * e.velocity / 127.0);
    v.attackLen = std::llround(st.attack * rate_);
    v.releaseLen = std::max<int64_t>(1, std::llround(st.release * rate_));
    v.serial = serial_++;
    if (voices_.size() >= kMaxVoices) {  // (the oldest gives way)
        auto oldest = std::min_element(voices_.begin(), voices_.end(), [](const Voice& a, const Voice& b) { return a.serial < b.serial; });
        voices_.erase(oldest);
    }
    voices_.push_back(std::move(v));
}

void Engine::release(int key, uint64_t id) {
    for (Voice& v : voices_)
        if (v.key == key && (!id || v.id == id) && !v.oneShot && v.released < 0) v.released = 0;
}

void Engine::renderVoices(float* out, int frames) {
    for (Voice& v : voices_) {
        const Kit::Sound& s = *v.sound;
        const float* src = s.buffer->samples.data();
        const int64_t len = s.end - s.start;
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
            const size_t k = size_t(s.start + at) * 2;
            if (frac == 0 || at + 1 >= len) {
                l = src[k], r = src[k + 1];
            } else {
                l = float(src[k] + (src[k + 2] - src[k]) * frac);
                r = float(src[k + 1] + (src[k + 3] - src[k + 1]) * frac);
            }
            const float g = v.gain * env;
            out[size_t(i) * 2] += l * g;
            out[size_t(i) * 2 + 1] += r * g;
            v.pos += v.step;
            ++v.age;
        }
    }
    std::erase_if(voices_, [](const Voice& v) { return v.pos >= double(v.sound->end - v.sound->start); });
}

void Engine::render(int64_t start, int frames, float* out) {
    if (frames <= 0) return;
    if (std::unique_lock<std::mutex> lock(kitMutex_, std::try_to_lock); lock.owns_lock()) kit_ = pendingKit_;
    if (std::unique_lock<std::mutex> lock(eventMutex_, std::try_to_lock); lock.owns_lock()) {
        if (resetPending_) {
            voices_.clear(), due_.clear(), robinKeys_.clear(), robinTurns_.clear();
            resetPending_ = false;
        }
        due_.insert(due_.end(), queued_.begin(), queued_.end());
        queued_.clear();
    }
    // In time order (as played, for notes at the same sample).
    std::stable_sort(due_.begin(), due_.end(), [&](const Event& a, const Event& b) {
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
    FrameTime noteFrame = FrameTime(std::floor(double(std::max<int64_t>(0, earliest)) * fps / s.sampleRate));
    // (the frame's first sample must not come after the note: llround of a frame's sample can land past it)
    while (noteFrame > 0 && std::llround(double(noteFrame) * s.sampleRate / fps) > earliest) --noteFrame;
    const FrameTime first = from >= 0 ? std::min(noteFrame, from) : noteFrame;
    int64_t frames = 0;
    const Id media = passMedia(p, s, kit, notes, seed, first, file, frames, why);
    if (!media) return edit::Result::fail(why);
    edit::Result r = edit::addTakeLane(p, s, track, media, first, name);
    if (!r.ok) return r;
    const Id lane = r.created.front();
    const FrameTime to = first + FrameTime(std::llround(double(frames) * fps / s.sampleRate));
    // The comp takes it from its first note on (what the track had before the first note stays).
    if (to > noteFrame) {
        edit::Result c = edit::compTake(p, s, track, lane, noteFrame, to);
        if (!c.ok) return c;
        r.created.insert(r.created.end(), c.created.begin(), c.created.end());
    }
    trackAt(s, track)->sampler.passes.push_back({lane, seed, std::move(notes)});
    return r;
}

edit::Result revoicePass(Project& p, Sequence& s, TrackRef track, Id lane, const KitPtr& kit, const std::string& file) {
    std::string why;
    Track* t = samplerTrack(s, track, why);
    if (!t) return edit::Result::fail(why);
    if (!kit) return edit::Result::fail("No sounds loaded");
    auto pass = std::find_if(t->sampler.passes.begin(), t->sampler.passes.end(), [&](const SamplerPass& ps) { return ps.lane == lane; });
    auto ln = std::find_if(t->lanes.begin(), t->lanes.end(), [&](const TakeLane& l) { return l.id == lane; });
    if (pass == t->sampler.passes.end() || ln == t->lanes.end()) return edit::Result::fail("That lane is not a sampler pass");
    int64_t frames = 0;
    const SamplerPass ps = *pass;
    const FrameTime from = ln->start;
    const Id old = ln->mediaId;
    const Id media = passMedia(p, s, kit, ps.notes, ps.seed, from, file, frames, why);
    if (!media) return edit::Result::fail(why);
    t = trackAt(s, track);  // (the project's media grew, not the track; looked up again all the same)
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
