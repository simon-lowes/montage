// Montage — HID devices (control/SpeedEditor.h's Blackmagic keyboards): the ones plugged in or paired with a vendor
// and product id, opened to read their input reports and send output and feature reports. hidraw on Linux (USB and
// Bluetooth alike; the devices need a udev rule to be opened by the user), IOKit on macOS and the Windows HID API, so
// no HID library is needed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace montage {

struct HidDeviceInfo {
    std::string path;  // how to open it (platform-specific)
    uint16_t vendor = 0, product = 0;
    std::string name, serial;
    bool bluetooth = false;
    uint16_t usagePage = 0, usage = 0;  // the interface's top-level collection (0: not known)
    // The system's own keyboard or mouse interface (a keyboard's ordinary keys): not one to open.
    bool isKeyboardOrMouse() const { return usagePage == 0x01 && (usage == 0x06 || usage == 0x02); }
};
// The usage page and usage of a HID report descriptor's first top-level collection. False if there is none.
bool hidPrimaryUsage(const uint8_t* descriptor, size_t size, uint16_t& page, uint16_t& usage);
// The HID devices (each interface of a device its own) with this vendor id, and product id unless 0.
std::vector<HidDeviceInfo> hidDevices(uint16_t vendor, uint16_t product = 0);

// An open device. Reports are whole, their id first.
class HidDevice {
public:
    virtual ~HidDevice() = default;
    virtual bool write(const std::vector<uint8_t>& report) = 0;        // an output report
    virtual bool sendFeature(const std::vector<uint8_t>& report) = 0;  // SET_REPORT (feature)
    // GET_REPORT (feature) of report `id`: `out` gets the report, its id first. False if the device has none.
    virtual bool getFeature(uint8_t id, std::vector<uint8_t>& out) = 0;
};
// Opens `path` (from hidDevices). `onReport` gets every input report, and `onLost` (at most once) when the device
// goes away, both from another thread, until the device is destroyed.
std::unique_ptr<HidDevice> openHid(const std::string& path, std::function<void(const uint8_t*, size_t)> onReport,
                                   std::function<void()> onLost, std::string* error = nullptr);

}  // namespace montage
