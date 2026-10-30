// Montage — live review in any web browser: the Program monitor streamed over WebRTC to a page Montage serves itself,
// which a client opens from a link with nothing to install. Montage listens on one port for HTTPS (a certificate the
// user gives, or one it makes for this computer) and for the WebRTC media (UDP, the same port number). The page asks
// for the passphrase, then offers to receive (WHEP: POST /whep with the passphrase as a bearer token); each viewer gets
// the picture as H.264 (or AV1 or HEVC, whichever the browser decodes; one encoder for each in use, shared by the
// viewers on it) with stereo Opus, encrypted by DTLS-SRTP. A data channel carries each picture's sequence frame and
// timecode (by its RTP timestamp, so the page knows which frame it shows), and brings back the reviewer's notes and
// drawings, which land as markers at the frame they saw (render/RemoteReview.h's RemoteMarker), and their pointer,
// shown to the other viewers and over the Program monitor. Several viewers at once; NAT traversal through STUN and TURN
// servers the user names (none by default). Other WHEP players can pull the same stream with the passphrase.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "render/RemoteReview.h"

namespace montage::output {

// A certificate and private key (PEM) for these host names and addresses: an ECDSA P-256 key (RSA 2048 where Qt's TLS
// is the system's own), self-signed, valid two years. Browsers warn about it until the reviewer accepts it. False and
// why when it cannot be made.
bool makeReviewCertificate(const std::vector<std::string>& hosts, std::string& certificatePem, std::string& keyPem,
                           std::string* error = nullptr);
// This computer's names and addresses, for such a certificate.
std::vector<std::string> reviewCertificateHosts();

class BrowserReviewBackend : public ReviewBackend {
public:
    BrowserReviewBackend();
    ~BrowserReviewBackend() override;
    std::string name() const override { return "Browser"; }
    bool available(std::string* why = nullptr) override;
    std::vector<Device> devices() override;
    void setSettings(const StreamSettings& s) override;
    StreamSettings settings() const override;
    void setSequence(Id id) override;
    bool open(const Device& device, const Mode& mode, const Signal& signal, int audioChannels, int audioRate,
              std::string* error) override;
    bool send(const VideoFrame& frame, const Audio& audio) override;
    void close() override;
    void wake() override;
    // Viewers watching.
    int receivers() override;
    ReviewStats stats() const override;
    std::vector<RemoteMarker> takeMarkers() override;
    std::vector<ReviewPointer> pointers() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// The review page and its script as served (the script reads its settings from the server once the passphrase is
// given).
const char* browserReviewPage();
const char* browserReviewScript();

}  // namespace montage::output
