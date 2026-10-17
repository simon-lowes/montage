// Montage — digital cinema security: the certificate chain Montage signs with (SMPTE ST 430-2), signed compositions
// and packing lists (XML signatures, ST 429-7 and 429-8), and Key Delivery Messages (ST 430-1, the ETM of ST 430-3)
// that carry an encrypted DCP's content keys to the cinema servers named by their certificates. With OpenSSL 3.
//
// The chain: a root, an intermediate and a leaf, RSA 2048 and SHA-256, X.509 v3 with the constraints, key usages and
// key identifiers the standard asks for, names in PrintableString with the public key's SHA-1 as dnQualifier and the
// roles in the common names (".montage.smpte-430-2.ROOT", "CS.montage.smpte-430-2.LEAF"); made once and kept in
// Montage's data folder, backdated two days and valid for just under ten years (checkers warn past that).
//
// Signatures are enveloped, as cinema servers and checkers expect them: a Signer naming the leaf, then a
// ds:Signature over the document's Canonical XML (render/XmlCanonical.h) with the leaf's key, its certificates leaf
// first. A KDM signs its public and private parts by reference (SHA-256); each content key travels as the 138-byte
// block of ST 430-1 (structure id, the signer's thumbprint, the composition, key type and id, the validity window and
// the key) under RSA-OAEP to the recipient's certificate. A DKDM is a KDM to Montage's own leaf: content keys are kept
// only that way, and KDMs for cinemas are made from it.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "DcpMxf.h"

namespace montage::dcp {

// Whether this build can encrypt, sign and make KDMs (OpenSSL 3).
bool securityAvailable();

// PEM certificates root first, leaf last, and the leaf's private key (PKCS#8 PEM, unencrypted, as KDM tools read it).
struct CertificateChain {
    std::vector<std::string> certificates;
    std::string privateKey;
    bool empty() const { return certificates.empty(); }
};
// A new chain for `organisation` (PrintableString letters only are kept; "Montage" when none are left).
bool makeCertificateChain(const std::string& organisation, CertificateChain& out, std::string* error = nullptr);
// The chain in `folder` (root.pem, intermediate.pem, leaf.pem, leaf.key), made there first when there is none. An empty
// folder means Montage's own: dcp-certificates in its data folder (MONTAGE_DCP_CERTIFICATES overrides it).
bool signerCertificateChain(CertificateChain& out, std::string* error = nullptr, const std::string& folder = {});
std::string signerFolder();
// Where Montage keeps the DKDMs of the encrypted DCPs it made: dkdm in its data folder (MONTAGE_DCP_DKDMS overrides it).
std::string dkdmFolder();
// Writes a file whole or not at all (a temporary file put in place); `secret`: readable by its owner only from the
// moment it is created (POSIX; on Windows the data folder is the user's own).
bool writeWholeFile(const std::string& path, const std::string& text, bool secret = false);

struct CertificateInfo {
    std::string subject, issuer;    // RFC 2253, as XML signatures and KDMs name certificates
    std::string serial;             // decimal
    std::string thumbprint;         // base64 SHA-1 of the TBSCertificate
    std::string commonName, organisation, dnQualifier;
    std::string notBefore, notAfter;  // ISO 8601, UTC
    bool ca = false;
    int bits = 0;
};
bool readCertificate(const std::string& pem, CertificateInfo& out, std::string* error = nullptr);
// The certificates in a PEM file or a cinema's chain (several, in file order).
std::vector<std::string> splitCertificates(const std::string& text);

// Signs a composition playlist or packing list in place: Signer and ds:Signature after the root's last child.
bool signXml(std::string& xml, const CertificateChain& chain, std::string* error = nullptr);
// Checks an enveloped signature: the digest of what each reference covers, the signature by the first KeyInfo
// certificate, and that each certificate there is issued and signed by the next, a certificate authority. `check`
// (when given) gets the references ("" the whole document, "#Id" an element) and the signer's thumbprint, so the
// caller can require what must be covered.
struct SignatureCheck {
    std::vector<std::string> references;
    std::string signerThumbprint;  // base64 SHA-1, as CertificateInfo::thumbprint
};
bool verifyXmlSignature(const std::string& xml, std::string* error = nullptr, SignatureCheck* check = nullptr);

// A track file's AES-128 content key and its id (the key id the composition and KDMs name), with its ST 430-1 type:
// "MDIK" picture, "MDAK" sound, "MDSK" subtitles.
struct ContentKey {
    Uuid id{};
    std::array<uint8_t, 16> key{};
    std::string type = "MDIK";
};
// A new key from the system's random source; `ok` false (and the key not to be used) when that fails.
ContentKey newContentKey(const std::string& type, bool* ok = nullptr);

struct KdmSettings {
    std::string cplId;                   // the composition's UUID
    std::string contentTitle;            // its ContentTitleText
    std::string annotation;
    std::string notValidBefore, notValidAfter;  // "YYYY-MM-DDTHH:MM:SS+HH:MM" (kdmTime)
    std::vector<ContentKey> keys;
    std::string recipient;               // the recipient's (the server's leaf) certificate, PEM
    std::vector<std::string> trustedDevices;  // thumbprints of the devices the keys may play on; empty: any device
};
// The timestamp form KDMs carry (exactly 25 characters), at `secondsSinceEpoch` in the offset given (minutes).
std::string kdmTime(int64_t secondsSinceEpoch, int offsetMinutes = 0);
bool makeKdm(const KdmSettings& settings, const CertificateChain& signer, std::string& xml, std::string* error = nullptr);

struct KdmContents {
    std::string cplId, contentTitle, annotation, notValidBefore, notValidAfter;
    std::string signerThumbprint;  // the certificate that signed it (base64 SHA-1)
    std::string recipientSubject, recipientIssuer, recipientSerial;
    std::vector<ContentKey> keys;  // with `privateKey`: each key; without, their ids and types only
};
// Reads a KDM: only what its signature covers (the public and private parts, each signed by reference); with the
// recipient's private key (PEM), opens its keys too (checking each block's structure, signer thumbprint, composition,
// key and window against the public part). False when the signature does not hold.
bool readKdm(const std::string& xml, const std::string& privateKey, KdmContents& out, std::string* error = nullptr);

// ST 429-6 track file encryption: AES-128-CBC of the essence after its plaintext offset, a check value under the same
// key, and an HMAC-SHA1 MIC under a key derived from the content key (render/DcpMxf.h writes the triplets).
bool aesCbcEncrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t size, uint8_t* out);
bool aesCbcDecrypt(const uint8_t key[16], const uint8_t iv[16], const uint8_t* in, size_t size, uint8_t* out);
bool hmacSha1(const uint8_t* key, size_t keySize, const uint8_t* data, size_t size, uint8_t out[20]);
bool sha1(const uint8_t* data, size_t size, uint8_t out[20]);
bool randomBytes(uint8_t* out, size_t size);

}  // namespace montage::dcp
