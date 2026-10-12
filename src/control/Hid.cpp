#include "Hid.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>
#elif defined(_WIN32)
#include <windows.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
}
#elif defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fstream>
#include <sstream>

#include "SpeedEditor.h"
#endif

namespace montage {

namespace {
// Calls onLost once, whichever thread notices first.
struct LostOnce {
    std::function<void()> fn;
    std::atomic<bool> done{false};
    void operator()() {
        if (!done.exchange(true) && fn) fn();
    }
};
}  // namespace

#if defined(__linux__)

std::vector<HidDeviceInfo> hidDevices(uint16_t vendor, uint16_t product) {
    std::vector<HidDeviceInfo> out;
    DIR* dir = opendir("/sys/class/hidraw");
    if (!dir) return out;
    while (dirent* e = readdir(dir)) {
        if (std::strncmp(e->d_name, "hidraw", 6) != 0) continue;
        std::ifstream f(std::string("/sys/class/hidraw/") + e->d_name + "/device/uevent");
        if (!f) continue;
        std::stringstream ss;
        ss << f.rdbuf();
        HidDeviceInfo d;
        int bus = 0;
        if (!speed::parseHidUevent(ss.str(), bus, d.vendor, d.product, d.name, d.serial)) continue;
        if (d.vendor != vendor || (product && d.product != product)) continue;
        d.path = std::string("/dev/") + e->d_name;
        d.bluetooth = bus == 0x05;  // BUS_BLUETOOTH
        out.push_back(d);
    }
    closedir(dir);
    std::sort(out.begin(), out.end(), [](const HidDeviceInfo& a, const HidDeviceInfo& b) { return a.path < b.path; });
    return out;
}

namespace {
class HidrawDevice : public HidDevice {
public:
    HidrawDevice(int fd, std::function<void(const uint8_t*, size_t)> onReport, std::function<void()> onLost)
        : fd_(fd), onReport_(std::move(onReport)) {
        lost_.fn = std::move(onLost);
        if (pipe(wake_) != 0) wake_[0] = wake_[1] = -1;
        reader_ = std::thread([this] { run(); });
    }
    ~HidrawDevice() override {
        stop_ = true;
        if (wake_[1] >= 0) {
            const char c = 0;
            [[maybe_unused]] const ssize_t n = ::write(wake_[1], &c, 1);
        }
        if (reader_.joinable()) reader_.join();
        for (int p : wake_)
            if (p >= 0) close(p);
        close(fd_);
    }
    bool write(const std::vector<uint8_t>& report) override {
        std::lock_guard<std::mutex> lock(io_);
        return !report.empty() && ::write(fd_, report.data(), report.size()) == ssize_t(report.size());
    }
    bool sendFeature(const std::vector<uint8_t>& report) override {
        std::lock_guard<std::mutex> lock(io_);
        std::vector<uint8_t> buf = report;
        return !buf.empty() && ioctl(fd_, HIDIOCSFEATURE(buf.size()), buf.data()) >= 0;
    }
    bool getFeature(uint8_t id, std::vector<uint8_t>& out) override {
        std::lock_guard<std::mutex> lock(io_);
        std::vector<uint8_t> buf(64, 0);
        buf[0] = id;
        const int n = ioctl(fd_, HIDIOCGFEATURE(buf.size()), buf.data());  // (the report number comes first)
        if (n <= 0) return false;
        out.assign(buf.begin(), buf.begin() + n);
        return true;
    }

private:
    void run() {
        uint8_t buf[256];
        while (!stop_) {
            pollfd p[2] = {{fd_, POLLIN, 0}, {wake_[0], POLLIN, 0}};
            if (poll(p, wake_[0] >= 0 ? 2 : 1, wake_[0] >= 0 ? -1 : 200) < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (stop_) break;
            if (p[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                lost_();
                break;
            }
            if (!(p[0].revents & POLLIN)) continue;
            const ssize_t n = read(fd_, buf, sizeof buf);
            if (n > 0) onReport_(buf, size_t(n));
            else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                lost_();  // unplugged, or out of range
                break;
            }
        }
    }

    int fd_;
    int wake_[2] = {-1, -1};
    std::function<void(const uint8_t*, size_t)> onReport_;
    LostOnce lost_;
    std::atomic<bool> stop_{false};
    std::mutex io_;
    std::thread reader_;
};
}  // namespace

std::unique_ptr<HidDevice> openHid(const std::string& path, std::function<void(const uint8_t*, size_t)> onReport,
                                   std::function<void()> onLost, std::string* error) {
    const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (error)
            *error = errno == EACCES ? "No permission to open " + path +
                                           ": add a udev rule giving your user the device (see README, Blackmagic keyboards)"
                                     : "Cannot open " + path + ": " + std::strerror(errno);
        return nullptr;
    }
    return std::make_unique<HidrawDevice>(fd, std::move(onReport), std::move(onLost));
}

#elif defined(__APPLE__)

namespace {
int intProperty(IOHIDDeviceRef d, CFStringRef key) {
    int v = 0;
    if (auto n = CFNumberRef(IOHIDDeviceGetProperty(d, key)); n && CFGetTypeID(n) == CFNumberGetTypeID())
        CFNumberGetValue(n, kCFNumberIntType, &v);
    return v;
}

std::string stringProperty(IOHIDDeviceRef d, CFStringRef key) {
    auto s = CFStringRef(IOHIDDeviceGetProperty(d, key));
    if (!s || CFGetTypeID(s) != CFStringGetTypeID()) return {};
    char buf[512];
    return CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8) ? std::string(buf) : std::string();
}

void setNumber(CFMutableDictionaryRef dict, CFStringRef key, int value) {
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
    CFDictionarySetValue(dict, key, n);
    CFRelease(n);
}
}  // namespace

std::vector<HidDeviceInfo> hidDevices(uint16_t vendor, uint16_t product) {
    std::vector<HidDeviceInfo> out;
    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!manager) return out;
    CFMutableDictionaryRef match =
        CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    setNumber(match, CFSTR(kIOHIDVendorIDKey), vendor);
    if (product) setNumber(match, CFSTR(kIOHIDProductIDKey), product);
    IOHIDManagerSetDeviceMatching(manager, match);
    CFRelease(match);
    if (CFSetRef set = IOHIDManagerCopyDevices(manager)) {
        const CFIndex n = CFSetGetCount(set);
        std::vector<const void*> devices(size_t(n));
        CFSetGetValues(set, devices.data());
        for (const void* v : devices) {
            auto d = IOHIDDeviceRef(v);
            uint64_t entry = 0;
            if (IORegistryEntryGetRegistryEntryID(IOHIDDeviceGetService(d), &entry) != KERN_SUCCESS) continue;
            HidDeviceInfo info;
            info.path = "DevSrvsID:" + std::to_string(entry);
            info.vendor = uint16_t(intProperty(d, CFSTR(kIOHIDVendorIDKey)));
            info.product = uint16_t(intProperty(d, CFSTR(kIOHIDProductIDKey)));
            info.name = stringProperty(d, CFSTR(kIOHIDProductKey));
            info.serial = stringProperty(d, CFSTR(kIOHIDSerialNumberKey));
            info.bluetooth = stringProperty(d, CFSTR(kIOHIDTransportKey)).find("Bluetooth") != std::string::npos;
            out.push_back(info);
        }
        CFRelease(set);
    }
    CFRelease(manager);
    std::sort(out.begin(), out.end(), [](const HidDeviceInfo& a, const HidDeviceInfo& b) { return a.path < b.path; });
    return out;
}

namespace {
class IoKitHidDevice : public HidDevice {
public:
    IoKitHidDevice(IOHIDDeviceRef device, std::function<void(const uint8_t*, size_t)> onReport, std::function<void()> onLost)
        : device_(device), onReport_(std::move(onReport)) {
        lost_.fn = std::move(onLost);
        buffer_.resize(1024);
        reader_ = std::thread([this] { run(); });
    }
    ~IoKitHidDevice() override {
        stop_ = true;
        if (reader_.joinable()) reader_.join();
        IOHIDDeviceClose(device_, kIOHIDOptionsTypeNone);
        CFRelease(device_);
    }
    bool write(const std::vector<uint8_t>& report) override {
        return !report.empty() &&
               IOHIDDeviceSetReport(device_, kIOHIDReportTypeOutput, report[0], report.data(), CFIndex(report.size())) == kIOReturnSuccess;
    }
    bool sendFeature(const std::vector<uint8_t>& report) override {
        return !report.empty() &&
               IOHIDDeviceSetReport(device_, kIOHIDReportTypeFeature, report[0], report.data(), CFIndex(report.size())) == kIOReturnSuccess;
    }
    bool getFeature(uint8_t id, std::vector<uint8_t>& out) override {
        uint8_t buf[64] = {id};
        CFIndex n = sizeof buf;
        if (IOHIDDeviceGetReport(device_, kIOHIDReportTypeFeature, id, buf, &n) != kIOReturnSuccess || n <= 0) return false;
        out.assign(buf, buf + n);
        if (out[0] != id) out.insert(out.begin(), id);  // (the report id first, as on the other systems)
        return true;
    }

private:
    static void report(void* context, IOReturn result, void*, IOHIDReportType, uint32_t, uint8_t* data, CFIndex n) {
        auto* self = static_cast<IoKitHidDevice*>(context);
        if (result == kIOReturnSuccess && n > 0 && !self->stop_) self->onReport_(data, size_t(n));
    }
    static void removed(void* context, IOReturn, void*) { static_cast<IoKitHidDevice*>(context)->lost_(); }
    void run() {
        IOHIDDeviceScheduleWithRunLoop(device_, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        IOHIDDeviceRegisterInputReportCallback(device_, buffer_.data(), CFIndex(buffer_.size()), &IoKitHidDevice::report, this);
        IOHIDDeviceRegisterRemovalCallback(device_, &IoKitHidDevice::removed, this);
        while (!stop_) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.2, false);
        IOHIDDeviceRegisterInputReportCallback(device_, buffer_.data(), CFIndex(buffer_.size()), nullptr, nullptr);
        IOHIDDeviceRegisterRemovalCallback(device_, nullptr, nullptr);
        IOHIDDeviceUnscheduleFromRunLoop(device_, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    }

    IOHIDDeviceRef device_;
    std::function<void(const uint8_t*, size_t)> onReport_;
    LostOnce lost_;
    std::vector<uint8_t> buffer_;
    std::atomic<bool> stop_{false};
    std::thread reader_;
};
}  // namespace

std::unique_ptr<HidDevice> openHid(const std::string& path, std::function<void(const uint8_t*, size_t)> onReport,
                                   std::function<void()> onLost, std::string* error) {
    auto fail = [&](const std::string& why) -> std::unique_ptr<HidDevice> {
        if (error) *error = why;
        return nullptr;
    };
    if (path.rfind("DevSrvsID:", 0) != 0) return fail("Not a device: " + path);
    const uint64_t entry = std::strtoull(path.c_str() + 10, nullptr, 10);
    io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL, IORegistryEntryIDMatching(entry));
    if (!service) return fail("The device is no longer there");
    IOHIDDeviceRef device = IOHIDDeviceCreate(kCFAllocatorDefault, service);
    IOObjectRelease(service);
    if (!device) return fail("Cannot open the device");
    if (const IOReturn r = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone); r != kIOReturnSuccess) {
        CFRelease(device);
        return fail(r == kIOReturnNotPermitted ? "macOS refused the device: allow Montage under Privacy & Security, Input Monitoring"
                                               : "Cannot open the device");
    }
    return std::make_unique<IoKitHidDevice>(device, std::move(onReport), std::move(onLost));
}

#elif defined(_WIN32)

namespace {
std::string utf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(size_t(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
}  // namespace

std::vector<HidDeviceInfo> hidDevices(uint16_t vendor, uint16_t product) {
    std::vector<HidDeviceInfo> out;
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA ifd{};
    ifd.cbSize = sizeof ifd;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &guid, i, &ifd); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        if (need == 0) continue;
        std::vector<uint8_t> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, nullptr)) continue;
        // The vendor is in the path (USB "...vid_1edb&pid_da0e...", Bluetooth "..._vid&00021edb_pid&da0e..."): other
        // devices are not opened at all.
        std::string lower = utf8(detail->DevicePath);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        char vid[8];
        std::snprintf(vid, sizeof vid, "%04x", unsigned(vendor));
        if (lower.find(vid) == std::string::npos) continue;
        // No access asked: enough to read its attributes, even of devices the system holds (keyboards).
        HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES a{};
        a.Size = sizeof a;
        if (HidD_GetAttributes(h, &a) && a.VendorID == vendor && (!product || a.ProductID == product)) {
            HidDeviceInfo d;
            d.path = utf8(detail->DevicePath);
            d.vendor = a.VendorID;
            d.product = a.ProductID;
            wchar_t text[256] = {};
            if (HidD_GetProductString(h, text, sizeof text)) d.name = utf8(text);
            wchar_t serial[256] = {};
            if (HidD_GetSerialNumberString(h, serial, sizeof serial)) d.serial = utf8(serial);
            d.bluetooth = lower.find("00001124-0000-1000-8000-00805f9b34fb") != std::string::npos || lower.find("bthledevice") != std::string::npos;
            out.push_back(d);
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    std::sort(out.begin(), out.end(), [](const HidDeviceInfo& a, const HidDeviceInfo& b) { return a.path < b.path; });
    return out;
}

namespace {
class WinHidDevice : public HidDevice {
public:
    WinHidDevice(HANDLE h, const HIDP_CAPS& caps, std::function<void(const uint8_t*, size_t)> onReport, std::function<void()> onLost)
        : h_(h), inLen_(caps.InputReportByteLength), outLen_(caps.OutputReportByteLength), featureLen_(caps.FeatureReportByteLength),
          onReport_(std::move(onReport)) {
        lost_.fn = std::move(onLost);
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        reader_ = std::thread([this] { run(); });
    }
    ~WinHidDevice() override {
        SetEvent(stopEvent_);
        if (reader_.joinable()) reader_.join();
        CloseHandle(stopEvent_);
        CloseHandle(h_);
    }
    bool write(const std::vector<uint8_t>& report) override {
        if (report.empty()) return false;
        std::lock_guard<std::mutex> lock(io_);
        std::vector<uint8_t> buf = report;
        buf.resize((std::max)(buf.size(), size_t(outLen_)), 0);  // (Windows wants the longest output report's length)
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD n = 0;
        bool ok = WriteFile(h_, buf.data(), DWORD(buf.size()), nullptr, &ov) != 0;
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, 1000) != WAIT_OBJECT_0) CancelIoEx(h_, &ov);
            ok = GetOverlappedResult(h_, &ov, &n, TRUE) != 0;
        } else if (ok) {
            ok = GetOverlappedResult(h_, &ov, &n, TRUE) != 0;
        }
        CloseHandle(ov.hEvent);
        return ok;
    }
    bool sendFeature(const std::vector<uint8_t>& report) override {
        if (report.empty()) return false;
        std::lock_guard<std::mutex> lock(io_);
        std::vector<uint8_t> buf = report;
        buf.resize((std::max)(buf.size(), size_t(featureLen_)), 0);
        return HidD_SetFeature(h_, buf.data(), ULONG(buf.size())) != 0;
    }
    bool getFeature(uint8_t id, std::vector<uint8_t>& out) override {
        std::lock_guard<std::mutex> lock(io_);
        std::vector<uint8_t> buf((std::max)(size_t(featureLen_), size_t(2)), 0);
        buf[0] = id;
        if (!HidD_GetFeature(h_, buf.data(), ULONG(buf.size()))) return false;
        out = std::move(buf);
        return true;
    }

private:
    void run() {
        std::vector<uint8_t> buf((std::max)(size_t(inLen_), size_t(64)));
        HANDLE readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        for (;;) {
            OVERLAPPED ov{};
            ov.hEvent = readEvent;
            ResetEvent(readEvent);
            DWORD n = 0;
            if (!ReadFile(h_, buf.data(), DWORD(buf.size()), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) {
                lost_();
                break;
            }
            HANDLE waits[2] = {readEvent, stopEvent_};
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
                CancelIoEx(h_, &ov);
                GetOverlappedResult(h_, &ov, &n, TRUE);
                break;
            }
            if (!GetOverlappedResult(h_, &ov, &n, FALSE)) {
                lost_();  // unplugged, or out of range
                break;
            }
            if (n > 0) onReport_(buf.data(), size_t(n));
        }
        CloseHandle(readEvent);
    }

    HANDLE h_;
    USHORT inLen_, outLen_, featureLen_;
    std::function<void(const uint8_t*, size_t)> onReport_;
    LostOnce lost_;
    HANDLE stopEvent_ = nullptr;
    std::mutex io_;
    std::thread reader_;
};
}  // namespace

std::unique_ptr<HidDevice> openHid(const std::string& path, std::function<void(const uint8_t*, size_t)> onReport,
                                   std::function<void()> onLost, std::string* error) {
    HANDLE h = CreateFileW(wide(path).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = "Cannot open the device (another program, such as DaVinci Resolve, may hold it)";
        return nullptr;
    }
    PHIDP_PREPARSED_DATA pp = nullptr;
    HIDP_CAPS caps{};
    if (!HidD_GetPreparsedData(h, &pp) || HidP_GetCaps(pp, &caps) != HIDP_STATUS_SUCCESS) {
        if (pp) HidD_FreePreparsedData(pp);
        CloseHandle(h);
        if (error) *error = "Cannot read the device's reports";
        return nullptr;
    }
    HidD_FreePreparsedData(pp);
    return std::make_unique<WinHidDevice>(h, caps, std::move(onReport), std::move(onLost));
}

#else

std::vector<HidDeviceInfo> hidDevices(uint16_t, uint16_t) { return {}; }

std::unique_ptr<HidDevice> openHid(const std::string&, std::function<void(const uint8_t*, size_t)>, std::function<void()>,
                                   std::string* error) {
    if (error) *error = "HID devices are not supported on this system";
    return nullptr;
}

#endif

}  // namespace montage
