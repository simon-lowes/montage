// Montage — the pieces plug-in host processes are built from (audio/PluginHost.h): memory shared between two
// processes by name, a wake-up on a 32-bit word in that memory, and a child process watched without blocking.
//
// Shared memory is POSIX shm_open and mmap on Linux and macOS, and a named file mapping on Windows. A wake-up is a
// futex on the word itself on Linux; a named semaphore on macOS and a named event on Windows ring it. Either way the
// word is the truth (a wake-up only says "look again"), so a late or doubled ring is harmless. Ringing and waiting
// neither allocate nor take locks, so the audio thread can use them.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace montage::plugins::ipc {

// Microseconds on a steady clock.
int64_t nowUs();
// A name for shared objects, unique on this computer while this process lives (short: macOS allows 31 characters).
std::string uniqueName(const char* tag);

class SharedMemory {
public:
    // A new block of `size` bytes (zeroed) under `name`, or null with why.
    static std::unique_ptr<SharedMemory> create(const std::string& name, size_t size, std::string* error = nullptr);
    // The block another process made.
    static std::unique_ptr<SharedMemory> open(const std::string& name, size_t size, std::string* error = nullptr);
    ~SharedMemory();
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;
    void* data() const { return data_; }
    size_t size() const { return size_; }
    const std::string& name() const { return name_; }
    // Takes the name away once the other side has opened it (POSIX; the memory stays while mapped), so nothing is
    // left behind if either side then crashes.
    void unlinkName();

private:
    SharedMemory() = default;
    void* data_ = nullptr;
    size_t size_ = 0;
    std::string name_;
    bool owner_ = false;
#ifdef _WIN32
    void* handle_ = nullptr;
#endif
};

// Wakes the other process when a word in shared memory changes.
class Bell {
public:
    static std::unique_ptr<Bell> create(const std::string& name, std::string* error = nullptr);
    static std::unique_ptr<Bell> open(const std::string& name, std::string* error = nullptr);
    ~Bell();
    Bell(const Bell&) = delete;
    Bell& operator=(const Bell&) = delete;
    void ring(std::atomic<uint32_t>* word);
    // Waits until *word is no longer `old` (true) or `timeoutUs` passes (false). Spins briefly first.
    bool wait(std::atomic<uint32_t>* word, uint32_t old, int64_t timeoutUs);
    void unlinkName();

private:
    Bell() = default;
    std::string name_;
    bool owner_ = false;
#if defined(_WIN32)
    void* event_ = nullptr;
#elif defined(__APPLE__)
    void* sem_ = nullptr;
#endif
};

// A child process. alive() and residentBytes() do not block.
class Process {
public:
    static std::unique_ptr<Process> spawn(const std::string& exe, const std::vector<std::string>& args, std::string* error = nullptr);
    ~Process();  // kills it if it still runs
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    bool alive();
    // How it ended, once it has: "exited with code N", "ended by signal N".
    std::string howItEnded();
    int exitCode();  // -1 while it runs or when ended by a signal
    void kill();
    int64_t pid() const { return pid_; }

private:
    Process() = default;
    int64_t pid_ = 0;
    std::atomic<int> state_{0};  // 0 running, 1 ended
    std::mutex reapM_;           // (one thread reaps it and writes how it ended; the others wait for that)
    int exitCode_ = -1, signal_ = 0;
#ifdef _WIN32
    void* handle_ = nullptr;
#endif
};

// Whether the process with this id still runs (used by a host to follow Montage: it ends when Montage does).
bool processRunning(int64_t pid);
// Ends a process at once (as a crash would end it).
bool killProcess(int64_t pid);
// The calling process's resident memory, bytes (-1 if unknown).
int64_t ownResidentBytes();
int64_t ownPid();

}  // namespace montage::plugins::ipc
