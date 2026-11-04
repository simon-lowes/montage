#include "SerialPort.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#endif

namespace montage {

SerialPort::~SerialPort() { close(); }

#ifndef _WIN32
namespace {

// Which of `fds` are ready to read (or write) within `ms`. select(), not poll(): macOS's poll() refuses terminals.
int waitFds(const int* fds, bool* ready, int n, bool forWrite, int ms) {
    int r;
    fd_set set;
    do {
        FD_ZERO(&set);
        int top = -1;
        for (int i = 0; i < n; ++i) FD_SET(fds[i], &set), top = std::max(top, fds[i]);
        timeval tv{ms / 1000, (ms % 1000) * 1000};
        r = select(top + 1, forWrite ? nullptr : &set, forWrite ? &set : nullptr, nullptr, &tv);
    } while (r < 0 && errno == EINTR);
    for (int i = 0; i < n; ++i) ready[i] = r > 0 && FD_ISSET(fds[i], &set);
    return r;
}

}  // namespace
#endif

#ifdef _WIN32

bool SerialPort::open(const std::string& name, int baud, char parity, std::string* error) {
    close();
    // ("\\.\" lets COM10 and above open)
    const std::string path = name.rfind("\\\\.\\", 0) == 0 ? name : "\\\\.\\" + name;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = "Cannot open " + name + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    DCB dcb{};
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h, &dcb)) {
        CloseHandle(h);
        if (error) *error = name + " is not a serial port";
        return false;
    }
    dcb.BaudRate = DWORD(baud);
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity = parity == 'O' ? ODDPARITY : parity == 'E' ? EVENPARITY : NOPARITY;
    dcb.fParity = parity != 'N';
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) {
        CloseHandle(h);
        if (error) *error = "Cannot set " + name + " to " + std::to_string(baud) + " baud";
        return false;
    }
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    handle_ = h;
    name_ = name;
    return true;
}

void SerialPort::close() {
    if (handle_) CloseHandle(HANDLE(handle_));
    handle_ = nullptr;
}

bool SerialPort::isOpen() const { return handle_ != nullptr; }

bool SerialPort::write(const uint8_t* data, size_t n) {
    if (!handle_) return false;
    while (n > 0) {
        DWORD done = 0;
        if (!WriteFile(HANDLE(handle_), data, DWORD(n), &done, nullptr)) return false;
        data += done;
        n -= done;
    }
    return true;
}

int SerialPort::read(uint8_t* out, size_t max, int ms) {
    if (!handle_) return -1;
    // The first byte waited for up to `ms`, then whatever follows straight after it.
    COMMTIMEOUTS t{};
    t.ReadIntervalTimeout = MAXDWORD;
    t.ReadTotalTimeoutMultiplier = MAXDWORD;
    t.ReadTotalTimeoutConstant = DWORD(std::max(ms, 1));
    SetCommTimeouts(HANDLE(handle_), &t);
    DWORD got = 0;
    if (!ReadFile(HANDLE(handle_), out, DWORD(max), &got, nullptr)) return -1;
    return int(got);
}

void SerialPort::flushInput() {
    if (handle_) PurgeComm(HANDLE(handle_), PURGE_RXCLEAR);
}

std::vector<std::string> SerialPort::available() {
    std::vector<std::string> out;
    std::vector<char> targets(65536);
    for (int i = 1; i <= 256; ++i) {
        const std::string name = "COM" + std::to_string(i);
        if (QueryDosDeviceA(name.c_str(), targets.data(), DWORD(targets.size())) != 0) out.push_back(name);
    }
    return out;
}

#else

namespace {

speed_t speedOf(int baud) {
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 57600: return B57600;
    case 115200: return B115200;
    default: return B38400;
    }
}

// Raw bytes: no echo, no line editing, no translation, 8 bits with the parity asked.
bool makeRaw(int fd, int baud, char parity) {
    termios t{};
    if (tcgetattr(fd, &t) != 0) return false;
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(CSTOPB | CRTSCTS | PARENB | PARODD);
    if (parity == 'O') t.c_cflag |= PARENB | PARODD;
    if (parity == 'E') t.c_cflag |= PARENB;
    t.c_iflag &= ~(IXON | IXOFF | IXANY | INPCK);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    cfsetispeed(&t, speedOf(baud));
    cfsetospeed(&t, speedOf(baud));
    if (tcsetattr(fd, TCSANOW, &t) == 0) return true;
    // (a pseudo-terminal has no parity to set, and newer kernels refuse it there: a line without one)
    t.c_cflag &= ~(PARENB | PARODD);
    return parity != 'N' && tcsetattr(fd, TCSANOW, &t) == 0;
}

}  // namespace

bool SerialPort::open(const std::string& name, int baud, char parity, std::string* error) {
    close();
    const int fd = ::open(name.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (error) *error = "Cannot open " + name + ": " + std::strerror(errno);
        return false;
    }
    if (!isatty(fd) || !makeRaw(fd, baud, parity)) {
        ::close(fd);
        if (error) *error = name + " is not a serial port";
        return false;
    }
    tcflush(fd, TCIOFLUSH);
    fd_ = fd;
    name_ = name;
    return true;
}

void SerialPort::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

bool SerialPort::isOpen() const { return fd_ >= 0; }

bool SerialPort::write(const uint8_t* data, size_t n) {
    if (fd_ < 0) return false;
    while (n > 0) {
        const ssize_t w = ::write(fd_, data, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                bool ready = false;
                if (waitFds(&fd_, &ready, 1, true, 1000) <= 0) return false;
                continue;
            }
            return false;
        }
        data += w;
        n -= size_t(w);
    }
    return true;
}

int SerialPort::read(uint8_t* out, size_t max, int ms) {
    if (fd_ < 0) return -1;
    bool ready = false;
    const int r = waitFds(&fd_, &ready, 1, false, std::max(ms, 0));
    if (r < 0) return -1;
    if (r == 0) return 0;
    const ssize_t got = ::read(fd_, out, max);
    if (got < 0) return errno == EAGAIN || errno == EINTR ? 0 : -1;
    if (got == 0) return -1;  // (ready with nothing: the line hung up)
    return int(got);
}

void SerialPort::flushInput() {
    if (fd_ >= 0) tcflush(fd_, TCIFLUSH);
}

std::vector<std::string> SerialPort::available() {
    std::vector<std::string> usb, other;
    if (DIR* d = opendir("/dev")) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            auto starts = [&](const char* p) { return n.rfind(p, 0) == 0; };
#ifdef __APPLE__
            if (starts("cu.") && n != "cu.Bluetooth-Incoming-Port") (n.find("usb") != std::string::npos ? usb : other).push_back("/dev/" + n);
#else
            if (starts("ttyUSB") || starts("ttyACM")) usb.push_back("/dev/" + n);
            else if (starts("ttyS") || starts("ttyAMA")) {
                // (the built-in ports Linux always lists are kept only when a device stands behind them)
                struct stat st{};
                const std::string sys = "/sys/class/tty/" + n + "/device/driver";
                if (stat(sys.c_str(), &st) == 0) {
                    char link[256] = {};
                    const ssize_t l = readlink(("/sys/class/tty/" + n + "/device/driver").c_str(), link, sizeof link - 1);
                    if (l <= 0 || std::string(link).find("serial8250") == std::string::npos || n == "ttyS0") other.push_back("/dev/" + n);
                }
            }
#endif
        }
        closedir(d);
    }
    std::sort(usb.begin(), usb.end());
    std::sort(other.begin(), other.end());
    usb.insert(usb.end(), other.begin(), other.end());
    return usb;
}

PseudoCable::PseudoCable() {
    auto make = [](int& master, int& slave, std::string& name) {
        master = posix_openpt(O_RDWR | O_NOCTTY);
        if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) return false;
        const char* n = ptsname(master);
        if (!n) return false;
        name = n;
        slave = ::open(n, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (slave < 0) return false;
        makeRaw(slave, 38400, 'O');
        fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
        return true;
    };
    if (!make(a_, sa_, nameA_) || !make(b_, sb_, nameB_)) {
        for (int* fd : {&a_, &b_, &sa_, &sb_})
            if (*fd >= 0) ::close(*fd), *fd = -1;
        return;
    }
    // What one end writes reaches the other, as down a cable.
    relay_ = std::thread([this] {
        uint8_t buf[4096];
        while (!stop_) {
            const int fds[2] = {a_, b_};
            bool ready[2] = {false, false};
            if (waitFds(fds, ready, 2, false, 20) <= 0) continue;
            for (int i = 0; i < 2; ++i) {
                if (!ready[i]) continue;
                const ssize_t n = ::read(fds[i], buf, sizeof buf);
                for (ssize_t done = 0; n > 0 && done < n && !stop_;) {
                    const ssize_t w = ::write(fds[1 - i], buf + done, size_t(n - done));
                    if (w > 0) done += w;
                    else if (w < 0 && errno != EAGAIN && errno != EINTR) break;
                    else std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }
    });
}

PseudoCable::~PseudoCable() {
    stop_ = true;
    if (relay_.joinable()) relay_.join();
    for (int fd : {sa_, sb_, a_, b_})
        if (fd >= 0) ::close(fd);
}

#endif

}  // namespace montage
