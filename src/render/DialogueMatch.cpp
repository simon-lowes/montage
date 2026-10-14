#include "DialogueMatch.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "AutoMix.h"
#include "core/EditOps.h"
#include "core/Effects.h"

namespace montage {

namespace {

// The decay time the late reverberation is judged at: the reference's broadband one, else its octaves' around speech.
double judgingTime(const RoomEstimate& e) {
    if (e.broadband > 0) return e.broadband;
    const RoomShape s = e.shape();
    return (s.rt60[2] + s.rt60[3] + s.rt60[4]) / 3;
}

std::vector<float> mixed(const std::vector<float>& dry, const std::vector<float>& wet, double wetDb) {
    const float g = float(std::pow(10.0, wetDb / 20));
    std::vector<float> out(dry.size());
    for (size_t i = 0; i < dry.size(); ++i) out[i] = dry[i] + (i < wet.size() ? wet[i] * g : 0.0f);
    return out;
}

double loudness(const std::vector<float>& mono, int rate) { return classifyAudio(mono, rate).loudness; }

}  // namespace

DialogueMatchPlan planDialogueMatch(const std::vector<float>& reference, const std::vector<float>& target, int rate,
                                    const DialogueMatchOptions& o) {
    DialogueMatchPlan plan;
    std::vector<float> now = target;
    // The room: as much of the reference's room as leaves the same late reverberation in the pauses.
    if (o.room) {
        plan.measured = estimateRoom(reference, rate);
        if (plan.measured.broadband > 0 || plan.measured.bandsFound > 0) {
            plan.room = plan.measured.shape();
            const double rt = judgingTime(plan.measured);
            plan.referenceLateDb = lateReverbDb(reference, rate, rt);
            plan.targetLateDb = lateReverbDb(target, rate, rt);
            const std::vector<float> wet = convolve(target, roomImpulse(rate, plan.room, 1));
            auto late = [&](double db) { return lateReverbDb(mixed(target, wet, db), rate, rt); };
            double lo = -40, hi = 6;
            if (plan.referenceLateDb > plan.targetLateDb + 0.5 && late(lo) < plan.referenceLateDb) {
                if (late(hi) <= plan.referenceLateDb) {
                    lo = hi;
                } else {
                    for (int i = 0; i < 14; ++i) {
                        const double mid = (lo + hi) / 2;
                        (late(mid) < plan.referenceLateDb ? lo : hi) = mid;
                    }
                }
                plan.roomAdded = true;
                plan.wetDb = (lo + hi) / 2;
                now = mixed(target, wet, plan.wetDb);
                plan.matchedLateDb = late(plan.wetDb);
            } else {
                plan.matchedLateDb = plan.targetLateDb;
            }
        }
    }
    // The tone of the room's sound, fitted to the reference's.
    if (o.tone) {
        const std::vector<double> refSpectrum = speechSpectrum(reference, rate);
        plan.eq = fitVoiceEq(speechSpectrum(now, rate), refSpectrum, rate);
        if (o.toneOverTime && now.size() > size_t(rate) * 3 && reference.size() >= size_t(rate) * 2) {
            // Two seconds every half second, against the same moment of the reference (in proportion, as a line said
            // again takes its own time), then each gain smoothed with its neighbours.
            const double length = double(now.size()) / rate, refLength = double(reference.size()) / rate;
            const size_t half = size_t(rate);
            std::vector<std::pair<double, VoiceEq>> fits;
            for (double t = 1; t + 1 <= length + 1e-9; t += 0.5) {
                const size_t at = size_t(t * rate), ref = size_t(std::clamp(t * refLength / length, 1.0, std::max(1.0, refLength - 1)) * rate);
                if (ref < half || ref + half > reference.size() || at + half > now.size()) break;
                const std::vector<float> a(now.begin() + std::ptrdiff_t(at - half), now.begin() + std::ptrdiff_t(at + half));
                const std::vector<float> b(reference.begin() + std::ptrdiff_t(ref - half), reference.begin() + std::ptrdiff_t(ref + half));
                // (a window without speech in both keeps the fit for the whole)
                const double la = loudness(a, rate), lb = loudness(b, rate);
                fits.push_back({t, la > -60 && lb > -60 ? fitVoiceEq(speechSpectrum(a, rate), speechSpectrum(b, rate), rate) : plan.eq});
            }
            for (size_t i = 0; i < fits.size(); ++i) {
                VoiceEq g;
                int n = 0;
                for (size_t j = i ? i - 1 : 0; j <= i + 1 && j < fits.size(); ++j, ++n) {
                    const VoiceEq& f = fits[j].second;
                    g.lowDb += f.lowDb, g.b1Db += f.b1Db, g.b2Db += f.b2Db, g.b3Db += f.b3Db, g.highDb += f.highDb;
                }
                g.lowDb /= n, g.b1Db /= n, g.b2Db /= n, g.b3Db /= n, g.highDb /= n;
                plan.eqOverTime.push_back({fits[i].first, g});
            }
        }
        applyVoiceEqTo(now, plan.eq, rate);
    }
    // The level: the speech as loud as the reference's.
    plan.referenceLufs = loudness(reference, rate);
    plan.targetLufs = loudness(target, rate);
    const double before = loudness(now, rate);
    if (o.level && plan.referenceLufs > -60 && before > -60) {
        plan.gainDb = std::clamp(plan.referenceLufs - before, -30.0, 24.0);
        plan.levelSet = true;
    }
    plan.matchedLufs = before + plan.gainDb;
    return plan;
}

std::vector<float> withDialogueMatch(const std::vector<float>& target, int rate, const DialogueMatchPlan& plan) {
    std::vector<float> out = plan.roomAdded ? mixed(target, convolve(target, roomImpulse(rate, plan.room, 1)), plan.wetDb) : target;
    applyVoiceEqTo(out, plan.eq, rate);
    const float g = float(std::pow(10.0, plan.gainDb / 20));
    for (float& v : out) v *= g;
    return out;
}

void applyDialogueMatch(Project& p, const Sequence& s, Clip& c, const DialogueMatchPlan& plan, const DialogueMatchOptions& o) {
    std::erase_if(c.effects, [&](const Effect& e) {
        return (e.type == "parametric_eq" && (e.s("match") == "dialogue" || (o.tone && e.s("match") == "voice"))) ||
               (e.type == "convolution_reverb" && e.s("match") == "dialogue");
    });
    size_t at = 0;
    if (o.tone) {
        Effect eq = voiceEqEffect(p, plan.eq);
        eq.strings["match"] = "dialogue";
        if (!plan.eqOverTime.empty()) {
            // Keys at the clip's own frames (its sound at its speed).
            const double fps = s.fpsValue(), speed = std::max(1e-6, std::fabs(c.speed));
            const std::pair<const char*, double VoiceEq::*> gains[] = {
                {"low_db", &VoiceEq::lowDb}, {"b1_db", &VoiceEq::b1Db}, {"b2_db", &VoiceEq::b2Db}, {"b3_db", &VoiceEq::b3Db}, {"high_db", &VoiceEq::highDb}};
            for (const auto& [name, member] : gains) {
                Param& prm = eq.params[name];
                for (const auto& [t, g] : plan.eqOverTime) prm.addKey(FrameTime(std::llround(t * fps / speed)), g.*member);
            }
        }
        c.effects.insert(c.effects.begin(), std::move(eq));
        at = 1;
    }
    if (plan.roomAdded) {
        Effect room = makeEffect(p, "convolution_reverb");
        room.params["wet_db"] = Param(plan.wetDb);
        room.params["dry_db"] = Param(0.0);
        room.params["predelay_ms"] = Param(plan.room.predelayMs);
        for (int b = 0; b < kRoomBands; ++b) room.params[kRoomDecayParams[b]] = Param(plan.room.rt60[size_t(b)]);
        room.strings["match"] = "dialogue";
        c.effects.insert(c.effects.begin() + std::ptrdiff_t(at), std::move(room));
    }
    if (plan.levelSet) c.audio.params["gain_db"] = Param(plan.gainDb);
}

int matchDialogue(Project& p, Sequence& s, Id reference, const std::vector<Id>& clips, const DialogueMatchOptions& o,
                  std::vector<std::pair<Id, DialogueMatchPlan>>* plans, std::string* error, const std::function<void(double)>& progress,
                  const std::atomic<bool>* cancel) {
    const Clip* ref = edit::clipById(s, reference);
    if (!ref) {
        if (error) *error = "No such reference clip";
        return 0;
    }
    constexpr int kRate = 48000;
    std::vector<float> refAudio;
    if (!clipSpeechAudio(p, s, *ref, refAudio, error, kRate)) return 0;
    // (the reference as it is heard: with its clip volume, taken at its middle)
    const double refGain = ref->audio.p("gain_db", ref->duration / 2, 0);
    int n = 0;
    for (size_t i = 0; i < clips.size(); ++i) {
        if (cancel && cancel->load()) break;
        if (progress) progress(double(i) / double(clips.size()));
        const Id id = clips[i];
        if (id == reference) continue;
        auto loc = edit::locate(s, id);
        Clip* c = edit::clipById(s, id);
        if (!loc || loc->track.kind != TrackKind::Audio || !c) continue;
        std::vector<float> audio;
        if (!clipSpeechAudio(p, s, *c, audio, error, kRate)) continue;
        DialogueMatchPlan plan = planDialogueMatch(refAudio, audio, kRate, o);
        if (plan.levelSet) {
            plan.gainDb = std::clamp(plan.gainDb + refGain, -30.0, 24.0);
            plan.referenceLufs += refGain, plan.matchedLufs += refGain;
        }
        applyDialogueMatch(p, s, *c, plan, o);
        if (plans) plans->push_back({id, plan});
        ++n;
    }
    return n;
}

}  // namespace montage
