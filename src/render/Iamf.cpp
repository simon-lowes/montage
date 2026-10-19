#include "Iamf.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "Compositor.h"
#include "core/Surround.h"
#include "media/Loudness.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

namespace montage {

namespace {

constexpr int kRate = 48000;
// Samples per frame (20 ms): a stereo FLAC frame of 24-bit noise stays under 8 KB, the most libiamf's decoder (Android's)
// reads of a frame.
constexpr int kFrame = 960;

// OBU types (IAMF 1.1, 3.2).
enum ObuType {
    kCodecConfig = 0,
    kAudioElement = 1,
    kMixPresentation = 2,
    kParameterBlock = 3,
    kTemporalDelimiter = 4,
    kAudioFrame = 5,
    kAudioFrameId0 = 6,  // to kAudioFrameId0 + 17: the substream's id in the type
    kSequenceHeader = 31,
};

void put8(QByteArray& b, unsigned v) { b.append(char(v & 0xff)); }
void put16(QByteArray& b, unsigned v) { put8(b, v >> 8), put8(b, v); }
void put32(QByteArray& b, uint32_t v) { put16(b, v >> 16), put16(b, v & 0xffff); }
void put64(QByteArray& b, uint64_t v) { put32(b, uint32_t(v >> 32)), put32(b, uint32_t(v)); }
void putLeb(QByteArray& b, uint64_t v) {
    do {
        uint8_t byte = v & 0x7f;
        v >>= 7;
        if (v) byte |= 0x80;
        put8(b, byte);
    } while (v);
}
void putString(QByteArray& b, const std::string& s) {
    // At most 127 bytes, cut before a character, not inside one.
    size_t n = std::min<size_t>(s.size(), 127);
    if (n < s.size())
        while (n > 0 && (uint8_t(s[n]) & 0xc0) == 0x80) --n;
    b.append(s.substr(0, n).c_str());
    b.append('\0');
}
void set32(QByteArray& b, int at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = char(v >> (24 - 8 * i));
}
void set64(QByteArray& b, int at, uint64_t v) { set32(b, at, uint32_t(v >> 32)), set32(b, at + 4, uint32_t(v)); }

// An OBU: its header (type, the trimming flag) and size, then its payload.
QByteArray obu(int type, const QByteArray& payload, uint32_t trimEnd = 0) {
    QByteArray body;
    if (trimEnd) {
        putLeb(body, trimEnd);
        putLeb(body, 0);
    }
    body += payload;
    QByteArray b;
    put8(b, unsigned(type) << 3 | (trimEnd ? 1u << 1 : 0u));
    putLeb(b, uint64_t(body.size()));
    return b + body;
}

// dB as IAMF's Q7.8.
int16_t q78(double db) { return int16_t(std::clamp(std::lround(db * 256), -32768L, 32767L)); }

QByteArray box(const char* type, const QByteArray& payload) {
    QByteArray b;
    put32(b, uint32_t(8 + payload.size()));
    b.append(type, 4);
    return b + payload;
}
QByteArray fullBox(const char* type, int version, uint32_t flags, const QByteArray& payload) {
    QByteArray b;
    put32(b, uint32_t(version) << 24 | (flags & 0xffffff));
    return box(type, b + payload);
}
QByteArray matrix() {
    QByteArray b;
    for (uint32_t v : {0x00010000u, 0u, 0u, 0u, 0x00010000u, 0u, 0u, 0u, 0x40000000u}) put32(b, v);
    return b;
}

// A substream's encoder: FLAC (libavcodec's) or 24-bit little-endian PCM, one frame in, one frame out.
class SubstreamEncoder {
public:
    SubstreamEncoder(bool flac, int channels) : flac_(flac), channels_(channels) {}
    ~SubstreamEncoder() {
        av_frame_free(&frame_);
        av_packet_free(&pkt_);
        avcodec_free_context(&ctx_);
    }
    bool open(std::string& error) {
        if (!flac_) return true;
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_FLAC);
        if (!codec || !(ctx_ = avcodec_alloc_context3(codec))) {
            error = "This build of FFmpeg has no FLAC encoder";
            return false;
        }
        ctx_->sample_rate = kRate;
        ctx_->sample_fmt = AV_SAMPLE_FMT_S32;
        ctx_->bits_per_raw_sample = 24;
        ctx_->frame_size = kFrame;
        av_channel_layout_default(&ctx_->ch_layout, channels_);
        ctx_->time_base = {1, kRate};
        if (avcodec_open2(ctx_, codec, nullptr) < 0) {
            error = "The FLAC encoder would not open";
            return false;
        }
        frame_ = av_frame_alloc();
        pkt_ = av_packet_alloc();
        return frame_ && pkt_;
    }
    // One frame of kFrame samples, interleaved 24-bit values; its coded bytes queued (FLAC may hold some back).
    bool encode(const int32_t* samples, std::string& error) {
        if (!flac_) {
            QByteArray b(kFrame * channels_ * 3, Qt::Uninitialized);
            char* d = b.data();
            for (int i = 0; i < kFrame * channels_; ++i) {
                *d++ = char(samples[i] & 0xff);
                *d++ = char((samples[i] >> 8) & 0xff);
                *d++ = char((samples[i] >> 16) & 0xff);
            }
            out_.push_back(std::move(b));
            return true;
        }
        av_frame_unref(frame_);
        frame_->format = AV_SAMPLE_FMT_S32;
        frame_->sample_rate = kRate;
        frame_->nb_samples = kFrame;
        av_channel_layout_copy(&frame_->ch_layout, &ctx_->ch_layout);
        if (av_frame_get_buffer(frame_, 0) < 0) return (error = "Out of memory", false);
        auto* d = reinterpret_cast<int32_t*>(frame_->data[0]);
        for (int i = 0; i < kFrame * channels_; ++i) d[i] = int32_t(uint32_t(samples[i]) << 8);
        frame_->pts = pts_;
        pts_ += kFrame;
        if (avcodec_send_frame(ctx_, frame_) < 0) return (error = "The FLAC encoder failed", false);
        return drain(error);
    }
    bool finish(std::string& error) {
        if (!flac_) return true;
        if (avcodec_send_frame(ctx_, nullptr) < 0) return (error = "The FLAC encoder failed", false);
        return drain(error);
    }
    bool ready() const { return !out_.empty(); }
    QByteArray take() {
        QByteArray b = std::move(out_.front());
        out_.erase(out_.begin());
        return b;
    }

private:
    bool drain(std::string& error) {
        for (;;) {
            const int r = avcodec_receive_packet(ctx_, pkt_);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
            if (r < 0) return (error = "The FLAC encoder failed", false);
            // (The last packet may carry only the final STREAMINFO.)
            if (pkt_->size > 0) out_.emplace_back(reinterpret_cast<const char*>(pkt_->data), pkt_->size);
            av_packet_unref(pkt_);
        }
    }
    bool flac_;
    int channels_;
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    int64_t pts_ = 0;
    std::vector<QByteArray> out_;
};

const char* layoutForSoundSystem(int ss) {
    switch (ss) {
        case 0: return "stereo";
        case 1: return "5.1";
        case 2: return "5.1.2";
        case 3: return "5.1.4";
        case 8: return "7.1";
        case 9: return "7.1.4";
        case 10: return "7.1.2";
        default: return "";
    }
}

}  // namespace

int iamfLoudspeakerLayout(const std::string& layout) {
    if (layout == "stereo") return 1;
    if (layout == "5.1") return 2;
    if (layout == "5.1.2") return 3;
    if (layout == "5.1.4") return 4;
    if (layout == "7.1") return 5;
    if (layout == "7.1.2") return 6;
    if (layout == "7.1.4") return 7;
    return -1;
}

int iamfSoundSystem(const std::string& layout) {
    for (int ss : {0, 1, 2, 3, 8, 9, 10})
        if (layout == layoutForSoundSystem(ss)) return ss;
    return -1;
}

std::vector<std::vector<int>> iamfSubstreams(const std::string& layout) {
    // Montage's channels (Surround.h): L R C LFE, then 5.1's Ls Rs or 7.1's Lb Rb Ls Rs, then the overhead pairs.
    // IAMF's: the pairs (front, side, back, top front or top, top back), then the centre and the LFE.
    if (const int order = ambisonicOrder(layout)) {
        std::vector<std::vector<int>> out;
        for (int c = 0; c < ambisonicChannels(order); ++c) out.push_back({c});
        return out;
    }
    if (layout == "stereo") return {{0, 1}};
    if (layout == "5.1") return {{0, 1}, {4, 5}, {2}, {3}};
    if (layout == "5.1.2") return {{0, 1}, {4, 5}, {6, 7}, {2}, {3}};
    if (layout == "5.1.4") return {{0, 1}, {4, 5}, {6, 7}, {8, 9}, {2}, {3}};
    if (layout == "7.1") return {{0, 1}, {6, 7}, {4, 5}, {2}, {3}};
    if (layout == "7.1.2") return {{0, 1}, {6, 7}, {4, 5}, {8, 9}, {2}, {3}};
    if (layout == "7.1.4") return {{0, 1}, {6, 7}, {4, 5}, {8, 9}, {10, 11}, {2}, {3}};
    return {};
}

namespace {

// The reference renderer's ambisonic (ACN, SN3D) decoders to stereo, first to third order: a row for L, then one for R.
// From libiamf's h2m_rdr.c (foa_bs020, soa_bs020, toa_bs020): Copyright (c) 2022, Alliance for Open Media,
// BSD-3-Clause-Clear (LICENSES/BSD-3-Clause-Clear-libiamf.txt).
const float kFoaStereo[2][4] = {{6.23752675e-01f, 5.72103593e-01f, -8.58322813e-07f, 7.40141262e-02f},
                                {6.23763190e-01f, -5.72095695e-01f, 4.66862857e-07f, 7.40415612e-02f}};
const float kSoaStereo[2][9] = {{6.11858651e-01f, 5.61194439e-01f, -8.41955890e-07f, 7.26027883e-02f, 2.46501808e-01f,
                                 2.35064399e-06f, 9.24039966e-02f, -1.44147615e-07f, 1.58528890e-01f},
                                {6.11868965e-01f, -5.61186692e-01f, 4.57960486e-07f, 7.26297001e-02f, -2.46493617e-01f,
                                 1.40872103e-05f, 9.23974685e-02f, 1.73248243e-06f, 1.58547959e-01f}};
const float kToaStereo[2][16] = {{6.10506790e-01f, 5.59954517e-01f, -8.40095644e-07f, 7.24423772e-02f, 2.45957178e-01f,
                                  2.34545040e-06f, 9.21998360e-02f, -1.43829130e-07f, 1.58178631e-01f, 1.05906734e-01f,
                                  -9.21200162e-06f, 1.44451230e-02f, 6.21297703e-06f, 3.96432134e-02f, -1.75258363e-05f,
                                  4.95199472e-02f},
                                 {6.10517081e-01f, -5.59946787e-01f, 4.56948652e-07f, 7.24692296e-02f, -2.45949005e-01f,
                                  1.40560856e-05f, 9.21933224e-02f, 1.72865462e-06f, 1.58197658e-01f, -1.05896072e-01f,
                                  9.84777929e-06f, -1.44662789e-02f, -2.26479586e-05f, 3.96647344e-02f, -1.30420167e-05f,
                                  4.95243016e-02f}};

}  // namespace

void iamfDownmixToStereo(const std::string& layout, const float* in, int frames, float* out) {
    const std::vector<Speaker>& sp = layoutSpeakers(layout);
    if (const int order = ambisonicOrder(layout)) {
        const int k = ambisonicChannels(order);
        const float* l = order == 1 ? kFoaStereo[0] : order == 2 ? kSoaStereo[0] : kToaStereo[0];
        const float* r = order == 1 ? kFoaStereo[1] : order == 2 ? kSoaStereo[1] : kToaStereo[1];
        for (int i = 0; i < frames; ++i) {
            float a = 0, b = 0;
            for (int c = 0; c < k; ++c) {
                const float v = in[size_t(i) * size_t(k) + size_t(c)];
                a += v * l[c];
                b += v * r[c];
            }
            out[size_t(i) * 2] = a;
            out[size_t(i) * 2 + 1] = b;
        }
        return;
    }
    if (sp.size() <= 2) return downmixToStereo(layout, in, frames, out);
    // IAMF's matrices to stereo (as libiamf renders): L and R whole, the centre and every other ear-level speaker at -3 dB
    // (to both sides, or its own), the LFE left out; overhead speakers whole, but the rear pair of four at -3 dB.
    const size_t n = sp.size();
    std::vector<std::pair<float, float>> w(n);
    for (size_t c = 0; c < n; ++c) {
        const std::string& name = sp[c].name;
        const bool left = name[0] == 'L';
        const float g = sp[c].elevation > 0 ? (name == "Ltr" || name == "Rtr" ? 0.70710678f : 1.f)
                        : name == "L" || name == "R" ? 1.f
                                                     : 0.70710678f;
        if (sp[c].lfe) w[c] = {0, 0};
        else if (name == "C") w[c] = {g, g};
        else w[c] = left ? std::pair{g, 0.f} : std::pair{0.f, g};
    }
    for (int i = 0; i < frames; ++i) {
        float l = 0, r = 0;
        for (size_t c = 0; c < n; ++c) {
            const float v = in[size_t(i) * n + c];
            l += v * w[c].first;
            r += v * w[c].second;
        }
        out[size_t(i) * 2] = l;
        out[size_t(i) * 2 + 1] = r;
    }
}

bool exportIamf(const Project& p, const Sequence& s, const IamfSettings& settings, const std::string& path,
                IamfResult* result, const std::function<bool(double)>& progress, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const std::string layout = std::find(audioLayouts().begin(), audioLayouts().end(), s.audioLayout) != audioLayouts().end()
                                   ? s.audioLayout
                                   : std::string("stereo");
    const bool scene = ambisonicOrder(layout) > 0;
    const std::vector<std::vector<int>> subs = iamfSubstreams(layout);
    if (subs.empty()) return fail("IAMF has no layout for " + layout);
    if (settings.codec != "flac" && settings.codec != "lpcm") return fail("IAMF here is FLAC or PCM (\"flac\", \"lpcm\")");
    const bool flac = settings.codec == "flac";
    const int channels = layoutChannels(layout);
    int coupled = 0;
    for (const auto& sub : subs) coupled += sub.size() == 2;
    FrameTime first = 0, end = s.duration();
    if (settings.inOut && s.inPoint >= 0 && s.outPoint >= s.inPoint) first = s.inPoint, end = s.outPoint + 1;
    if (end <= first) return fail("The sequence is empty");
    const double fps = s.fpsValue();
    const int64_t firstSample = int64_t(std::llround(double(first) * kRate / fps));
    const int64_t total = int64_t(std::llround(double(end - first) * kRate / fps));
    const int frames = int((total + kFrame - 1) / kFrame);
    const uint32_t trimEnd = uint32_t(int64_t(frames) * kFrame - total);

    std::vector<std::unique_ptr<SubstreamEncoder>> enc;
    for (const auto& sub : subs) {
        enc.push_back(std::make_unique<SubstreamEncoder>(flac, int(sub.size())));
        std::string why;
        if (!enc.back()->open(why)) return fail(why);
    }

    const QString qpath = QString::fromStdString(path);
    const bool bare = QFileInfo(qpath).suffix().compare(QStringLiteral("iamf"), Qt::CaseInsensitive) == 0;
    QFile f(qpath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return fail("Cannot write " + path);
    bool finished = false;
    struct Cleanup {
        QFile& f;
        bool& finished;
        ~Cleanup() {
            if (!finished) {
                f.close();
                f.remove();
            }
        }
    } cleanup{f, finished};
    // An MP4: the frames into its mdat (its size set at the end), its moov after. The bare stream: the
    // frames after the descriptors, which need the loudness, so they are written at the end and the frames moved up.
    qint64 mdatAt = 0;
    if (!bare) {
        QByteArray ftyp;
        ftyp.append("mp42", 4);
        put32(ftyp, 0);
        ftyp.append("iamfiso6mp42mp41", 16);
        QByteArray head = box("ftyp", ftyp);
        // (A free box then the mdat's 32-bit header: the 16 bytes become a 64-bit mdat header past 4 GB.)
        mdatAt = head.size();
        put32(head, 8);
        head.append("free", 4);
        put32(head, 0);
        head.append("mdat", 4);
        if (f.write(head) != head.size()) return fail("Cannot write " + path);
    }
    std::vector<uint32_t> sampleSizes;
    sampleSizes.reserve(size_t(frames));

    Sequence mixSeq = s;
    mixSeq.sampleRate = kRate;
    mixSeq.audioLayout = layout;
    AudioMixer mixer;
    // Loudness as heard in the element's own layout and in stereo as IAMF renders it (the field's: in stereo alone).
    const std::vector<Speaker>& speakers = layoutSpeakers(layout);
    std::vector<double> weights;
    for (const Speaker& sp : speakers) weights.push_back(sp.loudnessWeight);
    const bool native = !scene && channels > 2;
    LoudnessMeter nativeMeter(kRate), stereoMeter(kRate);
    double nativePeak = 0, stereoPeak = 0;
    std::vector<float> mix(size_t(kFrame) * size_t(channels)), stereo(size_t(kFrame) * 2);
    std::vector<int32_t> pcm(size_t(kFrame) * 2);
    int written = 0;
    auto writeUnits = [&](bool last) -> bool {
        // A temporal unit once every substream has its frame.
        while (std::all_of(enc.begin(), enc.end(), [](const auto& e) { return e->ready(); })) {
            const bool end = written == frames - 1;
            QByteArray unit;
            for (size_t k = 0; k < enc.size(); ++k)
                unit += obu(kAudioFrameId0 + int(k), enc[k]->take(), end ? trimEnd : 0);
            if (f.write(unit) != unit.size()) return false;
            sampleSizes.push_back(uint32_t(unit.size()));
            ++written;
        }
        return !last || written == frames;
    };
    std::string why;
    for (int fr = 0; fr < frames; ++fr) {
        const int64_t done = int64_t(fr) * kFrame;
        const int n = int(std::min<int64_t>(kFrame, total - done));
        std::fill(mix.begin(), mix.end(), 0.f);
        if (channels > 2) mixer.mixLayout(p, mixSeq, firstSample + done, n, mix.data());
        else mixer.mix(p, mixSeq, firstSample + done, n, mix.data());
        iamfDownmixToStereo(layout, mix.data(), n, stereo.data());
        if (native) {
            nativeMeter.addChannels(mix.data(), n, channels, weights.data());
            for (int i = 0; i < n * channels; ++i) nativePeak = std::max(nativePeak, double(std::fabs(mix[size_t(i)])));
        }
        stereoMeter.add(stereo.data(), n);
        for (int i = 0; i < n * 2; ++i) stereoPeak = std::max(stereoPeak, double(std::fabs(stereo[size_t(i)])));
        for (size_t k = 0; k < subs.size(); ++k) {
            const int w = int(subs[k].size());
            for (int i = 0; i < kFrame; ++i)
                for (int c = 0; c < w; ++c) {
                    const double v = i < n ? double(mix[size_t(i) * size_t(channels) + size_t(subs[k][size_t(c)])]) : 0.0;
                    pcm[size_t(i * w + c)] = int32_t(std::lround(std::clamp(v, -1.0, 8388607.0 / 8388608.0) * 8388608.0));
                }
            if (!enc[k]->encode(pcm.data(), why)) return fail(why);
        }
        if (!writeUnits(false)) return fail("Cannot write " + path + " (is the disk full?)");
        if (progress && fr % 10 == 0 && !progress(double(fr) / double(frames))) return fail("Stopped");
    }
    for (auto& e : enc)
        if (!e->finish(why)) return fail(why);
    if (!writeUnits(true)) return fail("The encoder lost frames");

    // The descriptors: the sequence header, the codec config, the audio element, the mix presentation.
    std::vector<IamfLoudness> loudness;
    auto measured = [](const char* name, const LoudnessResult& r, double peak) {
        IamfLoudness l;
        l.layout = name;
        l.integrated = r.valid ? r.integrated : -128;
        l.truePeak = r.valid ? r.truePeakDb : 20 * std::log10(std::max(peak, 1e-7));
        l.digitalPeak = 20 * std::log10(std::max(peak, 1e-7));
        return l;
    };
    if (native) loudness.push_back(measured(layout.c_str(), nativeMeter.result(), nativePeak));
    loudness.push_back(measured("stereo", stereoMeter.result(), stereoPeak));

    QByteArray config;
    {
        QByteArray sh;
        sh.append("iamf", 4);
        put8(sh, 0);  // simple profile
        put8(sh, 0);
        config += obu(kSequenceHeader, sh);
    }
    {
        QByteArray cc;
        putLeb(cc, 0);  // codec_config_id
        cc.append(flac ? "fLaC" : "ipcm", 4);
        putLeb(cc, kFrame);
        put16(cc, 0);  // audio_roll_distance
        if (flac) {
            // A METADATA_BLOCK_HEADER (the last) and STREAMINFO: block sizes, 48 kHz, 24 bits; sizes, length and MD5
            // unknown (zero), as FLAC allows; each substream's frames say their own channels.
            put8(cc, 0x80);
            put8(cc, 0), put16(cc, 34);
            put16(cc, kFrame), put16(cc, kFrame);
            put8(cc, 0), put16(cc, 0), put8(cc, 0), put16(cc, 0);
            const uint64_t bits = uint64_t(kRate) << 44 | uint64_t(coupled ? 1 : 0) << 41 | uint64_t(23) << 36;
            put64(cc, bits);
            cc.append(QByteArray(16, '\0'));
        } else {
            put8(cc, 1);  // little-endian
            put8(cc, 24);
            put32(cc, kRate);
        }
        config += obu(kCodecConfig, cc);
    }
    {
        QByteArray ae;
        putLeb(ae, 1);  // audio_element_id
        put8(ae, (scene ? 1u : 0u) << 5);
        putLeb(ae, 0);  // codec_config_id
        putLeb(ae, subs.size());
        for (size_t k = 0; k < subs.size(); ++k) putLeb(ae, k);
        putLeb(ae, 0);  // num_parameters
        if (scene) {
            putLeb(ae, 0);  // ambisonics_mode: mono, a substream per component
            put8(ae, unsigned(channels));
            put8(ae, unsigned(subs.size()));
            for (int c = 0; c < channels; ++c) put8(ae, unsigned(c));
        } else {
            put8(ae, 1u << 5);  // one layer
            put8(ae, unsigned(iamfLoudspeakerLayout(layout)) << 4);
            put8(ae, unsigned(subs.size()));
            put8(ae, unsigned(coupled));
        }
        config += obu(kAudioElement, ae);
    }
    {
        const std::string title = settings.title.empty() ? s.name : settings.title;
        QByteArray mp;
        putLeb(mp, 2);  // mix_presentation_id
        putLeb(mp, 1);  // count_label
        putString(mp, "en-us");
        putString(mp, title);
        putLeb(mp, 1);  // num_sub_mixes
        putLeb(mp, 1);  // num_audio_elements
        putLeb(mp, 1);  // audio_element_id
        putString(mp, title);
        put8(mp, (settings.binaural ? 1u : 0u) << 6);  // headphones_rendering_mode
        putLeb(mp, 0);                                 // rendering_config_extension_size
        // The element's mix gain and the output's: 0 dB, no parameter blocks.
        for (int id : {100, 101}) {
            putLeb(mp, uint64_t(id));
            putLeb(mp, kRate);
            put8(mp, 0x80);  // param_definition_mode 1
            put16(mp, 0);
        }
        putLeb(mp, loudness.size());
        for (const IamfLoudness& l : loudness) {
            put8(mp, 2u << 6 | unsigned(iamfSoundSystem(l.layout)) << 2);  // loudspeakers, by sound system
            put8(mp, 1);  // info_type: with the true peak
            put16(mp, uint16_t(q78(l.integrated)));
            put16(mp, uint16_t(q78(l.digitalPeak)));
            put16(mp, uint16_t(q78(l.truePeak)));
        }
        config += obu(kMixPresentation, mp);
    }

    if (bare) {
        // The frames written so far move up behind the descriptors.
        f.flush();
        const qint64 frameBytes = f.size();
        f.close();
        if (!f.open(QIODevice::ReadWrite)) return fail("Cannot write " + path);
        if (!f.resize(frameBytes + config.size())) return fail("Cannot write " + path + " (is the disk full?)");
        const qint64 step = 1 << 20;
        for (qint64 at = frameBytes; at > 0;) {
            const qint64 from = std::max<qint64>(0, at - step);
            if (!f.seek(from)) return fail("Cannot write " + path);
            const QByteArray chunk = f.read(at - from);
            if (chunk.size() != at - from || !f.seek(from + config.size()) || f.write(chunk) != chunk.size())
                return fail("Cannot write " + path + " (is the disk full?)");
            at = from;
        }
        if (!f.seek(0) || f.write(config) != config.size()) return fail("Cannot write " + path);
    } else {
        const qint64 mdatEnd = f.pos();
        QByteArray header;
        if (mdatEnd - (mdatAt + 8) <= 0xffffffffLL) {
            put32(header, 8);
            header.append("free", 4);
            put32(header, uint32_t(mdatEnd - (mdatAt + 8)));
            header.append("mdat", 4);
        } else {
            put32(header, 1);
            header.append("mdat", 4);
            put64(header, uint64_t(mdatEnd - mdatAt));
        }
        if (!f.seek(mdatAt) || f.write(header) != header.size() || !f.seek(mdatEnd)) return fail("Cannot write " + path);
        const uint64_t media = uint64_t(frames) * kFrame;
        // (Version 0 boxes, 32-bit times, as every reader takes, unless a day of sound needs more.)
        const int v = media > 0xffffffffu ? 1 : 0;
        auto putTime = [&](QByteArray& b, uint64_t t) { v ? put64(b, t) : put32(b, uint32_t(t)); };
        QByteArray mvhd;
        putTime(mvhd, 0), putTime(mvhd, 0);
        put32(mvhd, kRate);
        putTime(mvhd, uint64_t(total));
        put32(mvhd, 0x00010000), put16(mvhd, 0x0100), put16(mvhd, 0), put32(mvhd, 0), put32(mvhd, 0);
        mvhd += matrix();
        for (int i = 0; i < 6; ++i) put32(mvhd, 0);
        put32(mvhd, 2);
        QByteArray tkhd;
        putTime(tkhd, 0), putTime(tkhd, 0);
        put32(tkhd, 1), put32(tkhd, 0);
        putTime(tkhd, uint64_t(total));
        put32(tkhd, 0), put32(tkhd, 0);
        put16(tkhd, 0), put16(tkhd, 1), put16(tkhd, 0x0100), put16(tkhd, 0);
        tkhd += matrix();
        put32(tkhd, 0), put32(tkhd, 0);
        // (The padding at the end cut by an edit, as the last frame's trimming says.)
        QByteArray elst;
        put32(elst, 1);
        putTime(elst, uint64_t(total)), putTime(elst, 0);
        put16(elst, 1), put16(elst, 0);
        QByteArray mdhd;
        putTime(mdhd, 0), putTime(mdhd, 0);
        put32(mdhd, kRate);
        putTime(mdhd, media);
        put16(mdhd, 0x55c4), put16(mdhd, 0);  // "und"
        QByteArray hdlr;
        put32(hdlr, 0);
        hdlr.append("soun", 4);
        put32(hdlr, 0), put32(hdlr, 0), put32(hdlr, 0);
        hdlr.append("SoundHandler\0", 13);
        QByteArray smhd;
        put32(smhd, 0);
        QByteArray dref;
        put32(dref, 1);
        dref += fullBox("url ", 0, 1, {});
        // The sample entry: an audio one with no channel count or rate of its own (the descriptors give them).
        QByteArray entry;
        entry.append(QByteArray(6, '\0'));
        put16(entry, 1);  // data_reference_index
        put32(entry, 0), put32(entry, 0);
        put16(entry, 0), put16(entry, 16);  // channelcount, samplesize
        put16(entry, 0), put16(entry, 0);
        put32(entry, 0);  // samplerate
        QByteArray iacb;
        put8(iacb, 1);  // configurationVersion
        putLeb(iacb, uint64_t(config.size()));
        iacb += config;
        entry += box("iacb", iacb);
        QByteArray stsd;
        put32(stsd, 1);
        stsd += box("iamf", entry);
        QByteArray stts;
        put32(stts, 1), put32(stts, uint32_t(frames)), put32(stts, kFrame);
        QByteArray stsc;
        put32(stsc, 1), put32(stsc, 1), put32(stsc, uint32_t(frames)), put32(stsc, 1);
        QByteArray stsz;
        put32(stsz, 0), put32(stsz, uint32_t(frames));
        for (uint32_t sz : sampleSizes) put32(stsz, sz);
        QByteArray stco;  // one chunk: every frame, from the start of the mdat
        put32(stco, 1), put32(stco, uint32_t(mdatAt + 16));
        const QByteArray stbl = fullBox("stsd", 0, 0, stsd) + fullBox("stts", 0, 0, stts) + fullBox("stsc", 0, 0, stsc) +
                                fullBox("stsz", 0, 0, stsz) + fullBox("stco", 0, 0, stco);
        const QByteArray minf = fullBox("smhd", 0, 0, smhd) + box("dinf", fullBox("dref", 0, 0, dref)) + box("stbl", stbl);
        const QByteArray mdia = fullBox("mdhd", v, 0, mdhd) + fullBox("hdlr", 0, 0, hdlr) + box("minf", minf);
        const QByteArray trak = fullBox("tkhd", v, 3, tkhd) + box("edts", fullBox("elst", v, 0, elst)) + box("mdia", mdia);
        const QByteArray moov = box("moov", fullBox("mvhd", v, 0, mvhd) + box("trak", trak));
        if (f.write(moov) != moov.size()) return fail("Cannot write " + path + " (is the disk full?)");
    }
    if (!f.flush()) return fail("Cannot write " + path + " (is the disk full?)");
    f.close();
    finished = true;
    if (progress) progress(1);
    if (result) {
        result->path = path;
        result->layout = layout;
        result->codec = settings.codec;
        result->channels = channels;
        result->substreams = int(subs.size());
        result->coupled = coupled;
        result->samples = total;
        result->frames = frames;
        result->loudness = loudness;
    }
    return true;
}

namespace {

// Big-endian and LEB128 reading over a byte range; reading past the end gives zeros and marks it bad.
struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool bad = false;
    bool more() const { return p < end; }
    uint32_t u8() {
        if (p >= end) return bad = true, 0;
        return *p++;
    }
    uint32_t u16() {
        const uint32_t hi = u8();
        return hi << 8 | u8();
    }
    uint32_t u32() {
        const uint32_t hi = u16();
        return hi << 16 | u16();
    }
    uint64_t leb() {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            const uint32_t b = u8();
            v |= uint64_t(b & 0x7f) << (7 * i);
            if (!(b & 0x80)) break;
        }
        return v;
    }
    std::string str() {
        std::string s;
        while (more() && *p) s += char(*p++);
        u8();
        return s;
    }
    void skip(uint64_t n) { p = n > uint64_t(end - p) ? (bad = true, end) : p + n; }
};

double fromQ78(uint32_t v) { return double(int16_t(uint16_t(v))) / 256; }

// The descriptors in `data` (OBUs); the frames after them counted when `countFrames`.
bool parseObus(const uint8_t* data, size_t size, IamfInfo& out, bool countFrames, std::string& error) {
    Reader r{data, data + size};
    while (r.more()) {
        const uint32_t h = r.u8();
        const int type = int(h >> 3);
        const bool trimming = h & 2, extension = h & 1;
        const uint64_t len = r.leb();
        if (r.bad || len > uint64_t(r.end - r.p)) return (error = "A truncated OBU", false);
        Reader b{r.p, r.p + len};
        r.skip(len);
        if (trimming) {
            const uint64_t trimEnd = b.leb();
            b.leb();
            if (type >= kAudioFrame && type < kAudioFrameId0 + 18) out.trimmedAtEnd = int64_t(trimEnd);
        }
        if (extension) b.skip(b.leb());
        if (type == kSequenceHeader) {
            if (b.u32() != 0x69616d66) return (error = "Not IAMF (no 'iamf' in its sequence header)", false);
            out.primaryProfile = int(b.u8());
            out.additionalProfile = int(b.u8());
        } else if (type == kCodecConfig) {
            b.leb();
            const uint32_t id = b.u32();
            out.codec = std::string{char(id >> 24), char(id >> 16), char(id >> 8), char(id)};
            out.samplesPerFrame = int(b.leb());
            b.u16();
            if (out.codec == "fLaC") {
                b.u32();
                b.skip(10);
                const uint32_t v = b.u32();
                out.sampleRate = int(v >> 12);
                out.bitDepth = int((v >> 4 & 0x1f) + 1);
            } else if (out.codec == "ipcm") {
                b.u8();
                out.bitDepth = int(b.u8());
                out.sampleRate = int(b.u32());
            } else {
                out.sampleRate = kRate;
            }
        } else if (type == kAudioElement) {
            b.leb();
            out.sceneBased = (b.u8() >> 5) == 1;
            b.leb();
            const uint64_t n = b.leb();
            for (uint64_t i = 0; i < n && !b.bad; ++i) b.leb();
            const uint64_t params = b.leb();
            if (params) return (error = "Audio element parameters are not read here", false);
            if (out.sceneBased) {
                b.leb();
                out.ambisonicChannels = int(b.u8());
                out.substreams = int(b.u8());
            } else {
                b.u8();
                out.loudspeakerLayout = int(b.u8() >> 4);
                out.substreams = int(b.u8());
                out.coupled = int(b.u8());
            }
        } else if (type == kMixPresentation) {
            b.leb();
            const uint64_t labels = b.leb();
            if (labels > 255) return (error = "A mix presentation with too many labels", false);
            for (uint64_t i = 0; i < labels && !b.bad; ++i) b.str();
            for (uint64_t i = 0; i < labels && !b.bad; ++i) {
                const std::string t = b.str();
                if (i == 0) out.title = t;
            }
            const uint64_t subMixes = b.leb();
            for (uint64_t sm = 0; sm < subMixes && !b.bad; ++sm) {
                const uint64_t elements = b.leb();
                auto paramDefinition = [&] {
                    b.leb(), b.leb();
                    if (!(b.u8() & 0x80)) {
                        b.leb();
                        if (!b.leb())
                            for (uint64_t n = b.leb(), i = 0; i < n && !b.bad; ++i) b.leb();
                    }
                    b.u16();
                };
                for (uint64_t e = 0; e < elements && !b.bad; ++e) {
                    b.leb();
                    for (uint64_t i = 0; i < labels && !b.bad; ++i) b.str();
                    out.headphonesMode = int(b.u8() >> 6);
                    b.skip(b.leb());
                    paramDefinition();
                }
                paramDefinition();
                const uint64_t layouts = b.leb();
                for (uint64_t i = 0; i < layouts && !b.bad; ++i) {
                    const uint32_t lay = b.u8();
                    const int ss = (lay >> 6) == 2 ? int(lay >> 2 & 0xf) : (lay >> 6) == 3 ? -2 : -1;
                    const uint32_t info = b.u8();
                    IamfLoudness l;
                    l.layout = ss == -2 ? "binaural" : layoutForSoundSystem(ss);
                    l.integrated = fromQ78(b.u16());
                    l.digitalPeak = fromQ78(b.u16());
                    l.truePeak = info & 1 ? fromQ78(b.u16()) : l.digitalPeak;
                    if (info & 2)
                        for (uint32_t n = b.u8(), k = 0; k < n; ++k) b.u8(), b.u16();
                    if (info & ~3u) b.skip(b.leb());
                    out.loudnessSoundSystems.push_back(ss);
                    out.loudness.push_back(l);
                }
            }
        } else if (type == kAudioFrameId0 || (type == kAudioFrame && b.leb() == 0)) {
            if (!countFrames) return true;
            ++out.frames;
        }
        if (b.bad) return (error = "A truncated descriptor", false);
    }
    return true;
}

}  // namespace

bool readIamfInfo(const std::string& path, IamfInfo& out, std::string* error) {
    out = IamfInfo();
    std::string why;
    auto fail = [&](const std::string& w) {
        if (error) *error = w;
        return false;
    };
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) return fail("Cannot read " + path);
    const QByteArray head = f.peek(8);
    if (head.size() >= 8 && head.mid(4, 4) == "ftyp") {
        // An MP4: the 'iamf' sample entry's iacb box, and from the same track the number of samples (stsz) and the last
        // one, for its trimming. (Our files keep every sample in one chunk: the last one is the chunk's offset and the
        // sizes before it.)
        struct Track {
            QByteArray iacb;
            int samples = -1;
            qint64 lastAt = -1, chunkAt = -1;
            uint32_t lastSize = 0;
        } cur, iamf;
        std::function<bool(qint64, qint64, int)> walk;
        walk = [&](qint64 from, qint64 to, int depth) -> bool {
            if (depth > 8) return false;
            for (qint64 at = from; at + 8 <= to;) {
                if (!f.seek(at)) return false;
                const QByteArray h = f.read(16);
                if (h.size() < 8) return false;
                Reader r{reinterpret_cast<const uint8_t*>(h.data()), reinterpret_cast<const uint8_t*>(h.data()) + h.size()};
                uint64_t size = r.u32();
                const QByteArray type = h.mid(4, 4);
                qint64 body = at + 8;
                if (size == 1) {
                    r.skip(4);
                    size = uint64_t(r.u32()) << 32;
                    size |= r.u32();
                    body += 8;
                } else if (size == 0) {
                    size = uint64_t(to - at);
                }
                if (size < uint64_t(body - at) || size > uint64_t(to - at)) return false;
                const qint64 next = at + qint64(size);
                if (type == "trak") {
                    cur = Track();
                    if (!walk(body, next, depth + 1)) return false;
                    if (!cur.iacb.isEmpty() && iamf.iacb.isEmpty()) iamf = cur;
                } else if (type == "moov" || type == "mdia" || type == "minf" || type == "stbl") {
                    if (!walk(body, next, depth + 1)) return false;
                } else if (type == "stsd") {
                    // version and flags, the count, then the entry: 28 bytes of an audio entry before its boxes.
                    f.seek(body + 8);
                    const QByteArray e = f.read(8);
                    if (e.mid(4, 4) == "iamf" && next > body + 8 + 8 + 28) {
                        f.seek(body + 8 + 8 + 28);
                        const QByteArray in = f.read(std::min<qint64>(next - (body + 8 + 8 + 28), 1 << 20));
                        Reader r2{reinterpret_cast<const uint8_t*>(in.data()), reinterpret_cast<const uint8_t*>(in.data()) + in.size()};
                        const uint32_t len = r2.u32();
                        if (in.mid(4, 4) == "iacb" && len >= 8 && len <= uint32_t(in.size())) cur.iacb = in.mid(8, int(len) - 8);
                    }
                } else if (type == "stsz") {
                    f.seek(body + 4);
                    const QByteArray sz8 = f.read(8);
                    Reader r2{reinterpret_cast<const uint8_t*>(sz8.data()), reinterpret_cast<const uint8_t*>(sz8.data()) + sz8.size()};
                    const uint32_t fixed = r2.u32();
                    // (No more sizes than the box holds.)
                    const int64_t room = std::max<int64_t>(0, (next - (body + 12)) / 4);
                    const int64_t count = r2.u32();
                    cur.samples = int(fixed ? std::min<int64_t>(count, INT32_MAX) : std::min(count, room));
                    // The sizes before the last one, to find it (one chunk).
                    uint64_t before = 0;
                    if (fixed) before = uint64_t(fixed) * uint64_t(std::max(0, cur.samples - 1)), cur.lastSize = fixed;
                    else {
                        const QByteArray sizes = f.read(qint64(cur.samples) * 4);
                        Reader r3{reinterpret_cast<const uint8_t*>(sizes.data()), reinterpret_cast<const uint8_t*>(sizes.data()) + sizes.size()};
                        for (int i = 0; i < cur.samples; ++i) {
                            const uint32_t sz = r3.u32();
                            if (i + 1 < cur.samples) before += sz;
                            else cur.lastSize = sz;
                        }
                    }
                    cur.lastAt = before > uint64_t(f.size()) ? -1 : qint64(before);
                } else if (type == "co64" || type == "stco") {
                    f.seek(body + 8);
                    const QByteArray o = f.read(type == "co64" ? 8 : 4);
                    Reader r2{reinterpret_cast<const uint8_t*>(o.data()), reinterpret_cast<const uint8_t*>(o.data()) + o.size()};
                    uint64_t off = r2.u32();
                    if (type == "co64") off = off << 32 | r2.u32();
                    cur.chunkAt = off > uint64_t(f.size()) ? -1 : qint64(off);
                }
                at = next;
            }
            return true;
        };
        if (!walk(0, f.size(), 0) || iamf.iacb.isEmpty()) return fail("No IAMF track in " + path);
        Reader r{reinterpret_cast<const uint8_t*>(iamf.iacb.data()), reinterpret_cast<const uint8_t*>(iamf.iacb.data()) + iamf.iacb.size()};
        if (r.u8() != 1) return fail("An iacb box of an unknown version");
        const uint64_t n = r.leb();
        if (n > uint64_t(r.end - r.p)) return fail("A truncated iacb box");
        if (!parseObus(r.p, size_t(n), out, false, why)) return fail(why);
        out.frames = std::max(0, iamf.samples);
        if (iamf.lastAt >= 0 && iamf.chunkAt >= 0 && iamf.lastSize && iamf.chunkAt + iamf.lastAt + qint64(iamf.lastSize) <= f.size() &&
            f.seek(iamf.chunkAt + iamf.lastAt)) {
            const QByteArray lastSample = f.read(iamf.lastSize);
            IamfInfo last;
            if (parseObus(reinterpret_cast<const uint8_t*>(lastSample.data()), size_t(lastSample.size()), last, true, why))
                out.trimmedAtEnd = last.trimmedAtEnd;
        }
        return true;
    }
    // The bare stream, mapped rather than read (an hour of 7.1.4 PCM is gigabytes).
    const qint64 size = f.size();
    if (size <= 0) return fail(path + " is not IAMF");
    const uchar* mapped = f.map(0, size);
    QByteArray all;
    if (!mapped) {
        all = f.readAll();
        mapped = reinterpret_cast<const uchar*>(all.constData());
    }
    if (!parseObus(mapped, size_t(all.isEmpty() ? size : all.size()), out, true, why)) return fail(why);
    if (out.primaryProfile < 0) return fail(path + " is not IAMF");
    return true;
}

}  // namespace montage
