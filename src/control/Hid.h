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
};
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
