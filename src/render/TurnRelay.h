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

// One viewer's way through the relay when a review goes only through it. Montage's own end of the connection is kept
// on this computer's loopback, where it can reach nothing else, and the bridge carries what it sends to the relay and
// back: it asks the relay for an address (Allocate, with the login), lets the viewer's addresses in (CreatePermission)
// and wraps each packet for one of them in a Send indication, as RFC 8656 has them. Its one socket to the outside is
// connected to the relay, so nothing leaves this computer for anywhere else, whatever addresses a viewer offers. It
// runs a thread of its own.
class RelayBridge {
public:
    // `host` is the relay's name or address (IPv4 is used), reached over UDP.
    RelayBridge(std::string host, uint16_t port, TurnLogin login);
    ~RelayBridge();  // (gives the address back; not from one of its own callbacks)
    RelayBridge(const RelayBridge&) = delete;
    RelayBridge& operator=(const RelayBridge&) = delete;

    // Asks the relay for an address, once. `done` is called once, on the bridge's thread, with "" when there is one
    // or with why not (no answer in about six seconds, or the relay's refusal).
    void allocate(std::function<void(const std::string& error)> done);
    // The address at the relay that viewers send to ("" and 0 before there is one).
    std::string relayedAddress() const;
    uint16_t relayedPort() const;
    // The loopback port that stands for a viewer's address: what Montage's end sends there goes to that address
    // through the relay, and what the relay passes on from that address arrives from there. 0 when the address is not
    // IPv4 or there are 32 already.
    uint16_t peer(const std::string& ip, uint16_t port);
    // Montage's end: the loopback port that what arrives is passed on to (until it is said, the port the first packet
    // for a viewer came from), and its ICE password. With the password, the answers Montage's end gives to a viewer's
    // connectivity checks name the viewer's own address, as the viewer expects, in place of the loopback port that
    // stands for it (the address is put right and the answer signed again).
    void setLocal(uint16_t port, const std::string& icePassword);

    // Packets wrapped for the relay and unwrapped from it so far, and the relay's last refusal ("" for none).
    uint64_t toRelay() const;
    uint64_t fromRelay() const;
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// A viewer's offer for Montage's end behind a bridge: each of its UDP candidates with an IPv4 address is moved to the
// loopback port `alias` gives for it (as a host candidate), and the others (names, IPv6, TCP, and those `alias` gives
// 0 for) are left out.
std::string offerThroughRelay(const std::string& sdp, const std::function<uint16_t(const std::string& ip, uint16_t port)>& alias);

// Montage's answer from behind a bridge: its loopback candidate becomes the address at the relay, as a relay
// candidate, and any other candidate is left out. `localPort` is given the loopback port it had (0 when none).
std::string answerThroughRelay(const std::string& sdp, const std::string& relayedIp, uint16_t relayedPort, uint16_t* localPort);

}  // namespace montage::output
