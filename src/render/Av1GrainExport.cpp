// Montage — Carry Grain as Parameters (see Av1GrainExport.h).
#include "render/Av1GrainExport.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include "core/Effects.h"
#include "core/Provenance.h"
#include "media/ContentCredentials.h"
#include "render/Av1Headers.h"
#include "render/ColorSpace.h"
#include "render/HrdCheck.h"
#include "render/RateConversion.h"
#include "render/StreamingPackage.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

namespace montage {

namespace {

bool isAv1Encoder(const std::string& c) { return c == "libsvtav1" || c == "libaom-av1" || c == "librav1e"; }

// The topmost enabled clip of media with pictures at sequence frame `f`, or null.
const Clip* mediaClipAt(const Project& p, const Sequence& seq, FrameTime f) {
    for (size_t i = seq.videoTracks.size(); i-- > 0;) {
        const Track& t = seq.videoTracks[i];
        if (t.muted) continue;
        for (const Clip& c : t.clips) {
            if (!c.enabled || c.isGenerator() || f < c.start || f >= c.end()) continue;
            const MediaItem* m = p.findMedia(c.mediaId);
            if (m && m->hasVideo) return &c;
        }
    }
    return nullptr;
}

}  // namespace

bool checkFilmGrainSetting(const std::string& v, std::string* error) {
    if (v.empty() || v == "off" || v == "measure") return true;
    GrainProfile g;
    if (parseGrainProfile(v, g) && g.valid()) return true;
    if (error) *error = "Film grain is \"measure\", \"off\" or a grain profile (as Measure Grain gives it)";
    return false;
}

bool exportCarriesGrain(const ExportSettings& s) { return !s.filmGrain.empty() && s.filmGrain != "off"; }

bool checkGrainProfileName(const std::string& name, std::string* error) {
    const QString n = QString::fromStdString(name);
    std::string why;
    if (n.trimmed().isEmpty()) why = "A grain profile needs a name";
    else if (n.size() > 100 || n.contains(QLatin1Char('\n')) || n.contains(QLatin1Char('\r'))) why = "A grain profile's name is one line of up to 100 characters";
    else if (name == "measure" || name == "off") why = "A grain profile cannot be named \"" + name + "\"";
    if (!why.empty() && error) *error = why;
    return why.empty();
}

namespace {

QString grainLibraryPath() {
    const QString env = qEnvironmentVariable("MONTAGE_GRAIN_LIBRARY");
    return !env.isEmpty() ? env : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/Grain Profiles.json");
}

}  // namespace

std::map<std::string, std::string> userGrainProfiles() {
    std::map<std::string, std::string> out;
    QFile f(grainLibraryPath());
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("profiles")).toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
        GrainProfile g;
        const std::string text = it.value().toString().toStdString();
        if (checkGrainProfileName(it.key().toStdString()) && parseGrainProfile(text, g) && g.valid()) out[it.key().toStdString()] = text;
    }
    return out;
}

bool saveUserGrainProfile(const std::string& name, const GrainProfile& g, std::string* error) {
    if (!checkGrainProfileName(name, error)) return false;
    if (!g.valid()) {
        if (error) *error = "The grain profile is not a whole one";
        return false;
    }
    std::map<std::string, std::string> all = userGrainProfiles();
    all[name] = grainProfileText(g);
    QJsonObject profiles;
    for (const auto& [n, text] : all) profiles.insert(QString::fromStdString(n), QString::fromStdString(text));
    const QString path = grainLibraryPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(QJsonObject{{QStringLiteral("profiles"), profiles}}).toJson()) < 0 || !f.commit()) {
        if (error) *error = "Cannot write the grain profile library " + path.toStdString();
        return false;
    }
    return true;
}

bool resolveFilmGrainSetting(const Project& p, std::string& setting, std::string* error) {
    if (checkFilmGrainSetting(setting)) return true;
    if (auto it = p.grainProfiles.find(setting); it != p.grainProfiles.end()) {
        setting = it->second;
        return true;
    }
    const std::map<std::string, std::string> library = userGrainProfiles();
    if (auto it = library.find(setting); it != library.end()) {
        setting = it->second;
        return true;
    }
    if (error) *error = "No grain profile is named \"" + setting + "\" (in the project or the library); film grain is \"measure\", \"off\", a profile's name or its text";
    return false;
}

std::vector<Av1GrainShot> av1GrainShots(const Project& p, const Sequence& seq, FrameTime in, FrameTime out, const std::string& filmGrain,
                                        const std::atomic<bool>* cancel, const std::function<void(double)>& progress) {
    std::vector<Av1GrainShot> shots;
    if (out <= in) return shots;
    std::vector<FrameTime> starts{in};
    for (const FrameTime f : streamingShotChanges(seq))
        if (f > in && f < out) starts.push_back(f);
    GrainProfile given;
    const bool measure = filmGrain == "measure";
    const bool haveGiven = !measure && parseGrainProfile(filmGrain, given) && given.valid();
    std::map<Id, std::pair<bool, GrainProfile>> measured;  // (by clip: a clip over several shots is measured once)
    std::map<Id, std::string> why;
    for (size_t i = 0; i < starts.size(); ++i) {
        Av1GrainShot sh;
        sh.first = starts[i] - in;
        sh.end = (i + 1 < starts.size() ? starts[i + 1] : out) - in;
        if (haveGiven) {
            sh.profile = given, sh.source = "given";
        } else if (measure) {
            const Clip* c = mediaClipAt(p, seq, in + (sh.first + sh.end) / 2);
            if (!c) {
                sh.note = "no clip of media to measure";
            } else {
                auto it = measured.find(c->id);
                if (it == measured.end()) {
                    GrainProfile g;
                    std::string e;
                    const bool ok = measureClipGrain(p, seq, *c, {}, g, &e, {}, cancel) && g.valid();
                    it = measured.emplace(c->id, std::make_pair(ok, g)).first;
                    why[c->id] = ok ? std::string() : e.empty() ? "nothing to measure" : e;
                }
                if (it->second.first) sh.profile = it->second.second, sh.source = "measured";
                else sh.note = why[c->id];
            }
        }
        shots.push_back(std::move(sh));
        if (progress) progress(double(i + 1) / double(starts.size()));
        if (cancel && cancel->load()) break;
    }
    return shots;
}

void fitAv1GrainShots(std::vector<Av1GrainShot>& shots, const ColorSpace& working, const ColorSpace& delivery, const Av1GrainTarget& target) {
    for (Av1GrainShot& sh : shots) {
        sh.grain = Av1FilmGrain{};
        if (sh.source.empty()) continue;
        sh.grain = av1FilmGrainFrom(grainProfileIn(sh.profile, working, delivery), delivery, target);
        sh.grain.seed = av1GrainSeed(sh.first);
        if (!sh.grain.apply && sh.note.empty()) sh.note = "its grain is too faint to carry";
    }
}

bool carryAv1Grain(const std::string& path, const std::function<Av1FilmGrain(int64_t frame)>& grain, std::string* error, Av1GrainReport* report) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, path.c_str(), nullptr, nullptr) < 0) return fail("Cannot open " + path + " to write its grain");
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> inHeld(in, [](AVFormatContext* c) { avformat_close_input(&c); });
    if (avformat_find_stream_info(in, nullptr) < 0) return fail("Cannot read " + path);
    int vs = -1;
    for (unsigned i = 0; i < in->nb_streams; ++i)
        if (in->streams[i]->codecpar->codec_id == AV_CODEC_ID_AV1 && vs < 0) vs = int(i);
    if (vs < 0) return fail(path + " has no AV1 to carry grain in");
    // (written beside it under a name of the same kind, then put in its place: nothing is held open as it moves)
    const QFileInfo fi(QString::fromStdString(path));
    const std::string temp = (fi.dir().filePath(fi.completeBaseName() + ".grain-part." + fi.suffix())).toStdString();
    AVFormatContext* o = nullptr;
    if (avformat_alloc_output_context2(&o, nullptr, nullptr, temp.c_str()) < 0 || !o)
        return fail("Cannot write " + temp);
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> outHeld(o, [](AVFormatContext* c) {
        if (c && !(c->oformat->flags & AVFMT_NOFILE)) avio_closep(&c->pb);
        avformat_free_context(c);
    });
    Av1GrainWriter writer;
    std::string why;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        const AVStream* is = in->streams[i];
        AVStream* os = avformat_new_stream(o, nullptr);
        if (!os || avcodec_parameters_copy(os->codecpar, is->codecpar) < 0) return fail("Out of memory");
        // (the stream's own tag kept where the container takes it for that codec, as FFmpeg's own copy keeps it: a
        // timecode track's tmcd among them; else the container's own)
        unsigned int tag = is->codecpar->codec_tag, theirs = 0;
        if (tag && o->oformat->codec_tag && av_codec_get_id(o->oformat->codec_tag, tag) != is->codecpar->codec_id &&
            av_codec_get_tag2(o->oformat->codec_tag, is->codecpar->codec_id, &theirs))
            tag = 0;
        os->codecpar->codec_tag = tag;
        os->time_base = is->time_base;
        os->disposition = is->disposition;
        os->avg_frame_rate = is->avg_frame_rate, os->r_frame_rate = is->r_frame_rate;
        os->sample_aspect_ratio = is->sample_aspect_ratio;
        av_dict_copy(&os->metadata, is->metadata, 0);
        if (int(i) == vs) {
            std::vector<uint8_t> config(is->codecpar->extradata, is->codecpar->extradata + std::max(0, is->codecpar->extradata_size));
            if (!config.empty()) {
                if (!writer.configure(config, &why)) return fail("Cannot read the AV1's configuration: " + why);
                av_freep(&os->codecpar->extradata);
                os->codecpar->extradata = static_cast<uint8_t*>(av_mallocz(config.size() + AV_INPUT_BUFFER_PADDING_SIZE));
                if (!os->codecpar->extradata) return fail("Out of memory");
                std::copy(config.begin(), config.end(), os->codecpar->extradata);
                os->codecpar->extradata_size = int(config.size());
            }
        }
    }
    av_dict_copy(&o->metadata, in->metadata, 0);
    if (in->nb_chapters > 0) {
        o->chapters = static_cast<AVChapter**>(av_calloc(in->nb_chapters, sizeof(AVChapter*)));
        if (!o->chapters) return fail("Out of memory");
        for (unsigned i = 0; i < in->nb_chapters; ++i) {
            AVChapter* c = static_cast<AVChapter*>(av_mallocz(sizeof(AVChapter)));
            if (!c) return fail("Out of memory");
            const AVChapter* ic = in->chapters[i];
            c->id = ic->id, c->time_base = ic->time_base, c->start = ic->start, c->end = ic->end;
            av_dict_copy(&c->metadata, ic->metadata, 0);
            o->chapters[o->nb_chapters++] = c;
        }
    }
    if (!(o->oformat->flags & AVFMT_NOFILE) && avio_open(&o->pb, temp.c_str(), AVIO_FLAG_WRITE) < 0) return fail("Cannot write " + temp);
    AVDictionary* opts = nullptr;
    const std::string fmt = o->oformat->name ? o->oformat->name : "";
    if (fmt == "mp4" || fmt == "mov") av_dict_set(&opts, "movflags", "+faststart", 0);
    const int headerRc = avformat_write_header(o, &opts);
    av_dict_free(&opts);
    if (headerRc < 0) {
        outHeld.reset();
        QFile::remove(QString::fromStdString(temp));
        return fail("Cannot write " + temp);
    }
    AVPacket* pkt = av_packet_alloc();
    std::unique_ptr<AVPacket, void (*)(AVPacket*)> pktHeld(pkt, [](AVPacket* q) { av_packet_free(&q); });
    if (!pkt) return fail("Out of memory");
    int64_t shown = 0;
    bool ok = true;
    std::vector<uint8_t> rewritten;
    int read = 0;
    while (ok && (read = av_read_frame(in, pkt)) >= 0) {
        const AVStream* is = in->streams[pkt->stream_index];
        if (pkt->stream_index == vs) {
            // (each temporal unit shows one frame: the k-th in decoding order is the k-th shown)
            if (!writer.rewrite(pkt->data, size_t(pkt->size), shown, grain, rewritten, &why)) {
                ok = false;
                why = "Cannot write the grain into frame " + std::to_string(shown) + ": " + why;
            } else {
                AVPacket* np = av_packet_alloc();
                if (!np || av_new_packet(np, int(rewritten.size())) < 0) {
                    av_packet_free(&np);
                    ok = false, why = "Out of memory";
                } else {
                    std::copy(rewritten.begin(), rewritten.end(), np->data);
                    av_packet_copy_props(np, pkt);
                    av_packet_unref(pkt);
                    av_packet_move_ref(pkt, np);
                    av_packet_free(&np);
                }
            }
            ++shown;
        }
        if (ok) {
            av_packet_rescale_ts(pkt, is->time_base, o->streams[pkt->stream_index]->time_base);
            pkt->pos = -1;
            if (av_interleaved_write_frame(o, pkt) < 0) ok = false, why = "Cannot write " + temp;
        }
        av_packet_unref(pkt);
    }
    // (a read that failed before the end is not taken for the end: the file would lose what follows)
    if (ok && read != AVERROR_EOF) ok = false, why = "Cannot read " + path + " to its end";
    if (ok && av_write_trailer(o) < 0) ok = false, why = "Cannot finish " + temp;
    outHeld.reset();
    inHeld.reset();
    if (!ok) {
        QFile::remove(QString::fromStdString(temp));
        return fail(why);
    }
    // (the file as it was set aside until the new one is in its place, and put back if that fails)
    const QString target = QString::fromStdString(path), made = QString::fromStdString(temp), was = target + QStringLiteral(".grain-was");
    QFile::remove(was);
    if (!QFile::rename(target, was)) {
        QFile::remove(made);
        return fail("Cannot put " + temp + " in place of " + path);
    }
    if (!QFile::rename(made, target)) {
        QFile::rename(was, target);
        QFile::remove(made);
        return fail("Cannot put " + temp + " in place of " + path);
    }
    QFile::remove(was);
    if (report) {
        report->framesWithGrain = writer.framesWithGrain;
        report->framesByReference = writer.framesByReference;
        report->bytesAdded = writer.bytesAdded;
    }
    return true;
}

bool exportCarryingGrain(const Project& p, const Sequence& seq, const ExportSettings& s, const ExportProgress& progress,
                         const std::atomic<bool>* cancel, std::string* error, std::string* encoderUsed, LightLevels* light, bool* keptUnsigned,
                         Av1GrainReport* report, HrdReport* hrd) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (keptUnsigned) *keptUnsigned = false;
    if (hrd) *hrd = HrdReport{};
    std::string why;
    if (!checkFilmGrainSetting(s.filmGrain, &why)) return fail(why);
    if (!isAv1Encoder(s.videoCodec)) return fail("Grain is carried as parameters in AV1 only");
    // (a rate change's delivered frames, each the programme frame it shows first; checked before rendering)
    RateConversion rate;
    if (!planRateConversion(seq, s, rate, &why)) return fail(why);
    const bool signing = !s.credentialCertificate.empty();
    if (signing) {  // (refused before rendering rather than after)
        if (!contentCredentialsAvailable()) why = "This build of Montage cannot sign with Content Credentials";
        else if (!exportCarriesCredentials(s)) why = "Content Credentials cannot go in this kind of file";
        else if (!checkCredentialSigner({s.credentialCertificate, s.credentialKey, {}}, &why)) why = "Cannot sign with Content Credentials: " + why;
        if (!why.empty()) return fail(why);
    }
    const FrameTime in = s.in >= 0 ? s.in : 0;
    FrameTime out = s.out >= 0 ? s.out : seq.duration();
    if (seq.duration() > 0) out = std::min(out, seq.duration());
    if (out <= in) return fail("Nothing to export");
    auto report01 = [&](double f) {
        if (progress) progress(std::clamp(f, 0.0, 1.0), -1);
    };

    // Each shot's grain, then the sequence with it taken out (Replace Grain on each clip measured, putting none back).
    std::vector<Av1GrainShot> shots = av1GrainShots(p, seq, in, out, s.filmGrain, cancel, [&](double f) { report01(0.1 * f); });
    if (cancel && cancel->load()) return fail("Export cancelled");
    Project clean = p;
    Sequence cleanSeq = seq;
    {
        std::map<Id, std::string> profileOf;  // (each clip's, as text)
        const bool given = shots.empty() ? false : shots.front().source == "given";
        for (const Av1GrainShot& sh : shots) {
            if (sh.source.empty()) continue;
            if (const Clip* c = mediaClipAt(p, seq, in + (sh.first + sh.end) / 2)) profileOf[c->id] = grainProfileText(sh.profile);
        }
        for (Track& t : cleanSeq.videoTracks)
            for (Clip& c : t.clips) {
                if (c.isGenerator() || !c.enabled) continue;
                auto it = profileOf.find(c.id);
                if (it == profileOf.end() && !given) continue;
                const std::string text = it != profileOf.end() ? it->second : grainProfileText(shots.front().profile);
                Effect e = makeEffect(clean, "grain_match");
                e.params["source"] = 0.0;  // (the measured profile)
                e.params["mode"] = 1.0;    // (Replace Grain: the plate's own taken out)
                e.params["amount"] = 0.0;  // (and none put back: the decoder does that)
                e.strings["profile"] = text;
                c.effects.push_back(e);
            }
    }
    // (unsigned and unchecked: the file is signed and checked once its parameters are in it)
    ExportSettings plain = s;
    plain.filmGrain.clear();
    plain.credentialCertificate.clear(), plain.credentialKey.clear();
    plain.hrdCheck = false;
    if (!exportSequence(clean, cleanSeq, plain, [&](double f, FrameTime) { report01(0.1 + 0.85 * f); }, cancel, error, encoderUsed, nullptr, light,
                        nullptr))
        return false;

    // The parameters, fitted to the coded pictures as they are.
    Av1GrainTarget target;
    {
        AVFormatContext* f = nullptr;
        if (avformat_open_input(&f, s.path.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(f, nullptr) < 0) {
            avformat_close_input(&f);
            return fail("Cannot read " + s.path + " back");
        }
        const int v = av_find_best_stream(f, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (v < 0) {
            avformat_close_input(&f);
            return fail(s.path + " has no picture");
        }
        const AVCodecParameters* par = f->streams[v]->codecpar;
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(par->format));
        target.width = par->width;
        target.bitDepth = d ? d->comp[0].depth : 8;
        target.subX = d ? d->log2_chroma_w != 0 : true, target.subY = d ? d->log2_chroma_h != 0 : true;
        target.fullRange = par->color_range == AVCOL_RANGE_JPEG;
        if (par->color_space == AVCOL_SPC_BT2020_NCL || par->color_space == AVCOL_SPC_BT2020_CL) target.kr = 0.2627, target.kb = 0.0593;
        avformat_close_input(&f);
    }
    const ColorSpace* asked = findColorSpace(s.colorSpace);
    fitAv1GrainShots(shots, workingColorSpace(seq), asked ? *asked : sequenceColorSpace(seq), target);
    Av1GrainReport written;
    const FrameTime total = out - in;
    const int64_t delivered = rate.active() ? deliveredFrames(rate, total) : total;
    auto grainAt = [&](int64_t frame) {
        Av1FilmGrain g;
        if (frame < 0 || frame >= delivered) return g;
        // (the shot is the programme frame's; the seed the delivered frame's own, so no two frames share one)
        const int64_t shown = rate.active() ? deliveredPicture(rate, frame, total).a : frame;
        auto it = std::upper_bound(shots.begin(), shots.end(), FrameTime(shown), [](FrameTime f, const Av1GrainShot& sh) { return f < sh.end; });
        if (it == shots.end() || !it->grain.apply) return g;
        g = it->grain;
        g.seed = av1GrainSeed(frame);
        return g;
    };
    if (!carryAv1Grain(s.path, grainAt, error, &written)) {
        std::remove(s.path.c_str());
        return false;
    }
    if (signing) {
        const Provenance made = sequenceProvenance(p, seq, s.in, s.out);
        CredentialOptions o;
        o.allowAiTraining = s.credentialAllowAiTraining;
        o.sources = made.sources;
        if (!signContentCredentials(s.path, made, {s.credentialCertificate, s.credentialKey, {}}, o, &why)) {
            // (kept, unsigned, as any export whose signing fails: it can be signed again later)
            if (keptUnsigned) *keptUnsigned = true;
            return fail("Could not sign it with Content Credentials (" + why + "); the export is kept, unsigned, at " + s.path);
        }
    }
    if (exportNeedsHrdCheck(s, 0)) {  // (as exportSequence checks a file it makes)
        const HrdReport r = checkHrd(s.path, cancel);
        if (r.checked && r.signalled && !r.conforms())
            qWarning("The export %s does not keep its buffer model: %s", s.path.c_str(), r.summary().c_str());
        if (hrd) *hrd = r;
    }
    report01(1);
    if (report) {
        *report = written;
        report->shots = std::move(shots);
    }
    return true;
}

}  // namespace montage
