#include "render/MonitorSection.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

#include "core/Surround.h"

namespace montage {

namespace {

float dbGain(double db) { return float(std::pow(10.0, db / 20)); }

const char* soloName(MonitorSolo m) {
    switch (m) {
        case MonitorSolo::InPlace: return "in_place";
        case MonitorSolo::AfterFader: return "afl";
        case MonitorSolo::PreFader: return "pfl";
        default: return "project";
    }
}

MonitorSolo soloFrom(const QString& s) {
    if (s == "in_place") return MonitorSolo::InPlace;
    if (s == "afl") return MonitorSolo::AfterFader;
    if (s == "pfl") return MonitorSolo::PreFader;
    return MonitorSolo::Project;
}

QJsonArray ints(const std::vector<int>& v) {
    QJsonArray a;
    for (int i : v) a.append(i);
    return a;
}

std::vector<int> intsFrom(const QJsonValue& v) {
    std::vector<int> out;
    for (const QJsonValue& x : v.toArray()) out.push_back(x.toInt());
    return out;
}

int speakerIndex(const std::vector<Speaker>& sp, const char* name) {
    for (size_t i = 0; i < sp.size(); ++i)
        if (std::strcmp(sp[i].name, name) == 0) return int(i);
    return -1;
}

bool speakerLayout(const std::string& layout) {
    return layout != "binaural" && !ambisonicLayout(layout) && layoutChannels(layout) > 0;
}

}  // namespace

QJsonObject MonitorSettings::toJson() const {
    QJsonArray m;
    for (const MonitorSpeaker& s : map) m.append(QJsonObject{{"output", s.output}, {"trim_db", s.trimDb}, {"delay", s.delay}});
    QJsonObject c{{"on", cue.on},           {"output", cue.output},   {"level_db", cue.levelDb},      {"tracks", ints(cue.tracks)},
                  {"beeps", cue.beeps},     {"program", cue.program}, {"program_db", cue.programDb}, {"talkback_db", cue.talkbackDb}};
    return QJsonObject{{"on", on},
                       {"speakers", QString::fromStdString(speakers)},
                       {"map", m},
                       {"bass_management", bassManagement},
                       {"crossover_hz", crossoverHz},
                       {"listen_as", QString::fromStdString(listenAs)},
                       {"dim", dim},
                       {"dim_db", dimDb},
                       {"mono", mono},
                       {"mute", mute},
                       {"solo_speaker", soloSpeaker},
                       {"level_db", levelDb},
                       {"reference_spl", referenceSpl},
                       {"solo_mode", soloName(soloMode)},
                       {"solo_tracks", ints(soloTracks)},
                       {"cue", c}};
}

MonitorSettings MonitorSettings::fromJson(const QJsonObject& o) {
    MonitorSettings s;
    s.on = o.value("on").toBool(s.on);
    s.speakers = o.value("speakers").toString(QString::fromStdString(s.speakers)).toStdString();
    if (s.speakers != "headphones" && !speakerLayout(s.speakers)) s.speakers = "stereo";
    for (const QJsonValue& v : o.value("map").toArray()) {
        const QJsonObject x = v.toObject();
        MonitorSpeaker sp;
        sp.output = std::clamp(x.value("output").toInt(-1), -1, kMaxMonitorChannels - 1);
        sp.trimDb = std::clamp(std::round(x.value("trim_db").toDouble() * 10) / 10, -40.0, 20.0);
        sp.delay = std::clamp(x.value("delay").toInt(), 0, kMaxMonitorDelay);
        s.map.push_back(sp);
    }
    s.bassManagement = o.value("bass_management").toBool();
    s.crossoverHz = std::clamp(o.value("crossover_hz").toDouble(s.crossoverHz), 40.0, 200.0);
    s.listenAs = o.value("listen_as").toString().toStdString();
    s.dim = o.value("dim").toBool();
    s.dimDb = std::clamp(o.value("dim_db").toDouble(s.dimDb), -60.0, 0.0);
    s.mono = o.value("mono").toBool();
    s.mute = o.value("mute").toBool();
    s.soloSpeaker = o.value("solo_speaker").toInt(-1);
    s.levelDb = std::clamp(o.value("level_db").toDouble(), -60.0, 20.0);
    s.referenceSpl = std::clamp(std::round(o.value("reference_spl").toDouble(s.referenceSpl)), 60.0, 110.0);
    s.soloMode = soloFrom(o.value("solo_mode").toString());
    s.soloTracks = intsFrom(o.value("solo_tracks"));
    const QJsonObject c = o.value("cue").toObject();
    s.cue.on = c.value("on").toBool();
    s.cue.output = std::clamp(c.value("output").toInt(s.cue.output), 0, kMaxMonitorChannels - 2);
    s.cue.levelDb = std::clamp(c.value("level_db").toDouble(), -60.0, 20.0);
    s.cue.tracks = intsFrom(c.value("tracks"));
    s.cue.beeps = c.value("beeps").toBool(true);
    s.cue.program = c.value("program").toBool();
    s.cue.programDb = std::clamp(c.value("program_db").toDouble(s.cue.programDb), -60.0, 20.0);
    s.cue.talkbackDb = std::clamp(c.value("talkback_db").toDouble(), -60.0, 20.0);
    return s;
}

const std::vector<Speaker>& monitorSpeakers(const std::string& set) {
    return layoutSpeakers(set == "headphones" ? std::string("stereo") : set);
}

MonitorSpeaker monitorSpeaker(const MonitorSettings& s, int speaker) {
    MonitorSpeaker d;
    d.output = speaker;
    if (speaker >= 0 && speaker < int(s.map.size())) d = s.map[size_t(speaker)];
    d.output = std::clamp(d.output, -1, kMaxMonitorChannels - 1);
    return d;
}

int monitorFreeOutput(const MonitorSettings& s) {
    int n = 2;
    const int speakers = int(monitorSpeakers(s.speakers).size());
    for (int k = 0; k < speakers; ++k) n = std::max(n, monitorSpeaker(s, k).output + 1);
    return std::min(n, kMaxMonitorChannels - 2);
}

int monitorOutputs(const MonitorSettings& s) {
    int n = 2;
    const int speakers = int(monitorSpeakers(s.speakers).size());
    for (int k = 0; k < speakers; ++k) n = std::max(n, monitorSpeaker(s, k).output + 1);
    if (s.cue.on) n = std::max(n, std::clamp(s.cue.output, 0, kMaxMonitorChannels - 2) + 2);
    return n;
}

namespace {

// Every speaker of `layout` is one of the set's.
bool layoutFits(const std::string& layout, const std::vector<Speaker>& set) {
    for (const Speaker& sp : layoutSpeakers(layout))
        if (speakerIndex(set, sp.name) < 0) return false;
    return true;
}

}  // namespace

std::vector<std::string> listenChoices(const std::string& set) {
    if (set == "headphones") return {"stereo", "binaural"};
    std::vector<std::string> out;
    const int n = layoutChannels(set);
    for (const std::string& l : {std::string("7.1"), std::string("5.1")})
        if (layoutChannels(l) < n && layoutFits(l, monitorSpeakers(set))) out.push_back(l);
    if (n > 2) out.push_back("stereo");
    return out;
}

std::string listenLayout(const MonitorSettings& s, const Sequence& seq) {
    if (s.speakers == "headphones") return s.listenAs == "binaural" ? "binaural" : "stereo";
    std::string want = s.listenAs.empty() || s.listenAs == "binaural" ? s.speakers : s.listenAs;
    const auto& set = monitorSpeakers(s.speakers);
    if (want != "stereo" && (!speakerLayout(want) || !layoutFits(want, set))) want = s.speakers;
    // The programme as it is when the speakers have room for it (no up-mixing).
    if (speakerLayout(seq.audioLayout) && layoutChannels(seq.audioLayout) <= layoutChannels(want) && layoutFits(seq.audioLayout, set))
        return seq.audioLayout;
    return want;
}

Sequence monitorSequence(const Sequence& seq, const MonitorSettings& s) {
    Sequence out = seq;
    const std::string layout = listenLayout(s, seq);
    if (layout == "binaural") out.audioLayout = ambisonicLayout(seq.audioLayout) ? seq.audioLayout : "ambix3";
    else if (layout != "stereo") out.audioLayout = layout;
    if (s.soloMode == MonitorSolo::Project || s.soloTracks.empty()) return out;
    for (Track& t : out.audioTracks) t.solo = false;
    for (int i : s.soloTracks) {
        if (i < 0 || i >= int(out.audioTracks.size())) continue;
        Track& t = out.audioTracks[size_t(i)];
        t.solo = true;
        if (s.soloMode == MonitorSolo::InPlace) continue;
        // After the fader: the track straight to the master, not through buses, sends or the master's processing.
        t.output = 0;
        t.sends.clear();
        if (s.soloMode == MonitorSolo::PreFader) {  // and before its fader, pan and folder
            t.volumeDb = 0;
            t.pan = 0;
            t.surround = SurroundPan{};
            t.volumeAuto = Param{};
            t.panAuto = Param{};
            t.surroundXAuto = t.surroundYAuto = t.surroundZAuto = Param{};
            t.folder.clear();
        }
    }
    if (s.soloMode != MonitorSolo::InPlace) {
        out.masterEffects.clear();
        out.masterVolumeDb = 0;
    }
    return out;
}

std::vector<float> calibrationNoise(int sampleRate, double lowHz, double highHz) {
    const size_t n = size_t(std::max(1, sampleRate)) * 10;
    // Paul Kellet's pink filter on white noise, then the band's edges, run twice round so the loop has no seam.
    std::mt19937 rng(20);
    std::uniform_real_distribution<float> white(-1, 1);
    std::vector<float> w(n);
    for (float& v : w) v = white(rng);
    fx::Biquad hp[2], lp[2];
    for (auto& b : hp) b.highPass(sampleRate, lowHz);
    for (auto& b : lp) b.lowPass(sampleRate, highHz);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    std::vector<float> out(n);
    for (int pass = 0; pass < 2; ++pass)
        for (size_t i = 0; i < n; ++i) {
            const double x = w[i];
            b0 = 0.99886 * b0 + x * 0.0555179;
            b1 = 0.99332 * b1 + x * 0.0750759;
            b2 = 0.96900 * b2 + x * 0.1538520;
            b3 = 0.86650 * b3 + x * 0.3104856;
            b4 = 0.55000 * b4 + x * 0.5329522;
            b5 = -0.7616 * b5 - x * 0.0168980;
            const double pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + x * 0.5362;
            b6 = x * 0.115926;
            float y = float(pink);
            for (auto& b : hp) y = b.process(0, y);
            for (auto& b : lp) y = b.process(0, y);
            out[i] = y;
        }
    double sum = 0;
    for (float v : out) sum += double(v) * v;
    const double rms = std::sqrt(sum / double(n));
    const float g = rms > 0 ? float(std::pow(10.0, -20.0 / 20) / rms) : 0;
    for (float& v : out) v *= g;
    return out;
}

void MonitorSection::configure(const MonitorSettings& s, int sampleRate) {
    const bool fresh = s.speakers != s_.speakers || sampleRate != rate_ || s.crossoverHz != s_.crossoverHz || lines_.empty();
    s_ = s;
    rate_ = sampleRate;
    speakers_ = monitorSpeakers(s.speakers);
    outputs_ = monitorOutputs(s);
    if (fresh) {
        lines_.assign(speakers_.size(), Line{});
        for (Line& l : lines_) {
            l.buf.assign(kMaxMonitorDelay + 1, 0.0f);
            for (auto& b : l.hp) b.highPass(rate_, s_.crossoverHz);
            for (auto& b : l.lp) b.lowPass(rate_, s_.crossoverHz);
        }
    }
    if (fresh && calibrate_ >= 0) {
        const int k = calibrate_;
        calibrate_ = -1;  // (made afresh for the new set, rate or crossover)
        setCalibration(k);
    }
}

void MonitorSection::reset() {
    for (Line& l : lines_) {
        std::fill(l.buf.begin(), l.buf.end(), 0.0f);
        l.pos = 0;
        for (auto* bank : {l.hp, l.lp})
            for (int k = 0; k < 2; ++k) bank[k].z1[0] = bank[k].z2[0] = 0;
    }
    noisePos_ = 0;
}

std::vector<float> calibrationNoiseFor(const MonitorSettings& s, int sampleRate, int speaker) {
    const auto& sp = monitorSpeakers(s.speakers);
    if (speaker < 0 || speaker >= int(sp.size())) return {};
    return sp[size_t(speaker)].lfe ? calibrationNoise(sampleRate, 20, s.crossoverHz) : calibrationNoise(sampleRate);
}

void MonitorSection::setCalibration(int speaker) {
    const int k = speaker >= 0 && speaker < int(speakers_.size()) ? speaker : -1;
    if (k == calibrate_ && (k < 0 || !noise_.empty())) return;
    setCalibration(k, calibrationNoiseFor(s_, rate_, k));
}

void MonitorSection::setCalibration(int speaker, std::vector<float> noise) {
    const int k = speaker >= 0 && speaker < int(speakers_.size()) && !noise.empty() ? speaker : -1;
    if (k == calibrate_ && (k < 0 || !noise_.empty())) return;
    calibrate_ = k;
    noise_ = k >= 0 ? std::move(noise) : std::vector<float>{};
    noisePos_ = 0;
}

void MonitorSection::setTalkback(bool on) { talkback_ = on; }

void MonitorSection::feedTalkback(const float* mono, int frames) {
    std::lock_guard lock(talkM_);
    const size_t cap = size_t(std::max(1, rate_));  // a second
    if (talk_.size() != cap) talk_.assign(cap, 0.0f), talkRead_ = talkWrite_ = 0;
    for (int i = 0; i < frames; ++i) {
        talk_[talkWrite_ % cap] = mono[i];
        ++talkWrite_;
    }
    if (talkWrite_ - talkRead_ > cap) talkRead_ = talkWrite_ - cap;  // (fallen behind: the newest second)
}

void MonitorSection::process(const std::string& layout, const float* in, const float* beeps, int frames, float* out) {
    const int nsp = int(speakers_.size());
    std::fill(out, out + size_t(frames) * size_t(outputs_), 0.0f);
    placed_.assign(size_t(frames) * size_t(nsp), 0.0f);
    const int left = speakerIndex(speakers_, "L"), right = speakerIndex(speakers_, "R");
    const bool calibrating = calibrate_ >= 0;
    if (calibrating) {
        for (int i = 0; i < frames; ++i) {
            placed_[size_t(i) * size_t(nsp) + size_t(calibrate_)] = noise_[noisePos_];
            noisePos_ = (noisePos_ + 1) % noise_.size();
        }
    } else {
        // The programme on the speakers by name (stereo, binaural and mono on the front pair).
        const bool pair = layout == "stereo" || layout == "binaural" || layout == "mono";
        const auto& from = pair ? layoutSpeakers("stereo") : layoutSpeakers(layout);
        const int nin = pair ? 2 : int(from.size());
        std::vector<int> to(static_cast<size_t>(nin));
        for (int c = 0; c < nin; ++c) to[size_t(c)] = speakerIndex(speakers_, from[size_t(c)].name);
        for (int i = 0; i < frames; ++i) {
            const float* x = in + size_t(i) * size_t(nin);
            float* y = placed_.data() + size_t(i) * size_t(nsp);
            if (s_.mono) {  // both front speakers play (L + R) / 2, so a sound in the middle keeps its level
                float l = 0, r = 0;
                if (pair) l = x[0], r = x[1];
                else
                    for (int c = 0; c < nin; ++c) {
                        const std::string n = from[size_t(c)].name;
                        if (from[size_t(c)].lfe) continue;
                        const float w = n == "L" || n == "R" ? 1.0f : 0.70710678f;
                        if (n == "C") l += w * x[c], r += w * x[c];
                        else if (n[0] == 'L') l += w * x[c];
                        else r += w * x[c];
                    }
                const float m = 0.5f * (l + r);
                if (left >= 0) y[left] = m;
                if (right >= 0) y[right] = m;
            } else {
                for (int c = 0; c < nin; ++c)
                    if (to[size_t(c)] >= 0) y[to[size_t(c)]] += x[c];
            }
            if (beeps) {
                if (left >= 0) y[left] += beeps[size_t(i) * 2];
                if (right >= 0) y[right] += beeps[size_t(i) * 2 + 1];
            }
        }
    }
    // Bass management: each main's lows and the LFE (+10 dB) to the subwoofer, the mains high passed.
    int sub = -1;
    for (int k = 0; k < nsp; ++k)
        if (speakers_[size_t(k)].lfe) sub = k;
    if (s_.bassManagement && sub >= 0 && !calibrating) {
        for (int i = 0; i < frames; ++i) {
            float* y = placed_.data() + size_t(i) * size_t(nsp);
            float lows = y[sub] * 3.16227766f;
            for (int k = 0; k < nsp; ++k) {
                if (k == sub) continue;
                Line& l = lines_[size_t(k)];
                float lo = y[k], hi = y[k];
                for (auto& b : l.lp) lo = b.process(0, lo);
                for (auto& b : l.hp) hi = b.process(0, hi);
                lows += lo;
                y[k] = hi;
            }
            y[sub] = lows;
        }
    }
    // Each speaker: one alone, trim, delay, level, Dim, Mute; then onto its device channel.
    const float level = dbGain(s_.levelDb) * (!calibrating && s_.dim ? dbGain(s_.dimDb) : 1.0f);
    for (int k = 0; k < nsp; ++k) {
        const MonitorSpeaker m = monitorSpeaker(s_, k);
        bool heard = m.output >= 0 && m.output < outputs_;
        if (!calibrating) heard &= !s_.mute && (s_.soloSpeaker < 0 || s_.soloSpeaker == k);
        const float g = heard ? level * dbGain(std::round(m.trimDb * 10) / 10) : 0.0f;
        const int delay = std::clamp(m.delay, 0, kMaxMonitorDelay);
        Line& l = lines_[size_t(k)];
        const int len = int(l.buf.size());
        for (int i = 0; i < frames; ++i) {
            l.buf[size_t(l.pos)] = placed_[size_t(i) * size_t(nsp) + size_t(k)];
            const float v = l.buf[size_t((l.pos - delay + len) % len)];
            l.pos = (l.pos + 1) % len;
            if (heard) out[size_t(i) * size_t(outputs_) + size_t(m.output)] += g * v;
        }
    }
}

void MonitorSection::addCue(const float* guide, const float* beeps, const float* heard, int frames, float* out) {
    if (!s_.cue.on || s_.cue.output + 1 >= outputs_) return;
    const float g = dbGain(s_.cue.levelDb), pg = dbGain(s_.cue.programDb), tg = dbGain(s_.cue.talkbackDb);
    std::unique_lock lock(talkM_);
    const size_t cap = talk_.size();
    for (int i = 0; i < frames; ++i) {
        float l = 0, r = 0;
        if (guide) l += guide[size_t(i) * 2], r += guide[size_t(i) * 2 + 1];
        if (beeps && s_.cue.beeps) l += beeps[size_t(i) * 2], r += beeps[size_t(i) * 2 + 1];
        if (heard && s_.cue.program) l += pg * heard[size_t(i) * 2], r += pg * heard[size_t(i) * 2 + 1];
        if (cap && talkRead_ < talkWrite_) {
            const float t = talk_[talkRead_ % cap];
            ++talkRead_;
            if (talkback_) l += tg * t, r += tg * t;
        }
        out[size_t(i) * size_t(outputs_) + size_t(s_.cue.output)] += g * l;
        out[size_t(i) * size_t(outputs_) + size_t(s_.cue.output) + 1] += g * r;
    }
}

Monitor::Monitor() = default;

void Monitor::setNonBlocking(bool on) {
    program_.setNonBlocking(on);
    cue_.setNonBlocking(on);
}

void Monitor::setOfflineProxies(bool on) {
    program_.setOfflineProxies(on);
    cue_.setOfflineProxies(on);
}

void Monitor::setAmbisonicBinaural(bool on) {
    binaural_ = on;
    program_.setAmbisonicBinaural(on || layout_ == "binaural");
}

void Monitor::setSettings(const MonitorSettings& s, int sampleRate) {
    const MonitorSettings before = section_.settings();
    section_.configure(s, sampleRate);
    if (project_ && (before.listenAs != s.listenAs || before.speakers != s.speakers || before.soloMode != s.soloMode ||
                     before.soloTracks != s.soloTracks || before.cue.tracks != s.cue.tracks))
        prepare(project_, seq_ ? seq_->id : 0);
}

MonitorPlan planMonitor(std::shared_ptr<const Project> project, Id sequence, const MonitorSettings& m) {
    MonitorPlan plan;
    plan.project = std::move(project);
    plan.sequence = sequence;
    plan.settings = m;
    const Sequence* s = plan.project ? plan.project->findSequence(sequence) : nullptr;
    if (!s) return plan;
    plan.layout = listenLayout(m, *s);
    plan.seq = monitorSequence(*s, m);
    // The cue's tracks as they are mixed (their faders and the master's processing), the rest left out.
    plan.cueSeq = *s;
    plan.cueMask.assign(s->audioTracks.size(), false);
    for (int i : m.cue.tracks)
        if (i >= 0 && i < int(plan.cueMask.size())) plan.cueMask[size_t(i)] = true;
    for (Track& t : plan.cueSeq->audioTracks) t.solo = false;
    return plan;
}

void Monitor::prepare(std::shared_ptr<const Project> project, Id sequence) { prepare(planMonitor(std::move(project), sequence, section_.settings())); }

void Monitor::prepare(MonitorPlan plan) {
    project_ = std::move(plan.project);
    if (!plan.seq) {
        seq_.reset(), cueSeq_.reset();
        return;
    }
    layout_ = std::move(plan.layout);
    seq_ = std::move(plan.seq);
    program_.setAmbisonicBinaural(binaural_ || layout_ == "binaural");
    cueSeq_ = std::move(plan.cueSeq);
    cue_.setTrackMask(plan.cueMask);
}

void Monitor::reset() {
    program_.reset();
    cue_.reset();
    section_.reset();
}

void Monitor::render(int64_t start, int frames, float* out, float* heard, std::vector<MeterLevels>* tracks, const AdrCycle* cycle,
                     bool program) {
    if (frames <= 0) return;
    const bool pair = !seq_ || layout_ == "stereo" || layout_ == "binaural";
    const int nin = pair ? 2 : layoutChannels(layout_);
    full_.resize(size_t(frames) * size_t(nin));
    stereo_.resize(size_t(frames) * 2);
    // Stopped, or calibrating: no programme (the mixers start afresh when it comes back).
    if (!project_ || !seq_ || !program || section_.calibration() >= 0) {
        std::fill(full_.begin(), full_.end(), 0.0f);
        std::fill(stereo_.begin(), stereo_.end(), 0.0f);
        section_.process(pair ? "stereo" : layout_, full_.data(), nullptr, frames, out);
        if (section_.settings().cue.on) section_.addCue(nullptr, nullptr, nullptr, frames, out);
        if (heard) std::copy(stereo_.begin(), stereo_.end(), heard);
        return;
    }
    if (pair) {
        program_.mix(*project_, *seq_, start, frames, full_.data(), tracks);
        std::copy(full_.begin(), full_.end(), stereo_.begin());
    } else {
        program_.mixLayout(*project_, *seq_, start, frames, full_.data(), tracks);
        downmixToStereo(layout_, full_.data(), frames, stereo_.data());
    }
    const float* beeps = nullptr;
    if (cycle) {
        beeps_.assign(size_t(frames) * 2, 0.0f);
        addAdrBeeps(*cycle, seq_->fpsValue(), seq_->sampleRate, start, beeps_.data(), frames, 2);
        beeps = beeps_.data();
    }
    section_.process(layout_, full_.data(), beeps, frames, out);
    if (section_.settings().cue.on) {
        const float* guide = nullptr;
        if (!section_.settings().cue.tracks.empty()) {
            guide_.resize(size_t(frames) * 2);
            cue_.mix(*project_, *cueSeq_, start, frames, guide_.data());
            guide = guide_.data();
        }
        section_.addCue(guide, beeps, stereo_.data(), frames, out);
    }
    if (heard) std::copy(stereo_.begin(), stereo_.end(), heard);
}

}  // namespace montage
