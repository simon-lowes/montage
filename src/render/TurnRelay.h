// Montage — a browser review's TURN relay (RFC 8656), as Montage itself uses it: logins that run out, made from a
// secret the relay shares with Montage, so no viewer's page is ever given a password that lasts.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace montage::output {

struct TurnLogin {
    std::string user, password;
};

// The login a relay that shares `secret` with Montage takes until `expires` (seconds since 1970, UTC), as the REST
// scheme for TURN has it (draft-uberti-behave-turn-rest; coturn's use-auth-secret): the user name is the expiry and a
// name, "1700000000:montage", and the password the Base64 of HMAC-SHA1 of the user name under the secret. The relay
// works the same out for itself and refuses a login whose time has passed.
TurnLogin turnRestLogin(const std::string& secret, int64_t expires, const std::string& name = "montage");

// Where a relay is and how Montage reaches it: over UDP, over TCP, or over TLS on TCP (RFC 8656 has all three between
// a client and its relay; what the relay carries on to viewers is UDP in each). Over TLS the relay's certificate is
// checked against the system's roots and the relay's name, or, when `pin` is given, must be the very certificate
// whose SHA-256 fingerprint that is (hexadecimal, with or without colons), whoever signed it.
struct RelayAddress {
    enum class Transport { Udp, Tcp, Tls };
    std::string host;  // a name, or an IPv4 or IPv6 address (no brackets)
    uint16_t port = 0;
    Transport transport = Transport::Udp;
    std::string pin;
};

// "turn:host:3478", "turn:[2001:db8::1]:3478?transport=tcp" or "turns:host" (RFC 7065) read: false when it is not
// one. The port is 3478, or 5349 for turns:, unless said.
bool parseRelayAddress(const std::string& url, RelayAddress& relay);

// A certificate's fingerprint as the user may give it ("AB:CD:…", "abcd…"), as 64 lower-case hexadecimal digits; ""
// when it is not a SHA-256 fingerprint.
std::string relayPin(const std::string& text);

// One viewer's way through the relay when a review goes only through it. Montage's own end of the connection is kept
// on this computer's loopback, where it can reach nothing else, and the bridge carries what it sends to the relay and
// back: it asks the relay for an address (Allocate, with the login), lets the viewer's addresses in (CreatePermission)
// and wraps each packet for one of them in a Send indication, as RFC 8656 has them. Its one socket to the outside is
// connected to the relay, so nothing leaves this computer for anywhere else, whatever addresses a viewer offers.
// Every answer the relay gives once it knows the login must be signed with the login's key: one that is not is not
// used, and is counted. All the bridges there are share one thread.
class RelayBridge {
public:
    // `host` is the relay's name or address, reached over UDP.
    RelayBridge(std::string host, uint16_t port, TurnLogin login);
    RelayBridge(RelayAddress relay, TurnLogin login);
    ~RelayBridge();  // (gives the address back; not from a bridge's callback)
    RelayBridge(const RelayBridge&) = delete;
    RelayBridge& operator=(const RelayBridge&) = delete;

    // Asks the relay for an address, once: of the family `prefer` says (4: IPv4, 6: IPv6), and of the other where
    // the relay has none of that one. `done` is called once, on the bridges' thread, with "" when there is one or
    // with why not (no answer in about six seconds, the relay's refusal, or a certificate that is not the relay's).
    void allocate(std::function<void(const std::string& error)> done) { allocate(4, std::move(done)); }
    void allocate(int prefer, std::function<void(const std::string& error)> done);
    // The address at the relay that viewers send to ("" and 0 before there is one).
    std::string relayedAddress() const;
    uint16_t relayedPort() const;
    // The loopback port that stands for a viewer's address: what Montage's end sends there goes to that address
    // through the relay, and what the relay passes on from that address arrives from there. 0 when the address is not
    // of the family of the address at the relay (a relay carries IPv4 to IPv4 and IPv6 to IPv6), is no address, or
    // there are 32 already.
    uint16_t peer(const std::string& ip, uint16_t port);
    // Montage's end: the loopback port that what arrives is passed on to (until it is said, the port the first packet
    // for a viewer came from), and its ICE password. With the password, the answers Montage's end gives to a viewer's
    // connectivity checks name the viewer's own address, as the viewer expects, in place of the loopback port that
    // stands for it (the address is put right and the answer signed again), and a viewer's check that gives the
    // controlling end's tiebreaker as 0, as Chrome does from a relay it reaches over TCP or TLS, is given 1 (signed
    // again), as Montage's end would refuse it.
    void setLocal(uint16_t port, const std::string& icePassword);

    // Packets wrapped for the relay and unwrapped from it so far, and the relay's last refusal ("" for none).
    uint64_t toRelay() const;
    uint64_t fromRelay() const;
    std::string lastError() const;
    // Answers from the relay that were not used because they were not signed with the login's key.
    uint64_t answersRefused() const;
    // Whether the bridge carries nothing any more: the relay let its address go (the address's time ran out, or a
    // Refresh was answered 437), or the connection to the relay ended. A bridge does not ask for another address:
    // it would be a different one, and the viewer was told this one, so the viewer is let go and connects again.
    bool ended() const;
    // Begins giving the address back without waiting for the relay's answer (letting the bridge go then waits only
    // for what is left of that moment): many bridges let go together take a moment, not a moment each.
    void letGo();
    // Where the bridge has got to, for the panel: "Finding the relay", "Connecting to the relay over TLS", "Asking
    // the relay for an address", "Carrying over TCP", "Ended".
    std::string state() const;

    // The threads the bridges alive run on (0 with none alive, else 1), and how many were ever started.
    static int threads();
    static uint64_t threadsStarted();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// A viewer's offer for Montage's end behind a bridge: each of its UDP candidates with an IPv4 or IPv6 address is
// moved to the loopback port `alias` gives for it (as a host candidate), and the others (names, TCP, and those
// `alias` gives 0 for) are left out.
std::string offerThroughRelay(const std::string& sdp, const std::function<uint16_t(const std::string& ip, uint16_t port)>& alias);

// The family of relayed address a viewer's offer needs: 6 when its UDP candidates' addresses are all IPv6, else 4.
int offerFamily(const std::string& sdp);
// How many UDP candidates with an IPv4 or IPv6 address a viewer's offer has: what a bridge can carry to. None, and
// the viewer cannot be reached through the relay (a browser that got no address at the relay offers none).
int offerAddresses(const std::string& sdp);

// Montage's answer from behind a bridge: its loopback candidate becomes the address at the relay, as a relay
// candidate, and any other candidate is left out. `localPort` is given the loopback port it had (0 when none).
std::string answerThroughRelay(const std::string& sdp, const std::string& relayedIp, uint16_t relayedPort, uint16_t* localPort);

}  // namespace montage::output
