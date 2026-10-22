// Montage — finding Open Media Transport sources (media/Omt.h): DNS-SD as libomtnet does it (Avahi on Linux, loaded
// when its daemon runs; the system's DNS-SD on macOS; Windows' own, 10 and later), or an OMT discovery server when one
// is set. Sources are announced as "HOST (Name)" of type _omt._tcp with no TXT record. Written from libomtnet's source
// (MIT, Copyright (c) 2025 Open Media Transport Contributors; LICENSES/MIT-libomtnet.txt).
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include <QDir>
#include <QFile>
#include <QXmlStreamReader>

#include "Omt.h"
#include "OmtNet.h"
#include "audio/DynLib.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#include <windns.h>
#elif defined(__APPLE__)
#include <arpa/inet.h>
#include <dns_sd.h>
#include <sys/select.h>
#endif

namespace montage::omt {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

// "omt://host:port" (a bracketed IPv6 address too) as host and port.
bool parseUrl(const std::string& url, std::string& host, int& port) {
    if (lower(url).rfind("omt://", 0) != 0) return false;
    std::string rest = url.substr(6);
    while (!rest.empty() && rest.back() == '/') rest.pop_back();
    size_t colon;
    if (!rest.empty() && rest[0] == '[') {
        const size_t close = rest.find(']');
        if (close == std::string::npos) return false;
        host = rest.substr(1, close - 1);
        colon = rest.find(':', close);
    } else {
        colon = rest.rfind(':');
        host = rest.substr(0, colon);
    }
    if (colon == std::string::npos || host.empty()) return false;
    const std::string p = rest.substr(colon + 1);
    if (p.empty() || !std::all_of(p.begin(), p.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) return false;
    port = std::atoi(p.c_str());
    return port > 0 && port < 65536;
}

}  // namespace

// ---------------------------------------------------------------------------
// The discovery server

std::string discoveryServer() {
    if (const char* e = std::getenv("MONTAGE_OMT_DISCOVERY_SERVER"); e && *e) return e;
    // libomtnet's settings: ~/.OMT/settings.xml (OMT_STORAGE_PATH's when set), <Settings><DiscoveryServer>.
    QString dir;
    if (const char* e = std::getenv("OMT_STORAGE_PATH"); e && *e) dir = QString::fromLocal8Bit(e);
    else dir = QDir::home().filePath(QStringLiteral(".OMT"));
    QFile f(QDir(dir).filePath(QStringLiteral("settings.xml")));
    if (!f.open(QIODevice::ReadOnly)) return {};
    QXmlStreamReader x(&f);
    while (!x.atEnd())
        if (x.readNext() == QXmlStreamReader::StartElement && x.name() == QLatin1String("DiscoveryServer"))
            return trim(x.readElementText().toStdString());
    return {};
}

std::string addressXml(const std::string& name, int port, const std::vector<std::string>& addresses, bool removed) {
    auto esc = [](const std::string& s) {
        std::string out;
        for (char c : s) out += c == '&' ? std::string("&amp;") : c == '<' ? std::string("&lt;") : c == '>' ? std::string("&gt;") : std::string(1, c);
        return out;
    };
    // (as libomtnet writes it: Removed before Addresses, each address an IPAddress)
    std::string x = "<OMTAddress>\n  <Name>" + esc(name) + "</Name>\n  <Port>" + std::to_string(port) + "</Port>\n";
    if (removed) x += "  <Removed>True</Removed>\n";
    x += "  <Addresses>\n";
    for (const std::string& a : addresses) x += "    <IPAddress>" + esc(a) + "</IPAddress>\n";
    x += "  </Addresses>\n</OMTAddress>";
    return x;
}

bool parseAddressXml(const std::string& xml, Source& out, bool& removed) {
    QXmlStreamReader x(QByteArray::fromStdString(xml));
    out = {};
    removed = false;
    bool inAddress = false;
    while (!x.atEnd()) {
        if (x.readNext() != QXmlStreamReader::StartElement) continue;
        const QStringView n = x.name();
        if (n == QLatin1String("OMTAddress")) inAddress = true;
        else if (!inAddress) return false;
        else if (n == QLatin1String("Name")) out.name = trim(x.readElementText().toStdString());
        else if (n == QLatin1String("Port")) out.port = x.readElementText().trimmed().toInt();
        else if (n == QLatin1String("Removed")) removed = x.readElementText().trimmed().toLower() == QLatin1String("true");
        else if ((n == QLatin1String("IPAddress") || n == QLatin1String("Address")) && out.host.empty()) {
            std::string a = trim(x.readElementText().toStdString());
            if (a.rfind("::ffff:", 0) == 0 && a.find('.') != std::string::npos) a = a.substr(7);  // (IPv4 as IPv6)
            out.host = a;
        }
    }
    out.via = "discovery server";
    return inAddress && !out.name.empty() && out.port > 0;
}

namespace {

// A connection to the discovery server, metadata subscribed.
net::Socket serverConnection(std::string* why) {
    const std::string server = discoveryServer();
    std::string host;
    int port = kDiscoveryPort;
    if (!parseUrl(server, host, port)) {
        host = server;
        if (const size_t c = server.rfind(':'); c != std::string::npos && server.find(':') == c) {
            host = server.substr(0, c);
            port = std::atoi(server.c_str() + c + 1);
        }
    }
    const net::Socket s = net::connectTo(host, port, 2000, why);
    if (s == net::kInvalid) return s;
    net::configure(s, true);
    const std::string sub = subscribeCommand(Metadata);
    std::vector<uint8_t> frame(kHeaderBytes + sub.size());
    Header h;
    h.dataLength = int32_t(sub.size());
    writeHeader(h, frame.data());
    std::memcpy(frame.data() + kHeaderBytes, sub.data(), sub.size());
    if (!net::sendAll(s, frame.data(), frame.size())) {
        net::closeSocket(s);
        if (why) *why = "The discovery server closed the connection";
        return net::kInvalid;
    }
    return s;
}

bool sendXml(net::Socket s, const std::string& xml) {
    std::vector<uint8_t> frame(kHeaderBytes + xml.size());
    Header h;
    h.dataLength = int32_t(xml.size());
    writeHeader(h, frame.data());
    std::memcpy(frame.data() + kHeaderBytes, xml.data(), xml.size());
    return net::sendAll(s, frame.data(), frame.size());
}

// Reads the server's metadata into `sources` until `until` (or the connection ends).
void readServer(net::Socket s, std::chrono::steady_clock::time_point until, std::map<std::string, Source>& sources) {
    std::atomic<bool> stop{false};
    std::thread timer([&] {
        while (!stop && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        net::shutdownSocket(s);
    });
    uint8_t head[kHeaderBytes];
    std::vector<uint8_t> body;
    Header h;
    while (net::recvAll(s, head, kHeaderBytes) && readHeader(head, h) && h.dataLength <= (4 << 20)) {
        body.resize(size_t(h.dataLength));
        if (h.dataLength && !net::recvAll(s, body.data(), body.size())) break;
        if (h.type != Metadata) continue;
        Source src;
        bool removed = false;
        if (!parseAddressXml(std::string(body.begin(), body.end()), src, removed)) continue;
        if (removed) sources.erase(src.name);
        else sources[src.name] = src;
    }
    stop = true;
    timer.join();
}

// Registered with the discovery server for as long as its connection lasts.
class ServerAnnouncement : public Announcement {
public:
    ServerAnnouncement(net::Socket s, std::string name, int port) : s_(s), name_(std::move(name)), port_(port) {
        // (the server's broadcasts read and let go, so the connection stays open)
        reader_ = std::thread([this] {
            uint8_t buf[4096];
            while (net::recvAll(s_, buf, 1)) {
            }
        });
    }
    ~ServerAnnouncement() override {
        sendXml(s_, addressXml(name_, port_, {}, true));
        net::shutdownSocket(s_);
        reader_.join();
        net::closeSocket(s_);
    }
    std::string how() const override { return "discovery server " + discoveryServer(); }

private:
    net::Socket s_;
    std::string name_;
    int port_;
    std::thread reader_;
};

// ---------------------------------------------------------------------------
// DNS-SD

#if defined(__linux__)

// Avahi's client library, loaded (the daemon it talks to is what makes it work).
struct AvahiApi {
    using ClientCb = void (*)(void*, int, void*);
    using GroupCb = void (*)(void*, int, void*);
    using BrowserCb = void (*)(void*, int, int, int, const char*, const char*, const char*, int, void*);
    using ResolverCb = void (*)(void*, int, int, int, const char*, const char*, const char*, const char*, const void*, uint16_t, void*, int, void*);
    plugins::DynLib common, client;
    bool ok = false;
    void* (*pollNew)() = nullptr;
    void (*pollFree)(void*) = nullptr;
    const void* (*pollGet)(void*) = nullptr;
    int (*pollStart)(void*) = nullptr;
    int (*pollStop)(void*) = nullptr;
    void (*pollLock)(void*) = nullptr;
    void (*pollUnlock)(void*) = nullptr;
    char* (*addressPrint)(char*, size_t, const void*) = nullptr;
    void* (*clientNew)(const void*, int, ClientCb, void*, int*) = nullptr;
    void (*clientFree)(void*) = nullptr;
    void* (*groupNew)(void*, GroupCb, void*) = nullptr;
    int (*groupFree)(void*) = nullptr;
    int (*groupAdd)(void*, int, int, int, const char*, const char*, const char*, const char*, uint16_t, void*) = nullptr;
    int (*groupCommit)(void*) = nullptr;
    void* (*browserNew)(void*, int, int, const char*, const char*, int, BrowserCb, void*) = nullptr;
    int (*browserFree)(void*) = nullptr;
    void* (*resolverNew)(void*, int, int, const char*, const char*, const char*, int, int, ResolverCb, void*) = nullptr;
    int (*resolverFree)(void*) = nullptr;
};

AvahiApi& avahi() {
    static AvahiApi a;
    static std::once_flag once;
    std::call_once(once, [] {
        std::string err;
        if (!a.common.open("libavahi-common.so.3", &err) || !a.client.open("libavahi-client.so.3", &err)) return;
        auto get = [](plugins::DynLib& lib, auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(lib.symbol(name));
            return fn != nullptr;
        };
        a.ok = get(a.common, a.pollNew, "avahi_threaded_poll_new") && get(a.common, a.pollFree, "avahi_threaded_poll_free") &&
               get(a.common, a.pollGet, "avahi_threaded_poll_get") && get(a.common, a.pollStart, "avahi_threaded_poll_start") &&
               get(a.common, a.pollStop, "avahi_threaded_poll_stop") && get(a.common, a.pollLock, "avahi_threaded_poll_lock") &&
               get(a.common, a.pollUnlock, "avahi_threaded_poll_unlock") && get(a.common, a.addressPrint, "avahi_address_snprint") &&
               get(a.client, a.clientNew, "avahi_client_new") && get(a.client, a.clientFree, "avahi_client_free") &&
               get(a.client, a.groupNew, "avahi_entry_group_new") && get(a.client, a.groupFree, "avahi_entry_group_free") &&
               get(a.client, a.groupAdd, "avahi_entry_group_add_service_strlst") && get(a.client, a.groupCommit, "avahi_entry_group_commit") &&
               get(a.client, a.browserNew, "avahi_service_browser_new") && get(a.client, a.browserFree, "avahi_service_browser_free") &&
               get(a.client, a.resolverNew, "avahi_service_resolver_new") && get(a.client, a.resolverFree, "avahi_service_resolver_free");
    });
    return a;
}

constexpr int kIfUnspec = -1, kProtoUnspec = -1;
constexpr int kBrowserNew = 0, kBrowserRemove = 1, kBrowserAllForNow = 3, kBrowserFailure = 4;
constexpr int kResolverFound = 0;

// A client on a poll thread of its own.
struct AvahiSession {
    void* poll = nullptr;
    void* client = nullptr;
    std::string error;
    AvahiSession() {
        AvahiApi& a = avahi();
        if (!a.ok) {
            error = "Avahi is not installed";
            return;
        }
        poll = a.pollNew();
        int err = 0;
        if (poll) client = a.clientNew(a.pollGet(poll), 0, [](void*, int, void*) {}, nullptr, &err);
        if (!client) {
            error = "The Avahi daemon is not running";
            if (poll) a.pollFree(poll);
            poll = nullptr;
            return;
        }
        a.pollStart(poll);
    }
    ~AvahiSession() {
        AvahiApi& a = avahi();
        if (poll) a.pollStop(poll);
        if (client) a.clientFree(client);
        if (poll) a.pollFree(poll);
    }
    bool ok() const { return client != nullptr; }
};

class AvahiAnnouncement : public Announcement {
public:
    std::unique_ptr<AvahiSession> session;
    void* group = nullptr;
    ~AvahiAnnouncement() override {
        if (group) {
            avahi().pollLock(session->poll);
            avahi().groupFree(group);
            avahi().pollUnlock(session->poll);
        }
    }
    std::string how() const override { return "DNS-SD (Avahi)"; }
};

std::unique_ptr<Announcement> announceDnsSd(const std::string& name, int port, std::string* why) {
    auto a = std::make_unique<AvahiAnnouncement>();
    a->session = std::make_unique<AvahiSession>();
    if (!a->session->ok()) {
        if (why) *why = a->session->error;
        return nullptr;
    }
    AvahiApi& api = avahi();
    api.pollLock(a->session->poll);
    a->group = api.groupNew(a->session->client, [](void*, int, void*) {}, nullptr);
    const bool ok = a->group && api.groupAdd(a->group, kIfUnspec, kProtoUnspec, 0, name.c_str(), kServiceType, nullptr, nullptr, uint16_t(port), nullptr) == 0 &&
                    api.groupCommit(a->group) == 0;
    api.pollUnlock(a->session->poll);
    if (!ok) {
        if (why) *why = "Avahi would not announce \"" + name + "\"";
        return nullptr;
    }
    return a;
}

struct AvahiBrowse {
    std::mutex m;
    std::condition_variable cv;
    std::map<std::string, Source> found;
    int resolving = 0;
    bool allForNow = false;
    std::vector<void*> resolvers;
};

bool browseDnsSd(double seconds, std::vector<Source>& out, std::string* why) {
    AvahiSession session;
    if (!session.ok()) {
        if (why) *why = session.error;
        return false;
    }
    AvahiApi& api = avahi();
    AvahiBrowse b;
    struct Ctx {
        AvahiSession* session;
        AvahiBrowse* b;
    } ctx{&session, &b};
    api.pollLock(session.poll);
    void* browser = api.browserNew(session.client, kIfUnspec, kProtoUnspec, kServiceType, nullptr, 0,
        [](void*, int iface, int proto, int event, const char* name, const char* type, const char* domain, int, void* user) {
            auto* c = static_cast<Ctx*>(user);
            std::lock_guard lock(c->b->m);
            if (event == kBrowserNew) {
                void* r = avahi().resolverNew(c->session->client, iface, proto, name, type, domain, kProtoUnspec, 0,
                    [](void*, int, int, int event, const char* name, const char*, const char*, const char* host, const void* addr, uint16_t port, void*, int, void* user) {
                        auto* c = static_cast<Ctx*>(user);
                        std::lock_guard lock(c->b->m);
                        if (event == kResolverFound && name) {
                            char text[64] = {};
                            if (addr) avahi().addressPrint(text, sizeof text, addr);
                            Source s{name, text[0] ? text : (host ? host : ""), port, "DNS-SD"};
                            if (!c->b->found.count(s.name) || c->b->found[s.name].host.find(':') != std::string::npos) c->b->found[s.name] = s;  // (IPv4 preferred)
                        }
                        --c->b->resolving;
                        c->b->cv.notify_all();
                    },
                    user);
                if (r) {
                    ++c->b->resolving;
                    c->b->resolvers.push_back(r);
                }
            } else if (event == kBrowserRemove && name) {
                c->b->found.erase(name);
            } else if (event == kBrowserAllForNow || event == kBrowserFailure) {
                c->b->allForNow = true;
            }
            c->b->cv.notify_all();
        },
        &ctx);
    api.pollUnlock(session.poll);
    if (!browser) {
        if (why) *why = "Avahi would not browse";
        return false;
    }
    {
        // Until Avahi has all it knows and has resolved it (or the time is up).
        std::unique_lock lock(b.m);
        b.cv.wait_for(lock, std::chrono::milliseconds(int64_t(seconds * 1000)), [&] { return b.allForNow && b.resolving <= 0; });
    }
    api.pollLock(session.poll);
    for (void* r : b.resolvers) api.resolverFree(r);
    api.browserFree(browser);
    api.pollUnlock(session.poll);
    std::lock_guard lock(b.m);
    for (const auto& [n, s] : b.found) out.push_back(s);
    return true;
}

bool dnsSdHere(std::string* why) {
    AvahiSession s;
    if (!s.ok() && why) *why = s.error;
    return s.ok();
}

#elif defined(__APPLE__)

// The system's DNS-SD: the results a request's socket has within `ms`, handled.
bool processFor(DNSServiceRef ref, int ms, const std::function<bool()>& done) {
    const int fd = DNSServiceRefSockFD(ref);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done()) {
        const auto left = std::chrono::duration_cast<std::chrono::microseconds>(until - std::chrono::steady_clock::now()).count();
        if (left <= 0) return false;
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        timeval tv{long(left / 1000000), int(left % 1000000)};
        if (select(fd + 1, &set, nullptr, nullptr, &tv) <= 0) return false;
        if (DNSServiceProcessResult(ref) != kDNSServiceErr_NoError) return false;
    }
    return true;
}

class BonjourAnnouncement : public Announcement {
public:
    DNSServiceRef ref = nullptr;
    ~BonjourAnnouncement() override {
        if (ref) DNSServiceRefDeallocate(ref);
    }
    std::string how() const override { return "DNS-SD"; }
};

std::unique_ptr<Announcement> announceDnsSd(const std::string& name, int port, std::string* why) {
    auto a = std::make_unique<BonjourAnnouncement>();
    struct Result {
        bool done = false;
        DNSServiceErrorType err = kDNSServiceErr_NoError;
    } result;
    const DNSServiceErrorType e = DNSServiceRegister(&a->ref, 0, 0, name.c_str(), kServiceType, nullptr, nullptr, htons(uint16_t(port)), 0, nullptr,
        [](DNSServiceRef, DNSServiceFlags, DNSServiceErrorType err, const char*, const char*, const char*, void* ctx) {
            auto* r = static_cast<Result*>(ctx);
            r->done = true;
            r->err = err;
        },
        &result);
    if (e != kDNSServiceErr_NoError) {
        a->ref = nullptr;
        if (why) *why = "DNS-SD would not announce \"" + name + "\" (" + std::to_string(e) + ")";
        return nullptr;
    }
    processFor(a->ref, 1000, [&] { return result.done; });
    if (result.done && result.err != kDNSServiceErr_NoError) {
        if (why) *why = "DNS-SD would not announce \"" + name + "\" (" + std::to_string(result.err) + ")";
        return nullptr;
    }
    return a;
}

bool browseDnsSd(double seconds, std::vector<Source>& out, std::string* why) {
    struct Found {
        std::string name, type, domain;
        uint32_t iface = 0;
    };
    struct Browse {
        std::vector<Found> found;
        bool more = true;
    } browse;
    DNSServiceRef ref = nullptr;
    DNSServiceErrorType e = DNSServiceBrowse(&ref, 0, 0, kServiceType, nullptr,
        [](DNSServiceRef, DNSServiceFlags flags, uint32_t iface, DNSServiceErrorType err, const char* name, const char* type, const char* domain, void* ctx) {
            auto* b = static_cast<Browse*>(ctx);
            if (err == kDNSServiceErr_NoError && (flags & kDNSServiceFlagsAdd)) b->found.push_back({name, type, domain, iface});
            b->more = (flags & kDNSServiceFlagsMoreComing) != 0;
        },
        &browse);
    if (e != kDNSServiceErr_NoError) {
        if (why) *why = "DNS-SD would not browse (" + std::to_string(e) + ")";
        return false;
    }
    // (results for a little while; the rest of the time to resolve them)
    processFor(ref, int(seconds * 500), [] { return false; });
    DNSServiceRefDeallocate(ref);
    std::map<std::string, Source> sources;
    for (const Found& f : browse.found) {
        if (sources.count(f.name)) continue;
        struct Resolved {
            bool done = false;
            std::string host;
            int port = 0;
        } r;
        DNSServiceRef rr = nullptr;
        if (DNSServiceResolve(&rr, 0, f.iface, f.name.c_str(), f.type.c_str(), f.domain.c_str(),
                [](DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType err, const char*, const char* host, uint16_t port, uint16_t,
                   const unsigned char*, void* ctx) {
                    auto* r = static_cast<Resolved*>(ctx);
                    r->done = true;
                    if (err == kDNSServiceErr_NoError) r->host = host ? host : "", r->port = ntohs(port);
                },
                &r) != kDNSServiceErr_NoError)
            continue;
        processFor(rr, std::max(200, int(seconds * 500 / std::max<size_t>(1, browse.found.size()))), [&] { return r.done; });
        DNSServiceRefDeallocate(rr);
        if (r.port > 0 && !r.host.empty()) sources[f.name] = Source{f.name, r.host, r.port, "DNS-SD"};
    }
    for (const auto& [n, s] : sources) out.push_back(s);
    return true;
}

bool dnsSdHere(std::string*) { return true; }

#elif defined(_WIN32)

// Windows' own DNS-SD (dnsapi, Windows 10 1809 and later), loaded: its types as windns.h has them.
constexpr DWORD kDnsRequestPending = 9506;  // (DNS_REQUEST_PENDING)
struct WinInstance {
    PWSTR instanceName;
    PWSTR hostName;
    DWORD* ip4;
    void* ip6;
    WORD port, priority, weight;
    DWORD propertyCount;
    PWSTR* keys;
    PWSTR* values;
    DWORD interfaceIndex;
};
using WinRegisterDone = void(WINAPI*)(DWORD, void*, WinInstance*);
struct WinRegisterRequest {
    ULONG version;
    ULONG interfaceIndex;
    WinInstance* instance;
    WinRegisterDone done;
    void* context;
    HANDLE credentials;
    BOOL unicast;
};
struct WinCancel {
    void* reserved;
};
using WinBrowseCb = void(WINAPI*)(DWORD, void*, DNS_RECORDW*);
struct WinBrowseRequest {
    ULONG version;
    ULONG interfaceIndex;
    PCWSTR queryName;
    WinBrowseCb callback;
    void* context;
};
using WinResolveDone = void(WINAPI*)(DWORD, void*, WinInstance*);
struct WinResolveRequest {
    ULONG version;
    ULONG interfaceIndex;
    PWSTR queryName;
    WinResolveDone done;
    void* context;
};

struct DnsApiFns {
    HMODULE lib = nullptr;
    bool ok = false;
    WinInstance*(WINAPI* construct)(PCWSTR, PCWSTR, DWORD*, void*, WORD, WORD, WORD, DWORD, PCWSTR*, PCWSTR*) = nullptr;
    void(WINAPI* freeInstance)(WinInstance*) = nullptr;
    DWORD(WINAPI* registerService)(WinRegisterRequest*, WinCancel*) = nullptr;
    DWORD(WINAPI* deregisterService)(WinRegisterRequest*, WinCancel*) = nullptr;
    DNS_STATUS(WINAPI* browse)(WinBrowseRequest*, WinCancel*) = nullptr;
    DNS_STATUS(WINAPI* browseCancel)(WinCancel*) = nullptr;
    DNS_STATUS(WINAPI* resolve)(WinResolveRequest*, WinCancel*) = nullptr;
    DNS_STATUS(WINAPI* resolveCancel)(WinCancel*) = nullptr;
    void(WINAPI* recordListFree)(DNS_RECORDW*, int) = nullptr;
};

DnsApiFns& dnsApi() {
    static DnsApiFns a;
    static std::once_flag once;
    std::call_once(once, [] {
        a.lib = LoadLibraryW(L"dnsapi.dll");
        if (!a.lib) return;
        auto get = [](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(reinterpret_cast<void*>(GetProcAddress(a.lib, name)));
            return fn != nullptr;
        };
        a.ok = get(a.construct, "DnsServiceConstructInstance") && get(a.freeInstance, "DnsServiceFreeInstance") &&
               get(a.registerService, "DnsServiceRegister") && get(a.deregisterService, "DnsServiceDeRegister") &&
               get(a.browse, "DnsServiceBrowse") && get(a.browseCancel, "DnsServiceBrowseCancel") && get(a.resolve, "DnsServiceResolve") &&
               get(a.resolveCancel, "DnsServiceResolveCancel") && get(a.recordListFree, "DnsRecordListFree");
    });
    return a;
}

std::wstring wide(const std::string& s) {
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), int(w.size()));
    return w;
}
std::string narrow(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(0, n - 1)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

class WinAnnouncement : public Announcement {
public:
    WinRegisterRequest request{};
    WinInstance* instance = nullptr;
    ~WinAnnouncement() override {
        if (!instance) return;
        // Deregistering finishes later, in a callback: waited for (a while), its state shared so a late one is safe.
        struct Done {
            std::mutex m;
            std::condition_variable cv;
            bool done = false;
        };
        auto done = std::make_shared<Done>();
        auto* hold = new std::shared_ptr<Done>(done);
        request.context = hold;
        request.done = [](DWORD, void* ctx, WinInstance* i) {
            if (i) dnsApi().freeInstance(i);
            auto* h = static_cast<std::shared_ptr<Done>*>(ctx);
            const std::shared_ptr<Done> d = std::move(*h);
            delete h;
            std::lock_guard lock(d->m);
            d->done = true;
            d->cv.notify_all();
        };
        if (dnsApi().deregisterService(&request, nullptr) != kDnsRequestPending) {
            delete hold;
            dnsApi().freeInstance(instance);
            return;
        }
        std::unique_lock lock(done->m);
        if (done->cv.wait_for(lock, std::chrono::seconds(2), [&] { return done->done; })) dnsApi().freeInstance(instance);
        // (else the instance is left alone, a small leak, rather than freed while Windows may still read it)
    }
    std::string how() const override { return "DNS-SD"; }
};

std::unique_ptr<Announcement> announceDnsSd(const std::string& name, int port, std::string* why) {
    DnsApiFns& api = dnsApi();
    if (!api.ok) {
        if (why) *why = "DNS-SD needs Windows 10 (1809) or later";
        return nullptr;
    }
    // (Windows takes no dots in an instance's name, as libomtnet found)
    std::string instanceName = name;
    instanceName.erase(std::remove(instanceName.begin(), instanceName.end(), '.'), instanceName.end());
    const std::wstring service = wide(instanceName + "._omt._tcp.local"), host = wide(machineName() + ".local");
    auto a = std::make_unique<WinAnnouncement>();
    a->instance = api.construct(service.c_str(), host.c_str(), nullptr, nullptr, WORD(port), 0, 0, 0, nullptr, nullptr);
    if (!a->instance) {
        if (why) *why = "DNS-SD would not announce \"" + name + "\"";
        return nullptr;
    }
    a->request.version = 1;
    a->request.instance = a->instance;
    a->request.done = [](DWORD, void*, WinInstance* i) {
        if (i) dnsApi().freeInstance(i);
    };
    WinCancel cancel{};
    const DWORD r = api.registerService(&a->request, &cancel);
    if (r != kDnsRequestPending) {
        api.freeInstance(a->instance);
        a->instance = nullptr;
        if (why) *why = "DNS-SD would not announce \"" + name + "\" (" + std::to_string(r) + ")";
        return nullptr;
    }
    return a;
}

bool browseDnsSd(double seconds, std::vector<Source>& out, std::string* why) {
    DnsApiFns& api = dnsApi();
    if (!api.ok) {
        if (why) *why = "DNS-SD needs Windows 10 (1809) or later";
        return false;
    }
    // (the browse's state is shared with its callback, which lets go of it on the last call, the cancelled one)
    struct Browse {
        std::mutex m;
        std::vector<std::wstring> names;
    };
    auto b = std::make_shared<Browse>();
    auto* hold = new std::shared_ptr<Browse>(b);
    WinBrowseRequest req{};
    req.version = 1;
    req.queryName = L"_omt._tcp.local";
    req.context = hold;
    req.callback = [](DWORD status, void* ctx, DNS_RECORDW* records) {
        auto* h = static_cast<std::shared_ptr<Browse>*>(ctx);
        if (status == 0)
            for (DNS_RECORDW* r = records; r; r = r->pNext)
                if (r->wType == DNS_TYPE_PTR && r->Data.PTR.pNameHost) {
                    std::lock_guard lock((*h)->m);
                    std::vector<std::wstring>& names = (*h)->names;
                    if (std::find(names.begin(), names.end(), std::wstring(r->Data.PTR.pNameHost)) == names.end())
                        names.push_back(r->Data.PTR.pNameHost);
                }
        if (records) dnsApi().recordListFree(records, 1 /*DnsFreeRecordList*/);
        if (status == ERROR_CANCELLED) delete h;
    };
    WinCancel cancel{};
    if (api.browse(&req, &cancel) != kDnsRequestPending) {
        delete hold;
        if (why) *why = "DNS-SD would not browse (is a network connected?)";
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(int64_t(seconds * 600)));
    api.browseCancel(&cancel);
    std::vector<std::wstring> names;
    {
        std::lock_guard lock(b->m);
        names = b->names;
    }
    std::map<std::string, Source> sources;
    for (std::wstring& n : names) {
        struct Resolved {
            std::mutex m;
            std::condition_variable cv;
            bool done = false;
            std::string host;
            int port = 0;
            std::string ip;
        };
        // (shared with its callback, which comes once, answered or cancelled, and may come after the wait gives up)
        auto rp = std::make_shared<Resolved>();
        Resolved& r = *rp;
        auto* held = new std::shared_ptr<Resolved>(rp);
        WinResolveRequest rq{};
        rq.version = 1;
        rq.queryName = n.data();
        rq.context = held;
        rq.done = [](DWORD status, void* ctx, WinInstance* i) {
            auto* h = static_cast<std::shared_ptr<Resolved>*>(ctx);
            const std::shared_ptr<Resolved> r = std::move(*h);
            delete h;
            std::lock_guard lock(r->m);
            if (status == 0 && i) {
                r->host = narrow(i->hostName);
                r->port = i->port;
                if (i->ip4) {
                    char text[32];
                    const DWORD a = *i->ip4;
                    std::snprintf(text, sizeof text, "%u.%u.%u.%u", unsigned(a & 0xff), unsigned((a >> 8) & 0xff), unsigned((a >> 16) & 0xff),
                                  unsigned((a >> 24) & 0xff));
                    r->ip = text;
                }
            }
            if (i) dnsApi().freeInstance(i);
            r->done = true;
            r->cv.notify_all();
        };
        WinCancel rc{};
        if (api.resolve(&rq, &rc) != kDnsRequestPending) {
            delete held;
            continue;
        }
        std::unique_lock lock(r.m);
        if (!r.cv.wait_for(lock, std::chrono::milliseconds(1000), [&] { return r.done; })) {
            lock.unlock();
            api.resolveCancel(&rc);  // (its callback still comes, and lets go of what it shares)
            continue;
        }
        // The instance's name without its service: "HOST (Name)".
        std::string name = narrow(n.c_str());
        if (const size_t at = name.find("._omt._tcp"); at != std::string::npos) name.resize(at);
        if (r.port > 0) sources[name] = Source{name, !r.ip.empty() ? r.ip : r.host, r.port, "DNS-SD"};
    }
    for (const auto& [k, s] : sources) out.push_back(s);
    return true;
}

bool dnsSdHere(std::string* why) {
    if (!dnsApi().ok && why) *why = "DNS-SD needs Windows 10 (1809) or later";
    return dnsApi().ok;
}

#else

std::unique_ptr<Announcement> announceDnsSd(const std::string&, int, std::string* why) {
    if (why) *why = "No DNS-SD on this system";
    return nullptr;
}
bool browseDnsSd(double, std::vector<Source>&, std::string* why) {
    if (why) *why = "No DNS-SD on this system";
    return false;
}
bool dnsSdHere(std::string* why) {
    if (why) *why = "No DNS-SD on this system";
    return false;
}

#endif

}  // namespace

bool dnsSdAvailable(std::string* why) { return dnsSdHere(why); }

std::unique_ptr<Announcement> announce(const std::string& fullName, int port, std::string* why) {
    // With a discovery server set, sources register there only (as libomtnet does).
    if (!discoveryServer().empty()) {
        const net::Socket s = serverConnection(why);
        if (s == net::kInvalid) return nullptr;
        if (!sendXml(s, addressXml(fullName, port, net::localAddresses(), false))) {
            net::closeSocket(s);
            if (why) *why = "The discovery server closed the connection";
            return nullptr;
        }
        return std::make_unique<ServerAnnouncement>(s, fullName, port);
    }
    return announceDnsSd(fullName, port, why);
}

std::vector<Source> discover(double seconds, std::string* why) {
    std::map<std::string, Source> all;
    std::string dnsWhy, serverWhy;
    std::thread server;
    std::map<std::string, Source> fromServer;
    if (!discoveryServer().empty())
        server = std::thread([&] {
            const net::Socket s = serverConnection(&serverWhy);
            if (s == net::kInvalid) return;
            readServer(s, std::chrono::steady_clock::now() + std::chrono::milliseconds(int64_t(seconds * 1000)), fromServer);
            net::closeSocket(s);
        });
    std::vector<Source> dns;
    const bool dnsOk = browseDnsSd(seconds, dns, &dnsWhy);
    if (server.joinable()) server.join();
    for (const Source& s : dns) all[s.name] = s;
    for (const auto& [n, s] : fromServer) all[n] = s;
    if (why) *why = dnsOk ? serverWhy : discoveryServer().empty() ? dnsWhy : serverWhy.empty() ? std::string() : dnsWhy + "; " + serverWhy;
    std::vector<Source> out;
    for (const auto& [n, s] : all) out.push_back(s);
    return out;
}

bool resolve(const std::string& source, std::string& host, int& port, std::string* error, double seconds) {
    const std::string s = trim(source);
    if (s.empty()) {
        if (error) *error = "Name the OMT source";
        return false;
    }
    if (lower(s).rfind("omt://", 0) == 0) {
        if (parseUrl(s, host, port)) return true;
        if (error) *error = s + " is not omt://host:port";
        return false;
    }
    // A full name ("HOST (Name)"), as discovery finds it; else host:port.
    if (s.find('(') != std::string::npos) {
        std::string why;
        for (const Source& f : discover(seconds, &why))
            if (f.name == s) {
                host = f.host, port = f.port;
                return true;
            }
        if (error) *error = "No OMT source \"" + s + "\" found" + (why.empty() ? std::string() : " (" + why + ")");
        return false;
    }
    if (parseUrl("omt://" + s, host, port)) return true;
    if (error) *error = "Name the OMT source as omt://host:port or as it is announced, \"HOST (Name)\"";
    return false;
}

}  // namespace montage::omt
