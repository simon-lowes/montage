#include "ContentCredentials.h"
#include "ImageSequence.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryFile>
#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

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

}  // namespace

bool contentCredentialsFormat(const std::string& path) { return !credentialFormat(path).empty(); }

std::string credentialsLabel(const CredentialSummary& c) {
    if (!c.present) return {};
    std::string out = c.issuer.empty() ? "Content Credentials" : "Signed by " + c.issuer;
    if (!c.signedAt.empty()) out += ", " + c.signedAt.substr(0, 10);
    if (!c.generator.empty()) out += ", made with " + c.generator;
    std::string state = c.state;
    std::transform(state.begin(), state.end(), state.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    if (!state.empty()) out += " (" + state + ")";
    if (c.aiGenerated) out += "; made with AI";
    return out;
}

std::string credentialManifestJson(const std::string& path, const Provenance& provenance, const CredentialOptions& options) {
    const QString name = QFileInfo(QString::fromStdString(path)).fileName();
    const QJsonObject montage{{"name", "Montage"}, {"version", MONTAGE_VERSION}};
    // Made by Montage from other media (some of it made by AI, when it is), then each model's part.
    QJsonArray actions;
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
    return QJsonDocument(QJsonObject{{"claim_generator_info", QJsonArray{montage}},
                                     {"title", options.title.empty() ? name : QString::fromStdString(options.title)},
                                     {"format", QString::fromStdString(credentialFormat(path))},
                                     {"assertions", assertions}})
        .toJson(QJsonDocument::Compact)
        .toStdString();
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

C2paBuilder* makeBuilder(const std::string& definition) {
    C2paContext* context = c2pa_context_new();
    C2paBuilder* builder = context ? c2pa_builder_from_context(context) : nullptr;
    if (builder) builder = c2pa_builder_with_definition(builder, definition.c_str());
    if (context) c2pa_free(context);
    return builder;
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
        C2paBuilder* builder = makeBuilder(credentialManifestJson("check.wav", {}, {}));
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
    if (format.empty()) return true;  // (none can be there)
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
    if (!reader) {
        const std::string why = lastError();
        // No manifest at all is the usual case, not a failure.
        if (why.find("ManifestNotFound") != std::string::npos || why.find("JumbfNotFound") != std::string::npos ||
            why.find("not found") != std::string::npos)
            return true;
        out.present = true;
        out.state = "Invalid";
        out.problems.push_back(why);
        return true;
    }
    char* json = c2pa_reader_json(reader);
    out.json = json ? json : "";
    if (json) c2pa_free(json);
    c2pa_free(reader);
    const QJsonObject store = QJsonDocument::fromJson(QByteArray::fromStdString(out.json)).object();
    const QJsonObject manifests = store.value("manifests").toObject();
    const QJsonObject active = manifests.value(store.value("active_manifest").toString()).toObject();
    out.present = !active.isEmpty();
    if (!out.present) return true;
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
    const QJsonArray failures = store.value("validation_results").toObject().value("activeManifest").toObject().value("failure").toArray();
    for (const QJsonValue& f : failures.isEmpty() ? store.value("validation_status").toArray() : failures) {
        const QJsonObject o = f.toObject();
        out.problems.push_back(text(o.value("code")) + (o.contains("explanation") ? ": " + text(o.value("explanation")) : std::string()));
    }
    return true;
}

bool signContentCredentials(const std::string& path, const Provenance& provenance, const CredentialSigner& signer,
                            const CredentialOptions& options, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const std::string format = credentialFormat(path);
    if (format.empty())
        return fail("Content Credentials cannot go in ." + QFileInfo(QString::fromStdString(path)).suffix().toStdString() +
                    " files (MP4, MOV, WAV, MP3, JPEG, PNG and TIFF can hold them)");
    std::string why;
    C2paSigner* sign = makeSigner(signer, why);
    if (!sign) return fail(why);
    C2paBuilder* builder = makeBuilder(credentialManifestJson(path, provenance, options));
    if (!builder) {
        c2pa_free(sign);
        return fail("Content Credentials: " + lastError());
    }
    // Source files with credentials of their own (a camera's signature...) go in as ingredients, kept whole.
    int n = 0;
    std::set<std::string> added;
    for (const std::string& listed : options.sources) {
        const std::string source = mediaFileOnDisk(listed);  // (a layer's file, an image sequence's first frame)
        if (!added.insert(source).second) continue;
        const std::string type = credentialFormat(source);
        if (type.empty()) continue;
        QFile in(QString::fromStdString(source));
        if (!in.open(QIODevice::ReadOnly)) continue;
        CredentialSummary has;
        if (!readContentCredentials(source, has) || !has.present) continue;
        in.seek(0);
        FileStream stream(&in);
        const QJsonObject ingredient{{"title", QFileInfo(in).fileName()}, {"relationship", "componentOf"},
                                     {"label", QStringLiteral("source_%1").arg(++n)}};
        if (c2pa_builder_add_ingredient_from_stream(builder, QJsonDocument(ingredient).toJson(QJsonDocument::Compact).constData(),
                                                    type.c_str(), stream.get()) != 0) {
            const std::string why = lastError();
            c2pa_free(builder);
            c2pa_free(sign);
            return fail("Could not add the credentials of " + QFileInfo(in).fileName().toStdString() + ": " + why);
        }
        const QJsonObject placed{{"action", "c2pa.placed"},
                                 {"parameters", QJsonObject{{"ingredientIds", QJsonArray{QStringLiteral("source_%1").arg(n)}}}}};
        c2pa_builder_add_action(builder, QJsonDocument(placed).toJson(QJsonDocument::Compact).constData());
    }
    // Signed into a new copy beside it, which then takes the file's place (the file is untouched on failure).
    QFile source(QString::fromStdString(path));
    QTemporaryFile dest(QString::fromStdString(path) + QStringLiteral(".signing-XXXXXX"));
    bool ok = source.open(QIODevice::ReadOnly) && dest.open();
    why = ok ? std::string() : "Cannot write beside " + path;
    if (ok) why = signStream(builder, format.c_str(), &source, &dest, sign), ok = why.empty();
    c2pa_free(builder);
    c2pa_free(sign);
    const QFileDevice::Permissions permissions = source.permissions();
    source.close();
    if (!ok) return fail(why);
    dest.close();
    dest.setPermissions(permissions);  // (a temporary file is the owner's alone)
    std::error_code ec;
    std::filesystem::rename(std::filesystem::path(dest.fileName().toStdU16String()), std::filesystem::path(QString::fromStdString(path).toStdU16String()), ec);
    if (ec) return fail("Cannot replace " + path + " with its signed copy: " + ec.message());
    dest.setAutoRemove(false);
    return true;
}

#endif

}  // namespace montage
