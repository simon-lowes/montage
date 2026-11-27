#include "ContentCredentials.h"
#include "ImageSequence.h"

#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <set>
#include <thread>

#ifdef MONTAGE_WITH_C2PA
#include <c2pa.h>
#endif

namespace montage {

namespace {

// The MIME type c2pa-rs takes for a file name; "" for one it cannot hold credentials in.
std::string credentialFormat(const std::string& path) {
    static const std::map<QString, const char*> types = {
        {"mp4", "video/mp4"},   {"m4v", "video/mp4"},        {"mov", "video/quicktime"}, {"m4a", "audio/mp4"},
        {"jpg", "image/jpeg"},  {"jpeg", "image/jpeg"},      {"png", "image/png"},       {"tif", "image/tiff"},
        {"tiff", "image/tiff"}, {"dng", "image/x-adobe-dng"}, {"webp", "image/webp"},    {"heic", "image/heic"},
        {"heif", "image/heif"}, {"avif", "image/avif"},      {"wav", "audio/wav"},       {"mp3", "audio/mpeg"},
        {"avi", "video/avi"},   {"pdf", "application/pdf"},  {"svg", "image/svg+xml"}};
    const auto it = types.find(QFileInfo(QString::fromStdString(path)).suffix().toLower());
    return it == types.end() ? std::string() : std::string(it->second);
}

// A sequence's frames on disk, in order: from a media path, or from an export's pattern ("shot.%04d.exr"), whatever
// numbers are there. False when `path` names no sequence.
bool sequenceFrames(const std::string& path, std::vector<std::string>& files, std::string& pattern) {
    ImageSequence seq;
    if (parseImageSequencePath(path, seq)) {
        pattern = seq.pattern;
        for (int n = seq.first; n <= seq.last; ++n) files.push_back(imageSequenceFrame(seq, n));
        return true;
    }
    const QString p = QString::fromStdString(path);
    const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("%0?(\\d*)d")).match(QFileInfo(p).fileName());
    if (!m.hasMatch()) return false;
    pattern = path;
    const QString name = QFileInfo(p).fileName();
    const QRegularExpression frame(QStringLiteral("^") + QRegularExpression::escape(name.left(m.capturedStart())) + QStringLiteral("(\\d+)") +
                                   QRegularExpression::escape(name.mid(m.capturedEnd())) + QStringLiteral("$"));
    std::map<long long, std::string> numbered;
    for (const QFileInfo& f : QDir(QFileInfo(p).path()).entryInfoList(QDir::Files)) {
        const QRegularExpressionMatch got = frame.match(f.fileName());
        if (got.hasMatch()) numbered[got.captured(1).toLongLong()] = f.filePath().toStdString();
    }
    for (const auto& [n, f] : numbered) files.push_back(f);
    return true;
}

// The MIME type a manifest beside a file names for it.
std::string besideFormat(const std::string& path) {
    static const std::map<QString, const char*> types = {{"mxf", "application/mxf"}, {"mkv", "video/x-matroska"}, {"mka", "audio/x-matroska"},
                                                         {"webm", "video/webm"},      {"exr", "image/x-exr"},     {"dpx", "image/x-dpx"},
                                                         {"ts", "video/mp2t"},        {"m2ts", "video/mp2t"},     {"wav", "audio/wav"},
                                                         {"tif", "image/tiff"},       {"tiff", "image/tiff"},     {"png", "image/png"}};
    std::vector<std::string> frames;
    std::string pattern;
    const std::string file = sequenceFrames(path, frames, pattern) ? pattern : path;
    if (QFileInfo(QString::fromStdString(file)).isDir()) return "application/octet-stream";
    const auto it = types.find(QFileInfo(QString::fromStdString(file)).suffix().toLower());
    return it == types.end() ? std::string("application/octet-stream") : std::string(it->second);
}

}  // namespace

bool contentCredentialsFormat(const std::string& path) { return !credentialFormat(path).empty(); }

std::string credentialSidecarPath(const std::string& path) {
    if (path.empty()) return {};
    const QFileInfo info(QString::fromStdString(path));
    if (info.isDir()) {
        const QString dir = QDir::cleanPath(info.absoluteFilePath());
        return (QFileInfo(dir).path() + '/' + QFileInfo(dir).fileName() + QStringLiteral(".c2pa")).toStdString();
    }
    std::vector<std::string> frames;
    std::string pattern;
    if (sequenceFrames(path, frames, pattern)) {
        // (the name before the number, without the separators round it)
        const QString name = QFileInfo(QString::fromStdString(pattern)).fileName();
        QString base = name.left(int(name.indexOf('%')));
        while (!base.isEmpty() && QStringLiteral(" ._-").contains(base.back())) base.chop(1);
        if (base.isEmpty()) base = QStringLiteral("frames");
        return (QFileInfo(QString::fromStdString(pattern)).path() + '/' + base + QStringLiteral(".c2pa")).toStdString();
    }
    return (info.path() + '/' + info.completeBaseName() + QStringLiteral(".c2pa")).toStdString();
}

std::vector<std::string> credentialBoundFiles(const std::string& path) {
    std::vector<std::string> out;
    const QFileInfo info(QString::fromStdString(path));
    std::string pattern;
    if (info.isDir()) {
        QStringList files;
        QDirIterator it(info.absoluteFilePath(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) files << it.next();
        files.sort();  // (by path, so the same package always binds in the same order)
        for (const QString& f : files)
            if (!f.endsWith(QStringLiteral(".c2pa"))) out.push_back(f.toStdString());
    } else if (!sequenceFrames(path, out, pattern) && info.isFile()) {
        out.push_back(path);
    }
    return out;
}

bool hasCredentialSidecar(const std::string& path) {
    const std::string beside = credentialSidecarPath(path);
    return !beside.empty() && QFileInfo(QString::fromStdString(beside)).isFile();
}

std::vector<std::string> personAssertions(const std::string& storeJson) {
    std::vector<std::string> out;
    const QJsonObject manifests = QJsonDocument::fromJson(QByteArray::fromStdString(storeJson)).object().value("manifests").toObject();
    for (auto it = manifests.begin(); it != manifests.end(); ++it)
        for (const QJsonValue& a : it.value().toObject().value("assertions").toArray()) {
            const QString label = a.toObject().value("label").toString();
            if (label.startsWith(QStringLiteral("cawg.identity")) || label.startsWith(QStringLiteral("stds.schema-org.CreativeWork")) ||
                label.startsWith(QStringLiteral("stds.schema-org.Person")))
                out.push_back(("self#jumbf=/c2pa/" + it.key() + "/c2pa.assertions/" + label).toStdString());
        }
    return out;
}

std::string credentialsLabel(const CredentialSummary& c) {
    if (!c.present) return {};
    std::string out = c.issuer.empty() ? "Content Credentials" : "Signed by " + c.issuer;
    if (!c.signedAt.empty()) out += ", " + c.signedAt.substr(0, 10);
    if (!c.generator.empty()) out += ", made with " + c.generator;
    std::string state = c.state;
    std::transform(state.begin(), state.end(), state.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    if (!state.empty()) out += " (" + state + ")";
    if (c.aiGenerated) out += "; made with AI";
    if (!c.through.empty()) out += "; kept through a copy of " + c.through;
    if (c.beside) out += "; in a manifest beside the file";
    return out;
}

std::string credentialManifestJson(const std::string& path, const Provenance& provenance, const CredentialOptions& options,
                                   const std::string& parent, const std::vector<std::string>& redactions) {
    const QString name = QFileInfo(QString::fromStdString(path)).fileName();
    const QJsonObject montage{{"name", "Montage"}, {"version", MONTAGE_VERSION}};
    // Made by Montage from other media (some of it made by AI, when it is), or a signed file opened in Montage; then each
    // model's part.
    // (a manifest's actions begin with c2pa.created or c2pa.opened, which c2pa-rs's validation requires: a copy opens its
    // parent, then says what was done to it)
    QJsonArray actions;
    if (!parent.empty() && !options.parent.empty()) {
        actions.append(QJsonObject{{"action", "c2pa.opened"},
                                   {"softwareAgent", montage},
                                   {"parameters", QJsonObject{{"ingredientIds", QJsonArray{QString::fromStdString(parent)}}}},
                                   {"description", "A signed file opened in Montage to make a copy of it"}});
        actions.append(QJsonObject{{"action", QString::fromStdString(options.parentAction)},
                                   {"softwareAgent", montage},
                                   {"description", QString::fromStdString(options.parentDescription.empty() ? std::string("Made in Montage from a signed file")
                                                                                                            : options.parentDescription)}});
    } else if (!parent.empty())
        actions.append(QJsonObject{{"action", "c2pa.opened"},
                                   {"softwareAgent", montage},
                                   {"parameters", QJsonObject{{"ingredientIds", QJsonArray{QString::fromStdString(parent)}}}},
                                   {"description", "Signed again in Montage, its earlier credentials kept"}});
    else
        actions.append(QJsonObject{{"action", "c2pa.created"},
                                   {"digitalSourceType", QString::fromStdString(provenance.generated() ? iptcSourceType(AiRole::Generated)
                                                                                                       : "http://cv.iptc.org/newscodes/digitalsourcetype/composite")},
                                   {"softwareAgent", montage},
                                   {"description", "Edited in Montage from other media"}});
    for (const AiUse& u : provenance.ai) {
        const QString what = QString::fromStdString(u.feature + " (" + u.model + ")");
        QJsonObject a{{"action", "c2pa.edited"},
                      {"softwareAgent", QJsonObject{{"name", "Montage " + what}, {"version", MONTAGE_VERSION}}},
                      {"description", u.role == AiRole::Generated  ? what + ": content made by an AI model"
                                      : u.role == AiRole::Enhanced ? what + ": content changed by an AI model"
                                                                   : what + ": an AI model chose where an edit applies"}};
        if (const std::string type = iptcSourceType(u.role); !type.empty()) a["digitalSourceType"] = QString::fromStdString(type);
        actions.append(a);
    }
    QJsonArray assertions{QJsonObject{{"label", "c2pa.actions.v2"}, {"data", QJsonObject{{"actions", actions}}}}};
    if (!options.allowAiTraining) {
        const QJsonObject no{{"use", "notAllowed"}};
        assertions.append(QJsonObject{{"label", "cawg.training-mining"},
                                      {"data", QJsonObject{{"entries", QJsonObject{{"cawg.ai_generative_training", no},
                                                                                    {"cawg.ai_training", no},
                                                                                    {"cawg.ai_inference", no},
                                                                                    {"cawg.data_mining", no}}}}}});
    }
    const std::string format = credentialFormat(path);
    QJsonObject definition{{"claim_generator_info", QJsonArray{montage}},
                           {"title", options.title.empty() ? name : QString::fromStdString(options.title)},
                           {"format", QString::fromStdString(format.empty() ? besideFormat(path) : format)},
                           {"assertions", assertions}};
    if (!redactions.empty()) {
        QJsonArray r;
        for (const std::string& uri : redactions) r.append(QString::fromStdString(uri));
        definition["redactions"] = r;
    }
    return QJsonDocument(definition).toJson(QJsonDocument::Compact).toStdString();
}

bool keepCredentialsThroughCopy(const std::string& source, const std::string& copy, const CredentialSigner& signer,
                                const std::string& description, std::string* error) {
    if (signer.certificatePath.empty() || !contentCredentialsAvailable()) return true;
    CredentialSummary has;
    if (!readContentCredentials(source, has) || !has.present) return true;
    CredentialOptions o;
    o.parent = source;
    o.parentAction = "c2pa.transcoded";
    o.parentDescription = description;
    o.allowAiTraining = true;  // (what the source's own credentials say about training stands)
    std::string why;
    if (signContentCredentials(copy, {}, signer, o, &why)) return true;
    if (error) *error = "Could not keep the Content Credentials of " + source + " on its copy: " + why;
    return false;
}

#ifndef MONTAGE_WITH_C2PA

bool contentCredentialsAvailable() { return false; }

bool readContentCredentials(const std::string&, CredentialSummary& out, std::string* error) {
    out = {};
    if (error) *error = "This build of Montage cannot read Content Credentials (no C2PA library)";
    return false;
}

bool signContentCredentials(const std::string&, const Provenance&, const CredentialSigner&, const CredentialOptions&, std::string* error) {
    if (error) *error = "This build of Montage cannot sign Content Credentials (no C2PA library)";
    return false;
}

bool checkCredentialSigner(const CredentialSigner&, std::string* error) {
    if (error) *error = "This build of Montage cannot sign Content Credentials (no C2PA library)";
    return false;
}

#else

bool contentCredentialsAvailable() { return true; }

namespace {

// c2pa-rs's last error, freed.
std::string lastError() {
    char* e = c2pa_error();
    std::string out = e ? e : "unknown error";
    if (e) c2pa_free(e);
    return out;
}

// A file as a c2pa-rs stream.
class FileStream {
public:
    explicit FileStream(QIODevice* file) {
        stream_ = c2pa_create_stream(reinterpret_cast<StreamContext*>(file), &FileStream::read, &FileStream::seek, &FileStream::write,
                                     &FileStream::flush);
    }
    ~FileStream() {
        if (stream_) c2pa_release_stream(stream_);
    }
    FileStream(const FileStream&) = delete;
    FileStream& operator=(const FileStream&) = delete;
    C2paStream* get() const { return stream_; }

private:
    static QIODevice* device(StreamContext* c) { return reinterpret_cast<QIODevice*>(c); }
    static intptr_t read(StreamContext* c, uint8_t* data, intptr_t len) { return intptr_t(device(c)->read(reinterpret_cast<char*>(data), len)); }
    static intptr_t seek(StreamContext* c, intptr_t offset, C2paSeekMode mode) {
        QIODevice* d = device(c);
        const qint64 base = mode == Start ? 0 : mode == Current ? d->pos() : d->size();
        return base + offset >= 0 && d->seek(base + offset) ? intptr_t(d->pos()) : -1;
    }
    static intptr_t write(StreamContext* c, const uint8_t* data, intptr_t len) {
        return intptr_t(device(c)->write(reinterpret_cast<const char*>(data), len));
    }
    static intptr_t flush(StreamContext*) { return 0; }
    C2paStream* stream_ = nullptr;
};

std::string text(const QJsonValue& v) { return v.toString().toStdString(); }

// The signer for a certificate chain and key: the algorithm given, or the first the key works with.
C2paSigner* makeSigner(const CredentialSigner& signer, std::string& error) {
    auto readText = [](const std::string& file, std::string& into) {
        QFile f(QString::fromStdString(file));
        if (!f.open(QIODevice::ReadOnly)) return false;
        into = f.readAll().toStdString();
        return true;
    };
    std::string certs, key;
    if (!readText(signer.certificatePath, certs)) return error = "Cannot read the certificate " + signer.certificatePath, nullptr;
    if (!readText(signer.keyPath, key)) return error = "Cannot read the private key " + signer.keyPath, nullptr;
    std::string refused;
    for (const char* alg : {"es256", "es384", "es512", "ps256", "ed25519", "ps384", "ps512"}) {
        if (!signer.algorithm.empty() && signer.algorithm != alg) continue;
        const C2paSignerInfo info{alg, certs.c_str(), key.c_str(), nullptr};
        if (C2paSigner* sign = c2pa_signer_from_info(&info)) return sign;
        if (refused.empty()) refused = lastError();
    }
    error = "The signing certificate or key was refused: " + (refused.empty() ? "unknown algorithm " + signer.algorithm : refused);
    return nullptr;
}

// Signs `from` (of MIME type `format`) into `to` with the manifest `definition`; "" or why not.
std::string signStream(C2paBuilder* builder, const char* format, QIODevice* from, QIODevice* to, C2paSigner* sign) {
    FileStream in(from), out(to);
    const unsigned char* manifest = nullptr;
    const bool ok = c2pa_builder_sign(builder, format, in.get(), out.get(), sign, &manifest) >= 0;
    if (manifest) c2pa_free(manifest);
    return ok ? std::string() : "Signing failed: " + lastError();
}

C2paBuilder* makeBuilder(const std::string& definition, C2paSigner* sign = nullptr) {
    // (with a signer, it is the context's, for signing a manifest that goes beside the file; it is consumed)
    C2paContext* context = nullptr;
    if (sign) {
        C2paContextBuilder* cb = c2pa_context_builder_new();
        if (cb && c2pa_context_builder_set_signer(cb, sign) == 0) {
            context = c2pa_context_builder_build(cb);
        } else if (cb) {
            c2pa_free(cb);
        }
    } else {
        context = c2pa_context_new();
    }
    C2paBuilder* builder = context ? c2pa_builder_from_context(context) : nullptr;
    if (builder) builder = c2pa_builder_with_definition(builder, definition.c_str());
    if (context) c2pa_free(context);
    return builder;
}

// Files read one after another as one stream: what a manifest beside a sequence or a package binds. One file is open
// at a time (a sequence may have thousands).
class ConcatStream {
public:
    explicit ConcatStream(std::vector<std::string> files) : files_(std::move(files)) {
        qint64 at = 0;
        for (const std::string& f : files_) {
            starts_.push_back(at);
            at += QFileInfo(QString::fromStdString(f)).size();
        }
        size_ = at;
        stream_ = c2pa_create_stream(reinterpret_cast<StreamContext*>(this), &ConcatStream::read, &ConcatStream::seek, &ConcatStream::write,
                                     &ConcatStream::flush);
    }
    ~ConcatStream() {
        if (stream_) c2pa_release_stream(stream_);
    }
    ConcatStream(const ConcatStream&) = delete;
    ConcatStream& operator=(const ConcatStream&) = delete;
    C2paStream* get() const { return stream_; }

private:
    static ConcatStream* self(StreamContext* c) { return reinterpret_cast<ConcatStream*>(c); }
    static intptr_t read(StreamContext* c, uint8_t* data, intptr_t len) {
        ConcatStream* s = self(c);
        intptr_t done = 0;
        while (done < len && s->pos_ < s->size_) {
            // (the file the position is in)
            const size_t i = size_t(std::upper_bound(s->starts_.begin(), s->starts_.end(), s->pos_) - s->starts_.begin()) - 1;
            if (s->open_ != int(i)) {
                s->file_.close();
                s->file_.setFileName(QString::fromStdString(s->files_[i]));
                if (!s->file_.open(QIODevice::ReadOnly)) return done > 0 ? done : -1;
                s->open_ = int(i);
            }
            if (!s->file_.seek(s->pos_ - s->starts_[i])) return done > 0 ? done : -1;
            const qint64 got = s->file_.read(reinterpret_cast<char*>(data) + done, len - done);
            if (got <= 0) break;
            done += intptr_t(got);
            s->pos_ += got;
        }
        return done;
    }
    static intptr_t seek(StreamContext* c, intptr_t offset, C2paSeekMode mode) {
        ConcatStream* s = self(c);
        const qint64 base = mode == Start ? 0 : mode == Current ? s->pos_ : s->size_;
        if (base + offset < 0) return -1;
        s->pos_ = std::min<qint64>(base + offset, s->size_);
        return intptr_t(s->pos_);
    }
    static intptr_t write(StreamContext*, const uint8_t*, intptr_t) { return -1; }
    static intptr_t flush(StreamContext*) { return 0; }
    std::vector<std::string> files_;
    std::vector<qint64> starts_;
    qint64 size_ = 0, pos_ = 0;
    QFile file_;
    int open_ = -1;
    C2paStream* stream_ = nullptr;
};

// What a reader found, from its manifest store's JSON.
void summarize(C2paReader* reader, CredentialSummary& out) {
    char* json = c2pa_reader_json(reader);
    out.json = json ? json : "";
    if (json) c2pa_free(json);
    const QJsonObject store = QJsonDocument::fromJson(QByteArray::fromStdString(out.json)).object();
    const QJsonObject manifests = store.value("manifests").toObject();
    const QJsonObject active = manifests.value(store.value("active_manifest").toString()).toObject();
    out.present = !active.isEmpty();
    if (!out.present) return;
    out.state = text(store.value("validation_state"));
    const QJsonObject sig = active.value("signature_info").toObject();
    out.issuer = text(sig.value("issuer"));
    if (out.issuer.empty()) out.issuer = text(sig.value("common_name"));
    out.signedAt = text(sig.value("time"));
    const QJsonArray gen = active.value("claim_generator_info").toArray();
    if (!gen.isEmpty()) {
        const QJsonObject g = gen.first().toObject();
        out.generator = text(g.value("name")) + (g.contains("version") ? " " + text(g.value("version")) : std::string());
    } else {
        out.generator = text(active.value("claim_generator"));
    }
    out.title = text(active.value("title"));
    // What the active manifest says was done; whether any manifest in it says AI made something.
    for (auto it = manifests.begin(); it != manifests.end(); ++it)
        for (const QJsonValue& a : it.value().toObject().value("assertions").toArray()) {
            const QJsonObject assertion = a.toObject();
            if (!assertion.value("label").toString().startsWith("c2pa.actions")) continue;
            for (const QJsonValue& v : assertion.value("data").toObject().value("actions").toArray()) {
                const QJsonObject action = v.toObject();
                if (action.value("digitalSourceType").toString().contains("trainedAlgorithmicMedia", Qt::CaseInsensitive)) out.aiGenerated = true;
                if (it.key() == store.value("active_manifest").toString()) out.actions.push_back(text(action.value("action")));
            }
        }
    // A copy of a signed file: its parent carries credentials, and was not this file itself signed again (which opens
    // its parent and changes nothing of it; a copy opens it and transcodes it).
    if (out.actions.empty() || out.actions.front() != "c2pa.opened" ||
        std::find(out.actions.begin(), out.actions.end(), "c2pa.transcoded") != out.actions.end())
        for (const QJsonValue& v : active.value("ingredients").toArray()) {
            const QJsonObject ing = v.toObject();
            if (ing.value("relationship").toString() == "parentOf" && ing.contains("active_manifest")) out.through = text(ing.value("title"));
        }
    const QJsonArray failures = store.value("validation_results").toObject().value("activeManifest").toObject().value("failure").toArray();
    for (const QJsonValue& f : failures.isEmpty() ? store.value("validation_status").toArray() : failures) {
        const QJsonObject o = f.toObject();
        out.problems.push_back(text(o.value("code")) + (o.contains("explanation") ? ": " + text(o.value("explanation")) : std::string()));
    }
}

// A reader that did not open: no manifest at all is the usual case, not a failure; a manifest that is there but broken
// or badly signed is invalid credentials; anything else (a file c2pa-rs cannot follow, a read error) says nothing about
// credentials. False for that last.
bool unreadable(const std::string& path, CredentialSummary& out, std::string* error) {
    const std::string why = lastError();
    const std::string kind = why.substr(0, why.find(':'));
    if (kind == "ManifestNotFound") return true;
    if (kind == "Manifest" || kind == "Signature" || kind == "Verify" || kind == "Decoding" || kind == "Assertion" || kind == "AssertionNotFound" ||
        kind == "HashMismatch") {
        out.present = true;
        out.state = "Invalid";
        out.problems.push_back(why);
        return true;
    }
    if (error) *error = "Cannot read the Content Credentials of " + path + ": " + why;
    return false;
}

// A file's credentials as an ingredient: the file itself where they are in it, else the manifest beside it.
struct IngredientSource {
    std::string file, format;
};
bool ingredientSource(const std::string& listed, IngredientSource& out) {
    const std::string own = mediaFileOnDisk(listed);  // (a layer's file, an image sequence's first frame)
    if (!credentialFormat(own).empty()) {
        out = {own, credentialFormat(own)};
        return true;
    }
    if (hasCredentialSidecar(listed)) {
        out = {credentialSidecarPath(listed), "application/c2pa"};
        return true;
    }
    return false;
}

}  // namespace

bool checkCredentialSigner(const CredentialSigner& signer, std::string* error) {
    std::string why;
    C2paSigner* sign = makeSigner(signer, why);
    if (sign) {
        // A tenth of a second of silence, signed in memory: refuses what the export's signing would.
        QByteArray wav(44 + 1600, '\0');
        auto put = [&](int at, const char* four) { std::copy(four, four + 4, wav.begin() + at); };
        auto u32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; ++i) wav[at + i] = char(v >> (8 * i)); };
        auto u16 = [&](int at, uint16_t v) { wav[at] = char(v), wav[at + 1] = char(v >> 8); };
        put(0, "RIFF"), u32(4, uint32_t(wav.size() - 8)), put(8, "WAVE"), put(12, "fmt "), u32(16, 16), u16(20, 1), u16(22, 1);
        u32(24, 8000), u32(28, 16000), u16(32, 2), u16(34, 16), put(36, "data"), u32(40, 1600);
        QBuffer from(&wav), to;
        from.open(QIODevice::ReadOnly);
        to.open(QIODevice::ReadWrite);
        C2paBuilder* builder = makeBuilder(credentialManifestJson("check.wav", {}, {}, ""));
        why = builder ? signStream(builder, "audio/wav", &from, &to, sign) : "Content Credentials: " + lastError();
        if (builder) c2pa_free(builder);
        c2pa_free(sign);
    }
    if (error) *error = why;
    return why.empty();
}

bool readContentCredentials(const std::string& path, CredentialSummary& out, std::string* error) {
    out = {};
    const std::string format = credentialFormat(path);
    if (format.empty()) {
        // A manifest beside it, binding its bytes (or its frames', or its package's files').
        if (!hasCredentialSidecar(path)) return true;  // (none can be there)
        QFile beside(QString::fromStdString(credentialSidecarPath(path)));
        if (!beside.open(QIODevice::ReadOnly)) {
            if (error) *error = "Cannot read " + beside.fileName().toStdString();
            return false;
        }
        const QByteArray manifest = beside.readAll();
        beside.close();
        out.beside = true;
        ConcatStream bound(credentialBoundFiles(path));
        C2paContext* context = c2pa_context_new();
        C2paReader* reader = context ? c2pa_reader_from_context(context) : nullptr;
        if (reader)
            reader = c2pa_reader_with_manifest_data_and_stream(reader, besideFormat(path).c_str(), bound.get(),
                                                               reinterpret_cast<const unsigned char*>(manifest.constData()), size_t(manifest.size()));
        if (context) c2pa_free(context);
        if (!reader) {
            const bool ok = unreadable(path, out, error);
            out.beside = true;
            return ok;
        }
        summarize(reader, out);
        c2pa_free(reader);
        out.beside = true;
        return true;
    }
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = "Cannot read " + path;
        return false;
    }
    FileStream stream(&file);
    C2paContext* context = c2pa_context_new();
    C2paReader* reader = context ? c2pa_reader_from_context(context) : nullptr;
    if (reader) reader = c2pa_reader_with_stream(reader, format.c_str(), stream.get());
    if (context) c2pa_free(context);
    if (!reader) return unreadable(path, out, error);
    summarize(reader, out);
    c2pa_free(reader);
    return true;
}

bool signContentCredentials(const std::string& path, const Provenance& provenance, const CredentialSigner& signer,
                            const CredentialOptions& options, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const std::string format = credentialFormat(path);
    const bool beside = format.empty();
    const std::vector<std::string> bound = beside ? credentialBoundFiles(path) : std::vector<std::string>{};
    if (beside && bound.empty()) return fail("There is nothing at " + path + " to sign");
    std::string why;
    C2paSigner* sign = makeSigner(signer, why);
    if (!sign) return fail(why);
    // The ingredients: a file already signed (a camera original) keeps its credentials as the parent of the new ones;
    // a copy made from a signed file has that file's as its parent; source files with credentials of their own (a
    // camera's signature...) go in as components, kept whole.
    struct Ingredient {
        IngredientSource from;
        QString title, relationship, label;
    };
    std::vector<Ingredient> ingredients;
    std::vector<std::string> redactions;
    CredentialSummary own;
    // (beside it, a manifest left from an earlier file of the name that no longer binds is not kept)
    const bool reSigned = readContentCredentials(path, own) && own.present && !(beside && own.state == "Invalid");
    IngredientSource src;
    if (reSigned && ingredientSource(path, src)) {
        ingredients.push_back({src, QFileInfo(QString::fromStdString(path)).fileName(), "parentOf", "parent"});
    } else if (!options.parent.empty() && ingredientSource(options.parent, src)) {
        CredentialSummary has;
        if (readContentCredentials(options.parent, has) && has.present) {
            ingredients.push_back({src, QFileInfo(QString::fromStdString(options.parent)).fileName(), "parentOf", "parent"});
            if (options.redact)
                for (const std::string& r : personAssertions(has.json)) redactions.push_back(r);
        }
    }
    const bool parented = !ingredients.empty();
    std::set<std::string> taken;
    int n = 0;
    for (const std::string& listed : options.sources) {
        if (!ingredientSource(listed, src) || !taken.insert(src.file).second) continue;
        CredentialSummary has;
        if (!readContentCredentials(src.format == "application/c2pa" ? listed : src.file, has) || !has.present) continue;
        ingredients.push_back({src, QFileInfo(QString::fromStdString(mediaFileOnDisk(listed))).fileName(), "componentOf",
                               QStringLiteral("source_%1").arg(++n)});
        if (options.redact)
            for (const std::string& r : personAssertions(has.json)) redactions.push_back(r);
    }
    const std::string definition = credentialManifestJson(path, provenance, options, parented ? "parent" : "", redactions);
    // (beside the file, the signer is the builder's context's, and consumed with it)
    C2paBuilder* builder = makeBuilder(definition, beside ? sign : nullptr);
    if (beside) sign = nullptr;
    if (!builder) {
        if (sign) c2pa_free(sign);
        return fail("Content Credentials: " + lastError());
    }
    for (const Ingredient& ing : ingredients) {
        QFile in(QString::fromStdString(ing.from.file));
        bool added = in.open(QIODevice::ReadOnly);
        if (added) {
            FileStream stream(&in);
            const QJsonObject desc{{"title", ing.title}, {"relationship", ing.relationship}, {"label", ing.label}};
            added = c2pa_builder_add_ingredient_from_stream(builder, QJsonDocument(desc).toJson(QJsonDocument::Compact).constData(),
                                                            ing.from.format.c_str(), stream.get()) == 0;
        }
        if (!added && ing.relationship == "parentOf") {
            const std::string err = lastError();
            c2pa_free(builder);
            if (sign) c2pa_free(sign);
            return fail("Could not keep the credentials of " + ing.from.file + ": " + err);
        }
        if (!added) {
            (void)lastError();  // (one source c2pa-rs cannot take in does not cost the export its own credentials)
            continue;
        }
        if (ing.relationship == "componentOf") {
            const QJsonObject placed{{"action", "c2pa.placed"}, {"parameters", QJsonObject{{"ingredientIds", QJsonArray{ing.label}}}}};
            c2pa_builder_add_action(builder, QJsonDocument(placed).toJson(QJsonDocument::Compact).constData());
        }
    }
    if (beside) {
        // A manifest beside it binding every byte it names, written whole or not at all.
        ConcatStream all(bound);
        const unsigned char* bytes = nullptr;
        int64_t len = -1;
        if (c2pa_builder_update_hash_from_stream(builder, "application/c2pa", all.get()) == 0)
            len = c2pa_builder_sign_embeddable(builder, "application/c2pa", &bytes);
        const std::string err = len < 0 ? lastError() : std::string();
        c2pa_free(builder);
        if (len < 0) {
            if (bytes) c2pa_free(bytes);
            return fail("Signing failed: " + err);
        }
        QSaveFile out(QString::fromStdString(credentialSidecarPath(path)));
        const bool wrote = out.open(QIODevice::WriteOnly) && out.write(reinterpret_cast<const char*>(bytes), qint64(len)) == qint64(len) && out.commit();
        c2pa_free(bytes);
        return wrote ? true : fail("Cannot write " + out.fileName().toStdString());
    }
    // Signed into a new copy beside it, which then takes the file's place (the file is untouched on failure).
    QFile source(QString::fromStdString(path));
    // A plain file rather than a QTemporaryFile, whose handle Windows may still hold when it is renamed.
    QFile dest;
    for (int i = 0; i < 100 && !dest.isOpen(); ++i) {
        dest.setFileName(QString::fromStdString(path) + QStringLiteral(".signing-%1").arg(QRandomGenerator::global()->generate(), 0, 36));
        dest.open(QIODevice::ReadWrite | QIODevice::NewOnly);
    }
    bool ok = source.open(QIODevice::ReadOnly) && dest.isOpen();
    why = ok ? std::string() : "Cannot write beside " + path;
    if (ok) why = signStream(builder, format.c_str(), &source, &dest, sign), ok = why.empty();
    c2pa_free(builder);
    c2pa_free(sign);
    const QFileDevice::Permissions permissions = source.permissions();
    source.close();
    dest.close();
    if (!ok) {
        if (dest.exists()) dest.remove();
        return fail(why);
    }
    dest.setPermissions(permissions);  // (as the file was)
    // Windows refuses to replace a file something has open for a moment (a virus scanner looking at the new copy):
    // tried again for a second or so.
    std::error_code ec;
    const std::filesystem::path from(dest.fileName().toStdU16String()), to(QString::fromStdString(path).toStdU16String());
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::filesystem::rename(from, to, ec);
        if (!ec) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (ec) {
        dest.remove();
        return fail("Cannot replace " + path + " with its signed copy: " + ec.message());
    }
    return true;
}

#endif

}  // namespace montage
