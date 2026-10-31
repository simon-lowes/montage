#include "Fdl.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <numeric>

#include "Compositor.h"
#include "core/EditOps.h"

#ifdef MONTAGE_WITH_FDL
#include <fdl/fdl_core.h>
#endif

namespace montage {

namespace {

constexpr const char* kCreator = "Montage";
constexpr const char* kCutIntent = "CUT";  // the framing intent of what the cut shows
constexpr int kMaxSize = 32768;             // the largest canvas read
constexpr int kMaxSequence = 8192;          // the largest sequence a template resizes to (as the sequence settings)

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

#ifdef MONTAGE_WITH_FDL

// A string the library hands over (thread-local, valid until the next call for the same field): copied.
std::string text(const char* s) { return s ? std::string(s) : std::string(); }

// Takes a string the library allocated.
std::string take(const char* s) {
    std::string out = text(s);
    fdl_free(const_cast<char*>(s));
    return out;
}

struct DocFree {
    void operator()(fdl_doc_t* d) const { fdl_doc_free(d); }
};
using Doc = std::unique_ptr<fdl_doc_t, DocFree>;

FdlRect rectOf(fdl_rect_t r) { return {r.x, r.y, r.width, r.height}; }

// Parses (and, with `validate`, checks) a document; null with `error` when it does not read.
Doc parseDoc(const std::string& json, bool validate, std::string* error) {
    fdl_parse_result_t r = fdl_doc_parse_json(json.data(), json.size());
    if (!r.doc) {
        const std::string e = take(r.error);
        if (error) *error = "Not an FDL: " + e;
        return nullptr;
    }
    Doc doc(r.doc);
    if (validate) {
        fdl_validation_result_t* v = fdl_doc_validate(doc.get());
        const uint32_t n = v ? fdl_validation_result_error_count(v) : 0;
        std::string problems;
        for (uint32_t i = 0; i < n; ++i) problems += (problems.empty() ? "" : "\n") + text(fdl_validation_result_error_at(v, i));
        if (v) fdl_validation_result_free(v);
        if (n) {
            if (error) *error = "The FDL breaks its format's rules:\n" + problems;
            return nullptr;
        }
    }
    return doc;
}

FdlCanvas canvasOf(fdl_canvas_t* cv) {
    FdlCanvas c;
    c.id = text(fdl_canvas_get_id(cv));
    c.label = text(fdl_canvas_get_label(cv));
    c.sourceId = text(fdl_canvas_get_source_canvas_id(cv));
    const fdl_dimensions_i64_t d = fdl_canvas_get_dimensions(cv);
    c.width = int(std::clamp<int64_t>(d.width, 0, INT32_MAX));
    c.height = int(std::clamp<int64_t>(d.height, 0, INT32_MAX));
    c.squeeze = fdl_canvas_get_anamorphic_squeeze(cv);
    if (!(c.squeeze > 0)) c.squeeze = 1;
    fdl_rect_t er;
    if (fdl_canvas_get_effective_rect(cv, &er)) {
        c.effective = true;
        c.effectiveRect = rectOf(er);
    }
    for (uint32_t k = 0, n = fdl_canvas_framing_decisions_count(cv); k < n; ++k) {
        fdl_framing_decision_t* fd = fdl_canvas_framing_decision_at(cv, k);
        FdlFraming f;
        f.id = text(fdl_framing_decision_get_id(fd));
        f.label = text(fdl_framing_decision_get_label(fd));
        f.intent = text(fdl_framing_decision_get_framing_intent_id(fd));
        f.rect = rectOf(fdl_framing_decision_get_rect(fd));
        fdl_rect_t pr;
        if (fdl_framing_decision_get_protection_rect(fd, &pr)) {
            f.protection = true;
            f.protectionRect = rectOf(pr);
        }
        c.framings.push_back(std::move(f));
    }
    return c;
}

FdlDocument documentOf(fdl_doc_t* d) {
    FdlDocument out;
    out.uuid = text(fdl_doc_get_uuid(d));
    out.creator = text(fdl_doc_get_fdl_creator(d));
    out.defaultIntent = text(fdl_doc_get_default_framing_intent(d));
    out.versionMajor = fdl_doc_get_version_major(d);
    out.versionMinor = fdl_doc_get_version_minor(d);
    for (uint32_t i = 0, n = fdl_doc_framing_intents_count(d); i < n; ++i) {
        fdl_framing_intent_t* fi = fdl_doc_framing_intent_at(d, i);
        FdlIntent it;
        it.id = text(fdl_framing_intent_get_id(fi));
        it.label = text(fdl_framing_intent_get_label(fi));
        const fdl_dimensions_i64_t a = fdl_framing_intent_get_aspect_ratio(fi);
        it.aspectW = int(a.width);
        it.aspectH = int(a.height);
        it.protection = fdl_framing_intent_get_protection(fi);
        out.intents.push_back(std::move(it));
    }
    for (uint32_t i = 0, n = fdl_doc_contexts_count(d); i < n; ++i) {
        fdl_context_t* cx = fdl_doc_context_at(d, i);
        FdlContext c;
        c.label = text(fdl_context_get_label(cx));
        c.creator = text(fdl_context_get_context_creator(cx));
        if (fdl_context_has_clip_id(cx)) {
            fdl_clip_id_t* id = fdl_context_clip_id(cx);
            c.clipName = text(fdl_clip_id_get_clip_name(id));
            if (fdl_clip_id_has_file(id)) c.clipFile = text(fdl_clip_id_get_file(id));
        }
        for (uint32_t k = 0, m = fdl_context_canvases_count(cx); k < m; ++k) c.canvases.push_back(canvasOf(fdl_context_canvas_at(cx, k)));
        out.contexts.push_back(std::move(c));
    }
    for (uint32_t i = 0, n = fdl_doc_canvas_templates_count(d); i < n; ++i) {
        fdl_canvas_template_t* ct = fdl_doc_canvas_template_at(d, i);
        FdlTemplate t;
        t.id = text(fdl_canvas_template_get_id(ct));
        t.label = text(fdl_canvas_template_get_label(ct));
        const fdl_dimensions_i64_t td = fdl_canvas_template_get_target_dimensions(ct);
        t.width = int(std::clamp<int64_t>(td.width, 0, INT32_MAX));
        t.height = int(std::clamp<int64_t>(td.height, 0, INT32_MAX));
        t.squeeze = fdl_canvas_template_get_target_anamorphic_squeeze(ct);
        out.templates.push_back(std::move(t));
    }
    return out;
}

#endif  // MONTAGE_WITH_FDL

// The media FDLs read so far (the monitors draw frame lines on every frame).
const FdlDocument* cachedDocument(const std::string& json) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const FdlDocument>> cache;
    thread_local std::shared_ptr<const FdlDocument> held;  // (the one returned, kept while the cache may be cleared)
    std::lock_guard<std::mutex> lock(mutex);
    if (auto it = cache.find(json); it != cache.end()) return (held = it->second).get();
    FdlDocument doc;
    // (one that does not read is kept as such, so it is not read again on every frame)
    held = parseFdl(json, doc, nullptr) ? std::make_shared<const FdlDocument>(std::move(doc)) : nullptr;
    if (cache.size() >= 1024) cache.erase(cache.begin());
    cache.emplace(json, held);
    return held.get();
}

double mediaWidth(const MediaItem& m) { return m.width > 0 ? m.width : 0; }
double mediaHeight(const MediaItem& m) { return m.height > 0 ? m.height : 0; }

// Whether the canvas is the media's picture: its size (as recorded, or desqueezed), or its shape (a scaled copy).
int sizeMatch(const FdlCanvas& c, const MediaItem& m) {
    const double mw = mediaWidth(m), mh = mediaHeight(m);
    if (mw <= 0 || mh <= 0 || c.width <= 0 || c.height <= 0) return 0;
    if (c.height == int(mh) && (c.width == int(mw) || std::fabs(c.width * c.squeeze - mw) <= 2)) return 2;
    const double shape = mw / mh;
    for (const double w : {double(c.width), c.width * c.squeeze})
        if (std::fabs(w / c.height - shape) <= 0.005 * shape) return 1;
    return 0;
}

bool namesMedia(const std::string& name, const MediaItem& m) {
    if (name.empty()) return false;
    const QString n = QFileInfo(QString::fromStdString(name)).fileName().toLower();
    const QString nBase = QFileInfo(n).completeBaseName();
    for (const std::string& s : {m.name, m.path}) {
        if (s.empty()) continue;
        const QFileInfo fi(QString::fromStdString(s));
        const QString f = fi.fileName().toLower(), base = fi.completeBaseName().toLower();
        if (n == f || n == base || nBase == base) return true;
    }
    return false;
}

QString num(double v) { return QString::number(v, 'g', 12); }

QJsonObject dimsJson(double w, double h) { return QJsonObject{{"width", w}, {"height", h}}; }
QJsonObject pointJson(double x, double y) { return QJsonObject{{"x", x}, {"y", y}}; }

// Rounded to 1/1000 of a pixel (the format's numbers are written to a few places).
double round3(double v) { return std::round(v * 1000) / 1000; }

}  // namespace

bool parseFdl(const std::string& json, FdlDocument& out, std::string* error) {
#ifdef MONTAGE_WITH_FDL
    Doc d = parseDoc(json, true, error);
    if (!d) return false;
    out = documentOf(d.get());
    // (sizes Montage can make pictures of: the format sets no limit)
    auto tooBig = [](int64_t w, int64_t h) { return w > kMaxSize || h > kMaxSize; };
    for (const FdlContext& c : out.contexts)
        for (const FdlCanvas& cv : c.canvases)
            if (tooBig(cv.width, cv.height)) {
                if (error) *error = "The FDL's canvas " + cv.id + " is larger than " + std::to_string(kMaxSize) + " pixels across";
                return false;
            }
    for (const FdlTemplate& t : out.templates)
        if (tooBig(t.width, t.height)) {
            if (error) *error = "The FDL's canvas template " + t.id + " is larger than " + std::to_string(kMaxSize) + " pixels across";
            return false;
        }
    return true;
#else
    (void)json;
    (void)out;
    if (error) *error = "This Montage was built without FDL support";
    return false;
#endif
}

bool readFdlFile(const std::string& path, std::string& json, FdlDocument& out, std::string* error) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "Can't read " + path;
        return false;
    }
    if (f.size() > 64 << 20) {
        if (error) *error = path + " is too big for an FDL";
        return false;
    }
    json = f.readAll().toStdString();
    return parseFdl(json, out, error);
}

namespace {

bool namesClip(const FdlContext& c) { return !c.clipName.empty() || !c.clipFile.empty(); }

// fdlContextFor, by size only where a canvas matches at least `minScore` (1 its shape, 2 its size).
int contextFor(const FdlDocument& doc, const MediaItem& m, int minScore) {
    for (size_t i = 0; i < doc.contexts.size(); ++i)
        if (namesMedia(doc.contexts[i].clipName, m) || namesMedia(doc.contexts[i].clipFile, m)) return int(i);
    if (minScore <= 0) return -1;
    int best = -1, bestScore = minScore - 1;
    for (size_t i = 0; i < doc.contexts.size(); ++i) {
        if (namesClip(doc.contexts[i])) continue;  // (a context naming other footage is that footage's)
        for (const FdlCanvas& c : doc.contexts[i].canvases)
            if (const int s = sizeMatch(c, m); s > bestScore) best = int(i), bestScore = s;
    }
    return best;
}

}  // namespace

int fdlContextFor(const FdlDocument& doc, const MediaItem& m, bool bySize) { return contextFor(doc, m, bySize ? 1 : 0); }

int fdlCanvasFor(const FdlContext& ctx, const MediaItem& m) {
    int best = -1, bestScore = 0;
    for (size_t i = 0; i < ctx.canvases.size(); ++i)
        if (const int s = sizeMatch(ctx.canvases[i], m); s > bestScore) best = int(i), bestScore = s;
    if (best >= 0) return best;
    // (footage of another shape than every canvas: none, as framing it by one would stretch it)
    if (mediaWidth(m) > 0 && mediaHeight(m) > 0) return -1;
    for (size_t i = 0; i < ctx.canvases.size(); ++i)
        if (ctx.canvases[i].id == ctx.canvases[i].sourceId) return int(i);
    return ctx.canvases.empty() ? -1 : 0;
}

int attachFdl(Project& p, const std::string& json, const std::vector<Id>& media, std::string* error) {
    FdlDocument doc;
    if (!parseFdl(json, doc, error)) return 0;
    if (doc.contexts.empty()) {
        if (error) *error = "The FDL has no contexts (no canvases to frame): it may be a canvas template on its own";
        return 0;
    }
    const QJsonObject root = QJsonDocument::fromJson(QByteArray::fromStdString(json)).object();
    const QJsonArray contexts = root.value("contexts").toArray();
    // A camera format's FDL (no context names a clip) frames every recording of its size; footage chosen, one of its shape too.
    const bool anyNamed = std::any_of(doc.contexts.begin(), doc.contexts.end(), namesClip);
    int attached = 0;
    for (MediaItem& m : p.media) {
        const bool listed = std::find(media.begin(), media.end(), m.id) != media.end();
        if (!media.empty() && !listed) continue;
        if (m.kind == MediaKind::Audio || m.kind == MediaKind::Sequence || m.subclipOf) continue;
        const int ci = contextFor(doc, m, listed ? 1 : anyNamed ? 0 : 2);
        if (ci < 0 || ci >= contexts.size()) continue;
        QJsonObject one = root;
        one["contexts"] = QJsonArray{contexts[ci]};
        m.fdl = QJsonDocument(one).toJson(QJsonDocument::Indented).toStdString();
        m.fdlFraming.clear();
        ++attached;
    }
    if (!attached && error)
        *error = media.empty() ? "The FDL describes none of the project's footage (no clip it names, nor any of its canvases' size)"
                               : "The FDL describes none of that footage (no clip it names, nor any of its canvases' size)";
    return attached;
}

bool mediaFdl(const MediaItem& m, FdlDocument& doc) {
    if (m.fdl.empty()) return false;
    const FdlDocument* d = cachedDocument(m.fdl);
    if (!d) return false;
    doc = *d;
    return true;
}

bool mediaFdlFraming(const MediaItem& m, FdlCanvas& canvas, FdlFraming& framing) {
    if (m.fdl.empty()) return false;
    const FdlDocument* d = cachedDocument(m.fdl);
    if (!d || d->contexts.empty()) return false;
    const FdlContext& ctx = d->contexts.front();
    const int ci = fdlCanvasFor(ctx, m);
    if (ci < 0) return false;
    canvas = ctx.canvases[size_t(ci)];
    const FdlFraming* pick = nullptr;
    for (const FdlFraming& f : canvas.framings)
        if (!m.fdlFraming.empty() && f.id == m.fdlFraming) pick = &f;
    if (!pick)
        for (const FdlFraming& f : canvas.framings)
            if (!pick && !d->defaultIntent.empty() && f.intent == d->defaultIntent) pick = &f;
    if (!pick && !canvas.framings.empty()) pick = &canvas.framings.front();
    if (!pick) {
        framing = FdlFraming{};
        framing.rect = {0, 0, double(canvas.width), double(canvas.height)};
        return canvas.width > 0 && canvas.height > 0;
    }
    framing = *pick;
    return canvas.width > 0 && canvas.height > 0;
}

std::vector<FrameLine> mediaFrameLines(const MediaItem& m) {
    std::vector<FrameLine> out;
    FdlCanvas canvas;
    FdlFraming f;
    if (!mediaFdlFraming(m, canvas, f)) return out;
    auto add = [&](int kind, const std::string& label, const FdlRect& r) {
        if (r.w <= 0 || r.h <= 0) return;
        const double x0 = r.x / canvas.width, y0 = r.y / canvas.height, x1 = (r.x + r.w) / canvas.width, y1 = (r.y + r.h) / canvas.height;
        out.push_back(FrameLine{kind, label, {x0, y0, x1, y0, x1, y1, x0, y1}});
    };
    if (canvas.effective) add(2, "", canvas.effectiveRect);
    if (f.protection) add(1, "", f.protectionRect);
    std::string label = f.label;
    if (const FdlDocument* d = label.empty() ? cachedDocument(m.fdl) : nullptr)
        for (const FdlIntent& i : d->intents)
            if (i.id == f.intent) label = !i.label.empty() ? i.label : std::to_string(i.aspectW) + ":" + std::to_string(i.aspectH);
    add(0, label, f.rect);
    return out;
}

std::vector<FrameLine> sequenceFrameLines(const Project& p, const Sequence& seq, FrameTime t) {
    if (seq.width <= 0 || seq.height <= 0) return {};
    for (int i = int(seq.videoTracks.size()) - 1; i >= 0; --i) {
        if (seq.videoTracks[size_t(i)].muted) continue;
        const Clip* c = edit::clipAt(seq, TrackRef{TrackKind::Video, i}, t);
        if (!c || !c->enabled || c->isGenerator() || !c->mediaId) continue;
        const MediaItem* m = p.findMedia(c->mediaId);
        if (!m || m->kind == MediaKind::Audio) continue;
        if (m->kind == MediaKind::Sequence) return {};
        // (the top recording hides those under it, with frame lines or not)
        std::vector<FrameLine> lines = mediaFrameLines(*m);
        for (FrameLine& l : lines)
            for (int k = 0; k < 4; ++k) {
                double x = 0, y = 0;
                if (!clipFrameToSequence(p, seq, *c, t, l.corners[size_t(2 * k)], l.corners[size_t(2 * k + 1)], x, y)) return {};
                l.corners[size_t(2 * k)] = x / seq.width;
                l.corners[size_t(2 * k + 1)] = y / seq.height;
            }
        return lines;
    }
    return {};
}

void paintFrameLines(QPainter& painter, const QRectF& picture, const std::vector<FrameLine>& lines, double scale) {
    if (lines.empty() || picture.width() <= 0) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    const double w = std::max(1.0, 1.5 * scale);
    for (const FrameLine& l : lines) {
        QPolygonF poly;
        for (int k = 0; k < 4; ++k)
            poly << QPointF(picture.left() + l.corners[size_t(2 * k)] * picture.width(), picture.top() + l.corners[size_t(2 * k + 1)] * picture.height());
        // A dark line under a light one: seen on any picture.
        QPen under(QColor(0, 0, 0, 150), w + 2 * scale);
        QPen over(l.kind == 0 ? QColor(255, 255, 255) : l.kind == 1 ? QColor(255, 196, 0) : QColor(160, 160, 160), w);
        if (l.kind == 1) over.setStyle(Qt::DashLine);
        if (l.kind == 2) over.setStyle(Qt::DotLine);
        for (const QPen& pen : {under, over}) {
            painter.setPen(pen);
            painter.drawPolygon(poly);
        }
        if (!l.label.empty() && l.kind == 0) {
            QFont f = painter.font();
            f.setPixelSize(std::max(8, int(std::lround(11 * scale))));
            painter.setFont(f);
            const QPointF at = poly[0] + QPointF(4 * scale, 4 * scale + f.pixelSize());
            painter.setPen(QColor(0, 0, 0, 180));
            painter.drawText(at + QPointF(scale, scale), QString::fromStdString(l.label));
            painter.setPen(Qt::white);
            painter.drawText(at, QString::fromStdString(l.label));
        }
    }
    painter.restore();
}

bool fitFdlTemplate(const std::string& source, const std::string& context, const std::string& canvasId, const std::string& framingId,
                    const std::string& templates, const std::string& templateId, FdlFit& out, std::string* error) {
#ifdef MONTAGE_WITH_FDL
    Doc src = parseDoc(source, false, error);
    if (!src) return false;
    Doc tdoc = parseDoc(templates, false, error);
    if (!tdoc) return false;
    fdl_context_t* cx = context.empty() ? (fdl_doc_contexts_count(src.get()) ? fdl_doc_context_at(src.get(), 0) : nullptr)
                                        : fdl_doc_context_find_by_label(src.get(), context.c_str());
    if (!cx) {
        if (error) *error = context.empty() ? "The FDL has no contexts" : "The FDL has no context labelled " + context;
        return false;
    }
    fdl_canvas_t* cv = canvasId.empty() ? (fdl_context_canvases_count(cx) ? fdl_context_canvas_at(cx, 0) : nullptr)
                                        : fdl_context_find_canvas_by_id(cx, canvasId.c_str());
    if (!cv) {
        if (error) *error = canvasId.empty() ? "The FDL's context has no canvases" : "The FDL has no canvas " + canvasId;
        return false;
    }
    fdl_framing_decision_t* fd = nullptr;
    if (!framingId.empty()) fd = fdl_canvas_find_framing_decision_by_id(cv, framingId.c_str());
    const std::string def = text(fdl_doc_get_default_framing_intent(src.get()));
    for (uint32_t k = 0, n = fdl_canvas_framing_decisions_count(cv); !fd && !def.empty() && k < n; ++k)
        if (text(fdl_framing_decision_get_framing_intent_id(fdl_canvas_framing_decision_at(cv, k))) == def) fd = fdl_canvas_framing_decision_at(cv, k);
    if (!fd && fdl_canvas_framing_decisions_count(cv)) fd = fdl_canvas_framing_decision_at(cv, 0);
    if (!fd) {
        if (error) *error = "The canvas has no framing decision to fit";
        return false;
    }
    fdl_canvas_template_t* ct = templateId.empty() ? (fdl_doc_canvas_templates_count(tdoc.get()) ? fdl_doc_canvas_template_at(tdoc.get(), 0) : nullptr)
                                                   : fdl_doc_canvas_template_find_by_id(tdoc.get(), templateId.c_str());
    if (!ct) {
        if (error) *error = templateId.empty() ? "The template FDL has no canvas templates" : "No canvas template " + templateId;
        return false;
    }
    // (a new canvas id the way the format makes them: 30 characters)
    const std::string newId = QUuid::createUuid().toString(QUuid::Id128).left(30).toUpper().toStdString();
    const std::string label = text(fdl_canvas_template_get_label(ct));
    const std::string ctxLabel = text(fdl_context_get_label(cx));
    fdl_template_result_t r = fdl_apply_canvas_template(ct, cv, fd, newId.c_str(), label.c_str(), ctxLabel.c_str(), kCreator);
    struct Free {
        fdl_template_result_t* r;
        ~Free() { fdl_template_result_free(r); }
    } freeLater{&r};
    if (!r.output_fdl) {
        if (error) *error = "The template could not be applied: " + text(r.error);
        return false;
    }
    fdl_context_t* ocx = fdl_doc_context_find_by_label(r.output_fdl, text(r.context_label).c_str());
    fdl_canvas_t* ocv = ocx ? fdl_context_find_canvas_by_id(ocx, text(r.canvas_id).c_str()) : nullptr;
    fdl_framing_decision_t* ofd = ocv ? fdl_canvas_find_framing_decision_by_id(ocv, text(r.framing_decision_id).c_str()) : nullptr;
    if (!ofd) {
        if (error) *error = "The template's result has no new canvas";
        return false;
    }
    const fdl_dimensions_i64_t od = fdl_canvas_get_dimensions(ocv);
    if (od.width <= 0 || od.height <= 0 || od.width > kMaxSize || od.height > kMaxSize) {
        if (error) *error = "The template makes a canvas of " + std::to_string(od.width) + " x " + std::to_string(od.height) + " pixels";
        return false;
    }
    out.width = int(od.width);
    out.height = int(od.height);
    out.squeeze = fdl_canvas_get_anamorphic_squeeze(ocv);
    out.framing = rectOf(fdl_framing_decision_get_rect(ofd));
    fdl_rect_t pr;
    out.protection = fdl_framing_decision_get_protection_rect(ofd, &pr);
    if (out.protection) out.protectionRect = rectOf(pr);
    out.canvasId = text(r.canvas_id);
    out.framingId = text(r.framing_decision_id);
    // Where the source lands: the pipeline's scale and shift (kept on the new canvas), from the corner of the layer it
    // keeps (what it preserves, else what it fits).
    double scale = 1;
    fdl_point_f64_t shift{0, 0};
    fdl_canvas_get_custom_attr_float(ocv, FDL_ATTR_SCALE_FACTOR, &scale);
    fdl_canvas_get_custom_attr_point_f64(ocv, FDL_ATTR_CONTENT_TRANSLATION, &shift);
    fdl_dimensions_f64_t dims{0, 0};
    fdl_point_f64_t anchor{0, 0};
    fdl_resolve_geometry_layer(cv, fd, fdl_canvas_template_get_fit_source(ct), &dims, &anchor);
    if (fdl_canvas_template_has_preserve_from_source_canvas(ct)) {
        fdl_dimensions_f64_t pd{0, 0};
        fdl_point_f64_t pa{0, 0};
        if (fdl_resolve_geometry_layer(cv, fd, fdl_canvas_template_get_preserve_from_source_canvas(ct), &pd, &pa) == 0 && !fdl_dimensions_is_zero(pd))
            anchor = pa;
    }
    double inSqueeze = fdl_canvas_get_anamorphic_squeeze(cv), outSqueeze = fdl_canvas_template_get_target_anamorphic_squeeze(ct);
    if (!(inSqueeze > 0)) inSqueeze = 1;
    if (!(outSqueeze > 0)) outSqueeze = 1;
    out.scaleX = scale * inSqueeze / outSqueeze;
    out.scaleY = scale;
    out.anchorX = anchor.x;
    out.anchorY = anchor.y;
    out.offsetX = shift.x;
    out.offsetY = shift.y;
    out.json = take(fdl_doc_to_json(r.output_fdl, 2));
    return true;
#else
    (void)source, (void)context, (void)canvasId, (void)framingId, (void)templates, (void)templateId, (void)out;
    if (error) *error = "This Montage was built without FDL support";
    return false;
#endif
}

namespace {

void setStill(Effect& e, const char* name, double v) { e.params[name] = Param(v); }

}  // namespace

std::vector<Id> frameClipsWithFdl(Project& p, Sequence& seq, const std::vector<Id>& clips, const std::string& templates,
                                  const std::string& templateId, bool resizeSequence, std::vector<std::string>* skipped,
                                  std::string* error) {
    std::vector<Id> done;
    std::vector<Clip*> chosen;
    for (Track& t : seq.videoTracks)
        for (Clip& c : t.clips)
            if (clips.empty() || std::find(clips.begin(), clips.end(), c.id) != clips.end()) chosen.push_back(&c);
    bool resized = false;
    std::string lastError;
    for (Clip* c : chosen) {
        const std::string name = c->name.empty() ? "Clip " + std::to_string(c->id) : c->name;
        auto skip = [&](const std::string& why) {
            if (skipped) skipped->push_back(name + ": " + why);
            lastError = why;
        };
        const MediaItem* m = c->mediaId && !c->isGenerator() ? p.findMedia(c->mediaId) : nullptr;
        if (!m || m->kind == MediaKind::Audio || m->kind == MediaKind::Sequence) continue;  // (not footage: nothing to frame)
        if (m->fdl.empty()) {
            skip("its footage has no FDL");
            continue;
        }
        const double mw = mediaWidth(*m), mh = mediaHeight(*m);
        FdlCanvas canvas;
        FdlFraming framing;
        if (mw <= 0 || mh <= 0 || !mediaFdlFraming(*m, canvas, framing)) {
            skip("its FDL frames no canvas of it");
            continue;
        }
        FdlFit fit;
        std::string err;
        if (!fitFdlTemplate(m->fdl, "", canvas.id, framing.id, templates.empty() ? m->fdl : templates, templateId, fit, &err)) {
            skip(err);
            continue;
        }
        if (fit.width <= 0 || fit.height <= 0) {
            skip("the template makes an empty canvas");
            continue;
        }
        // (no larger than the sequence settings allow: a template can ask for a canvas a frame of which is gigabytes)
        if (resizeSequence && !resized && (fit.width > kMaxSequence || fit.height > kMaxSequence)) {
            skip("the template makes a " + std::to_string(fit.width) + " x " + std::to_string(fit.height) + " canvas, larger than a sequence can be (" +
                 std::to_string(kMaxSequence) + " x " + std::to_string(kMaxSequence) + ")");
            continue;
        }
        if (resizeSequence && !resized) {
            seq.width = fit.width;
            seq.height = fit.height;
            resized = true;
        }
        const double SW = seq.width, SH = seq.height;
        // The new canvas fitted into the sequence's frame and centred (as it is, when they are the same size).
        const double f = std::min(SW / fit.width, SH / fit.height);
        const double offX = (SW - f * fit.width) / 2, offY = (SH - f * fit.height) / 2;
        // Sequence pixels per media pixel, and where the media's left and top edges land.
        const double kx = f * fit.scaleX * canvas.width / mw, ky = f * fit.scaleY * canvas.height / mh;
        const double x0 = offX + f * (fit.offsetX - fit.scaleX * fit.anchorX), y0 = offY + f * (fit.offsetY - fit.scaleY * fit.anchorY);
        Effect& mo = c->motion;
        setStill(mo, "fit", 3);
        setStill(mo, "scale", 100);
        setStill(mo, "scale_x", 100 * kx);
        setStill(mo, "scale_y", 100 * ky);
        setStill(mo, "anchor_x", 0);
        setStill(mo, "anchor_y", 0);
        setStill(mo, "rotation", 0);
        setStill(mo, "flip_h", 0);
        setStill(mo, "flip_v", 0);
        // (X = SW / 2 + pos_x + (u - mw / 2) * kx)
        setStill(mo, "pos_x", x0 - SW / 2 + mw / 2 * kx);
        setStill(mo, "pos_y", y0 - SH / 2 + mh / 2 * ky);
        // Cropped to the new canvas: what the template cuts away stays away.
        const double uL = (offX - x0) / kx, uR = (offX + f * fit.width - x0) / kx;
        const double vT = (offY - y0) / ky, vB = (offY + f * fit.height - y0) / ky;
        setStill(mo, "crop_left", 100 * std::clamp(uL / mw, 0.0, 1.0));
        setStill(mo, "crop_right", 100 * std::clamp(1 - uR / mw, 0.0, 1.0));
        setStill(mo, "crop_top", 100 * std::clamp(vT / mh, 0.0, 1.0));
        setStill(mo, "crop_bottom", 100 * std::clamp(1 - vB / mh, 0.0, 1.0));
        done.push_back(c->id);
    }
    if (done.empty() && error) *error = lastError.empty() ? "No footage clips to frame" : lastError;
    return done;
}

namespace {

// A context's JSON for a clip: its canvas (the media's FDL's, else the media's size) at `width` x `height`, with the
// cut's framing decision; `intents` gets the framing intents it uses that are not there yet.
bool clipContext(const Project& p, const Sequence& seq, const Clip& c, int width, int height, QJsonObject& ctx, QJsonArray& intents,
                 std::string& defaultIntent, std::string* error) {
    const MediaItem* m = c.mediaId && !c.isGenerator() ? p.findMedia(c.mediaId) : nullptr;
    if (!m || m->kind == MediaKind::Audio || m->kind == MediaKind::Sequence) {
        if (error) *error = "The clip shows no recording";
        return false;
    }
    const double mw = mediaWidth(*m), mh = mediaHeight(*m);
    if (mw <= 0 || mh <= 0) {
        if (error) *error = "The clip's footage has no picture size";
        return false;
    }
    // Its canvas: from the media's FDL (as recorded, with its framing decisions), else the picture as it is.
    QJsonObject canvas;
    std::vector<QJsonObject> mediaIntents;
    double cw = mw, ch = mh, squeeze = 1;
    FdlCanvas fc;
    FdlFraming ff;
    if (mediaFdlFraming(*m, fc, ff)) {
        const QJsonObject root = QJsonDocument::fromJson(QByteArray::fromStdString(m->fdl)).object();
        const QJsonObject mctx = root.value("contexts").toArray().at(0).toObject();
        for (const QJsonValue& v : mctx.value("canvases").toArray())
            if (v.toObject().value("id").toString().toStdString() == fc.id) canvas = v.toObject();
        // (only this canvas is written: it stands as its own source)
        canvas["source_canvas_id"] = canvas.value("id");
        for (const QJsonValue& v : root.value("framing_intents").toArray()) mediaIntents.push_back(v.toObject());
        if (defaultIntent.empty()) defaultIntent = root.value("default_framing_intent").toString().toStdString();
        cw = fc.width;
        ch = fc.height;
        squeeze = fc.squeeze;
        // (its clip_id, when it names this footage: an FDL attached by size may name another clip)
        if (const QJsonValue cid = mctx.value("clip_id");
            cid.isObject() && (namesMedia(cid.toObject().value("clip_name").toString().toStdString(), *m) ||
                               namesMedia(cid.toObject().value("file").toString().toStdString(), *m)))
            ctx["clip_id"] = cid;
    }
    if (canvas.isEmpty()) canvas = QJsonObject{{"label", QString::fromStdString(m->name)}, {"id", "1"}, {"source_canvas_id", "1"},
                                               {"dimensions", dimsJson(mw, mh)}, {"anamorphic_squeeze", 1.0}, {"framing_decisions", QJsonArray()}};
    const int W = width > 0 ? width : int(std::lround(cw)), H = height > 0 ? height : int(std::lround(ch));
    const double sx = W / cw, sy = H / ch;
    if (W != int(std::lround(cw)) || H != int(std::lround(ch))) {
        // Another size (a plate written at the footage's decoded size): its framing scaled, its squeeze what keeps the shape.
        auto scaleRect = [&](QJsonObject& o, const char* dimsKey, const char* anchorKey) {
            if (o.contains(dimsKey)) {
                const QJsonObject d = o.value(dimsKey).toObject();
                o[dimsKey] = dimsJson(round3(d.value("width").toDouble() * sx), round3(d.value("height").toDouble() * sy));
            }
            if (o.contains(anchorKey)) {
                const QJsonObject a = o.value(anchorKey).toObject();
                o[anchorKey] = pointJson(round3(a.value("x").toDouble() * sx), round3(a.value("y").toDouble() * sy));
            }
        };
        canvas["dimensions"] = dimsJson(W, H);
        if (canvas.contains("effective_dimensions")) {
            // (whole pixels, rounded outward so the framing inside it stays inside)
            const QJsonObject d = canvas.value("effective_dimensions").toObject();
            const QJsonObject a = canvas.value("effective_anchor_point").toObject();
            const double ax = a.value("x").toDouble(), ay = a.value("y").toDouble();
            const double x0 = std::max(0.0, std::floor(ax * sx + 1e-6)), y0 = std::max(0.0, std::floor(ay * sy + 1e-6));
            const double x1 = std::min(double(W), std::ceil((ax + d.value("width").toDouble()) * sx - 1e-6));
            const double y1 = std::min(double(H), std::ceil((ay + d.value("height").toDouble()) * sy - 1e-6));
            canvas["effective_dimensions"] = dimsJson(x1 - x0, y1 - y0);
            if (canvas.contains("effective_anchor_point")) canvas["effective_anchor_point"] = pointJson(x0, y0);
        }
        canvas.remove("photosite_dimensions");
        canvas.remove("physical_dimensions");
        squeeze = std::round(cw * squeeze * H / (ch * W) * 10000) / 10000;
        canvas["anamorphic_squeeze"] = squeeze;
        QJsonArray fds;
        for (const QJsonValue& v : canvas.value("framing_decisions").toArray()) {
            QJsonObject fd = v.toObject();
            scaleRect(fd, "dimensions", "anchor_point");
            scaleRect(fd, "protection_dimensions", "protection_anchor_point");
            fds.append(fd);
        }
        canvas["framing_decisions"] = fds;
    }
    // What the cut shows: the sequence's frame through the clip's transform at its first frame, within its crop.
    double u0 = 1, v0 = 1, u1 = 0, v1 = 0;
    for (const auto& [x, y] : {std::pair{0.0, 0.0}, {double(seq.width), 0.0}, {double(seq.width), double(seq.height)}, {0.0, double(seq.height)}}) {
        double u = 0, v = 0;
        if (!sequenceToClipFrame(p, seq, c, c.start, x, y, u, v)) {
            if (error) *error = "The clip has no picture";
            return false;
        }
        u0 = std::min(u0, u), u1 = std::max(u1, u), v0 = std::min(v0, v), v1 = std::max(v1, v);
    }
    const Effect& mo = c.motion;
    u0 = std::max({u0, 0.0, std::clamp(mo.p("crop_left", 0) / 100, 0.0, 1.0)});
    u1 = std::min({u1, 1.0, 1 - std::clamp(mo.p("crop_right", 0) / 100, 0.0, 1.0)});
    v0 = std::max({v0, 0.0, std::clamp(mo.p("crop_top", 0) / 100, 0.0, 1.0)});
    v1 = std::min({v1, 1.0, 1 - std::clamp(mo.p("crop_bottom", 0) / 100, 0.0, 1.0)});
    // (and within the canvas's effective area: a framing decision lies inside it)
    if (canvas.contains("effective_dimensions")) {
        const QJsonObject d = canvas.value("effective_dimensions").toObject(), a = canvas.value("effective_anchor_point").toObject();
        const double ex = a.value("x").toDouble() / W, ey = a.value("y").toDouble() / H;
        u0 = std::max(u0, ex), v0 = std::max(v0, ey);
        u1 = std::min(u1, ex + d.value("width").toDouble() / W), v1 = std::min(v1, ey + d.value("height").toDouble() / H);
    }
    if (u1 <= u0 || v1 <= v0) {
        if (error) *error = "The clip shows none of its picture in the sequence's frame";
        return false;
    }
    const std::string canvasId = canvas.value("id").toString().toStdString();
    QJsonArray fds = canvas.value("framing_decisions").toArray();
    for (int i = fds.size() - 1; i >= 0; --i)
        if (fds[i].toObject().value("framing_intent_id").toString() == kCutIntent) fds.removeAt(i);
    fds.append(QJsonObject{{"label", QString::fromStdString(seq.name)},
                           {"id", QString::fromStdString(canvasId + "-" + kCutIntent)},
                           {"framing_intent_id", kCutIntent},
                           {"dimensions", dimsJson(round3((u1 - u0) * W), round3((v1 - v0) * H))},
                           {"anchor_point", pointJson(round3(u0 * W), round3(v0 * H))}});
    canvas["framing_decisions"] = fds;
    // Its intent: the sequence's shape, as it shows the frame.
    const int g = std::gcd(std::max(1, seq.width), std::max(1, seq.height));
    auto haveIntent = [&](const QString& id) {
        return std::any_of(intents.begin(), intents.end(), [&](const QJsonValue& v) { return v.toObject().value("id").toString() == id; });
    };
    // The footage's intents; one whose id another clip's intent already has, with another shape, gets a number (and its
    // framing decisions with it).
    std::map<QString, QString> intentIds;
    for (QJsonObject i : mediaIntents) {
        const QString id = i.value("id").toString();
        if (id == kCutIntent) continue;
        QString now = id;
        for (int n = 2;; ++n) {
            const auto same = std::find_if(intents.begin(), intents.end(), [&](const QJsonValue& v) { return v.toObject().value("id").toString() == now; });
            if (same == intents.end()) {
                i["id"] = now;
                intents.append(i);
                break;
            }
            QJsonObject other = same->toObject(), mine = i;
            other["id"] = mine["id"] = id;
            if (other == mine) break;  // (the same intent)
            now = QStringLiteral("%1_%2").arg(id.left(28)).arg(n);
        }
        intentIds[id] = now;
    }
    for (int k = 0; k < fds.size(); ++k) {
        QJsonObject fd = fds[k].toObject();
        if (const auto it = intentIds.find(fd.value("framing_intent_id").toString()); it != intentIds.end() && it->first != it->second) {
            fd["framing_intent_id"] = it->second;
            fd["id"] = QString::fromStdString(canvasId) + "-" + it->second;
            fds[k] = fd;
        }
    }
    canvas["framing_decisions"] = fds;
    if (!haveIntent(kCutIntent))
        intents.append(QJsonObject{{"label", "Cut"}, {"id", kCutIntent}, {"aspect_ratio", dimsJson(seq.width / g, seq.height / g)}, {"protection", 0.0}});
    std::string label = c.name.empty() ? m->name : c.name;
    ctx["label"] = QString::fromStdString(label);
    ctx["context_creator"] = kCreator;
    if (!ctx.contains("clip_id")) {
        QJsonObject cid{{"clip_name", QFileInfo(QString::fromStdString(m->name)).completeBaseName()}};
        if (!m->path.empty()) cid["file"] = QFileInfo(QString::fromStdString(m->path)).fileName();
        ctx["clip_id"] = cid;
    }
    ctx["canvases"] = QJsonArray{canvas};
    return true;
}

std::string finishDocument(const QJsonArray& intents, const QJsonArray& contexts, const std::string& defaultIntent, std::string* error) {
    QJsonObject root{{"uuid", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                     {"version", QJsonObject{{"major", 2}, {"minor", 0}}},
                     {"fdl_creator", kCreator},
                     {"default_framing_intent", QString::fromStdString(defaultIntent.empty() ? kCutIntent : defaultIntent)},
                     {"framing_intents", intents},
                     {"contexts", contexts},
                     {"canvas_templates", QJsonArray()}};
    const std::string json = QJsonDocument(root).toJson(QJsonDocument::Indented).toStdString();
    FdlDocument check;
    std::string err;
    if (!parseFdl(json, check, &err)) {
        if (error) *error = "The FDL written would not be valid: " + err;
        return {};
    }
    return json;
}

}  // namespace

std::string clipFdl(const Project& p, const Sequence& seq, const Clip& c, int width, int height, const FdlClipId* clipId, std::string* error) {
    QJsonObject ctx;
    QJsonArray intents;
    std::string def;
    if (!clipContext(p, seq, c, width, height, ctx, intents, def, error)) return {};
    if (clipId) {
        QJsonObject cid{{"clip_name", QString::fromStdString(clipId->name)}};
        if (!clipId->sequence.empty())
            cid["sequence"] = QJsonObject{{"value", QString::fromStdString(clipId->sequence)}, {"idx", "#"},
                                          {"min", std::max(0, clipId->first)}, {"max", std::max(0, clipId->last)}};
        else if (!clipId->file.empty())
            cid["file"] = QString::fromStdString(clipId->file);
        ctx["clip_id"] = cid;
    }
    return finishDocument(intents, QJsonArray{ctx}, def, error);
}

std::string sequenceFdl(const Project& p, const Sequence& seq, const std::vector<Id>& clips, std::string* error) {
    QJsonArray intents, contexts;
    std::string def, lastError;
    std::set<QString> usedCanvasIds;
    for (const Track& t : seq.videoTracks)
        for (const Clip& c : t.clips) {
            if (!clips.empty() && std::find(clips.begin(), clips.end(), c.id) == clips.end()) continue;
            const MediaItem* m = c.mediaId && !c.isGenerator() ? p.findMedia(c.mediaId) : nullptr;
            if (!m || m->kind == MediaKind::Audio || m->kind == MediaKind::Sequence) continue;
            QJsonObject ctx;
            if (!clipContext(p, seq, c, 0, 0, ctx, intents, def, &lastError)) continue;
            // (canvas ids are the document's: a clip's that are taken get a number, and their framing decisions with them)
            QJsonArray canvases = ctx.value("canvases").toArray();
            std::map<QString, QString> renamed;
            for (const QJsonValue& v : canvases) {
                const QString id = v.toObject().value("id").toString();
                QString now = id;
                for (int n = 2; usedCanvasIds.count(now); ++n) now = QStringLiteral("%1_%2").arg(id.left(26)).arg(n);
                renamed[id] = now;
                usedCanvasIds.insert(now);
            }
            for (int i = 0; i < canvases.size(); ++i) {
                QJsonObject cv = canvases[i].toObject();
                const QString old = cv.value("id").toString();
                cv["id"] = renamed[old];
                if (const auto it = renamed.find(cv.value("source_canvas_id").toString()); it != renamed.end()) cv["source_canvas_id"] = it->second;
                QJsonArray fds = cv.value("framing_decisions").toArray();
                for (int k = 0; k < fds.size(); ++k) {
                    QJsonObject fd = fds[k].toObject();
                    fd["id"] = renamed[old] + "-" + fd.value("framing_intent_id").toString();
                    fds[k] = fd;
                }
                cv["framing_decisions"] = fds;
                canvases[i] = cv;
            }
            ctx["canvases"] = canvases;
            // (a label for each: the same footage cut in twice is told apart)
            const QString label = ctx.value("label").toString();
            for (int n = 2; std::any_of(contexts.begin(), contexts.end(), [&](const QJsonValue& v) { return v.toObject().value("label") == ctx.value("label"); }); ++n)
                ctx["label"] = QStringLiteral("%1 (%2)").arg(label).arg(n);
            contexts.append(ctx);
        }
    if (contexts.isEmpty()) {
        if (error) *error = lastError.empty() ? "The sequence has no footage clips" : lastError;
        return {};
    }
    return finishDocument(intents, contexts, def, error);
}

}  // namespace montage
