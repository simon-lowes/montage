// Montage — the cinemas and screens KDMs are made for: read from Facility List Messages, the Extended FLM of SMPTE
// ST 430-16 (cinemas, their auditoriums and each auditorium's suites of devices, with the devices' certificate chains)
// and the older FLM of ST 430-7 (a signed ETM listing a cinema's security devices), or added by hand from the
// certificates a cinema sends. Kept as a list in Montage's data folder, from which a booking (a DKDM, a window and
// some screens) makes one KDM per screen (render/Dcp.h makeKdms): to the screen's security manager, with the rest of
// its suite (the projector and other devices) in the KDM's AuthorizedDeviceInfo.
//
// Every certificate chain is checked as it is read (render/DcpSecurity.h checkCertificateChain): a changed
// certificate, or one whose issuer is not given, is refused, and with it the FLM. An ST 430-7 FLM's signature must hold
// and cover its public part; an Extended FLM is not signed (ST 430-16 leaves the signature out), so only its chains
// are checked.
#pragma once

#include <string>
#include <vector>

#include "Dcp.h"

namespace montage {

struct FacilityDevice {
    std::string type;        // the ST 433 device type: "SM" security manager, "PR" projector, "LD", "MDI", ...
    std::string thumbprint;  // its leaf's ST 430-2 thumbprint (base64 SHA-1); or the FLM's CertThumbprint when it has no certificate
    std::vector<std::string> certificates;  // PEM, root first, leaf last (checked); empty when none was given
    bool chainComplete = false;             // the certificates go up to a root (false for a certificate given alone)
    std::string name, manufacturer, model, serial;
    std::string commonName, notAfter;  // its leaf's common name and end (ISO 8601)
    bool active = true;                // IsActive (an inactive device is left out of KDMs)
};

struct FacilityScreen {
    std::string name;                     // the auditorium's number or name (a second suite in it: "<name> #2")
    FacilityDevice server;                // the security manager the screen's KDMs are encrypted to
    std::vector<FacilityDevice> devices;  // the rest of its suite, listed in its KDMs
};

struct Facility {
    std::string id;      // the FLM's FacilityID, or a urn:uuid for one added by hand
    std::string name;
    std::string timeZone;  // IANA ("Australia/Melbourne") or a UTC offset ("UTC+01:00"); KDM windows are written in it
    std::string circuit;
    std::string source;  // "ST 430-16", "ST 430-7" or "by hand"
    std::string issueDate;
    std::vector<std::string> kdmEmails;  // where the FLM asks for KDMs to be sent
    std::vector<FacilityScreen> screens;
    // An ST 430-7 FLM with several security managers does not say which screen its other devices are in: they are
    // kept here, out of KDMs.
    std::vector<FacilityDevice> otherDevices;
    std::vector<std::string> notes;  // what reading it noticed that did not stop it
};

// Reads an Extended FLM (ST 430-16) or an FLM (ST 430-7); false (and why) when it is neither, a chain does not check,
// an ST 430-7 FLM's signature does not hold, or it lists no screen with a server certificate.
bool readFlm(const std::string& xml, Facility& out, std::string* error = nullptr);
bool readFlmFile(const std::string& path, Facility& out, std::string* error = nullptr);

// The facility list: dcp-facilities.json in Montage's data folder (MONTAGE_DCP_FACILITIES overrides it). A missing
// file is an empty list.
std::string facilitiesFile();
bool loadFacilities(std::vector<Facility>& out, std::string* error = nullptr, const std::string& file = {});
bool saveFacilities(const std::vector<Facility>& list, std::string* error = nullptr, const std::string& file = {});
// Puts `f` in the list: in place of the one with its id (a newer FLM of a cinema), or at the end. Its index.
size_t mergeFacility(std::vector<Facility>& list, Facility f);
// A screen added by hand to the cinema of that name (made when there is none): the server's certificate (PEM text or
// a file of it; a chain is checked to its root, a leaf alone is taken as it is) and other devices' certificates
// (likewise), which go into its KDMs. Replaces a screen of that name.
bool addScreenByHand(std::vector<Facility>& list, const std::string& cinema, const std::string& screen, const std::string& server,
                     const std::vector<std::string>& devices, std::string* error = nullptr);

// What a booking needs of a screen: its server's leaf and its active devices' thumbprints.
KdmScreen kdmScreen(const Facility& cinema, const FacilityScreen& screen);
// Screens named "Cinema/Screen", or "Cinema" for all its screens (the cinema by name or id, names without regard to
// case); false (and which) when a name matches none.
bool pickScreens(const std::vector<Facility>& list, const std::vector<std::string>& names, std::vector<KdmScreen>& out,
                 std::string* error = nullptr);

}  // namespace montage
