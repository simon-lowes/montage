// Montage — Canonical XML (W3C C14N 1.0 and Exclusive C14N 1.0), the byte form XML signatures are computed over: no
// declaration, attributes in order, namespace declarations where the form asks for them, empty elements opened and
// closed, characters escaped one way. Montage signs the DCP documents it writes (render/DcpSecurity.h) and checks what
// it reads with it.
#pragma once

#include <string>

namespace montage {

struct CanonicalOptions {
    bool exclusive = false;          // Exclusive C14N (namespaces only where an element or attribute uses them)
    bool comments = false;           // keep comments (the "#WithComments" forms)
    std::string id;                  // the element whose Id attribute (any case: Id, ID, id) this is: only it and what
                                     // is in it; empty for the whole document
    std::string element;             // or the first element of this name, as "{namespace}local"
    bool envelopedSignature = true;  // leave out ds:Signature elements (the enveloped-signature transform)
};

// The canonical form of `xml` (UTF-8). Empty, with `error`, when it is not well-formed or has no element with the id.
std::string canonicalXml(const std::string& xml, const CanonicalOptions& options = {}, std::string* error = nullptr);

}  // namespace montage
