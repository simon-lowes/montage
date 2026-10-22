#include "QualityCheck.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "ColorSpace.h"
#include "Compositor.h"
#include "core/EditOps.h"
#include "core/SpellCheck.h"
#include "media/Loudness.h"
#include "media/StereoAnalysis.h"
#include "render/DialogueLoudness.h"
#include "render/Intelligibility.h"

namespace montage {

namespace {

constexpr int kRedLabel = 11;  // core/MediaLog.h "Red"

double srgbToLinear(double c) {
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

std::string fmt(const char* f, double a, double b = 0) {
    char buf[160];
    std::snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

// Frames flagged true grouped into spans of at least `minFrames`, with gaps up to `bridge` frames joined.
std::vector<std::pair<FrameTime, FrameTime>> spans(const std::vector<char>& on, FrameTime origin, FrameTime minFrames, FrameTime bridge = 0) {
    std::vector<std::pair<FrameTime, FrameTime>> out;
    FrameTime start = -1, lastOn = -1;
    auto close = [&] {
        if (start >= 0 && lastOn + 1 - start >= minFrames) out.push_back({origin + start, origin + lastOn + 1});
        start = -1;
    };
    for (FrameTime i = 0; i < FrameTime(on.size()); ++i) {
        if (!on[size_t(i)]) continue;
        if (start >= 0 && i - lastOn - 1 > bridge) close();
        if (start < 0) start = i;
        lastOn = i;
    }
    close();
    return out;
}

}  // namespace

const char* qcKindName(QcKind k) {
    switch (k) {
        case QcKind::Flashing: return "Flashing";
        case QcKind::RedFlashing: return "Red flashing";
        case QcKind::Levels: return "Illegal levels";
        case QcKind::Black: return "Black";
        case QcKind::Freeze: return "Frozen picture";
        case QcKind::Silence: return "Silence";
        case QcKind::Clipping: return "Clipping";
        case QcKind::Loudness: return "Loudness";
        case QcKind::TruePeak: return "True peak";
        case QcKind::Spelling: return "Spelling";
        case QcKind::OutOfSync: return "Out of sync";
        case QcKind::Intelligibility: return "Dialogue intelligibility";
        case QcKind::Phase: return "Out of phase";
        case QcKind::Inverted: return "Channel inverted";
        case QcKind::DcOffset: return "DC offset";
    }
    return "";
}

void FlashDetector::step(Track& t, double change, double before, double after, double threshold, double darkLimit, bool& hit) {
    if (change == 0) return;  // nothing changed together: a run carries on through a still
    if ((change > 0) != (t.acc > 0) || t.acc == 0) {
        t.acc = change;
        t.darker = std::min(before, after);
        t.counted = false;
    } else {
        t.acc += change;
        t.darker = std::min({t.darker, before, after});
    }
    if (!t.counted && std::fabs(t.acc) >= threshold && t.darker < darkLimit) {
        t.counted = true;
        t.frames.push_back(frame_);
        hit = true;
    }
}

int FlashDetector::add(const float* rgb, int w, int h) {
    const size_t n = size_t(w) * size_t(h);
    std::vector<float> Y(n), red(n);
    for (size_t i = 0; i < n; ++i) {
        const double r = rgb[i * 3], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
        Y[i] = float(0.2126 * r + 0.7152 * g + 0.0722 * b);
        const double sum = r + g + b;
        red[i] = sum > 0 && r / sum >= 0.8 ? float(std::max(0.0, r - g - b) * 320) : 0.0f;  // saturated red (WCAG)
    }
    int found = 0;
    if (prevY_.size() == n) {
        // The change over the part of the screen that moved one way together, if that is at least a quarter of it.
        auto change = [&](const std::vector<float>& cur, const std::vector<float>& prev, double eps, double& before, double& after) {
            double upN = 0, upD = 0, upB = 0, upA = 0, dnN = 0, dnD = 0, dnB = 0, dnA = 0;
            for (size_t i = 0; i < n; ++i) {
                const double d = double(cur[i]) - prev[i];
                if (d > eps) upN += 1, upD += d, upB += prev[i], upA += cur[i];
                else if (d < -eps) dnN += 1, dnD += d, dnB += prev[i], dnA += cur[i];
            }
            const double quarter = 0.25 * double(n);
            if (upN >= quarter && upN >= dnN) {
                before = upB / upN, after = upA / upN;
                return upD / upN;
            }
            if (dnN >= quarter) {
                before = dnB / dnN, after = dnA / dnN;
                return dnD / dnN;
            }
            return 0.0;
        };
        double before = 0, after = 0;
        bool hit = false;
        const double dl = change(Y, prevY_, 0.002, before, after);
        step(lum_, dl, before, after, 0.1, 0.8, hit);
        if (hit) found |= 1;
        hit = false;
        const double dr = change(red, prevRed_, 0.5, before, after);
        step(red_, dr, before, after, 20, 1e9, hit);
        if (hit) found |= 2;
    }
    prevY_ = std::move(Y);
    prevRed_ = std::move(red);
    ++frame_;
    return found;
}

int FlashDetector::transitionsInLastSecond(bool red) const {
    const std::vector<int>& f = red ? red_.frames : lum_.frames;
    const int last = frame_ - 1, window = std::max(1, int(std::lround(fps_)));
    return int(std::count_if(f.begin(), f.end(), [&](int x) { return x > last - window; }));
}

std::pair<int, int> FlashDetector::lastSecondSpan(bool red) const {
    const std::vector<int>& f = red ? red_.frames : lum_.frames;
    const int last = frame_ - 1, window = std::max(1, int(std::lround(fps_)));
    const auto first = std::find_if(f.begin(), f.end(), [&](int x) { return x > last - window; });
    if (first == f.end()) return {-1, -1};
    return {*first, f.back()};
}

std::vector<QcIssue> qualityCheck(const Project& p, const Sequence& s, FrameTime in, FrameTime out, const QcSettings& q,
                                  const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
    std::vector<QcIssue> issues;
    if (out < 0) out = s.duration();
    in = std::max<FrameTime>(0, in);
    if (out <= in || s.width <= 0) return issues;
    const double fps = s.fpsValue();
    const FrameTime frames = out - in;
    const bool picture = q.flashing || q.levels || q.blackSeconds > 0 || q.freezeSeconds > 0;
    const bool sound = q.silenceSeconds > 0 || q.clipping || q.loudnessTarget != 0 || q.intelligibility > 0 || q.phase || q.dcOffsetDb != 0;
    const double pictureShare = picture ? (sound ? 0.8 : 1.0) : 0.0;

    if (picture) {
        RenderOptions ro;
        ro.scale = std::min(1.0, double(std::max(16, q.analysisWidth)) / s.width);
        const ColorSpace& space = sequenceColorSpace(s);
        const bool hdr = space.hdr() || space.sceneReferred;  // judged as shown on an SDR display
        FlashDetector flashes(fps);
        std::vector<char> flashOn(size_t(frames), 0), redOn(size_t(frames), 0), illegal(size_t(frames), 0), black(size_t(frames), 0);
        // A still lasts while each frame matches the one before; black does not count (it has its own check).
        const FrameTime minFreeze = std::max<FrameTime>(2, FrameTime(std::ceil(q.freezeSeconds * fps - 1e-9)));
        FrameTime stillFrom = -1;
        std::vector<std::pair<FrameTime, FrameTime>> stills;
        auto endStill = [&](FrameTime end) {
            if (stillFrom >= 0 && end - stillFrom >= minFreeze) stills.push_back({in + stillFrom, in + end});
            stillFrom = -1;
        };
        std::vector<int> flashCount(size_t(frames), 0), redCount(size_t(frames), 0);
        double worstIllegal = 0;
        std::vector<float> prevLuma;
        for (FrameTime i = 0; i < frames; ++i) {
            if (cancel && cancel->load()) return {};
            Image img = renderProgramFrame(p, s, in + i, ro);
            const size_t n = size_t(img.width) * size_t(img.height);
            if (n == 0) continue;
            // Levels are judged on the signal as delivered (SDR only); the rest on how it looks on an SDR display.
            if (q.levels && !hdr) {
                size_t bad = 0;
                for (size_t k = 0; k < n; ++k) {
                    const float* px = &img.px[k * 4];
                    const double y = 0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2];
                    bool out_ = y < -0.01 || y > 1.03;
                    for (int c = 0; c < 3 && !out_; ++c) out_ = px[c] < -0.05f || px[c] > 1.05f;
                    bad += out_;
                }
                const double share = double(bad) / double(n);
                if (share > 0.01) illegal[size_t(i)] = 1, worstIllegal = std::max(worstIllegal, share);
            }
            if (hdr) convertColor(img, space, rec709Space(), std::clamp(s.hdrPeakNits, 100.0, 10000.0));
            std::vector<float> luma(n), lin(n * 3);
            double sum = 0;
            size_t lit = 0;
            for (size_t k = 0; k < n; ++k) {
                const float* px = &img.px[k * 4];
                const double y = std::clamp(0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2], 0.0, 1.0);
                luma[k] = float(y);
                sum += y;
                lit += y > 0.1;
                for (int c = 0; c < 3; ++c) lin[k * 3 + size_t(c)] = float(srgbToLinear(px[c]));
            }
            if (q.flashing) {
                flashes.add(lin.data(), img.width, img.height);
                const int t = flashes.transitionsInLastSecond(false), tr = flashes.transitionsInLastSecond(true);
                // More than three flashes (six transitions) in a second: the whole second fails.
                for (int pass = 0; pass < 2; ++pass) {
                    const int count = pass ? tr : t;
                    if (count <= 6) continue;
                    std::vector<char>& on = pass ? redOn : flashOn;
                    std::vector<int>& cnt = pass ? redCount : flashCount;
                    const auto [a, b] = flashes.lastSecondSpan(pass == 1);
                    for (FrameTime j = std::max<FrameTime>(0, a); j <= b; ++j) {
                        on[size_t(j)] = 1;
                        cnt[size_t(j)] = std::max(cnt[size_t(j)], count / 2);
                    }
                }
            }
            const bool isBlack = sum / double(n) < 0.04 && double(lit) < 0.002 * double(n);
            black[size_t(i)] = isBlack;
            bool same = false;
            if (prevLuma.size() == n && !isBlack) {
                double diff = 0;
                for (size_t k = 0; k < n; ++k) diff += std::fabs(double(luma[k]) - prevLuma[k]);
                same = diff / double(n) < 0.0015;
            }
            if (!same) endStill(i);
            else if (stillFrom < 0) stillFrom = i - 1;
            prevLuma = isBlack ? std::vector<float>() : std::move(luma);
            if (progress) progress(pictureShare * double(i + 1) / double(frames));
        }
        for (int pass = 0; pass < 2; ++pass)
            for (const auto& [a, b] : spans(pass ? redOn : flashOn, in, 1)) {
                const std::vector<int>& cnt = pass ? redCount : flashCount;
                const int most = *std::max_element(cnt.begin() + (a - in), cnt.begin() + (b - in));
                issues.push_back({pass ? QcKind::RedFlashing : QcKind::Flashing, a, b,
                                  fmt(pass ? "Saturated red flashing, up to %.0f flashes a second (more than 3 can trigger seizures)"
                                           : "Flashing, up to %.0f flashes a second (more than 3 can trigger seizures)",
                                      most)});
            }
        if (q.levels)
            for (const auto& [a, b] : spans(illegal, in, 1))
                issues.push_back({QcKind::Levels, a, b, fmt("Levels outside EBU R103 on up to %.1f %% of the picture", worstIllegal * 100)});
        if (q.blackSeconds > 0)
            for (const auto& [a, b] : spans(black, in, std::max<FrameTime>(1, FrameTime(std::ceil(q.blackSeconds * fps - 1e-9)))))
                issues.push_back({QcKind::Black, a, b, fmt("Black picture for %.1f s", double(b - a) / fps)});
        endStill(frames);
        if (q.freezeSeconds > 0)
            for (const auto& [a, b] : stills)
                issues.push_back({QcKind::Freeze, a, b, fmt("The picture does not change for %.1f s", double(b - a) / fps)});
    }

    if (sound) {
        const int sr = s.sampleRate > 0 ? s.sampleRate : 48000;
        const int64_t first = int64_t(std::llround(double(in) / fps * sr)), total = int64_t(std::llround(double(frames) / fps * sr));
        AudioMixer mixer;
        LoudnessMeter meter(sr);
        const int win = std::max(1, sr / 20);  // 50 ms
        std::vector<char> silentWin, clipWin;  // per 50 ms window
        std::vector<std::array<double, 5>> stereoWin;  // per 50 ms window: sums of left², right², left×right, left, right
        silentWin.reserve(size_t(total / win + 1));
        std::array<double, 5> winStereo{};
        int64_t clipped = 0;
        std::vector<float> buf;
        const int chunk = win * 20;  // a second
        double winEnergy = 0;
        int winFill = 0;
        bool winClip = false;
        for (int64_t pos = 0; pos < total; pos += chunk) {
            if (cancel && cancel->load()) return {};
            const int count = int(std::min<int64_t>(chunk, total - pos));
            buf.assign(size_t(count) * 2, 0.0f);
            mixer.mix(p, s, first + pos, count, buf.data());
            meter.add(buf.data(), count);
            for (int k = 0; k < count; ++k) {
                const float l = buf[size_t(k) * 2], r = buf[size_t(k) * 2 + 1];
                winEnergy += double(l) * l + double(r) * r;
                winStereo[0] += double(l) * l, winStereo[1] += double(r) * r, winStereo[2] += double(l) * r;
                winStereo[3] += l, winStereo[4] += r;
                if (std::fabs(l) >= 0.9999f || std::fabs(r) >= 0.9999f) ++clipped, winClip = true;
                if (++winFill == win) {
                    silentWin.push_back(std::sqrt(winEnergy / (2.0 * win)) < 0.001);  // -60 dBFS
                    clipWin.push_back(winClip);
                    stereoWin.push_back(winStereo);
                    winEnergy = 0, winFill = 0, winClip = false, winStereo = {};
                }
            }
            if (progress) progress(pictureShare + (1 - pictureShare) * double(pos + count) / double(total));
        }
        if (winFill > 0) {
            silentWin.push_back(std::sqrt(winEnergy / (2.0 * winFill)) < 0.001);
            clipWin.push_back(winClip);
            stereoWin.push_back(winStereo);
        }
        // Window spans back to sequence frames.
        auto toFrame = [&](FrameTime w) { return in + FrameTime(std::floor(double(w) * win / sr * fps + 1e-9)); };
        auto fromWindows = [&](const std::vector<char>& on, double minSeconds, double bridgeSeconds) {
            std::vector<std::pair<FrameTime, FrameTime>> out_;
            for (const auto& [a, b] : spans(on, 0, std::max<FrameTime>(1, FrameTime(std::ceil(minSeconds * sr / win - 1e-9))),
                                            FrameTime(bridgeSeconds * sr / win)))
                out_.push_back({toFrame(a), std::min(out, std::max(toFrame(a) + 1, toFrame(b)))});
            return out_;
        };
        if (q.silenceSeconds > 0)
            for (const auto& [a, b] : fromWindows(silentWin, q.silenceSeconds, 0))
                issues.push_back({QcKind::Silence, a, b, fmt("Silence (below -60 dBFS) for %.1f s", double(b - a) / fps)});
        if (q.clipping && clipped > 0)
            for (const auto& [a, b] : fromWindows(clipWin, 0, 0.25))
                issues.push_back({QcKind::Clipping, a, b, fmt("Sound at full scale (it will clip): %.0f samples in all", double(clipped))});
        // The windows a span of sequence frames covers.
        auto windowsIn = [&](FrameTime a, FrameTime b) {
            const auto at = [&](FrameTime f) {
                return std::min(stereoWin.size(), size_t(std::max(0.0, std::ceil(double(f - in) / fps * sr / win - 1e-6))));
            };
            return std::pair<size_t, size_t>(at(a), at(b));
        };
        if (q.phase) {
            // Each window judged over the 350 ms around it, where both sides are above -60 dBFS.
            const int nw = int(stereoWin.size()), half = 3;
            std::vector<char> inverted(size_t(nw), 0), outOfPhase(size_t(nw), 0);
            std::vector<double> loss(size_t(nw), 0);
            for (int w = 0; w < nw; ++w) {
                double ll = 0, rr = 0, lr = 0;
                int64_t n = 0;
                for (int k = std::max(0, w - half); k < std::min(nw, w + half + 1); ++k) {
                    ll += stereoWin[size_t(k)][0], rr += stereoWin[size_t(k)][1], lr += stereoWin[size_t(k)][2];
                    n += win;
                }
                if (ll < 1e-6 * double(n) || rr < 1e-6 * double(n)) continue;
                const double c = stereoCorrelation(ll, rr, lr);
                loss[size_t(w)] = foldDownLossDb(ll, rr, lr);
                if (c < -0.9 && ll < 4 * rr && rr < 4 * ll) inverted[size_t(w)] = 1;
                else if (c < -0.5) outOfPhase[size_t(w)] = 1;
            }
            // The fold-down's loss over a span, the median window's (a window at a span's edge straddles it).
            auto lossOver = [&](FrameTime a, FrameTime b) {
                std::vector<double> v;
                const auto [w0, w1] = windowsIn(a, b);
                for (size_t w = w0; w < w1; ++w) v.push_back(loss[w]);
                if (v.empty()) return 0.0;
                std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(v.size() / 2), v.end());
                return v[v.size() / 2];
            };
            for (const auto& [a, b] : fromWindows(inverted, q.phaseSeconds, 0.25))
                issues.push_back({QcKind::Inverted, a, b,
                                  fmt("One channel is the other inverted (opposite polarity): a mono fold-down loses %.0f dB", lossOver(a, b))});
            for (const auto& [a, b] : fromWindows(outOfPhase, q.phaseSeconds, 0.25))
                issues.push_back({QcKind::Phase, a, b, fmt("Left and right out of phase: a mono fold-down loses %.1f dB", lossOver(a, b))});
        }
        if (q.dcOffsetDb != 0) {
            // Each second's mean, each side (a last part under a second joins the second before it; a range under half
            // a second is not judged, as a low note's own swing would read as an offset over so short a time).
            const int per = 20;
            const double limit = std::pow(10.0, q.dcOffsetDb / 20);
            std::vector<char> offset(stereoWin.size(), 0);
            std::vector<double> mean(stereoWin.size(), 0);
            for (size_t w0 = 0, w1 = 0; w0 < stereoWin.size() && stereoWin.size() >= size_t(per / 2); w0 = w1) {
                w1 = std::min(stereoWin.size(), w0 + per);
                if (stereoWin.size() - w1 < size_t(per)) w1 = stereoWin.size();
                double sl = 0, sr_ = 0;
                for (size_t k = w0; k < w1; ++k) sl += stereoWin[k][3], sr_ += stereoWin[k][4];
                const double n = double(w1 - w0) * win, m = std::max(std::fabs(sl), std::fabs(sr_)) / n;
                std::fill(mean.begin() + std::ptrdiff_t(w0), mean.begin() + std::ptrdiff_t(w1), m);
                if (m > limit) std::fill(offset.begin() + std::ptrdiff_t(w0), offset.begin() + std::ptrdiff_t(w1), 1);
            }
            for (const auto& [a, b] : fromWindows(offset, 0, 0)) {
                const auto [w0, w1] = windowsIn(a, b);
                double worst = limit;
                for (size_t w = w0; w < w1; ++w) worst = std::max(worst, mean[w]);
                issues.push_back({QcKind::DcOffset, a, b, fmt("DC offset: the sound's mean is %.1f dBFS (the limit is %.0f dBFS)", 20 * std::log10(worst), q.dcOffsetDb)});
            }
        }
        if (q.loudnessTarget != 0) {
            LoudnessResult r = meter.result();
            std::string measured = fmt("Integrated loudness %.1f LUFS", r.integrated);
            if (q.dialogueGated) {
                DialogueLoudnessOptions o;
                o.in = in;
                o.out = out;
                const DialogueLoudness d = measureDialogueLoudness(p, s, o, {}, cancel);
                if (cancel && cancel->load()) return {};
                r.valid = d.valid;
                r.integrated = d.value();
                measured = describeDialogueLoudness(d);
            }
            const std::string target = fmt(q.dialogueGated ? "the target is %.0f LKFS" : "the target is %.0f LUFS", q.loudnessTarget) +
                                       (q.loudnessTolerance != 1 ? fmt(" ±%g LU", q.loudnessTolerance) : std::string());
            if (!r.valid || std::fabs(r.integrated - q.loudnessTarget) > q.loudnessTolerance + 1e-9)
                issues.push_back({QcKind::Loudness, in, out, (r.valid ? measured : std::string("Too quiet to measure")) + "; " + target});
            if (r.valid && r.truePeakDb > q.peakCeiling + 0.05)
                issues.push_back({QcKind::TruePeak, in, out, fmt("True peak %.1f dBTP; the ceiling is %.1f dBTP", r.truePeakDb, q.peakCeiling)});
        }
        // Dialogue the music and effects make hard to follow (render/Intelligibility.h); nothing when there is no
        // dialogue to measure.
        if (q.intelligibility > 0) {
            IntelligibilityOptions o;
            o.in = in;
            o.out = out;
            o.source = DialogueSource::Roles;  // (not the slow separation: Quality Check measures Dialogue clips only)
            const Intelligibility r = measureIntelligibility(p, s, o, {}, cancel);
            if (cancel && cancel->load()) return {};
            for (const auto& [a, b] : r.under(q.intelligibility, q.intelligibilitySeconds)) {
                double sum = 0;
                int n = 0;
                for (const IntelligibilityBlock& blk : r.blocks)
                    if (blk.speech && blk.start >= a - 1e-9 && blk.end <= b + 1e-9) sum += blk.index, ++n;
                const FrameTime fa = std::max(in, FrameTime(std::floor(a * fps + 1e-6))), fb = std::min(out, FrameTime(std::ceil(b * fps - 1e-6)));
                issues.push_back({QcKind::Intelligibility, fa, std::max(fa + 1, fb),
                                  fmt("Dialogue hard to follow under the rest of the mix: intelligibility %.2f; the limit is %.2f",
                                      n ? sum / n : 0.0, q.intelligibility)});
            }
        }
    }
    if (q.spelling) {
        // The misspelt words and the first suggestion for each: "'teh' (the?), 'knwon' (known?)".
        auto listed = [](const std::vector<Misspelling>& bad) {
            std::string out;
            for (const Misspelling& m : bad) {
                if (!out.empty()) out += ", ";
                out += "'" + m.word + "'";
                if (!m.suggestions.empty()) out += " (" + m.suggestions.front() + "?)";
            }
            return out;
        };
        for (const CaptionTrack& t : s.captionTracks) {
            const SpellChecker* sc = SpellChecker::forLanguage(t.language);
            if (!sc) continue;
            for (size_t i = 0; i < t.captions.size(); ++i) {
                const Caption& c = t.captions[i];
                if (c.end <= in || c.start >= out) continue;
                const std::vector<Misspelling> bad = sc->check(c.text, p.vocabulary, true);
                if (!bad.empty())
                    issues.push_back({QcKind::Spelling, std::max(c.start, in), std::min(c.end, out),
                                      "Spelling in caption " + std::to_string(i + 1) + " of " + t.name + ": " + listed(bad)});
            }
        }
        if (const SpellChecker* sc = SpellChecker::forLanguage(q.titleLanguage))
            for (const Track& t : s.videoTracks)
                for (const Clip& c : t.clips) {
                    const auto text = c.generator.strings.find("text");
                    if (!c.enabled || c.generator.type.rfind("title", 0) != 0 || text == c.generator.strings.end()) continue;
                    if (c.end() <= in || c.start >= out) continue;
                    const std::vector<Misspelling> bad = sc->check(text->second, p.vocabulary, true);
                    if (!bad.empty())
                        issues.push_back({QcKind::Spelling, std::max(c.start, in), std::min(c.end(), out),
                                          "Spelling in the title '" + (c.name.empty() ? text->second.substr(0, 40) : c.name) + "': " + listed(bad)});
                }
    }
    if (q.sync)
        for (const edit::SyncOffset& o : edit::syncOffsets(s)) {
            const Clip* c = edit::clipById(s, o.clip);
            const Clip* a = edit::clipById(s, o.anchor);
            if (!c->enabled || c->end() <= in || c->start >= out) continue;
            const double frames = std::fabs(o.frames);
            issues.push_back({QcKind::OutOfSync, std::max(c->start, in), std::min(c->end(), out),
                              "'" + c->name + "' is " + fmt("%g", std::round(frames * 10) / 10) + (frames == 1 ? " frame " : " frames ") +
                                  (o.frames > 0 ? "late" : "early") + " against '" + a->name + "', the clip it is linked to"});
        }
    std::stable_sort(issues.begin(), issues.end(), [](const QcIssue& a, const QcIssue& b) { return a.start < b.start; });
    if (progress) progress(1.0);
    return issues;
}

int addQcMarkers(Sequence& s, const std::vector<QcIssue>& issues) {
    std::erase_if(s.markers, [](const Marker& m) { return m.name.rfind("QC: ", 0) == 0; });
    for (const QcIssue& i : issues)
        s.markers.push_back(Marker{i.start, std::max<FrameTime>(0, i.end - i.start - 1), std::string("QC: ") + qcKindName(i.kind), i.text, kRedLabel});
    std::stable_sort(s.markers.begin(), s.markers.end(), [](const Marker& a, const Marker& b) { return a.t < b.t; });
    return int(issues.size());
}

int broadcastSafe(float* rgba, int w, int h, bool strict, double knee, bool highlight) {
    const double rgbLo = strict ? 0.0 : -0.05, rgbHi = strict ? 1.0 : 1.05, yLo = strict ? 0.0 : -0.01, yHi = strict ? 1.0 : 1.03;
    knee = std::clamp(knee, 0.0, 0.25);
    // Soft limit: unchanged inside [lo + knee, hi - knee], easing towards the limit beyond.
    auto limit = [&](double v, double lo, double hi) {
        if (knee <= 0) return std::clamp(v, lo, hi);
        if (v > hi - knee) return hi - knee + knee * (1 - std::exp(-(v - (hi - knee)) / knee));
        if (v < lo + knee) return lo + knee - knee * (1 - std::exp(-((lo + knee) - v) / knee));
        return v;
    };
    int bad = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* px = rgba + (size_t(y) * size_t(w) + size_t(x)) * 4;
            const double a = px[3];
            if (a <= 0) continue;
            const double inv = a < 1 ? 1 / a : 1;  // premultiplied
            double c[3] = {px[0] * inv, px[1] * inv, px[2] * inv};
            const double luma = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
            const bool out = luma < yLo || luma > yHi || std::any_of(c, c + 3, [&](double v) { return v < rgbLo || v > rgbHi; });
            bad += out;
            if (out && highlight) {
                const bool stripe = ((x + y) / 4) % 2 == 0;
                px[0] = float(stripe ? a : 0), px[1] = 0, px[2] = float(stripe ? a : 0);
                continue;
            }
            const double l2 = limit(luma, yLo, yHi);
            for (double& v : c) v += l2 - luma;
            // Pull towards luma just enough for every channel to fit.
            double sat = 1;
            for (double v : c) {
                if (v > rgbHi && v > l2) sat = std::min(sat, (rgbHi - l2) / (v - l2));
                if (v < rgbLo && v < l2) sat = std::min(sat, (l2 - rgbLo) / (l2 - v));
            }
            sat = std::clamp(sat, 0.0, 1.0);
            for (int k = 0; k < 3; ++k) px[k] = float(std::clamp(l2 + (c[k] - l2) * sat, rgbLo, rgbHi) * a);
        }
    return bad;
}

}  // namespace montage
