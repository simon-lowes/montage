// Montage — digital cinema security (render/DcpSecurity.h).
#include "DcpSecurity.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimeZone>
#include <QXmlStreamReader>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include "XmlCanonical.h"

#ifdef MONTAGE_WITH_OPENSSL
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif

namespace montage::dcp {

namespace {

const char* kDsig = "http://www.w3.org/2000/09/xmldsig#";
const uint8_t kStructureId[16] = {0xf1, 0xdc, 0x12, 0x44, 0x60, 0x16, 0x9a, 0x0e, 0x85, 0xbc, 0x30, 0x06, 0x42, 0xf8, 0x66, 0xab};

std::string base64(const uint8_t* data, size_t size, bool wrap = false) {
    const QByteArray b = QByteArray(reinterpret_cast<const char*>(data), qsizetype(size)).toBase64();
    std::string out;
    for (qsizetype i = 0; i < b.size(); i += 64) {
        if (wrap && i) out += '\n';
        out += b.mid(i, wrap ? 64 : b.size()).toStdString();
        if (!wrap) break;
    }
    return out;
}

std::string base64(const std::string& s, bool wrap = false) { return base64(reinterpret_cast<const uint8_t*>(s.data()), s.size(), wrap); }

std::string unbase64(const QString& text) {
    QString clean = text;
    clean.remove(QRegularExpression(QStringLiteral("\\s")));
    return QByteArray::fromBase64(clean.toLatin1()).toStdString();
}

std::string xmlEscaped(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

// Replaces the one occurrence of `marker` in `s`.
bool fill(std::string& s, const std::string& marker, const std::string& value) {
    const size_t at = s.find(marker);
    if (at == std::string::npos) return false;
    s.replace(at, marker.size(), value);
    return true;
}

}  // namespace

std::string kdmTime(int64_t seconds, int offsetMinutes) {
    const QDateTime t = QDateTime::fromSecsSinceEpoch(seconds, QTimeZone(offsetMinutes * 60));
    const int m = std::abs(offsetMinutes);
    return (t.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss")) + (offsetMinutes < 0 ? '-' : '+') +
            QStringLiteral("%1:%2").arg(m / 60, 2, 10, QChar('0')).arg(m % 60, 2, 10, QChar('0')))
        .toStdString();
}

bool writeWholeFile(const std::string& file, const std::string& text, bool secret) {
    const QString path = QString::fromStdString(file);
#ifndef _WIN32
    if (secret) {
        // Created owner-only (not made and then narrowed), in full, then put in place.
        const QByteArray tmp = QFile::encodeName(path + QStringLiteral(".partial"));
        const int fd = ::open(tmp.constData(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) return false;
        size_t done = 0;
        while (done < text.size()) {
            const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
            if (n <= 0) break;
            done += size_t(n);
        }
        const bool whole = done == text.size() && ::fsync(fd) == 0;
        if (::close(fd) != 0 || !whole || ::rename(tmp.constData(), QFile::encodeName(path).constData()) != 0) {
            ::unlink(tmp.constData());
            return false;
        }
        return true;
    }
#else
    (void)secret;  // (the data folder is the user's own on Windows)
#endif
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    if (f.write(text.data(), qint64(text.size())) != qint64(text.size())) {
        f.cancelWriting();
        return false;
    }
    return f.commit();
}

std::string signerFolder() {
    if (const QByteArray env = qgetenv("MONTAGE_DCP_CERTIFICATES"); !env.isEmpty()) return env.toStdString();
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("dcp-certificates")).toStdString();
}

std::string dkdmFolder() {
    if (const QByteArray env = qgetenv("MONTAGE_DCP_DKDMS"); !env.isEmpty()) return env.toStdString();
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("dkdm")).toStdString();
}

std::vector<std::string> splitCertificates(const std::string& text) {
    std::vector<std::string> out;
    const std::string begin = "-----BEGIN CERTIFICATE-----", end = "-----END CERTIFICATE-----";
    for (size_t at = text.find(begin); at != std::string::npos; at = text.find(begin, at + 1)) {
        const size_t e = text.find(end, at);
        if (e == std::string::npos) break;
        out.push_back(text.substr(at, e + end.size() - at) + "\n");
    }
    return out;
}

#ifdef MONTAGE_WITH_OPENSSL

namespace {

struct Free {
    void operator()(X509* p) const { X509_free(p); }
    void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); }
    void operator()(BIO* p) const { BIO_free(p); }
    void operator()(EVP_PKEY_CTX* p) const { EVP_PKEY_CTX_free(p); }
    void operator()(EVP_MD_CTX* p) const { EVP_MD_CTX_free(p); }
    void operator()(EVP_CIPHER_CTX* p) const { EVP_CIPHER_CTX_free(p); }
    void operator()(BIGNUM* p) const { BN_free(p); }
    void operator()(X509_EXTENSION* p) const { X509_EXTENSION_free(p); }
};
template <class T>
using Ptr = std::unique_ptr<T, Free>;

std::string opensslError(const std::string& what) {
    char buf[256] = {};
    if (const unsigned long e = ERR_get_error()) ERR_error_string_n(e, buf, sizeof buf);
    ERR_clear_error();
    return buf[0] ? what + " (" + buf + ")" : what;
}

Ptr<X509> certificateFrom(const std::string& pem) {
    Ptr<BIO> bio(BIO_new_mem_buf(pem.data(), int(pem.size())));
    return Ptr<X509>(bio ? PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr) : nullptr);
}

Ptr<EVP_PKEY> privateKeyFrom(const std::string& pem) {
    Ptr<BIO> bio(BIO_new_mem_buf(pem.data(), int(pem.size())));
    return Ptr<EVP_PKEY>(bio ? PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr) : nullptr);
}

std::string bioText(BIO* bio) {
    char* data = nullptr;
    const long n = BIO_get_mem_data(bio, &data);
    return n > 0 ? std::string(data, size_t(n)) : std::string();
}

std::string pemOf(X509* c) {
    Ptr<BIO> bio(BIO_new(BIO_s_mem()));
    PEM_write_bio_X509(bio.get(), c);
    return bioText(bio.get());
}

std::string nameText(const X509_NAME* name) {
    Ptr<BIO> bio(BIO_new(BIO_s_mem()));
    X509_NAME_print_ex(bio.get(), name, 0, XN_FLAG_RFC2253);
    return bioText(bio.get());
}

std::string serialText(const X509* c) {
    Ptr<BIGNUM> bn(ASN1_INTEGER_to_BN(X509_get0_serialNumber(c), nullptr));
    if (!bn) return {};
    char* dec = BN_bn2dec(bn.get());
    std::string out = dec ? dec : "";
    OPENSSL_free(dec);
    return out;
}

std::string thumbprintBytes(X509* c) {
    unsigned char* der = nullptr;
    const int n = i2d_re_X509_tbs(c, &der);
    if (n <= 0) return {};
    uint8_t md[20];
    SHA1(der, size_t(n), md);
    OPENSSL_free(der);
    return std::string(reinterpret_cast<char*>(md), 20);
}

std::string entryText(const X509_NAME* name, int nid) {
    const int at = X509_NAME_get_index_by_NID(name, nid, -1);
    if (at < 0) return {};
    const ASN1_STRING* s = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(name, at));
    unsigned char* utf8 = nullptr;
    const int n = ASN1_STRING_to_UTF8(&utf8, s);
    std::string out = n > 0 ? std::string(reinterpret_cast<char*>(utf8), size_t(n)) : std::string();
    OPENSSL_free(utf8);
    return out;
}

std::string isoTime(const ASN1_TIME* t) {
    struct tm tm {};
    if (!ASN1_TIME_to_tm(t, &tm)) return {};
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// The PEM's base64 body, as ds:X509Certificate holds it (64-character lines, no trailing break).
std::string certificateBody(const std::string& pem) {
    std::string body;
    for (const QString& line : QString::fromStdString(pem).split('\n'))
        if (!line.startsWith(QStringLiteral("-----")) && !line.trimmed().isEmpty()) body += (body.empty() ? "" : "\n") + line.trimmed().toStdString();
    return body;
}

std::string digest(const std::string& data, const EVP_MD* md) {
    uint8_t out[EVP_MAX_MD_SIZE];
    unsigned n = 0;
    EVP_Digest(data.data(), data.size(), out, &n, md, nullptr);
    return std::string(reinterpret_cast<char*>(out), n);
}

bool signWith(EVP_PKEY* key, const std::string& data, std::string& signature) {
    Ptr<EVP_MD_CTX> ctx(EVP_MD_CTX_new());
    size_t n = 0;
    if (!ctx || EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, key) != 1 ||
        EVP_DigestSign(ctx.get(), nullptr, &n, reinterpret_cast<const uint8_t*>(data.data()), data.size()) != 1)
        return false;
    signature.resize(n);
    if (EVP_DigestSign(ctx.get(), reinterpret_cast<uint8_t*>(signature.data()), &n, reinterpret_cast<const uint8_t*>(data.data()), data.size()) != 1)
        return false;
    signature.resize(n);
    return true;
}

bool verifyWith(EVP_PKEY* key, const EVP_MD* md, const std::string& data, const std::string& signature) {
    Ptr<EVP_MD_CTX> ctx(EVP_MD_CTX_new());
    return ctx && EVP_DigestVerifyInit(ctx.get(), nullptr, md, nullptr, key) == 1 &&
           EVP_DigestVerify(ctx.get(), reinterpret_cast<const uint8_t*>(signature.data()), signature.size(),
                            reinterpret_cast<const uint8_t*>(data.data()), data.size()) == 1;
}

// One ds:X509Data for each certificate, leaf first (the KeyInfo of a signature).
std::string keyInfo(const CertificateChain& chain, const std::string& prefix, const std::string& indent) {
    std::string out;
    for (auto it = chain.certificates.rbegin(); it != chain.certificates.rend(); ++it) {
        Ptr<X509> c = certificateFrom(*it);
        if (!c) continue;
        out += indent + "<" + prefix + ":X509Data>\n";
        out += indent + "  <" + prefix + ":X509IssuerSerial>\n";
        out += indent + "    <" + prefix + ":X509IssuerName>" + xmlEscaped(nameText(X509_get_issuer_name(c.get()))) + "</" + prefix + ":X509IssuerName>\n";
        out += indent + "    <" + prefix + ":X509SerialNumber>" + serialText(c.get()) + "</" + prefix + ":X509SerialNumber>\n";
        out += indent + "  </" + prefix + ":X509IssuerSerial>\n";
        out += indent + "  <" + prefix + ":X509Certificate>" + certificateBody(*it) + "</" + prefix + ":X509Certificate>\n";
        out += indent + "</" + prefix + ":X509Data>\n";
    }
    return out;
}

// What a signature's parts say, read from the document.
struct SignatureParts {
    struct Reference {
        QString uri, digestMethod, digest;
        bool enveloped = false;
    };
    std::vector<Reference> references;
    QString canonicalization, method, value;
    std::vector<std::string> certificates;  // PEM, as KeyInfo lists them (leaf first)
};

bool readSignature(const std::string& xml, SignatureParts& out) {
    QXmlStreamReader x(QByteArray(xml.data(), qsizetype(xml.size())));
    bool inSignature = false;
    QString element;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            const QString name = x.name().toString();
            const bool dsig = x.namespaceUri() == QLatin1String(kDsig);
            if (dsig && name == QLatin1String("Signature")) inSignature = true;
            if (!inSignature || !dsig) continue;
            element = name;
            const QString algorithm = x.attributes().value(QStringLiteral("Algorithm")).toString();
            if (name == QLatin1String("Reference")) out.references.push_back({x.attributes().value(QStringLiteral("URI")).toString(), {}, {}});
            else if (name == QLatin1String("CanonicalizationMethod")) out.canonicalization = algorithm;
            else if (name == QLatin1String("SignatureMethod")) out.method = algorithm;
            else if (name == QLatin1String("DigestMethod") && !out.references.empty()) out.references.back().digestMethod = algorithm;
            else if (name == QLatin1String("Transform") && !out.references.empty() && algorithm.endsWith(QLatin1String("enveloped-signature")))
                out.references.back().enveloped = true;
            else if (name == QLatin1String("X509Certificate")) out.certificates.emplace_back();
        } else if (x.isCharacters() && inSignature) {
            if (element == QLatin1String("DigestValue") && !out.references.empty()) out.references.back().digest += x.text();
            else if (element == QLatin1String("SignatureValue")) out.value += x.text();
            else if (element == QLatin1String("X509Certificate") && !out.certificates.empty()) out.certificates.back() += x.text().toString().toStdString();
        } else if (x.isEndElement()) {
            element.clear();
            if (x.namespaceUri() == QLatin1String(kDsig) && x.name() == QLatin1String("Signature")) inSignature = false;
        }
    }
    // Back to PEM: the base64 in 64-character lines.
    for (std::string& body : out.certificates) {
        std::string flat;
        for (char c : body)
            if (!std::isspace(static_cast<unsigned char>(c))) flat += c;
        std::string pem = "-----BEGIN CERTIFICATE-----\n";
        for (size_t i = 0; i < flat.size(); i += 64) pem += flat.substr(i, 64) + "\n";
        body = pem + "-----END CERTIFICATE-----\n";
    }
    return !x.hasError() && !out.references.empty();
}

const EVP_MD* digestFor(const QString& uri) {
    if (uri.endsWith(QLatin1String("#sha1"))) return EVP_sha1();
    if (uri.endsWith(QLatin1String("#sha256"))) return EVP_sha256();
    return nullptr;
}

}  // namespace

bool securityAvailable() { return true; }

bool makeCertificateChain(const std::string& organisation, CertificateChain& out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = opensslError(why);
        return false;
    };
    // PrintableString: letters, digits, space and ' ( ) + , - . / : = ?
    std::string org;
    for (char c : organisation)
        if (std::isalnum(static_cast<unsigned char>(c)) || std::strchr(" '()+,-./:=?", c)) org += c;
    while (!org.empty() && org.back() == ' ') org.pop_back();
    if (org.empty()) org = "Montage";
    const char* names[3] = {".montage.smpte-430-2.ROOT", ".montage.smpte-430-2.INTERMEDIATE", "CS.montage.smpte-430-2.LEAF"};
    const char* constraints[3] = {"critical,CA:TRUE,pathlen:3", "critical,CA:TRUE,pathlen:2", "critical,CA:FALSE"};
    const char* usages[3] = {"keyCertSign,cRLSign", "keyCertSign,cRLSign", "digitalSignature,keyEncipherment"};
    const char* authority[3] = {"keyid:always,issuer:always", "keyid:always,issuer:always", "keyid,issuer:always"};
    // Valid from two days ago (checkers read the issue date without its offset; KDM tools want the window to start on
    // a later day), each inside its issuer's, under ten years.
    const time_t from = std::time(nullptr) - 2 * 86400;
    const long days[3] = {3600, 3599, 3598};
    Ptr<EVP_PKEY> keys[3];
    Ptr<X509> certs[3];
    for (int i = 0; i < 3; ++i) {
        keys[i].reset(EVP_RSA_gen(2048));
        if (!keys[i]) return fail("Cannot make an RSA key");
        certs[i].reset(X509_new());
        X509* c = certs[i].get();
        X509_set_version(c, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(c), 5 + i);
        ASN1_TIME_set(X509_getm_notBefore(c), from);
        ASN1_TIME_set(X509_getm_notAfter(c), from + days[i] * 86400);
        X509_set_pubkey(c, keys[i].get());
        // dnQualifier: the public key's (PKCS #1) SHA-1, base64.
        unsigned char* der = nullptr;
        const int n = i2d_PublicKey(keys[i].get(), &der);
        if (n <= 0) return fail("Cannot read the public key");
        uint8_t md[20];
        SHA1(der, size_t(n), md);
        OPENSSL_free(der);
        const std::string dnq = base64(md, 20);
        X509_NAME* name = X509_get_subject_name(c);
        const std::pair<const char*, std::string> entries[] = {{"O", org}, {"OU", "Montage DCP signer"}, {"CN", names[i]}, {"dnQualifier", dnq}};
        for (const auto& [field, value] : entries)
            if (!X509_NAME_add_entry_by_txt(name, field, V_ASN1_PRINTABLESTRING, reinterpret_cast<const unsigned char*>(value.data()), int(value.size()), -1, 0))
                return fail("Cannot name the certificate");
        X509* issuer = i == 0 ? c : certs[i - 1].get();
        X509_set_issuer_name(c, X509_get_subject_name(issuer));
        X509V3_CTX ctx;
        X509V3_set_ctx(&ctx, issuer, c, nullptr, nullptr, 0);
        const std::pair<int, const char*> exts[] = {{NID_basic_constraints, constraints[i]},
                                                    {NID_key_usage, usages[i]},
                                                    {NID_subject_key_identifier, "hash"},
                                                    {NID_authority_key_identifier, authority[i]}};
        for (const auto& [nid, value] : exts) {
            Ptr<X509_EXTENSION> ext(X509V3_EXT_conf_nid(nullptr, &ctx, nid, value));
            if (!ext || !X509_add_ext(c, ext.get(), -1)) return fail("Cannot add the certificate's extensions");
        }
        if (!X509_sign(c, keys[i == 0 ? 0 : i - 1].get(), EVP_sha256())) return fail("Cannot sign the certificate");
    }
    CertificateChain chain;
    for (auto& c : certs) chain.certificates.push_back(pemOf(c.get()));
    Ptr<BIO> bio(BIO_new(BIO_s_mem()));
    if (!PEM_write_bio_PrivateKey(bio.get(), keys[2].get(), nullptr, nullptr, 0, nullptr, nullptr)) return fail("Cannot write the key");
    chain.privateKey = bioText(bio.get());
    out = std::move(chain);
    return true;
}

bool signerCertificateChain(CertificateChain& out, std::string* error, const std::string& folderPath) {
    // One chain for good: the DKDMs (the only copies of encrypted packages' keys) are encrypted to its leaf, so a chain
    // with a file missing is reported, never replaced; and two exports at once make it once.
    static std::mutex making;
    const std::lock_guard<std::mutex> lock(making);
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const QDir dir(QString::fromStdString(folderPath.empty() ? signerFolder() : folderPath));
    const char* files[] = {"root.pem", "intermediate.pem", "leaf.pem", "leaf.key"};
    auto count = [&] {
        int n = 0;
        for (const char* name : files) n += QFileInfo::exists(dir.filePath(QString::fromLatin1(name))) ? 1 : 0;
        return n;
    };
    int present = count();
    // (and two programs at once, the app and the command-line tool sharing the folder: a lock file in it while one
    // is made, and another look once it is held)
    std::optional<QLockFile> others;
    if (present != 4 && QDir().mkpath(dir.path())) {
        others.emplace(dir.filePath(QStringLiteral(".chain.lock")));
        others->setStaleLockTime(120000);
        if (others->tryLock(60000)) present = count();
        else if (others->error() == QLockFile::LockFailedError)
            return fail("Another program is making the DCP signing chain in " + dir.path().toStdString() + ": try again");
        // (a folder it cannot lock in: said below, as the chain is incomplete or cannot be written there)
    }
    if (present == 4) {
        CertificateChain chain;
        for (const char* name : files) {
            QFile f(dir.filePath(QString::fromLatin1(name)));
            if (!f.open(QIODevice::ReadOnly)) return fail("Cannot read " + f.fileName().toStdString());
            std::string text = f.readAll().toStdString();
            if (text.empty()) return fail(f.fileName().toStdString() + " is empty: restore it from a backup (DKDMs need this signing chain)");
            if (std::strcmp(name, "leaf.key") == 0) chain.privateKey = std::move(text);
            else chain.certificates.push_back(std::move(text));
        }
        out = std::move(chain);
        return true;
    }
    if (present > 0)
        return fail("The DCP signing chain in " + dir.path().toStdString() +
                    " is incomplete: restore its missing files from a backup (DKDMs need this chain; a new one cannot open them)");
    CertificateChain chain;
    if (!makeCertificateChain("Montage", chain, error)) return false;
    // A folder only its owner reads, the key file likewise from the moment it exists; each file written whole.
    if (!QDir().mkpath(dir.path())) return fail("Cannot make " + dir.path().toStdString());
    QFile::setPermissions(dir.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    std::vector<QString> written;
    auto write = [&](const char* name, const std::string& text, bool secret) {
        const QString path = dir.filePath(QString::fromLatin1(name));
        if (!writeWholeFile(path.toStdString(), text, secret)) return false;
        written.push_back(path);
        return true;
    };
    bool ok = true;
    for (int i = 0; i < 3 && ok; ++i) ok = write(files[i], chain.certificates[size_t(i)], false);
    ok = ok && write("leaf.key", chain.privateKey, true);
    if (!ok) {
        for (const QString& f : written) QFile::remove(f);  // nothing depends on a chain not yet whole
        return fail("Cannot write the DCP signing chain to " + dir.path().toStdString());
    }
    out = std::move(chain);
    return true;
}

bool readCertificate(const std::string& pem, CertificateInfo& out, std::string* error) {
    Ptr<X509> c = certificateFrom(pem);
    if (!c) {
        if (error) *error = "Not a PEM certificate";
        return false;
    }
    CertificateInfo info;
    info.subject = nameText(X509_get_subject_name(c.get()));
    info.issuer = nameText(X509_get_issuer_name(c.get()));
    info.serial = serialText(c.get());
    info.thumbprint = base64(thumbprintBytes(c.get()));
    info.commonName = entryText(X509_get_subject_name(c.get()), NID_commonName);
    info.organisation = entryText(X509_get_subject_name(c.get()), NID_organizationName);
    info.dnQualifier = entryText(X509_get_subject_name(c.get()), NID_dnQualifier);
    info.notBefore = isoTime(X509_get0_notBefore(c.get()));
    info.notAfter = isoTime(X509_get0_notAfter(c.get()));
    info.ca = X509_check_ca(c.get()) > 0;
    if (EVP_PKEY* key = X509_get0_pubkey(c.get())) info.bits = EVP_PKEY_get_bits(key);
    out = std::move(info);
    return true;
}

bool signXml(std::string& xml, const CertificateChain& chain, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = opensslError(why);
        return false;
    };
    if (chain.certificates.empty()) return fail("No certificates to sign with");
    Ptr<X509> leaf = certificateFrom(chain.certificates.back());
    Ptr<EVP_PKEY> key = privateKeyFrom(chain.privateKey);
    if (!leaf || !key) return fail("The signing certificate or key cannot be read");
    const size_t end = xml.rfind("</");
    if (end == std::string::npos) return fail("Not an XML document");
    std::string sig;
    sig += "  <Signer xmlns:dsig=\"http://www.w3.org/2000/09/xmldsig#\">\n";
    sig += "    <dsig:X509Data>\n      <dsig:X509IssuerSerial>\n";
    sig += "        <dsig:X509IssuerName>" + xmlEscaped(nameText(X509_get_issuer_name(leaf.get()))) + "</dsig:X509IssuerName>\n";
    sig += "        <dsig:X509SerialNumber>" + serialText(leaf.get()) + "</dsig:X509SerialNumber>\n";
    sig += "      </dsig:X509IssuerSerial>\n";
    sig += "      <dsig:X509SubjectName>" + xmlEscaped(nameText(X509_get_subject_name(leaf.get()))) + "</dsig:X509SubjectName>\n";
    sig += "    </dsig:X509Data>\n  </Signer>\n";
    sig += "  <dsig:Signature xmlns:dsig=\"http://www.w3.org/2000/09/xmldsig#\">\n";
    sig += "    <dsig:SignedInfo>\n";
    sig += "      <dsig:CanonicalizationMethod Algorithm=\"http://www.w3.org/TR/2001/REC-xml-c14n-20010315\"/>\n";
    sig += "      <dsig:SignatureMethod Algorithm=\"http://www.w3.org/2001/04/xmldsig-more#rsa-sha256\"/>\n";
    sig += "      <dsig:Reference URI=\"\">\n        <dsig:Transforms>\n";
    sig += "          <dsig:Transform Algorithm=\"http://www.w3.org/2000/09/xmldsig#enveloped-signature\"/>\n";
    sig += "        </dsig:Transforms>\n";
    // SHA-1 here even for SMPTE packages: what servers and checkers ask for.
    sig += "        <dsig:DigestMethod Algorithm=\"http://www.w3.org/2000/09/xmldsig#sha1\"/>\n";
    sig += "        <dsig:DigestValue>@DIGEST@</dsig:DigestValue>\n";
    sig += "      </dsig:Reference>\n    </dsig:SignedInfo>\n";
    sig += "    <dsig:SignatureValue>@SIGNATURE@</dsig:SignatureValue>\n";
    sig += "    <dsig:KeyInfo>\n" + keyInfo(chain, "dsig", "      ") + "    </dsig:KeyInfo>\n";
    sig += "  </dsig:Signature>\n";
    std::string doc = xml;
    doc.insert(end, sig);
    std::string err;
    const std::string whole = canonicalXml(doc, {}, &err);
    if (whole.empty()) return fail(err);
    fill(doc, "@DIGEST@", base64(digest(whole, EVP_sha1())));
    CanonicalOptions info;
    info.element = std::string("{") + kDsig + "}SignedInfo";
    info.envelopedSignature = false;
    const std::string signedInfo = canonicalXml(doc, info, &err);
    std::string signature;
    if (signedInfo.empty() || !signWith(key.get(), signedInfo, signature)) return fail("Cannot sign the document");
    fill(doc, "@SIGNATURE@", base64(signature, true));
    xml = std::move(doc);
    return true;
}

bool verifyXmlSignature(const std::string& xml, std::string* error, SignatureCheck* check) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        ERR_clear_error();
        return false;
    };
    SignatureParts parts;
    if (!readSignature(xml, parts)) return fail("The document has no signature");
    if (parts.certificates.empty()) return fail("The signature names no certificate");
    if (parts.references.empty()) return fail("The signature covers nothing");
    for (const auto& r : parts.references) {
        const EVP_MD* md = digestFor(r.digestMethod);
        if (!md) return fail("Unknown digest method " + r.digestMethod.toStdString());
        CanonicalOptions o;
        if (r.uri.startsWith('#')) o.id = r.uri.mid(1).toStdString(), o.envelopedSignature = r.enveloped;
        else if (!r.uri.isEmpty()) return fail("A reference outside the document: " + r.uri.toStdString());
        std::string err;
        const std::string c = canonicalXml(xml, o, &err);
        if (c.empty()) return fail(err);
        if (base64(digest(c, md)) != base64(unbase64(r.digest)))
            return fail("The document has changed since it was signed (" + (r.uri.isEmpty() ? std::string("the whole document") : r.uri.toStdString()) + ")");
    }
    CanonicalOptions info;
    info.element = std::string("{") + kDsig + "}SignedInfo";
    info.envelopedSignature = false;
    info.comments = parts.canonicalization.endsWith(QLatin1String("#WithComments"));
    const std::string signedInfo = canonicalXml(xml, info);
    Ptr<X509> leaf = certificateFrom(parts.certificates.front());
    if (!leaf) return fail("The signer's certificate cannot be read");
    const EVP_MD* md = parts.method.endsWith(QLatin1String("rsa-sha256")) ? EVP_sha256() : parts.method.endsWith(QLatin1String("rsa-sha1")) ? EVP_sha1() : nullptr;
    if (!md) return fail("Unknown signature method " + parts.method.toStdString());
    if (!verifyWith(X509_get0_pubkey(leaf.get()), md, signedInfo, unbase64(parts.value))) return fail("The signature does not match its certificate");
    // Each certificate issued (names and key identifiers) and signed by the next, a certificate authority; the last by
    // itself.
    for (size_t i = 0; i < parts.certificates.size(); ++i) {
        Ptr<X509> c = certificateFrom(parts.certificates[i]);
        const bool last = i + 1 == parts.certificates.size();
        Ptr<X509> by = certificateFrom(parts.certificates[last ? i : i + 1]);
        if (!c || !by || X509_verify(c.get(), X509_get0_pubkey(by.get())) != 1 || X509_check_issued(by.get(), c.get()) != X509_V_OK ||
            (parts.certificates.size() > 1 && X509_check_ca(by.get()) == 0))
            return fail("The signer's certificate chain is broken");
    }
    if (check) {
        check->references.clear();
        for (const auto& r : parts.references) check->references.push_back(r.uri.toStdString());
        check->signerThumbprint = base64(thumbprintBytes(leaf.get()));
    }
    return true;
}

ContentKey newContentKey(const std::string& type, bool* ok) {
    ContentKey k;
    k.id = newUuid();
    const bool made = RAND_bytes(k.key.data(), int(k.key.size())) == 1;
    if (!made) k.key.fill(0);
    if (ok) *ok = made;
    k.type = type;
    return k;
}

bool makeKdm(const KdmSettings& s, const CertificateChain& chain, std::string& xml, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = opensslError(why);
        return false;
    };
    if (chain.certificates.empty()) return fail("No certificates to sign with");
    Ptr<X509> signer = certificateFrom(chain.certificates.back());
    Ptr<EVP_PKEY> key = privateKeyFrom(chain.privateKey);
    Ptr<X509> recipient = certificateFrom(s.recipient);
    if (!signer || !key) return fail("The signing certificate or key cannot be read");
    if (!recipient) return fail("The recipient's certificate cannot be read");
    Uuid cpl{};
    if (!parseUuid(s.cplId, cpl)) return fail("The composition's id is not a UUID");
    if (s.keys.empty()) return fail("No content keys");
    if (s.notValidBefore.size() != 25 || s.notValidAfter.size() != 25) return fail("The validity times are not in the KDM form");
    EVP_PKEY* to = X509_get0_pubkey(recipient.get());
    if (!to || EVP_PKEY_get_bits(to) != 2048) return fail("The recipient's key is not RSA 2048 (as cinema servers' are)");
    const std::string thumb = thumbprintBytes(signer.get());
    std::string description = entryText(X509_get_subject_name(recipient.get()), NID_commonName);
    if (const size_t dot = description.find('.'); dot != std::string::npos) description = description.substr(dot + 1);

    std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    x += "<DCinemaSecurityMessage xmlns=\"http://www.smpte-ra.org/schemas/430-3/2006/ETM\" xmlns:ds=\"http://www.w3.org/2000/09/xmldsig#\" "
         "xmlns:enc=\"http://www.w3.org/2001/04/xmlenc#\">\n";
    x += "  <AuthenticatedPublic Id=\"ID_AuthenticatedPublic\">\n";
    x += "    <MessageId>urn:uuid:" + uuidString(newUuid()) + "</MessageId>\n";
    x += "    <MessageType>http://www.smpte-ra.org/430-1/2006/KDM#kdm-key-type</MessageType>\n";
    if (!s.annotation.empty()) x += "    <AnnotationText>" + xmlEscaped(s.annotation) + "</AnnotationText>\n";
    const QDateTime now = QDateTime::currentDateTime();
    x += "    <IssueDate>" + kdmTime(now.toSecsSinceEpoch(), now.offsetFromUtc() / 60) + "</IssueDate>\n";
    x += "    <Signer>\n";
    x += "      <ds:X509IssuerName>" + xmlEscaped(nameText(X509_get_issuer_name(signer.get()))) + "</ds:X509IssuerName>\n";
    x += "      <ds:X509SerialNumber>" + serialText(signer.get()) + "</ds:X509SerialNumber>\n";
    x += "    </Signer>\n";
    x += "    <RequiredExtensions>\n";
    x += "      <KDMRequiredExtensions xmlns=\"http://www.smpte-ra.org/schemas/430-1/2006/KDM\">\n";
    x += "        <Recipient>\n          <X509IssuerSerial>\n";
    x += "            <ds:X509IssuerName>" + xmlEscaped(nameText(X509_get_issuer_name(recipient.get()))) + "</ds:X509IssuerName>\n";
    x += "            <ds:X509SerialNumber>" + serialText(recipient.get()) + "</ds:X509SerialNumber>\n";
    x += "          </X509IssuerSerial>\n";
    x += "          <X509SubjectName>" + xmlEscaped(nameText(X509_get_subject_name(recipient.get()))) + "</X509SubjectName>\n";
    x += "        </Recipient>\n";
    x += "        <CompositionPlaylistId>urn:uuid:" + uuidString(cpl) + "</CompositionPlaylistId>\n";
    x += "        <ContentTitleText>" + xmlEscaped(s.contentTitle) + "</ContentTitleText>\n";
    x += "        <ContentAuthenticator>" + base64(thumb) + "</ContentAuthenticator>\n";
    x += "        <ContentKeysNotValidBefore>" + s.notValidBefore + "</ContentKeysNotValidBefore>\n";
    x += "        <ContentKeysNotValidAfter>" + s.notValidAfter + "</ContentKeysNotValidAfter>\n";
    x += "        <AuthorizedDeviceInfo>\n";
    x += "          <DeviceListIdentifier>urn:uuid:" + uuidString(newUuid()) + "</DeviceListIdentifier>\n";
    x += "          <DeviceListDescription>" + xmlEscaped(description) + "</DeviceListDescription>\n";
    x += "          <DeviceList>\n";
    // Any device ("assume trust": the SHA-1 of nothing), or the ones named.
    if (s.trustedDevices.empty()) x += "            <CertificateThumbprint>2jmj7l5rSw0yVb/vlWAYkK/YBwk=</CertificateThumbprint>\n";
    for (const std::string& t : s.trustedDevices) x += "            <CertificateThumbprint>" + xmlEscaped(t) + "</CertificateThumbprint>\n";
    x += "          </DeviceList>\n        </AuthorizedDeviceInfo>\n";
    x += "        <KeyIdList>\n";
    for (const ContentKey& k : s.keys) {
        x += "          <TypedKeyId>\n";
        x += "            <KeyType scope=\"http://www.smpte-ra.org/430-1/2006/KDM#kdm-key-type\">" + xmlEscaped(k.type) + "</KeyType>\n";
        x += "            <KeyId>urn:uuid:" + uuidString(k.id) + "</KeyId>\n";
        x += "          </TypedKeyId>\n";
    }
    x += "        </KeyIdList>\n";
    x += "      </KDMRequiredExtensions>\n    </RequiredExtensions>\n";
    x += "    <NonCriticalExtensions/>\n";
    x += "  </AuthenticatedPublic>\n";
    x += "  <AuthenticatedPrivate Id=\"ID_AuthenticatedPrivate\">\n";
    for (const ContentKey& k : s.keys) {
        if (k.type.size() != 4) return fail("A key type is not four letters");
        std::string block(reinterpret_cast<const char*>(kStructureId), 16);
        block += thumb;
        block.append(reinterpret_cast<const char*>(cpl.data()), 16);
        block += k.type;
        block.append(reinterpret_cast<const char*>(k.id.data()), 16);
        block += s.notValidBefore + s.notValidAfter;
        block.append(reinterpret_cast<const char*>(k.key.data()), 16);
        if (block.size() != 138) return fail("A key block came out the wrong size");
        Ptr<EVP_PKEY_CTX> ctx(EVP_PKEY_CTX_new(to, nullptr));
        size_t n = 0;
        if (!ctx || EVP_PKEY_encrypt_init(ctx.get()) != 1 || EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_OAEP_PADDING) != 1 ||
            EVP_PKEY_encrypt(ctx.get(), nullptr, &n, reinterpret_cast<const uint8_t*>(block.data()), block.size()) != 1)
            return fail("Cannot encrypt a key to the recipient");
        std::string cipher(n, '\0');
        if (EVP_PKEY_encrypt(ctx.get(), reinterpret_cast<uint8_t*>(cipher.data()), &n, reinterpret_cast<const uint8_t*>(block.data()), block.size()) != 1)
            return fail("Cannot encrypt a key to the recipient");
        cipher.resize(n);
        x += "    <enc:EncryptedKey>\n";
        x += "      <enc:EncryptionMethod Algorithm=\"http://www.w3.org/2001/04/xmlenc#rsa-oaep-mgf1p\">\n";
        x += "        <ds:DigestMethod Algorithm=\"http://www.w3.org/2000/09/xmldsig#sha1\"/>\n";
        x += "      </enc:EncryptionMethod>\n";
        x += "      <enc:CipherData>\n        <enc:CipherValue>" + base64(cipher, true) + "</enc:CipherValue>\n      </enc:CipherData>\n";
        x += "    </enc:EncryptedKey>\n";
    }
    x += "  </AuthenticatedPrivate>\n";
    x += "  <ds:Signature>\n    <ds:SignedInfo>\n";
    x += "      <ds:CanonicalizationMethod Algorithm=\"http://www.w3.org/TR/2001/REC-xml-c14n-20010315#WithComments\"/>\n";
    x += "      <ds:SignatureMethod Algorithm=\"http://www.w3.org/2001/04/xmldsig-more#rsa-sha256\"/>\n";
    for (const char* id : {"ID_AuthenticatedPublic", "ID_AuthenticatedPrivate"}) {
        x += std::string("      <ds:Reference URI=\"#") + id + "\">\n";
        x += "        <ds:DigestMethod Algorithm=\"http://www.w3.org/2001/04/xmlenc#sha256\"/>\n";
        x += std::string("        <ds:DigestValue>@") + id + "@</ds:DigestValue>\n";
        x += "      </ds:Reference>\n";
    }
    x += "    </ds:SignedInfo>\n";
    x += "    <ds:SignatureValue>@SIGNATURE@</ds:SignatureValue>\n";
    x += "    <ds:KeyInfo>\n" + keyInfo(chain, "ds", "      ") + "    </ds:KeyInfo>\n";
    x += "  </ds:Signature>\n</DCinemaSecurityMessage>\n";
    std::string err;
    for (const char* id : {"ID_AuthenticatedPublic", "ID_AuthenticatedPrivate"}) {
        CanonicalOptions o;
        o.id = id;
        const std::string c = canonicalXml(x, o, &err);
        if (c.empty()) return fail(err);
        fill(x, std::string("@") + id + "@", base64(digest(c, EVP_sha256())));
    }
    CanonicalOptions info;
    info.element = std::string("{") + kDsig + "}SignedInfo";
    info.envelopedSignature = false;
    info.comments = true;
    const std::string signedInfo = canonicalXml(x, info, &err);
    std::string signature;
    if (signedInfo.empty() || !signWith(key.get(), signedInfo, signature)) return fail("Cannot sign the KDM");
    fill(x, "@SIGNATURE@", base64(signature, true));
    xml = std::move(x);
    return true;
}

bool readKdm(const std::string& xml, const std::string& privateKey, KdmContents& out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = opensslError(why);
        return false;
    };
    std::string err;
    SignatureCheck signature;
    if (!verifyXmlSignature(xml, &err, &signature)) return fail("The KDM's signature does not hold: " + err);
    SignatureParts parts;
    readSignature(xml, parts);
    Ptr<X509> signer = certificateFrom(parts.certificates.front());
    const std::string thumb = signer ? thumbprintBytes(signer.get()) : std::string();
    KdmContents k;
    k.signerThumbprint = signature.signerThumbprint;
    std::vector<std::string> ciphers;
    QXmlStreamReader x(QByteArray(xml.data(), qsizetype(xml.size())));
    x.setNamespaceProcessing(true);
    std::vector<QString> stack;
    QString publicId, privateId;
    int part = 0;  // 1 inside the public part, 2 inside the private part (the only parts read: the ones signed)
    int partDepth = 0;
    std::map<QString, int> seen;  // fields that must appear once
    auto once = [&](const QString& name) { return ++seen[name] == 1; };
    bool twice = false;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            stack.push_back(x.name().toString());
            if (!part && (x.name() == QLatin1String("AuthenticatedPublic") || x.name() == QLatin1String("AuthenticatedPrivate"))) {
                part = x.name() == QLatin1String("AuthenticatedPublic") ? 1 : 2;
                partDepth = int(stack.size());
                (part == 1 ? publicId : privateId) = x.attributes().value(QLatin1String("Id")).toString();
                if (!once(x.name().toString())) twice = true;
            }
            if (part == 1 && x.name() == QLatin1String("TypedKeyId")) k.keys.emplace_back();
            for (const char* field : {"CompositionPlaylistId", "ContentTitleText", "ContentKeysNotValidBefore", "ContentKeysNotValidAfter"})
                if (part == 1 && x.name() == QLatin1String(field) && !once(x.name().toString())) twice = true;
            if (part == 2 && x.name() == QLatin1String("CipherValue") && x.namespaceUri() == QLatin1String("http://www.w3.org/2001/04/xmlenc#"))
                ciphers.emplace_back();
        } else if (x.isEndElement()) {
            if (part && int(stack.size()) == partDepth) part = 0;
            if (!stack.empty()) stack.pop_back();
        } else if (x.isCharacters() && !stack.empty() && part) {
            const QString name = stack.back(), up = stack.size() > 1 ? stack[stack.size() - 2] : QString();
            const std::string text = x.text().toString().trimmed().toStdString();
            auto bare = [](std::string s) { return s.rfind("urn:uuid:", 0) == 0 ? s.substr(9) : s; };
            const std::string raw = x.text().toString().toStdString();  // (a field's text can come in pieces)
            if (part == 2) {
                if (name == QLatin1String("CipherValue") && !ciphers.empty()) ciphers.back() += x.text().toString().toStdString();
            } else if (name == QLatin1String("CompositionPlaylistId")) k.cplId += raw;
            else if (name == QLatin1String("ContentTitleText")) k.contentTitle += raw;
            else if (name == QLatin1String("AnnotationText") && up == QLatin1String("AuthenticatedPublic")) k.annotation += raw;
            else if (name == QLatin1String("ContentKeysNotValidBefore")) k.notValidBefore += raw;
            else if (name == QLatin1String("ContentKeysNotValidAfter")) k.notValidAfter += raw;
            else if (name == QLatin1String("X509SubjectName") && up == QLatin1String("Recipient")) k.recipientSubject = text;
            else if (name == QLatin1String("X509IssuerName") && up == QLatin1String("X509IssuerSerial") && stack.size() > 2 &&
                     stack[stack.size() - 3] == QLatin1String("Recipient"))
                k.recipientIssuer = text;
            else if (name == QLatin1String("X509SerialNumber") && up == QLatin1String("X509IssuerSerial") && stack.size() > 2 &&
                     stack[stack.size() - 3] == QLatin1String("Recipient"))
                k.recipientSerial = text;
            else if (name == QLatin1String("KeyType") && !k.keys.empty()) k.keys.back().type = text;
            else if (name == QLatin1String("KeyId") && !k.keys.empty()) parseUuid(bare(text), k.keys.back().id);
        }
    }
    if (x.hasError()) return fail("The KDM is not well-formed");
    if (twice) return fail("The KDM gives a part or a field twice");
    for (std::string* field : {&k.cplId, &k.contentTitle, &k.annotation, &k.notValidBefore, &k.notValidAfter})
        *field = QString::fromStdString(*field).trimmed().toStdString();
    if (k.cplId.rfind("urn:uuid:", 0) == 0) k.cplId = k.cplId.substr(9);
    // Both parts, and only they, signed (by reference to their ids).
    std::vector<std::string> wanted{"#" + publicId.toStdString(), "#" + privateId.toStdString()}, covered = signature.references;
    std::sort(wanted.begin(), wanted.end());
    std::sort(covered.begin(), covered.end());
    if (publicId.isEmpty() || privateId.isEmpty() || covered != wanted) return fail("The KDM's signature does not cover its public and private parts");
    if (!privateKey.empty()) {
        Ptr<EVP_PKEY> key = privateKeyFrom(privateKey);
        if (!key) return fail("The private key cannot be read");
        if (ciphers.size() != k.keys.size()) return fail("The KDM's keys and their ids do not pair up");
        Uuid cpl{};
        parseUuid(k.cplId, cpl);
        for (size_t i = 0; i < ciphers.size(); ++i) {
            const std::string cipher = unbase64(QString::fromStdString(ciphers[i]));
            Ptr<EVP_PKEY_CTX> ctx(EVP_PKEY_CTX_new(key.get(), nullptr));
            size_t n = 0;
            if (!ctx || EVP_PKEY_decrypt_init(ctx.get()) != 1 || EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_OAEP_PADDING) != 1 ||
                EVP_PKEY_decrypt(ctx.get(), nullptr, &n, reinterpret_cast<const uint8_t*>(cipher.data()), cipher.size()) != 1)
                return fail("The KDM is not for this key");
            std::string block(n, '\0');
            if (EVP_PKEY_decrypt(ctx.get(), reinterpret_cast<uint8_t*>(block.data()), &n, reinterpret_cast<const uint8_t*>(cipher.data()), cipher.size()) != 1)
                return fail("The KDM is not for this key");
            block.resize(n);
            if (block.size() != 138 || std::memcmp(block.data(), kStructureId, 16) != 0) return fail("A key block is not an ST 430-1 key");
            if (block.compare(16, 20, thumb) != 0) return fail("A key block was not made by the KDM's signer");
            if (std::memcmp(block.data() + 36, cpl.data(), 16) != 0) return fail("A key block is for another composition");
            Uuid id{};
            std::memcpy(id.data(), block.data() + 56, 16);
            if (id != k.keys[i].id || block.compare(52, 4, k.keys[i].type) != 0) return fail("A key block does not match its key id");
            // The window the key is given for is the block's (encrypted with the key); the public part must agree.
            if (block.compare(72, 25, k.notValidBefore) != 0 || block.compare(97, 25, k.notValidAfter) != 0)
                return fail("A key block's window is not the KDM's");
            std::memcpy(k.keys[i].key.data(), block.data() + 122, 16);
        }
    }
    out = std::move(k);
    return true;
}

bool aesCbcEncrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t size, uint8_t* out) {
    if (size % 16) return false;
    Ptr<EVP_CIPHER_CTX> ctx(EVP_CIPHER_CTX_new());
    int n = 0, m = 0;
    return ctx && EVP_EncryptInit_ex(ctx.get(), EVP_aes_128_cbc(), nullptr, key, iv) == 1 && EVP_CIPHER_CTX_set_padding(ctx.get(), 0) == 1 &&
           EVP_EncryptUpdate(ctx.get(), out, &n, in, int(size)) == 1 && EVP_EncryptFinal_ex(ctx.get(), out + n, &m) == 1 && size_t(n + m) == size;
}

bool aesCbcDecrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t size, uint8_t* out) {
    if (size % 16) return false;
    Ptr<EVP_CIPHER_CTX> ctx(EVP_CIPHER_CTX_new());
    int n = 0, m = 0;
    return ctx && EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_cbc(), nullptr, key, iv) == 1 && EVP_CIPHER_CTX_set_padding(ctx.get(), 0) == 1 &&
           EVP_DecryptUpdate(ctx.get(), out, &n, in, int(size)) == 1 && EVP_DecryptFinal_ex(ctx.get(), out + n, &m) == 1 && size_t(n + m) == size;
}

bool hmacSha1(const uint8_t* key, size_t keySize, const uint8_t* data, size_t size, uint8_t out[20]) {
    unsigned n = 0;
    return HMAC(EVP_sha1(), key, int(keySize), data, size, out, &n) && n == 20;
}

bool sha1(const uint8_t* data, size_t size, uint8_t out[20]) { return SHA1(data, size, out) != nullptr; }

bool randomBytes(uint8_t* out, size_t size) { return RAND_bytes(out, int(size)) == 1; }

#else  // no OpenSSL

namespace {
bool none(std::string* error) {
    if (error) *error = "This build has no OpenSSL: DCPs cannot be encrypted or signed";
    return false;
}
}  // namespace

bool securityAvailable() { return false; }
bool makeCertificateChain(const std::string&, CertificateChain&, std::string* error) { return none(error); }
bool signerCertificateChain(CertificateChain&, std::string* error, const std::string&) { return none(error); }
bool readCertificate(const std::string&, CertificateInfo&, std::string* error) { return none(error); }
bool signXml(std::string&, const CertificateChain&, std::string* error) { return none(error); }
bool verifyXmlSignature(const std::string&, std::string* error, SignatureCheck*) { return none(error); }
ContentKey newContentKey(const std::string& type, bool* ok) {
    ContentKey k;
    k.id = newUuid();
    k.type = type;
    if (ok) *ok = false;
    return k;
}
bool makeKdm(const KdmSettings&, const CertificateChain&, std::string&, std::string* error) { return none(error); }
bool readKdm(const std::string&, const std::string&, KdmContents&, std::string* error) { return none(error); }
bool aesCbcEncrypt(const uint8_t*, const uint8_t*, const uint8_t*, size_t, uint8_t*) { return false; }
bool aesCbcDecrypt(const uint8_t*, const uint8_t*, const uint8_t*, size_t, uint8_t*) { return false; }
bool hmacSha1(const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*) { return false; }
bool sha1(const uint8_t*, size_t, uint8_t*) { return false; }
bool randomBytes(uint8_t*, size_t) { return false; }

#endif

}  // namespace montage::dcp
