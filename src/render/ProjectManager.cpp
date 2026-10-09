#include "ProjectManager.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>

#include "Exporter.h"
#include "core/Interpretation.h"
#include "core/EditOps.h"
#include "core/ProjectIO.h"
#include "core/Transcript.h"
#include "media/Decoder.h"
#include "media/ImageSequence.h"
#include "media/Psd.h"

namespace montage {

namespace fs = std::filesystem;

namespace {

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
// With forward slashes on every system, so a collected project's paths read the same everywhere (Windows takes both).
std::string utf8(const fs::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

// The sequences kept: those chosen and every one they nest, at any depth.
std::set<Id> keptSequences(const Project& p, const std::vector<Id>& chosen) {
    std::set<Id> kept;
    std::vector<Id> todo = chosen;
    if (todo.empty())
        for (const Sequence& s : p.sequences) todo.push_back(s.id);
    while (!todo.empty()) {
        const Id id = todo.back();
        todo.pop_back();
        const Sequence* s = p.findSequence(id);
        if (!s || !kept.insert(id).second) continue;
        for (TrackRef r : allTracks(*s))
            for (const Clip& c : trackAt(*s, r)->clips)
                if (const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr; m && m->kind == MediaKind::Sequence) todo.push_back(m->sequenceId);
    }
    return kept;
}

// A file name not yet used in `taken`.
fs::path uniqueIn(const fs::path& dir, const std::string& stem, const std::string& ext, std::set<std::string>& taken) {
    for (int n = 1;; ++n) {
        const std::string name = stem + (n > 1 ? "-" + std::to_string(n) : "") + ext;
        if (!taken.count(name) && !fs::exists(dir / u8path(name))) {
            taken.insert(name);
            return dir / u8path(name);
        }
    }
}

}  // namespace

std::vector<Id> usedMedia(const Project& p, const std::vector<Id>& sequences) {
    std::set<Id> used;
    for (Id sid : keptSequences(p, sequences)) {
        const Sequence* s = p.findSequence(sid);
        for (TrackRef r : allTracks(*s))
            for (const Clip& c : trackAt(*s, r)->clips) {
                if (c.mediaId && p.findMedia(c.mediaId)) used.insert(c.mediaId);
                for (const Take& t : c.takes)  // an audition's other takes
                    if (p.findMedia(t.mediaId)) used.insert(t.mediaId);
            }
    }
    return {used.begin(), used.end()};
}

bool consolidateProject(const Project& p, const ConsolidateOptions& o, ConsolidateResult* result, const std::function<void(double)>& progress,
                        const std::atomic<bool>* cancel, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    if (o.folder.empty()) return fail("Choose a folder for the project");
    ConsolidateResult res;
    const fs::path folder = u8path(o.folder), mediaDir = folder / "Media";
    std::error_code ec;
    fs::create_directories(mediaDir, ec);
    if (ec) return fail("Cannot make " + utf8(mediaDir) + ": " + ec.message());

    const std::set<Id> kept = keptSequences(p, o.sequences);
    if (kept.empty()) return fail("There is no sequence to keep");
    const std::vector<Id> usedList = usedMedia(p, {kept.begin(), kept.end()});
    const std::set<Id> used(usedList.begin(), usedList.end());

    Project q = p;
    std::erase_if(q.sequences, [&](const Sequence& s) { return !kept.count(s.id); });
    if (!kept.count(q.activeSequence)) q.activeSequence = q.sequences.front().id;
    std::erase_if(q.media, [&](const MediaItem& m) {
        if (o.keepUnused || used.count(m.id)) return false;
        if (m.kind == MediaKind::Sequence) return !kept.count(m.sequenceId);
        return !(m.subclipOf && used.count(m.subclipOf));  // subclips of kept media stay
    });

    std::set<std::string> taken;
    std::map<std::string, std::string> copiedFiles;  // source file -> its copy
    const size_t total = std::max<size_t>(1, q.media.size());
    for (size_t i = 0; i < q.media.size(); ++i) {
        if (cancel && cancel->load()) return fail("Cancelled");
        MediaItem& m = q.media[i];
        if (progress) progress(double(i) / double(total));
        if (m.kind == MediaKind::Sequence || m.subclipOf || m.path.empty()) continue;
        ImageSequence frames;
        const bool imageSequence = parseImageSequencePath(m.path, frames);
        const Interpretation reading = interpretationOf(m);  // kept on the copy (Interpret Footage)
        if (!fs::exists(u8path(mediaFileOnDisk(m.path)), ec)) {
            res.missing.push_back(mediaFileOnDisk(m.path));
            continue;
        }
        const fs::path src = u8path(mediaFileOnDisk(m.path));
        if (imageSequence && !o.trim) {
            // Every frame into a folder of its own, named as they were.
            const fs::path folder = uniqueIn(mediaDir, utf8(src.stem()), "", taken);
            fs::create_directories(folder, ec);
            for (int n = frames.first; n <= frames.last; ++n) {
                const fs::path from = u8path(imageSequenceFrame(frames, n)), to = folder / from.filename();
                if (!fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec)) return fail("Cannot copy " + utf8(from) + ": " + ec.message());
                res.bytes += int64_t(fs::file_size(to, ec));
            }
            frames.pattern = utf8(folder / u8path(frames.pattern).filename());
            m.path = interpretedPath(imageSequencePath(frames), reading);
            m.proxyPath.clear();
            res.copied++;
            continue;
        }
        // The span the kept sequences use, in media seconds.
        double first = 1e18, last = -1e18;
        for (const Sequence& s : q.sequences)
            for (TrackRef r : allTracks(s))
                for (const Clip& c : trackAt(s, r)->clips) {
                    if (c.mediaId != m.id) continue;
                    const double fps = s.fpsValue(), a = c.sourceAt(0) / fps, b = c.sourceAt(double(c.duration)) / fps;
                    first = std::min({first, a, b});
                    last = std::max({last, a, b});
                }
        // A take of an audition is copied whole: its clip may be trimmed to any part of it.
        bool aTake = false;
        for (const Sequence& s : q.sequences)
            for (TrackRef r : allTracks(s))
                for (const Clip& c : trackAt(s, r)->clips)
                    for (size_t k = 0; k < c.takes.size(); ++k) aTake |= int(k) != c.take && c.takes[k].mediaId == m.id;
        const bool trimHere = o.trim && m.kind == MediaKind::Video && m.hasVideo && last > first && !aTake;
        if (trimHere) {
            const double from = std::max(0.0, first - o.handles), to = m.duration > 0 ? std::min(m.duration, last + o.handles) : last + o.handles;
            const bool prores = o.codec != "libx264";
            const fs::path dest = uniqueIn(mediaDir, utf8(src.stem()) + "_trim", prores ? ".mov" : ".mp4", taken);
            // Render the span through a sequence of the media's own size and rate.
            Project t = makeDefaultProject();
            Sequence& ts = *t.active();
            ts.width = m.width, ts.height = m.height;
            ts.fps = m.fps.valid() ? m.fps : Rational{30, 1};
            if (const std::string cs = m.colorOverride.empty() ? m.colorSpace : m.colorOverride; !cs.empty()) ts.colorSpace = cs;
            MediaItem copy = m;
            copy.id = t.newId();
            copy.transcript.reset(), copy.visual.reset(), copy.faces.reset();
            t.media.push_back(copy);
            const double fps = ts.fpsValue();
            if (!edit::placeMedia(t, ts, copy.id, 0, from * fps, to * fps, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok)
                return fail("Cannot place " + m.name + " to consolidate it");
            ExportSettings st;
            st.path = utf8(dest);
            st.videoCodec = o.codec;
            st.profile = prores ? "hq" : "";
            st.crf = 16;
            st.preset = "medium";
            st.audioCodec = m.hasAudio ? (prores ? "pcm_s24le" : "aac") : "none";
            std::string err;
            const size_t index = i;
            if (!exportSequence(
                    t, ts, st, [&](double f, FrameTime) { if (progress) progress((double(index) + f) / double(total)); }, cancel, &err))
                return fail("Consolidating " + m.name + " failed: " + err);
            MediaItem fresh;
            if (!probeMedia(st.path, fresh, &err)) return fail("Cannot read the consolidated " + m.name + ": " + err);
            m.path = st.path;
            m.duration = fresh.duration;
            m.videoCodec = fresh.videoCodec;
            m.audioCodec = fresh.audioCodec;
            m.proxyPath.clear();
            m.visual.reset();
            m.faces.reset();
            if (m.timecode >= 0) m.timecode += from;
            // What was said, moved to the new file's time.
            if (m.transcript) {
                auto shifted = std::make_shared<Transcript>(*m.transcript);
                std::erase_if(shifted->segments, [&](const TranscriptSegment& g) { return g.end <= from || g.start >= to; });
                for (TranscriptSegment& g : shifted->segments) {
                    g.start -= from, g.end -= from;
                    std::erase_if(g.words, [&](const TranscriptWord& w) { return w.end <= 0 || w.start >= to - from; });
                    for (TranscriptWord& w : g.words) w.start -= from, w.end -= from;
                }
                m.transcript = shifted;
            }
            // Clips and subclips now count from the span's start.
            for (Sequence& s : q.sequences)
                for (TrackRef r : allTracks(s))
                    for (Clip& c : trackAt(s, r)->clips)
                        if (c.mediaId == m.id) c.sourceIn -= from * s.fpsValue();
            for (MediaItem& sub : q.media)
                if (sub.subclipOf == m.id) {
                    sub.subclipIn = std::max(0.0, sub.subclipIn - from);
                    sub.subclipOut = std::max(sub.subclipIn, sub.subclipOut - from);
                }
            res.trimmed++;
            res.bytes += int64_t(fs::file_size(dest, ec));
        } else {
            // A Photoshop layer is its file's: the file is copied once for all its layers.
            std::string file;
            int layer = -1;
            const bool psdLayer = parsePsdLayerPath(m.path, file, layer);
            if (const auto done = copiedFiles.find(utf8(src)); done != copiedFiles.end()) {
                m.path = interpretedPath(psdLayer ? psdLayerPath(done->second, layer) : done->second, reading);
                m.proxyPath.clear();
                continue;
            }
            const fs::path dest = uniqueIn(mediaDir, utf8(src.stem()), utf8(src.extension()), taken);
            if (!fs::copy_file(src, dest, fs::copy_options::none, ec)) return fail("Cannot copy " + m.path + ": " + ec.message());
            copiedFiles[utf8(src)] = utf8(dest);
            m.path = interpretedPath(psdLayer ? psdLayerPath(utf8(dest), layer) : utf8(dest), reading);
            m.proxyPath.clear();
            res.copied++;
            res.bytes += int64_t(fs::file_size(dest, ec));
        }
    }
    const fs::path projectPath = folder / u8path(o.name + ".montage");
    std::string err;
    if (!saveProject(q, utf8(projectPath), &err)) return fail(err.empty() ? "Cannot save the project" : err);
    res.projectPath = utf8(projectPath);
    if (progress) progress(1.0);
    if (result) *result = res;
    return true;
}

}  // namespace montage
