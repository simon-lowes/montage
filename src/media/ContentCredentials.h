// Montage — Content Credentials (C2PA, as Premiere 26.2 attaches them): a signed record inside an exported file of how
// it was made, which anyone can check (contentcredentials.org/verify, Adobe's and others' tools) and which breaks if
// the file is changed. An export records that Montage made it from other media, each AI model that generated or
// changed what is in it (core/Provenance.h), and the credentials its source files already carried (a camera's
// signature, a stock library's), as ingredients, so the chain back to the camera is kept. Signed with the editor's own
// certificate and key. On import, a file's credentials are read and checked (who signed it, whether it is intact, and
// whether it says AI made it). Through c2pa-rs (the Content Authenticity Initiative's library, MIT or Apache-2.0).
#pragma once

#include <string>
#include <vector>

#include "core/Provenance.h"

namespace montage {

bool contentCredentialsAvailable();  // built with the C2PA library (scripts/build-c2pa.sh)
// Whether credentials can go in (and be read from) a file of this name: MP4, MOV, M4A, JPEG, PNG, TIFF, WebP, HEIF,
// AVIF, WAV, MP3, AVI, PDF... (not MXF, MKV or WebM).
bool contentCredentialsFormat(const std::string& path);

// What a file's credentials say.
struct CredentialSummary {
    bool present = false;
    // "Trusted" (a signer on the trust list), "Valid" (intact, its signer not known to be trusted) or "Invalid"
    // (changed since it was signed, or a bad signature).
    std::string state;
    std::string issuer;     // who the signing certificate belongs to
    std::string signedAt;   // when, if a time stamp says
    std::string generator;  // the app or device that made it ("Sony ILCE-1", "Adobe Premiere 26.2"...)
    std::string title;
    std::vector<std::string> actions;  // c2pa.created, c2pa.edited...
    bool aiGenerated = false;          // an action or ingredient made (or partly made) by a generative model
    std::vector<std::string> problems; // why it is not valid
    std::string json;                  // the whole report (c2pa-rs's manifest store JSON)
};
// Reads and checks a file's credentials; false with `error` when the file could not be read (a file without
// credentials is not an error: `present` is false).
bool readContentCredentials(const std::string& path, CredentialSummary& out, std::string* error = nullptr);
// One line for people: "Signed by Sony Corporation, 2026-10-01 (valid); made with AI".
std::string credentialsLabel(const CredentialSummary& c);

// The editor's signing identity: a certificate chain (the signing certificate first) and its private key, in PEM.
// The certificate needs the digital signature key usage and the e-mail protection or document signing extended key
// usage, and cannot sign itself (a certificate authority must issue it).
struct CredentialSigner {
    std::string certificatePath;
    std::string keyPath;
    std::string algorithm;  // es256, es384, es512, ps256, ps384, ps512 or ed25519; "" from the key
};

struct CredentialOptions {
    std::string title;        // the file's title (default its name)
    bool allowAiTraining = false;  // false: says the content may not be used to train AI models (CAWG training and mining)
    std::vector<std::string> sources;  // source files whose own credentials go in as ingredients (those that have any)
};

// Signs the file at `path` in place: its credentials say Montage made it from other media, with `provenance`'s AI
// use, and carry the credentials of `options.sources`. False with `error` when the format cannot hold them, the
// signer is refused or the file cannot be written (the file is then as it was).
bool signContentCredentials(const std::string& path, const Provenance& provenance, const CredentialSigner& signer,
                            const CredentialOptions& options = {}, std::string* error = nullptr);

// Whether this signer can sign (its certificate and key read, match and are accepted), tried on a moment of sound in
// memory, so a refused certificate is known before a long export rather than after; false with why.
bool checkCredentialSigner(const CredentialSigner& signer, std::string* error = nullptr);

// The manifest definition signContentCredentials gives c2pa-rs (exposed for tests).
std::string credentialManifestJson(const std::string& path, const Provenance& provenance, const CredentialOptions& options);

}  // namespace montage
