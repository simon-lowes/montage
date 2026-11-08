// Montage — cinemas and screens for KDMs (render/DcpFacilities.h).
#include "DcpFacilities.h"

#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>

#include "DcpMxf.h"
#include "DcpSecurity.h"

namespace montage {

namespace {

const char* kFlm16 = "http://www.smpte-ra.org/ns/430-16/2017/FLM";
const char* kFlm7 = "http://www.smpte-ra.org/schemas/430-7/2008/FLM";

// Elements by local name (FLMs come with any prefixes; the namespace is checked at the root).
QDomElement child(const QDomElement& e, const char* name) {
    for (QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
        if (c.localName() == QLatin1String(name)) return c;
    return {};
}
std::vector<QDomElement> children(const QDomElement& e, const char* name) {
    std::vector<QDomElement> out;
    for (QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement())
        if (c.localName() == QLatin1String(name)) out.push_back(c);
    return out;
}
// Every descendant of that name, in document order.
void descendants(const QDomElement& e, const char* name, std::vector<QDomElement>& out) {
    for (QDomElement c = e.firstChildElement(); !c.isNull(); c = c.nextSiblingElement()) {
        if (c.localName() == QLatin1String(name)) out.push_back(c);
        descendants(c, name, out);
    }
}
std::string text(const QDomElement& e, const char* name) {
    const QDomElement c = child(e, name);
    return c.isNull() ? std::string() : c.text().simplified().toStdString();
}

// ds:X509Certificate's base64 as PEM.
std::string pemOf(const QString& base64) {
    QString flat = base64;
    flat.remove(QRegularExpression(QStringLiteral("\\s")));
    std::string pem = "-----BEGIN CERTIFICATE-----\n";
    for (qsizetype i = 0; i < flat.size(); i += 64) pem += flat.mid(i, 64).toStdString() + "\n";
    return pem + "-----END CERTIFICATE-----\n";
}

std::vector<std::string> certificatesIn(const QDomElement& e) {
    std::vector<QDomElement> found;
    descendants(e, "X509Certificate", found);
    std::vector<std::string> out;
    for (const QDomElement& c : found)
        if (!c.text().trimmed().isEmpty()) out.push_back(pemOf(c.text()));
    return out;
}

// The roles in an ST 430-2 common name ("SM MB.server.example" -> {"SM", "MB"}).
std::vector<std::string> roles(const std::string& commonName) {
    std::vector<std::string> out;
    const size_t dot = commonName.find('.');
    if (dot == std::string::npos) return out;
    for (const QString& r : QString::fromStdString(commonName.substr(0, dot)).split(' ', Qt::SkipEmptyParts)) out.push_back(r.toStdString());
    return out;
}

// A device's chain checked and its thumbprint taken; `what` names it in errors.
bool takeChain(FacilityDevice& d, const std::vector<std::string>& certificates, bool allowPartial, const std::string& what, std::string* error) {
    dcp::ChainCheck check;
    std::string err;
    if (!dcp::checkCertificateChain(certificates, check, &err, allowPartial)) {
        if (error) *error = what + ": " + err;
        return false;
    }
    d.certificates = check.certificates;
    d.chainComplete = check.complete;
    d.thumbprint = check.leaf.thumbprint;
    d.commonName = check.leaf.commonName;
    d.notAfter = check.leaf.notAfter;
    if (d.type.empty()) {
        const std::vector<std::string> r = roles(check.leaf.commonName);
        d.type = std::find(r.begin(), r.end(), "SM") != r.end() ? "SM" : r.empty() ? std::string() : r.front();
    }
    return true;
}

bool fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

// ST 430-16 DeviceType.
bool readDevice16(const QDomElement& e, FacilityDevice& d, const std::string& where, Facility& f, std::string* error) {
    d.type = text(e, "DeviceTypeID");
    d.serial = text(e, "DeviceSerial");
    d.manufacturer = text(e, "Manufacturer");
    d.model = text(e, "ModelNumber");
    const std::string active = text(e, "IsActive");
    d.active = active != "false" && active != "0";
    const QDomElement id = child(e, "DeviceIdentifier");
    const bool byThumbprint = id.attribute(QStringLiteral("idtype")) == QLatin1String("CertThumbprint");
    const std::string identifier = id.text().simplified().toStdString();
    const std::string what = where + ", " + (d.type.empty() ? std::string("a device") : d.type) + (d.serial.empty() ? "" : " " + d.serial);
    d.name = d.model.empty() ? d.type : d.model;
    const std::vector<std::string> certs = certificatesIn(child(e, "KeyInfoList"));
    if (!certs.empty()) {
        if (!takeChain(d, certs, false, what, error)) return false;
        if (byThumbprint && identifier != d.thumbprint) {
            dcp::CertificateInfo leaf;
            dcp::readCertificate(d.certificates.back(), leaf);
            if (identifier != leaf.dnQualifier) f.notes.push_back(what + ": its identifier is not its certificate's thumbprint (the certificate's is used)");
        }
    } else if (byThumbprint) {
        d.thumbprint = identifier;
    }
    return true;
}

bool readExtended(const QDomElement& root, Facility& f, std::string* error) {
    f.source = "ST 430-16";
    f.issueDate = text(root, "IssueDate");
    const QDomElement info = child(root, "FacilityInfo");
    f.id = text(info, "FacilityID");
    f.name = text(info, "FacilityName");
    f.timeZone = text(info, "FacilityTimeZone");
    f.circuit = text(info, "Circuit");
    if (f.id.empty()) return fail(error, "The FLM has no FacilityID");
    if (f.name.empty()) f.name = f.id;
    std::vector<QDomElement> emails;
    descendants(child(child(info, "Capabilities"), "KDMDeliveryMethodList"), "EmailAddress", emails);
    for (const QDomElement& e : emails) f.kdmEmails.push_back(e.text().simplified().toStdString());
    for (const QDomElement& a : children(child(root, "AuditoriumList"), "Auditorium")) {
        const std::string auditorium = text(a, "AuditoriumNumberOrName");
        const std::string where = f.name + ", screen " + auditorium;
        const std::vector<QDomElement> suites = children(child(a, "SuiteList"), "Suite");
        if (suites.empty()) f.notes.push_back(where + " lists no security devices (no KDMs can be made for it)");
        int n = 0;
        for (const QDomElement& suite : suites) {
            FacilityScreen screen;
            screen.name = ++n == 1 ? auditorium : auditorium + " #" + std::to_string(n);
            bool server = false;
            for (const QDomElement& e : children(suite, "Device")) {
                FacilityDevice d;
                if (!readDevice16(e, d, where, f, error)) return false;
                if (d.type == "SM") {
                    if (server) return fail(error, where + ": a suite with two security managers");
                    if (d.certificates.empty()) return fail(error, where + ": its security manager has no certificate");
                    screen.server = std::move(d);
                    server = true;
                } else {
                    if (d.thumbprint.empty()) f.notes.push_back(where + ": " + (d.type.empty() ? "a device" : d.type) + " has no certificate or thumbprint (left out of KDMs)");
                    screen.devices.push_back(std::move(d));
                }
            }
            if (!server) return fail(error, where + ": a suite with no security manager (SM)");
            f.screens.push_back(std::move(screen));
        }
    }
    return true;
}

bool readEtm(const std::string& xml, const QDomElement& root, Facility& f, std::string* error) {
    f.source = "ST 430-7";
    const QDomElement pub = child(root, "AuthenticatedPublic");
    if (text(pub, "MessageType") != kFlm7) return fail(error, "Not a Facility List Message (its MessageType is " + text(pub, "MessageType") + ")");
    // Signed, the signature holding and covering the public part.
    dcp::SignatureCheck check;
    std::string err;
    if (!dcp::verifyXmlSignature(xml, &err, &check)) return fail(error, "The FLM's signature does not hold: " + err);
    const std::string publicRef = "#" + pub.attribute(QStringLiteral("Id")).toStdString();
    if (std::none_of(check.references.begin(), check.references.end(), [&](const std::string& r) { return r.empty() || (r == publicRef && r.size() > 1); }))
        return fail(error, "The FLM's signature does not cover its public part");
    f.issueDate = text(pub, "IssueDate");
    const QDomElement ext = child(child(pub, "RequiredExtensions"), "FLMRequiredExtensions");
    if (ext.isNull()) return fail(error, "The FLM has no FLMRequiredExtensions");
    const QDomElement info = child(ext, "FacilityInfo");
    f.id = text(info, "FacilityID");
    f.name = text(info, "FacilityName");
    if (f.id.empty()) return fail(error, "The FLM has no FacilityID");
    if (f.name.empty()) f.name = f.id;
    if (const std::string offset = text(child(info, "UTCOffset"), "Offset"); !offset.empty())
        f.timeZone = "UTC" + std::string(offset[0] == '-' ? "" : "+") + offset;
    if (const std::string email = text(child(info, "ContactInfo"), "Email"); !email.empty()) f.kdmEmails.push_back(email);
    if (text(ext, "FLMPartial") == "true") f.notes.push_back("A partial FLM: " + text(pub, "AnnotationText"));
    std::vector<FacilityDevice> servers, others;
    const QDomElement list = child(ext, "SecurityDeviceList");
    int n = 0;
    for (QDomElement e = list.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
        if (e.localName() != QLatin1String("SecurityDevice") && e.localName() != QLatin1String("SecurityDeviceCertificate")) continue;
        FacilityDevice d;
        const QDomElement desc = child(e, "DeviceDescription");
        d.type = text(desc, "DeviceTypeID");
        d.serial = text(desc, "DeviceSerial");
        d.manufacturer = text(desc, "ManufacturerName");
        d.model = text(desc, "ModelNumber");
        d.name = text(desc, "DeviceName");
        if (!takeChain(d, certificatesIn(child(e, "KeyInfo")), false, f.name + ", device " + std::to_string(++n), error)) return false;
        if (d.name.empty()) d.name = d.commonName.substr(d.commonName.find('.') == std::string::npos ? 0 : d.commonName.find('.') + 1);
        (d.type == "SM" ? servers : others).push_back(std::move(d));
    }
    for (FacilityDevice& s : servers) {
        FacilityScreen screen;
        screen.name = s.name.empty() ? s.serial : s.name;
        for (int k = 2; std::any_of(f.screens.begin(), f.screens.end(), [&](const FacilityScreen& o) { return o.name == screen.name; }); ++k)
            screen.name = (s.name.empty() ? s.serial : s.name) + " #" + std::to_string(k);
        screen.server = std::move(s);
        f.screens.push_back(std::move(screen));
    }
    // One screen: its devices are the rest; several: an ST 430-7 FLM does not say whose they are.
    if (f.screens.size() == 1) f.screens.front().devices = std::move(others);
    else if (!others.empty()) {
        f.otherDevices = std::move(others);
        f.notes.push_back(std::to_string(f.otherDevices.size()) + " device(s) not in any screen's KDMs: the FLM does not say which screen each is in");
    }
    return true;
}

QJsonObject toJson(const FacilityDevice& d) {
    QJsonArray certs;
    for (const std::string& c : d.certificates) certs.append(QString::fromStdString(c));
    return {{"type", QString::fromStdString(d.type)},          {"thumbprint", QString::fromStdString(d.thumbprint)},
            {"certificates", certs},                           {"chain_complete", d.chainComplete},
            {"name", QString::fromStdString(d.name)},          {"manufacturer", QString::fromStdString(d.manufacturer)},
            {"model", QString::fromStdString(d.model)},        {"serial", QString::fromStdString(d.serial)},
            {"common_name", QString::fromStdString(d.commonName)}, {"not_after", QString::fromStdString(d.notAfter)},
            {"active", d.active}};
}
FacilityDevice deviceFrom(const QJsonObject& o) {
    FacilityDevice d;
    auto s = [&](const char* key) { return o.value(QLatin1String(key)).toString().toStdString(); };
    d.type = s("type");
    d.thumbprint = s("thumbprint");
    for (const QJsonValue& v : o.value("certificates").toArray()) d.certificates.push_back(v.toString().toStdString());
    d.chainComplete = o.value("chain_complete").toBool();
    d.name = s("name");
    d.manufacturer = s("manufacturer");
    d.model = s("model");
    d.serial = s("serial");
    d.commonName = s("common_name");
    d.notAfter = s("not_after");
    d.active = o.value("active").toBool(true);
    return d;
}

std::string readText(const std::string& textOrFile, std::string* error) {
    if (textOrFile.find("-----BEGIN") != std::string::npos) return textOrFile;
    QFile f(QString::fromStdString(textOrFile));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "Cannot read " + textOrFile;
        return {};
    }
    return f.readAll().toStdString();
}

bool sameName(const std::string& a, const std::string& b) { return QString::fromStdString(a).compare(QString::fromStdString(b), Qt::CaseInsensitive) == 0; }

}  // namespace

bool readFlm(const std::string& xml, Facility& out, std::string* error) {
    if (!dcp::securityAvailable()) return fail(error, "This build has no OpenSSL, so it cannot check certificates");
    QDomDocument doc;
    if (!doc.setContent(QByteArray::fromStdString(xml), true)) return fail(error, "Not an XML document");
    const QDomElement root = doc.documentElement();
    Facility f;
    bool ok = false;
    if (root.localName() == QLatin1String("FacilityListMessage") && root.namespaceURI() == QLatin1String(kFlm16)) ok = readExtended(root, f, error);
    else if (root.localName() == QLatin1String("DCinemaSecurityMessage")) ok = readEtm(xml, root, f, error);
    else return fail(error, "Not a Facility List Message (ST 430-16 or ST 430-7)");
    if (!ok) return false;
    if (f.screens.empty()) return fail(error, "The FLM of " + f.name + " lists no screen with a server certificate");
    out = std::move(f);
    return true;
}

bool readFlmFile(const std::string& path, Facility& out, std::string* error) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) return fail(error, "Cannot read " + path);
    if (f.size() > 64 * 1024 * 1024) return fail(error, path + " is too large for an FLM");
    std::string err;
    if (!readFlm(f.readAll().toStdString(), out, &err)) return fail(error, QFileInfo(f).fileName().toStdString() + ": " + err);
    return true;
}

std::string facilitiesFile() {
    if (const QByteArray env = qgetenv("MONTAGE_DCP_FACILITIES"); !env.isEmpty()) return env.toStdString();
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("dcp-facilities.json")).toStdString();
}

bool loadFacilities(std::vector<Facility>& out, std::string* error, const std::string& file) {
    QFile f(QString::fromStdString(file.empty() ? facilitiesFile() : file));
    out.clear();
    if (!f.exists()) return true;
    if (!f.open(QIODevice::ReadOnly)) return fail(error, "Cannot read " + f.fileName().toStdString());
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parse);
    if (!doc.isObject()) return fail(error, f.fileName().toStdString() + " is not a facility list: " + parse.errorString().toStdString());
    for (const QJsonValue& v : doc.object().value("cinemas").toArray()) {
        const QJsonObject o = v.toObject();
        Facility c;
        auto s = [&](const char* key) { return o.value(QLatin1String(key)).toString().toStdString(); };
        c.id = s("id");
        c.name = s("name");
        c.timeZone = s("time_zone");
        c.circuit = s("circuit");
        c.source = s("source");
        c.issueDate = s("issue_date");
        for (const QJsonValue& e : o.value("kdm_emails").toArray()) c.kdmEmails.push_back(e.toString().toStdString());
        for (const QJsonValue& n : o.value("notes").toArray()) c.notes.push_back(n.toString().toStdString());
        for (const QJsonValue& sv : o.value("screens").toArray()) {
            const QJsonObject so = sv.toObject();
            FacilityScreen screen;
            screen.name = so.value("name").toString().toStdString();
            screen.server = deviceFrom(so.value("server").toObject());
            for (const QJsonValue& d : so.value("devices").toArray()) screen.devices.push_back(deviceFrom(d.toObject()));
            c.screens.push_back(std::move(screen));
        }
        for (const QJsonValue& d : o.value("other_devices").toArray()) c.otherDevices.push_back(deviceFrom(d.toObject()));
        out.push_back(std::move(c));
    }
    return true;
}

bool saveFacilities(const std::vector<Facility>& list, std::string* error, const std::string& file) {
    const QString path = QString::fromStdString(file.empty() ? facilitiesFile() : file);
    QJsonArray cinemas;
    for (const Facility& c : list) {
        QJsonArray screens, others, emails, notes;
        for (const FacilityScreen& s : c.screens) {
            QJsonArray devices;
            for (const FacilityDevice& d : s.devices) devices.append(toJson(d));
            screens.append(QJsonObject{{"name", QString::fromStdString(s.name)}, {"server", toJson(s.server)}, {"devices", devices}});
        }
        for (const FacilityDevice& d : c.otherDevices) others.append(toJson(d));
        for (const std::string& e : c.kdmEmails) emails.append(QString::fromStdString(e));
        for (const std::string& n : c.notes) notes.append(QString::fromStdString(n));
        cinemas.append(QJsonObject{{"id", QString::fromStdString(c.id)},
                                   {"name", QString::fromStdString(c.name)},
                                   {"time_zone", QString::fromStdString(c.timeZone)},
                                   {"circuit", QString::fromStdString(c.circuit)},
                                   {"source", QString::fromStdString(c.source)},
                                   {"issue_date", QString::fromStdString(c.issueDate)},
                                   {"kdm_emails", emails},
                                   {"notes", notes},
                                   {"screens", screens},
                                   {"other_devices", others}});
    }
    if (!QDir().mkpath(QFileInfo(path).path())) return fail(error, "Cannot make " + QFileInfo(path).path().toStdString());
    if (!dcp::writeWholeFile(path.toStdString(), QJsonDocument(QJsonObject{{"cinemas", cinemas}}).toJson().toStdString()))
        return fail(error, "Cannot write " + path.toStdString());
    return true;
}

size_t mergeFacility(std::vector<Facility>& list, Facility f) {
    for (size_t i = 0; i < list.size(); ++i)
        if (!f.id.empty() && list[i].id == f.id) {
            list[i] = std::move(f);
            return i;
        }
    list.push_back(std::move(f));
    return list.size() - 1;
}

bool addScreenByHand(std::vector<Facility>& list, const std::string& cinema, const std::string& screen, const std::string& server,
                     const std::vector<std::string>& devices, std::string* error) {
    if (!dcp::securityAvailable()) return fail(error, "This build has no OpenSSL, so it cannot check certificates");
    const std::string cinemaName = QString::fromStdString(cinema).simplified().toStdString();
    const std::string screenName = QString::fromStdString(screen).simplified().toStdString();
    if (cinemaName.empty() || screenName.empty()) return fail(error, "A screen needs a cinema and a name");
    FacilityScreen s;
    s.name = screenName;
    std::string err;
    const std::string serverText = readText(server, &err);
    if (serverText.empty()) return fail(error, err.empty() ? "No server certificate" : err);
    s.server.type = "SM";
    if (!takeChain(s.server, {serverText}, true, "The server certificate", error)) return false;
    s.server.name = s.server.commonName;
    for (const std::string& given : devices) {
        const std::string t = readText(given, &err);
        if (t.empty()) return fail(error, err);
        FacilityDevice d;
        if (!takeChain(d, {t}, true, "A device certificate", error)) return false;
        d.name = d.commonName;
        s.devices.push_back(std::move(d));
    }
    auto it = std::find_if(list.begin(), list.end(), [&](const Facility& f) { return sameName(f.name, cinemaName); });
    if (it == list.end()) {
        Facility f;
        f.id = "urn:uuid:" + dcp::uuidString(dcp::newUuid());
        f.name = cinemaName;
        f.source = "by hand";
        list.push_back(std::move(f));
        it = list.end() - 1;
    }
    auto same = std::find_if(it->screens.begin(), it->screens.end(), [&](const FacilityScreen& o) { return sameName(o.name, screenName); });
    if (same != it->screens.end()) *same = std::move(s);
    else it->screens.push_back(std::move(s));
    return true;
}

KdmScreen kdmScreen(const Facility& cinema, const FacilityScreen& screen) {
    KdmScreen k;
    k.cinema = cinema.name;
    k.screen = screen.name;
    k.certificate = screen.server.certificates.empty() ? std::string() : screen.server.certificates.back();
    k.timeZone = cinema.timeZone;
    for (const FacilityDevice& d : screen.devices)
        if (d.active && !d.thumbprint.empty() && std::find(k.trustedDevices.begin(), k.trustedDevices.end(), d.thumbprint) == k.trustedDevices.end())
            k.trustedDevices.push_back(d.thumbprint);
    return k;
}

bool pickScreens(const std::vector<Facility>& list, const std::vector<std::string>& names, std::vector<KdmScreen>& out, std::string* error) {
    std::vector<KdmScreen> picked;
    auto cinemaNamed = [&](const std::string& n) -> const Facility* {
        for (const Facility& f : list)
            if (sameName(f.name, n) || f.id == n) return &f;
        return nullptr;
    };
    auto take = [&](const Facility& f, const FacilityScreen& s) {
        if (std::none_of(picked.begin(), picked.end(), [&](const KdmScreen& k) { return k.cinema == f.name && k.screen == s.name; }))
            picked.push_back(kdmScreen(f, s));
    };
    for (const std::string& name : names) {
        if (const Facility* f = cinemaNamed(name)) {
            for (const FacilityScreen& s : f->screens) take(*f, s);
            continue;
        }
        // "Cinema/Screen", the cinema's name itself possibly with a "/" in it.
        bool found = false;
        for (size_t at = name.find('/'); at != std::string::npos && !found; at = name.find('/', at + 1)) {
            const Facility* f = cinemaNamed(name.substr(0, at));
            if (!f) continue;
            for (const FacilityScreen& s : f->screens)
                if (sameName(s.name, name.substr(at + 1))) take(*f, s), found = true;
        }
        if (!found) return fail(error, "No screen or cinema named " + name + " in the facility list");
    }
    out = std::move(picked);
    return true;
}

}  // namespace montage
