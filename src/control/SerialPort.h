// Montage — a serial port, as RS-422 deck control runs over (USB RS-422 adapters appear as serial ports: /dev/ttyUSB0
// on Linux, /dev/cu.usbserial-... on macOS, COM3 on Windows), and the pseudo-terminals the tests stand in for the cable
// with. Raw bytes at a set rate and parity, read with a time limit; POSIX termios or the Win32 communications API.
#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace montage {

class SerialPort {
public:
    SerialPort() = default;
    ~SerialPort();
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    // `parity`: 'N', 'O' or 'E'; one stop bit, 8 data bits, no flow control. Sony's 9-pin runs at 38400 baud, odd
    // parity. A device that is not a serial line (a file) is refused.
    bool open(const std::string& name, int baud, char parity, std::string* error = nullptr);
    void close();
    bool isOpen() const;
    const std::string& name() const { return name_; }

    // All of `data`, or false (the port gone).
    bool write(const uint8_t* data, size_t n);
    bool write(const std::vector<uint8_t>& data) { return write(data.data(), data.size()); }
    // What has arrived, waiting up to `ms` for its first byte: the bytes read (0 when nothing came), or -1 when the
    // port failed (an adapter pulled out).
    int read(uint8_t* out, size_t max, int ms);
    // Throws away anything not yet read.
    void flushInput();

    // The serial ports on this computer, by the names open() takes (USB adapters first).
    static std::vector<std::string> available();

private:
    std::string name_;
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

#ifndef _WIN32
// A pseudo-terminal pair (POSIX only), standing for a cable in tests: the two ends' names, opened by SerialPort like
// any port. Closed with the object.
class PseudoCable {
public:
    PseudoCable();
    ~PseudoCable();
    bool ok() const { return a_ >= 0 && b_ >= 0; }
    std::string endA() const { return nameA_; }
    std::string endB() const { return nameB_; }

private:
    int a_ = -1, b_ = -1;  // the masters, held so the ends stay up
    int sa_ = -1, sb_ = -1;  // a slave of each kept open, so a reopen finds the line as set
    std::atomic<bool> stop_{false};
    std::string nameA_, nameB_;
    std::thread relay_;  // copies bytes between the two masters
};
#endif

}  // namespace montage
