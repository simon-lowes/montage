#pragma once

// A WHIP endpoint (RFC 9725): Capture listening for a feed sent over WebRTC by a browser, a phone or an encoder.
//
// The sender POSTs its offer to the endpoint's address with the token as a bearer token and is answered 201 with
// Montage's answer and the session's own address (Location); it may PATCH that address with more ICE candidates, and
// DELETE it to end the feed. A wrong token is answered 401, a second sender while one is connected 409. A GET of the
// endpoint's address gives a page that sends a camera and microphone, or a screen, for a guest with only a browser.
//
// The endpoint is plain HTTP on this computer's own loopback and HTTPS everywhere else (browsers give a page its
// camera only in a secure context): with the certificate and key given, or one Montage makes for the address, which
// a browser asks its user to accept and an encoder may refuse.
//
// What arrives (H.264, VP8 or VP9 pictures, AV1 where the linked FFmpeg unpacks it, and Opus sound) is handed on as
// RTP packets for FFmpeg's own RTP reader (Capture opens it with the description sdp() gives, reading the packets
// through read()). Pictures start at a keyframe; the sound is lined up with them by the sender's reports (each
// stream's RTP time against the sender's clock), or by when each arrived where the sender reports nothing, and kept
// lined up as the sender's clocks drift.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace montage::whip {

struct Settings {
    std::string host = "127.0.0.1";  // the address listened on ("0.0.0.0" or "::" for every interface)
    int port = 0;                    // 0: any free port (port() tells which)
    std::string path = "/whip";
    std::string token;               // the bearer token a sender must give ("" only on the loopback)
    std::string certificatePem, keyPem;  // HTTPS with these (PEM text); both "" off the loopback: one made for `host`
    std::vector<std::string> iceServers;  // "stun:host:port", "turn:user:password@host:port"
};

// Whether a Capture source names a WHIP endpoint: "whip://host:port/path".
bool isSource(const std::string& url);
// The endpoint a source names: the URL's host, port and path, with the options "token", "certificate" and "key"
// (files of PEM text) and "ice" (ICE servers apart by spaces). False with the reason when it cannot be one.
bool settingsFrom(const std::string& url, const std::map<std::string, std::string>& options, Settings& out, std::string* error);
// Whether this build has WebRTC (else why not).
bool available(std::string* why = nullptr);

struct Status {
    bool listening = false;
    bool sender = false;        // a sender is connected (its media may not have begun)
    std::string from;           // its address
    std::string video, audio;   // the codecs agreed ("h264", "vp8", "vp9", "av1"; "opus")
    bool reports = false;       // the sound is lined up by the sender's reports (else by arrival)
    int64_t packets = 0, lost = 0, dropped = 0;  // RTP packets taken, missing by their numbers, dropped as too far behind
    int keyframesAsked = 0;
    int refused = 0;            // requests answered 401
    bool ended = false;         // the sender ended the feed (DELETE), or its connection closed
};

class Endpoint {
public:
    Endpoint();
    ~Endpoint();  // stops
    Endpoint(const Endpoint&) = delete;
    Endpoint& operator=(const Endpoint&) = delete;

    bool start(const Settings& settings, std::string* error = nullptr);
    void stop();
    int port() const;
    // The address a sender is given: "http://127.0.0.1:8189/whip", "https://192.0.2.7:8189/whip".
    std::string url() const;
    // The SHA-256 fingerprint of the certificate served ("" over plain HTTP), for a sender to check it by.
    std::string certificateFingerprint() const;

    // Waits until a sender's pictures (and its sound, when it sends any) have begun, `stopped` says to give up, or
    // the sender left before they did; then true with the description FFmpeg's "sdp" reader takes (custom I/O).
    bool waitForMedia(std::string& sdp, const std::function<bool()>& stopped, std::string* error = nullptr);
    // The next RTP packet, whole, into `buf`: its size; 0 when none came within `waitMs`; -1 when the feed has
    // ended and every packet was read.
    int read(uint8_t* buf, int size, int waitMs);
    // The pictures' rate, as measured from the first held (0 before the feed began, or with no pictures).
    double frameRate() const;
    Status status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// The page a GET of the endpoint gives.
const char* guestPage();

}  // namespace montage::whip
