#include "MediaLog.h"

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>

#include "History.h"

namespace montage {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

bool sameText(const std::string& a, const std::string& b) { return qs(a).compare(qs(b), Qt::CaseInsensitive) == 0; }

// Bins sort by their names, case-insensitively, parents first.
bool binLess(const std::string& a, const std::string& b) {
    const QStringList x = qs(a).split('/'), y = qs(b).split('/');
    for (qsizetype i = 0; i < std::min(x.size(), y.size()); ++i) {
        const int c = x[i].compare(y[i], Qt::CaseInsensitive);
        if (c) return c < 0;
        if (x[i] != y[i]) return x[i] < y[i];
    }
    return x.size() < y.size();
}

// Seconds from "90", "1:30" or "1:02:03".
bool parseNumber(const std::string& text, double& out) {
    const QString t = qs(text).trimmed();
    if (t.isEmpty()) return false;
    double v = 0;
    for (const QString& part : t.split(':')) {
        bool ok = false;
        const double x = part.toDouble(&ok);
        if (!ok) return false;
        v = v * 60 + x;
    }
    out = v;
    return true;
}

bool parseRating(const std::string& text, int& out) {
    const QString t = qs(text).trimmed().toLower();
    if (t == "-1" || t == "x" || t == "reject" || t == "rejected") {
        out = -1;
        return true;
    }
    bool ok = false;
    const int n = t.toInt(&ok);
    if (ok) {
        if (n < -1 || n > 5) return false;
        out = n;
        return true;
    }
    if (t.isEmpty() || t == "unrated" || t == "none") {
        out = 0;
        return true;
    }
    int stars = 0;
    for (QChar c : t) {
        if (c == '*' || c == QChar(0x2605)) ++stars;
        else if (!c.isSpace()) return false;
    }
    if (stars > 5) return false;
    out = stars;
    return true;
}

const char* kindKey(MediaKind k) {
    switch (k) {
        case MediaKind::Video: return "video";
        case MediaKind::Audio: return "audio";
        case MediaKind::Image: return "image";
        case MediaKind::Sequence: return "sequence";
    }
    return "";
}

std::string fmtNumber(double v, int decimals) {
    return QString::number(v, 'f', decimals).remove(QRegularExpression("\\.?0+$")).toStdString();
}

std::string fmtDuration(double seconds) {
    const long total = std::lround(seconds * 100);
    const long cs = total % 100, s = (total / 100) % 60, m = (total / 6000) % 60, h = total / 360000;
    return QString::asprintf("%02ld:%02ld:%02ld.%02ld", h, m, s, cs).toStdString();
}

const char* const kLabelNames[] = {"None", "Violet", "Iris",   "Caribbean", "Lavender", "Cerulean",
                                   "Forest", "Rose",  "Mango", "Yellow",    "Tan",      "Red"};

}  // namespace

// ---------------------------------------------------------------------------
// Labels

int labelCount() { return int(std::size(kLabelNames)); }

const char* labelName(int index) { return index > 0 && index < labelCount() ? kLabelNames[index] : kLabelNames[0]; }

int labelFromName(const std::string& name) {
    for (int i = 0; i < labelCount(); ++i)
        if (sameText(name, kLabelNames[i])) return i;
    bool ok = false;
    const int n = qs(name).trimmed().toInt(&ok);
    return ok && n >= 0 && n < labelCount() ? n : -1;
}

// ---------------------------------------------------------------------------
// Bins

std::string binParent(const std::string& bin) {
    const size_t slash = bin.rfind('/');
    return slash == std::string::npos ? std::string() : bin.substr(0, slash);
}

std::string binLeaf(const std::string& bin) {
    const size_t slash = bin.rfind('/');
    return slash == std::string::npos ? bin : bin.substr(slash + 1);
}

std::string joinBin(const std::string& parent, const std::string& leaf) {
    if (parent.empty()) return leaf;
    if (leaf.empty()) return parent;
    return parent + "/" + leaf;
}

bool binWithin(const std::string& bin, const std::string& ancestor) {
    if (ancestor.empty()) return true;
    return bin == ancestor || (bin.size() > ancestor.size() && bin.compare(0, ancestor.size(), ancestor) == 0 &&
                               bin[ancestor.size()] == '/');
}

std::vector<std::string> projectBins(const Project& p) {
    std::set<std::string> all;
    auto add = [&](std::string b) {
        while (!b.empty() && all.insert(b).second) b = binParent(b);
    };
    for (const std::string& b : p.bins) add(b);
    for (const MediaItem& m : p.media) add(m.bin);
    std::vector<std::string> out(all.begin(), all.end());
    std::sort(out.begin(), out.end(), binLess);
    return out;
}

std::string uniqueBinName(const Project& p, const std::string& parent, const std::string& base) {
    const std::vector<std::string> bins = projectBins(p);
    auto taken = [&](const std::string& leaf) {
        const std::string path = joinBin(parent, leaf);
        return std::any_of(bins.begin(), bins.end(), [&](const std::string& b) { return sameText(b, path); });
    };
    if (!taken(base)) return base;
    for (int i = 2;; ++i)
        if (const std::string name = base + " " + std::to_string(i); !taken(name)) return name;
}

bool addBin(Project& p, const std::string& bin) {
    if (bin.empty()) return false;
    const std::vector<std::string> bins = projectBins(p);
    if (std::find(bins.begin(), bins.end(), bin) != bins.end()) return false;
    p.bins.push_back(bin);
    return true;
}

bool renameBin(Project& p, const std::string& from, const std::string& to) {
    if (from.empty() || to.empty() || from == to || binWithin(to, from)) return false;
    const std::vector<std::string> bins = projectBins(p);
    if (std::find(bins.begin(), bins.end(), from) == bins.end()) return false;
    if (std::find(bins.begin(), bins.end(), to) != bins.end()) return false;
    auto moved = [&](const std::string& b) { return to + b.substr(from.size()); };
    for (std::string& b : p.bins)
        if (binWithin(b, from)) b = moved(b);
    for (MediaItem& m : p.media)
        if (binWithin(m.bin, from)) m.bin = moved(m.bin);
    if (std::find(p.bins.begin(), p.bins.end(), to) == p.bins.end()) p.bins.push_back(to);
    return true;
}

bool removeBin(Project& p, const std::string& bin) {
    if (bin.empty()) return false;
    const std::vector<std::string> bins = projectBins(p);
    if (std::find(bins.begin(), bins.end(), bin) == bins.end()) return false;
    const std::string parent = binParent(bin);
    auto up = [&](const std::string& b) { return b == bin ? parent : joinBin(parent, b.substr(bin.size() + 1)); };
    std::vector<std::string> kept;
    for (const std::string& b : p.bins) {
        if (b == bin) continue;
        const std::string n = binWithin(b, bin) ? up(b) : b;
        if (!n.empty() && std::find(kept.begin(), kept.end(), n) == kept.end()) kept.push_back(n);
    }
    p.bins = std::move(kept);
    for (MediaItem& m : p.media)
        if (binWithin(m.bin, bin)) m.bin = up(m.bin);
    return true;
}

bool moveMediaToBin(Project& p, const std::vector<Id>& media, const std::string& bin) {
    bool any = false;
    for (Id id : media)
        if (MediaItem* m = p.findMedia(id); m && m->bin != bin) {
            m->bin = bin;
            any = true;
        }
    if (any && !bin.empty() && std::find(p.bins.begin(), p.bins.end(), bin) == p.bins.end()) p.bins.push_back(bin);
    return any;
}

bool moveBin(Project& p, const std::string& bin, const std::string& into) {
    if (bin.empty() || binWithin(into, bin)) return false;
    return renameBin(p, bin, joinBin(into, binLeaf(bin)));
}

// ---------------------------------------------------------------------------
// Keywords

std::vector<std::string> parseKeywords(const std::string& text) {
    std::vector<std::string> out;
    for (const QString& part : qs(text).split(QRegularExpression("[,;]"))) {
        const QString k = part.simplified();
        if (!k.isEmpty()) addKeywords(out, {k.toStdString()});
    }
    return out;
}

std::string joinKeywords(const std::vector<std::string>& keywords) {
    std::string out;
    for (const std::string& k : keywords) out += (out.empty() ? "" : ", ") + k;
    return out;
}

bool addKeywords(std::vector<std::string>& to, const std::vector<std::string>& keywords) {
    bool changed = false;
    for (const std::string& k : keywords) {
        if (k.empty() || std::any_of(to.begin(), to.end(), [&](const std::string& e) { return sameText(e, k); })) continue;
        to.push_back(k);
        changed = true;
    }
    return changed;
}

bool removeKeywords(std::vector<std::string>& from, const std::vector<std::string>& keywords) {
    const size_t before = from.size();
    std::erase_if(from, [&](const std::string& e) {
        return std::any_of(keywords.begin(), keywords.end(), [&](const std::string& k) { return sameText(e, k); });
    });
    return from.size() != before;
}

std::vector<std::string> projectKeywords(const Project& p) {
    std::vector<std::string> all;
    for (const MediaItem& m : p.media) addKeywords(all, m.keywords);
    std::sort(all.begin(), all.end(), [](const std::string& a, const std::string& b) {
        return qs(a).compare(qs(b), Qt::CaseInsensitive) < 0;
    });
    return all;
}

// ---------------------------------------------------------------------------
// Fields

const std::vector<MediaField>& mediaFields() {
    using T = FieldType;
    static const std::vector<MediaField> fields{
        {"any", "Any Text", T::Text, false, false, true},
        {"name", "Name", T::Text, true, true, true},
        {"rating", "Rating", T::Rating, true, true, true},
        {"label", "Label", T::Label, true, true, true},
        {"duration", "Duration", T::Number, false, true, true},
        {"kind", "Type", T::Kind, false, true, true},
        {"keywords", "Keywords", T::Keywords, true, true, true},
        {"usage", "Usage", T::Number, false, true, true},
        {"scene", "Scene", T::Text, true, true, true},
        {"shot", "Shot", T::Text, true, true, true},
        {"take", "Take", T::Text, true, true, true},
        {"camera", "Camera", T::Text, true, true, true},
        {"device", "Camera Model", T::Text, true, true, true},
        {"description", "Description", T::Text, true, true, true},
        {"comment", "Comment", T::Text, true, true, true},
        {"created", "Date Recorded", T::Text, false, true, true},
        {"resolution", "Resolution", T::Text, false, true, false},
        {"width", "Width", T::Number, false, false, true},
        {"height", "Height", T::Number, false, false, true},
        {"fps", "Frame Rate", T::Number, false, true, true},
        {"timecode", "Start Timecode", T::Text, false, true, false},
        {"videoCodec", "Video Codec", T::Text, false, true, true},
        {"audio", "Audio", T::Text, false, true, false},
        {"audioCodec", "Audio Codec", T::Text, false, false, true},
        {"channels", "Audio Channels", T::Number, false, false, true},
        {"colour", "Colour Space", T::Text, false, true, true},
        {"transcript", "Transcript", T::Text, false, true, true},
        {"proxy", "Proxy", T::Text, false, true, true},
        {"bin", "Bin", T::Text, false, true, true},
        {"path", "File", T::Text, false, true, true},
    };
    return fields;
}

const MediaField* mediaField(const std::string& key) {
    for (const MediaField& f : mediaFields())
        if (key == f.key) return &f;
    return nullptr;
}

const std::vector<std::string>& metadataKeys() {
    static const std::vector<std::string> keys{"scene", "shot", "take", "camera", "device", "description", "comment"};
    return keys;
}

std::map<Id, int> mediaUsage(const Project& p) {
    std::map<Id, int> usage;
    for (const Sequence& s : p.sequences)
        for (const auto* tracks : {&s.videoTracks, &s.audioTracks})
            for (const Track& t : *tracks)
                for (const Clip& c : t.clips)
                    if (c.mediaId) ++usage[c.mediaId];
    return usage;
}

std::string mediaFieldText(const MediaItem& m, const std::string& key, const std::map<Id, int>* usage) {
    if (key == "name") return m.name;
    if (key == "rating") {
        if (m.rating < 0) return "Rejected";
        std::string stars;
        for (int i = 0; i < std::min(m.rating, 5); ++i) stars += "★";
        return stars;
    }
    if (key == "label") return m.label > 0 ? labelName(m.label) : "";
    if (key == "duration") return m.duration > 0 ? fmtDuration(m.duration) : "";
    if (key == "kind") return kindKey(m.kind);
    if (key == "keywords") return joinKeywords(m.keywords);
    if (key == "usage") {
        if (!usage) return "";
        const auto it = usage->find(m.id);
        return std::to_string(it == usage->end() ? 0 : it->second);
    }
    if (std::find(metadataKeys().begin(), metadataKeys().end(), key) != metadataKeys().end()) {
        const auto it = m.metadata.find(key);
        return it == m.metadata.end() ? "" : it->second;
    }
    if (key == "created") return m.created;
    if (key == "resolution") return m.hasVideo && m.width > 0 ? std::to_string(m.width) + "×" + std::to_string(m.height) : "";
    if (key == "width") return m.hasVideo && m.width > 0 ? std::to_string(m.width) : "";
    if (key == "height") return m.hasVideo && m.height > 0 ? std::to_string(m.height) : "";
    if (key == "fps") return m.hasVideo && m.fps.valid() && m.kind != MediaKind::Image ? fmtNumber(m.fps.toDouble(), 3) : "";
    if (key == "timecode") {
        if (m.timecode < 0 || !m.fps.valid()) return "";
        return formatTimecode(FrameTime(std::llround(m.timecode * m.fps.toDouble())), m.fps);
    }
    if (key == "videoCodec") return m.hasVideo ? m.videoCodec : "";
    if (key == "audio") {
        if (!m.hasAudio || m.sampleRate <= 0) return "";
        return std::to_string(m.sampleRate) + " Hz, " + std::to_string(m.channels) + " ch" +
               (m.audioCodec.empty() ? "" : ", " + m.audioCodec);
    }
    if (key == "audioCodec") return m.hasAudio ? m.audioCodec : "";
    if (key == "channels") return m.hasAudio ? std::to_string(m.channels) : "";
    if (key == "colour") return m.colorOverride.empty() ? m.colorSpace : m.colorOverride;
    if (key == "transcript") {
        if (!m.transcript || m.transcript->empty()) return "";
        return std::to_string(m.transcript->wordCount()) + " words" +
               (m.transcript->language.empty() ? "" : " (" + m.transcript->language + ")");
    }
    if (key == "proxy") return m.proxyPath.empty() ? "" : "Yes";
    if (key == "bin") return m.bin;
    if (key == "path") return m.path;
    return "";
}

double mediaFieldNumber(const MediaItem& m, const std::string& key, const std::map<Id, int>* usage) {
    if (key == "rating") return m.rating;
    if (key == "label") return m.label;
    if (key == "duration") return m.duration;
    if (key == "kind") return double(int(m.kind));
    if (key == "usage") {
        if (!usage) return 0;
        const auto it = usage->find(m.id);
        return it == usage->end() ? 0 : it->second;
    }
    if (key == "width" || key == "resolution") return m.hasVideo ? double(m.width) * (key == "resolution" ? m.height : 1) : 0;
    if (key == "height") return m.hasVideo ? m.height : 0;
    if (key == "fps") return m.hasVideo && m.fps.valid() && m.kind != MediaKind::Image ? m.fps.toDouble() : 0;
    if (key == "timecode") return m.timecode;
    if (key == "channels") return m.hasAudio ? m.channels : 0;
    if (key == "audio") return m.hasAudio ? m.sampleRate : 0;
    if (key == "transcript") return m.transcript ? double(m.transcript->wordCount()) : 0;
    return 0;
}

bool setMediaField(MediaItem& m, const std::string& key, const std::string& value) {
    const MediaField* f = mediaField(key);
    if (!f || !f->editable) return false;
    if (key == "name") {
        const QString name = qs(value).trimmed();
        if (name.isEmpty()) return false;
        m.name = name.toStdString();
        return true;
    }
    if (key == "rating") return parseRating(value, m.rating);
    if (key == "label") {
        const int l = qs(value).trimmed().isEmpty() ? 0 : labelFromName(qs(value).trimmed().toStdString());
        if (l < 0) return false;
        m.label = l;
        return true;
    }
    if (key == "keywords") {
        m.keywords = parseKeywords(value);
        return true;
    }
    const std::string v = qs(value).trimmed().toStdString();
    if (v.empty()) m.metadata.erase(key);
    else m.metadata[key] = v;
    return true;
}

// ---------------------------------------------------------------------------
// Search and smart bins

bool mediaMatchesSearch(const MediaItem& m, const std::string& query) {
    // Terms: words, or "quoted phrases".
    QStringList terms;
    static const QRegularExpression term("\"([^\"]*)\"|(\\S+)");
    for (auto it = term.globalMatch(qs(query)); it.hasNext();) {
        const auto match = it.next();
        const QString t = match.captured(1).isNull() ? match.captured(2) : match.captured(1);
        if (!t.trimmed().isEmpty()) terms << t.simplified();
    }
    if (terms.isEmpty()) return true;
    QString text = qs(m.name);
    for (const std::string& k : m.keywords) text += "\n" + qs(k);
    for (const auto& [key, v] : m.metadata) text += "\n" + qs(v);
    const QString spoken = m.transcript ? qs(m.transcript->text()) : QString();
    for (const QString& t : terms)
        if (!text.contains(t, Qt::CaseInsensitive) && !spoken.contains(t, Qt::CaseInsensitive)) return false;
    return true;
}

std::vector<RuleOp> ruleOps(FieldType type) {
    switch (type) {
        case FieldType::Text:
            return {{"contains", "contains", true}, {"!contains", "does not contain", true}, {"is", "is", true},
                    {"!is", "is not", true},        {"starts", "starts with", true},         {"empty", "is empty", false},
                    {"!empty", "is not empty", false}};
        case FieldType::Number:
            return {{">=", "is at least", true}, {"<=", "is at most", true}, {">", "is more than", true},
                    {"<", "is less than", true}, {"is", "is", true},         {"!is", "is not", true}};
        case FieldType::Rating: return {{">=", "is at least", true}, {"<=", "is at most", true}, {"is", "is", true}, {"!is", "is not", true}};
        case FieldType::Label:
        case FieldType::Kind: return {{"is", "is", true}, {"!is", "is not", true}};
        case FieldType::Keywords:
            return {{"includes", "includes", true}, {"!includes", "does not include", true}, {"empty", "is empty", false},
                    {"!empty", "is not empty", false}};
    }
    return {};
}

namespace {

// Text a rule tests: the transcript's words rather than its summary, and everything for "any".
QString ruleText(const MediaItem& m, const std::string& key, const std::map<Id, int>& usage) {
    if (key == "transcript") return m.transcript ? qs(m.transcript->text()) : QString();
    if (key == "any") {
        QString text = qs(m.name);
        for (const std::string& k : m.keywords) text += "\n" + qs(k);
        for (const auto& [k, v] : m.metadata) text += "\n" + qs(v);
        if (m.transcript) text += "\n" + qs(m.transcript->text());
        return text;
    }
    return qs(mediaFieldText(m, key, &usage));
}

bool compare(double a, const std::string& op, double b, double eps) {
    if (op == "is") return std::fabs(a - b) <= eps;
    if (op == "!is") return std::fabs(a - b) > eps;
    if (op == ">") return a > b + eps;
    if (op == ">=") return a >= b - eps;
    if (op == "<") return a < b - eps;
    if (op == "<=") return a <= b + eps;
    return false;
}

}  // namespace

bool ruleMatches(const SmartRule& r, const MediaItem& m, const std::map<Id, int>& usage) {
    const MediaField* f = mediaField(r.field);
    if (!f) return false;
    switch (f->type) {
        case FieldType::Text: {
            const QString text = ruleText(m, r.field, usage), value = qs(r.value).trimmed();
            if (r.op == "contains") return text.contains(value, Qt::CaseInsensitive);
            if (r.op == "!contains") return !text.contains(value, Qt::CaseInsensitive);
            if (r.op == "is") return text.compare(value, Qt::CaseInsensitive) == 0;
            if (r.op == "!is") return text.compare(value, Qt::CaseInsensitive) != 0;
            if (r.op == "starts") return text.startsWith(value, Qt::CaseInsensitive);
            if (r.op == "empty") return text.trimmed().isEmpty();
            if (r.op == "!empty") return !text.trimmed().isEmpty();
            return false;
        }
        case FieldType::Number: {
            double v = 0;
            if (!parseNumber(r.value, v)) return false;
            return compare(mediaFieldNumber(m, r.field, &usage), r.op, v, r.field == "fps" ? 0.01 : 1e-6);
        }
        case FieldType::Rating: {
            int v = 0;
            if (!parseRating(r.value, v)) return false;
            return compare(m.rating, r.op, v, 0);
        }
        case FieldType::Label: {
            const int v = qs(r.value).trimmed().isEmpty() ? 0 : labelFromName(r.value);
            if (v < 0) return false;
            return compare(m.label, r.op, v, 0);
        }
        case FieldType::Kind: {
            const bool same = sameText(kindKey(m.kind), qs(r.value).trimmed().toStdString());
            return r.op == "is" ? same : r.op == "!is" ? !same : false;
        }
        case FieldType::Keywords: {
            const bool has = std::any_of(m.keywords.begin(), m.keywords.end(),
                                         [&](const std::string& k) { return sameText(k, qs(r.value).trimmed().toStdString()); });
            if (r.op == "includes") return has;
            if (r.op == "!includes") return !has;
            if (r.op == "empty") return m.keywords.empty();
            if (r.op == "!empty") return !m.keywords.empty();
            return false;
        }
    }
    return false;
}

bool smartBinMatches(const SmartBin& b, const MediaItem& m, const std::map<Id, int>& usage) {
    if (b.rules.empty()) return true;
    for (const SmartRule& r : b.rules) {
        const bool ok = ruleMatches(r, m, usage);
        if (b.matchAll && !ok) return false;
        if (!b.matchAll && ok) return true;
    }
    return b.matchAll;
}

std::vector<Id> smartBinMedia(const Project& p, const SmartBin& b) {
    const std::map<Id, int> usage = mediaUsage(p);
    std::vector<Id> out;
    for (const MediaItem& m : p.media)
        if (smartBinMatches(b, m, usage)) out.push_back(m.id);
    return out;
}

SmartBin* findSmartBin(Project& p, Id id) {
    for (SmartBin& b : p.smartBins)
        if (b.id == id) return &b;
    return nullptr;
}

const SmartBin* findSmartBin(const Project& p, Id id) {
    for (const SmartBin& b : p.smartBins)
        if (b.id == id) return &b;
    return nullptr;
}

}  // namespace montage
