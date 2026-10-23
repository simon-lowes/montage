#include "OmtNet.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace montage::omt::net {

namespace {

#ifdef _WIN32
using sock_t = SOCKET;
int lastError() { return WSAGetLastError(); }
bool inProgress(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
int pollOne(sock_t s, short events, int ms) {
    WSAPOLLFD p{s, events, 0};
    return WSAPoll(&p, 1, ms) > 0 ? int(p.revents) : 0;
}
void setNonBlocking(sock_t s, bool on) {
    u_long v = on ? 1 : 0;
    ioctlsocket(s, FIONBIO, &v);
}
constexpr int kSendFlags = 0;
#else
using sock_t = int;
int lastError() { return errno; }
bool inProgress(int e) { return e == EINPROGRESS || e == EWOULDBLOCK; }
int pollOne(sock_t s, short events, int ms) {
    pollfd p{s, events, 0};
    return ::poll(&p, 1, ms) > 0 ? int(p.revents) : 0;
}
void setNonBlocking(sock_t s, bool on) {
    const int f = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK));
}
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif
#endif

sock_t raw(Socket s) { return static_cast<sock_t>(s); }

// Sockets are not handed on to programs Montage starts.
sock_t noInherit(sock_t s) {
#ifdef _WIN32
    if (s != INVALID_SOCKET) SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
#else
    if (s >= 0) fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
    return s;
}

template <typename T>
void option(Socket s, int level, int name, T value) {
    setsockopt(raw(s), level, name, reinterpret_cast<const char*>(&value), sizeof value);
}

void noSigPipe([[maybe_unused]] Socket s) {
#ifdef SO_NOSIGPIPE
    option(s, SOL_SOCKET, SO_NOSIGPIPE, 1);
#endif
}

}  // namespace

void init() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    });
#endif
}

void closeSocket(Socket s) {
    if (s == kInvalid) return;
#ifdef _WIN32
    closesocket(raw(s));
#else
    ::close(raw(s));
#endif
}

void shutdownSocket(Socket s) {
    if (s == kInvalid) return;
#ifdef _WIN32
    ::shutdown(raw(s), SD_BOTH);
#else
    ::shutdown(raw(s), SHUT_RDWR);
#endif
}

void configure(Socket s, bool receiver) {
    option(s, IPPROTO_TCP, TCP_NODELAY, 1);
    option(s, SOL_SOCKET, SO_KEEPALIVE, 1);
#if defined(TCP_KEEPIDLE)
    option(s, IPPROTO_TCP, TCP_KEEPIDLE, 5);
#elif defined(TCP_KEEPALIVE)
    option(s, IPPROTO_TCP, TCP_KEEPALIVE, 5);
#endif
    option(s, SOL_SOCKET, SO_SNDBUF, 64 * 1024);
    option(s, SOL_SOCKET, SO_RCVBUF, receiver ? 8 * 1024 * 1024 : 64 * 1024);
    noSigPipe(s);
}

bool sendAll(Socket s, const uint8_t* data, size_t size) {
    while (size > 0) {
        const int chunk = int(std::min<size_t>(size, size_t(1) << 30));
        const auto n = ::send(raw(s), reinterpret_cast<const char*>(data), chunk, kSendFlags);
        if (n <= 0) {
#ifndef _WIN32
            if (n < 0 && errno == EINTR) continue;
#endif
            return false;
        }
        data += n;
        size -= size_t(n);
    }
    return true;
}

bool recvAll(Socket s, uint8_t* data, size_t size, const std::atomic<bool>* stop) {
    while (size > 0) {
        if (stop && *stop) return false;
        // (with a stop flag, waited for in short steps: Windows does not wake a blocked recv when its socket is shut
        // down from another thread, only when the other end answers)
        if (stop && !pollOne(raw(s), POLLIN, 100)) continue;
        const int chunk = int(std::min<size_t>(size, size_t(1) << 30));
        const auto n = ::recv(raw(s), reinterpret_cast<char*>(data), chunk, 0);
        if (n <= 0) {
#ifndef _WIN32
            if (n < 0 && errno == EINTR) continue;
#endif
            return false;
        }
        data += n;
        size -= size_t(n);
    }
    return true;
}

Socket listenOnFirstFree(int first, int last, int* port, std::string* why) {
    init();
    // Dual stack (IPv6 and IPv4-mapped), as OMT's senders listen; IPv4 alone where IPv6 is not there.
    for (const bool v6 : {true, false}) {
        const sock_t probe = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (probe == sock_t(kInvalid)) continue;
        closeSocket(Socket(probe));
        for (int p = first; p <= last; ++p) {
            const sock_t s = noInherit(::socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (s == sock_t(kInvalid)) break;
#ifndef _WIN32
            option(Socket(s), SOL_SOCKET, SO_REUSEADDR, 1);  // (not on Windows, where it lets two listen on one port)
#endif
            int bound;
            if (v6) {
                option(Socket(s), IPPROTO_IPV6, IPV6_V6ONLY, 0);
                sockaddr_in6 a{};
                a.sin6_family = AF_INET6;
                a.sin6_addr = in6addr_any;
                a.sin6_port = htons(uint16_t(p));
                bound = ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
            } else {
                sockaddr_in a{};
                a.sin_family = AF_INET;
                a.sin_addr.s_addr = htonl(INADDR_ANY);
                a.sin_port = htons(uint16_t(p));
                bound = ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
            }
            if (bound == 0 && ::listen(s, 5) == 0) {
                *port = p;
                return Socket(s);
            }
            closeSocket(Socket(s));
        }
        if (why) *why = "No free port from " + std::to_string(first) + " to " + std::to_string(last);
        return kInvalid;
    }
    if (why) *why = "Cannot open a network socket";
    return kInvalid;
}

Socket acceptWithin(Socket listener, int ms) {
    if (!(pollOne(raw(listener), POLLIN, ms) & POLLIN)) return kInvalid;
    const sock_t c = noInherit(::accept(raw(listener), nullptr, nullptr));
    return c == sock_t(kInvalid) ? kInvalid : Socket(c);
}

Socket connectTo(const std::string& host, int port, int ms, std::string* why, const std::atomic<bool>* stop) {
    init();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* list = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &list) != 0 || !list) {
        if (why) *why = "Cannot find " + host;
        return kInvalid;
    }
    std::vector<const addrinfo*> order;
    for (const addrinfo* a = list; a; a = a->ai_next) order.push_back(a);
    std::stable_sort(order.begin(), order.end(), [](const addrinfo* x, const addrinfo* y) { return x->ai_family == AF_INET && y->ai_family != AF_INET; });
    Socket found = kInvalid;
    for (const addrinfo* a : order) {
        if (stop && *stop) break;
        const sock_t s = noInherit(::socket(a->ai_family, a->ai_socktype, a->ai_protocol));
        if (s == sock_t(kInvalid)) continue;
        setNonBlocking(s, true);
        int r = ::connect(s, a->ai_addr, int(a->ai_addrlen));
        if (r != 0 && inProgress(lastError())) {
            r = -1;
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (!(stop && *stop) && std::chrono::steady_clock::now() < until) {
                const int ev = pollOne(s, POLLOUT, 100);
                if (!ev) continue;
                int err = 0;
                socklen_t len = sizeof err;
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
                r = (ev & POLLOUT) && err == 0 ? 0 : -1;
                break;
            }
        }
        if (r == 0) {
            setNonBlocking(s, false);
            found = Socket(s);
            break;
        }
        closeSocket(Socket(s));
    }
    freeaddrinfo(list);
    if (found == kInvalid && why) *why = "Cannot connect to " + host + ":" + service;
    return found;
}

std::vector<std::string> localAddresses() {
    init();
    std::vector<std::string> v4, v6;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, list, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, list, &size) != NO_ERROR) list = nullptr;
    }
    for (auto* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            char text[INET6_ADDRSTRLEN] = {};
            const sockaddr* sa = u->Address.lpSockaddr;
            if (sa->sa_family == AF_INET) inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, text, sizeof text), v4.push_back(text);
            else if (sa->sa_family == AF_INET6 && !IN6_IS_ADDR_LINKLOCAL(&reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr))
                inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr, text, sizeof text), v6.push_back(text);
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (const ifaddrs* a = list; a; a = a->ifa_next) {
            if (!a->ifa_addr || (a->ifa_flags & IFF_LOOPBACK) || !(a->ifa_flags & IFF_UP)) continue;
            char text[INET6_ADDRSTRLEN] = {};
            if (a->ifa_addr->sa_family == AF_INET) {
                inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(a->ifa_addr)->sin_addr, text, sizeof text);
                v4.push_back(text);
            } else if (a->ifa_addr->sa_family == AF_INET6 && !IN6_IS_ADDR_LINKLOCAL(&reinterpret_cast<const sockaddr_in6*>(a->ifa_addr)->sin6_addr)) {
                inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(a->ifa_addr)->sin6_addr, text, sizeof text);
                v6.push_back(text);
            }
        }
        freeifaddrs(list);
    }
#endif
    v4.insert(v4.end(), v6.begin(), v6.end());
    return v4;
}

}  // namespace montage::omt::net
