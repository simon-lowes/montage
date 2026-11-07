#include "PluginIpc.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <semaphore.h>
#else
#include <climits>
#include <cstdio>
#include <linux/futex.h>
#include <sys/syscall.h>
#endif
extern char** environ;
#endif

namespace montage::plugins::ipc {

int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int64_t ownPid() {
#ifdef _WIN32
    return int64_t(GetCurrentProcessId());
#else
    return int64_t(getpid());
#endif
}

std::string uniqueName(const char* tag) {
    static std::atomic<uint32_t> counter{0};
    const uint32_t n = ++counter;
    const uint32_t salt = uint32_t(nowUs() & 0xffff);
    char buf[48];
    std::snprintf(buf, sizeof buf, "/mph%s%llx-%x%x", tag, (unsigned long long)ownPid(), n, salt);
    return buf;
}

namespace {
inline void cpuPause() {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64)
#if defined(_MSC_VER)
    YieldProcessor();
#else
    __builtin_ia32_pause();
#endif
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}
}  // namespace

// ---------------------------------------------------------------------------
// Shared memory

#ifdef _WIN32
namespace {
std::wstring wideName(const std::string& name) {
    // "Local\" keeps the objects to this session; the leading slash of a POSIX name is dropped.
    std::string n = "Local\\" + (name.size() > 1 && name[0] == '/' ? name.substr(1) : name);
    return std::wstring(n.begin(), n.end());
}
}  // namespace

std::unique_ptr<SharedMemory> SharedMemory::create(const std::string& name, size_t size, std::string* error) {
    std::unique_ptr<SharedMemory> m(new SharedMemory);
    const uint64_t s = size;
    m->handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(s >> 32), DWORD(s & 0xffffffffu),
                                    wideName(name).c_str());
    if (!m->handle_) {
        if (error) *error = "cannot create shared memory (" + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    m->data_ = MapViewOfFile(m->handle_, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!m->data_) {
        if (error) *error = "cannot map shared memory (" + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    m->size_ = size;
    m->name_ = name;
    m->owner_ = true;
    return m;
}

std::unique_ptr<SharedMemory> SharedMemory::open(const std::string& name, size_t size, std::string* error) {
    std::unique_ptr<SharedMemory> m(new SharedMemory);
    m->handle_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wideName(name).c_str());
    if (!m->handle_) {
        if (error) *error = "cannot open shared memory " + name;
        return nullptr;
    }
    m->data_ = MapViewOfFile(m->handle_, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!m->data_) {
        if (error) *error = "cannot map shared memory " + name;
        return nullptr;
    }
    m->size_ = size;
    m->name_ = name;
    return m;
}

SharedMemory::~SharedMemory() {
    if (data_) UnmapViewOfFile(data_);
    if (handle_) CloseHandle(handle_);
}

void SharedMemory::unlinkName() {}  // (a mapping's name goes with its last handle)
#else
std::unique_ptr<SharedMemory> SharedMemory::create(const std::string& name, size_t size, std::string* error) {
    const int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        if (error) *error = "cannot create shared memory: " + std::string(std::strerror(errno));
        return nullptr;
    }
    if (ftruncate(fd, off_t(size)) != 0) {
        if (error) *error = "cannot size shared memory: " + std::string(std::strerror(errno));
        close(fd);
        shm_unlink(name.c_str());
        return nullptr;
    }
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        if (error) *error = "cannot map shared memory: " + std::string(std::strerror(errno));
        shm_unlink(name.c_str());
        return nullptr;
    }
    std::unique_ptr<SharedMemory> m(new SharedMemory);
    m->data_ = p;
    m->size_ = size;
    m->name_ = name;
    m->owner_ = true;
    return m;
}

std::unique_ptr<SharedMemory> SharedMemory::open(const std::string& name, size_t size, std::string* error) {
    const int fd = shm_open(name.c_str(), O_RDWR, 0600);
    if (fd < 0) {
        if (error) *error = "cannot open shared memory " + name + ": " + std::string(std::strerror(errno));
        return nullptr;
    }
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        if (error) *error = "cannot map shared memory " + name;
        return nullptr;
    }
    std::unique_ptr<SharedMemory> m(new SharedMemory);
    m->data_ = p;
    m->size_ = size;
    m->name_ = name;
    return m;
}

SharedMemory::~SharedMemory() {
    if (data_) munmap(data_, size_);
    if (owner_) shm_unlink(name_.c_str());  // (harmless when already unlinked)
}

void SharedMemory::unlinkName() {
    if (!name_.empty()) shm_unlink(name_.c_str());
    owner_ = false;
}
#endif

// ---------------------------------------------------------------------------
// Wake-ups

namespace {
// Spins this long before sleeping: a reply that comes back within it costs no system call.
constexpr int kSpins = 4000;
}

#if defined(_WIN32)
std::unique_ptr<Bell> Bell::create(const std::string& name, std::string* error) {
    std::unique_ptr<Bell> b(new Bell);
    b->event_ = CreateEventW(nullptr, FALSE, FALSE, wideName(name + "e").c_str());
    if (!b->event_) {
        if (error) *error = "cannot create an event";
        return nullptr;
    }
    b->name_ = name;
    b->owner_ = true;
    return b;
}
std::unique_ptr<Bell> Bell::open(const std::string& name, std::string* error) {
    std::unique_ptr<Bell> b(new Bell);
    b->event_ = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, wideName(name + "e").c_str());
    if (!b->event_) {
        if (error) *error = "cannot open the event " + name;
        return nullptr;
    }
    b->name_ = name;
    return b;
}
Bell::~Bell() {
    if (event_) CloseHandle(event_);
}
void Bell::unlinkName() {}
void Bell::ring(std::atomic<uint32_t>*) { SetEvent(event_); }
bool Bell::wait(std::atomic<uint32_t>* word, uint32_t old, int64_t timeoutUs) {
    for (int i = 0; i < kSpins; ++i) {
        if (word->load(std::memory_order_acquire) != old) return true;
        cpuPause();
    }
    const int64_t end = nowUs() + timeoutUs;
    for (;;) {
        if (word->load(std::memory_order_acquire) != old) return true;
        const int64_t left = end - nowUs();
        if (left <= 0) return false;
        WaitForSingleObject(event_, DWORD(std::max<int64_t>(1, (left + 999) / 1000)));
    }
}
#elif defined(__APPLE__)
// macOS has no timed wait on a named semaphore: try it, and sleep in short, growing steps in between.
std::unique_ptr<Bell> Bell::create(const std::string& name, std::string* error) {
    const std::string n = name + "s";
    sem_unlink(n.c_str());
    sem_t* s = sem_open(n.c_str(), O_CREAT | O_EXCL, 0600, 0);
    if (s == SEM_FAILED) {
        if (error) *error = "cannot create a semaphore: " + std::string(std::strerror(errno));
        return nullptr;
    }
    std::unique_ptr<Bell> b(new Bell);
    b->sem_ = s;
    b->name_ = n;
    b->owner_ = true;
    return b;
}
std::unique_ptr<Bell> Bell::open(const std::string& name, std::string* error) {
    const std::string n = name + "s";
    sem_t* s = sem_open(n.c_str(), 0);
    if (s == SEM_FAILED) {
        if (error) *error = "cannot open the semaphore " + n;
        return nullptr;
    }
    std::unique_ptr<Bell> b(new Bell);
    b->sem_ = s;
    b->name_ = n;
    return b;
}
Bell::~Bell() {
    if (sem_) sem_close(static_cast<sem_t*>(sem_));
    if (owner_) sem_unlink(name_.c_str());
}
void Bell::unlinkName() {
    if (owner_) sem_unlink(name_.c_str());
    owner_ = false;
}
void Bell::ring(std::atomic<uint32_t>*) { sem_post(static_cast<sem_t*>(sem_)); }
bool Bell::wait(std::atomic<uint32_t>* word, uint32_t old, int64_t timeoutUs) {
    for (int i = 0; i < kSpins; ++i) {
        if (word->load(std::memory_order_acquire) != old) return true;
        cpuPause();
    }
    const int64_t end = nowUs() + timeoutUs;
    int64_t step = 20;
    for (;;) {
        if (word->load(std::memory_order_acquire) != old) return true;
        if (sem_trywait(static_cast<sem_t*>(sem_)) == 0) continue;  // rung: look again
        const int64_t left = end - nowUs();
        if (left <= 0) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(std::min(step, left)));
        step = std::min<int64_t>(step * 2, 500);
    }
}
#else
namespace {
long futex(std::atomic<uint32_t>* word, int op, uint32_t val, const timespec* ts) {
    // (no FUTEX_PRIVATE_FLAG: the word is shared with another process)
    return syscall(SYS_futex, reinterpret_cast<uint32_t*>(word), op, val, ts, nullptr, 0);
}
}  // namespace
std::unique_ptr<Bell> Bell::create(const std::string& name, std::string*) {
    std::unique_ptr<Bell> b(new Bell);
    b->name_ = name;
    return b;
}
std::unique_ptr<Bell> Bell::open(const std::string& name, std::string*) {
    std::unique_ptr<Bell> b(new Bell);
    b->name_ = name;
    return b;
}
Bell::~Bell() = default;
void Bell::unlinkName() {}
void Bell::ring(std::atomic<uint32_t>* word) { futex(word, FUTEX_WAKE, INT_MAX, nullptr); }
bool Bell::wait(std::atomic<uint32_t>* word, uint32_t old, int64_t timeoutUs) {
    for (int i = 0; i < kSpins; ++i) {
        if (word->load(std::memory_order_acquire) != old) return true;
        cpuPause();
    }
    const int64_t end = nowUs() + timeoutUs;
    for (;;) {
        if (word->load(std::memory_order_acquire) != old) return true;
        const int64_t left = end - nowUs();
        if (left <= 0) return false;
        timespec ts{time_t(left / 1000000), long(left % 1000000) * 1000};
        futex(word, FUTEX_WAIT, old, &ts);  // (returns at once when the word already changed)
    }
}
#endif

// ---------------------------------------------------------------------------
// Child processes

#ifdef _WIN32
namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
// One argument quoted as CommandLineToArgvW reads it back.
void appendQuoted(std::wstring& cmd, const std::wstring& arg) {
    if (!cmd.empty()) cmd += L' ';
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') cmd.append(backslashes * 2 + 1, L'\\');
        else cmd.append(backslashes, L'\\');
        backslashes = 0;
        cmd += c;
    }
    cmd.append(backslashes * 2, L'\\');
    cmd += L'"';
}
// Every host goes in one job that ends them all when Montage ends, however it ends.
HANDLE hostJob() {
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        if (j) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
            SetInformationJobObject(j, JobObjectExtendedLimitInformation, &info, sizeof info);
        }
        return j;
    }();
    return job;
}
}  // namespace

std::unique_ptr<Process> Process::spawn(const std::string& exe, const std::vector<std::string>& args, std::string* error) {
    std::wstring cmd;
    appendQuoted(cmd, widen(exe));
    for (const std::string& a : args) appendQuoted(cmd, widen(a));
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    // (no crash dialog for the host: a plug-in that crashes ends it, and Montage carries on)
    const UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const BOOL ok = CreateProcessW(widen(exe).c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                   nullptr, nullptr, &si, &pi);
    SetErrorMode(oldMode);
    if (!ok) {
        if (error) *error = "cannot start " + exe + " (" + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    if (HANDLE job = hostJob()) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    std::unique_ptr<Process> p(new Process);
    p->handle_ = pi.hProcess;
    p->pid_ = int64_t(pi.dwProcessId);
    return p;
}

Process::~Process() {
    kill();
    if (handle_) CloseHandle(handle_);
}

bool Process::alive() {
    if (state_.load(std::memory_order_acquire) == 1) return false;
    std::lock_guard lock(reapM_);
    if (state_.load(std::memory_order_acquire) == 1) return false;
    if (WaitForSingleObject(handle_, 0) != WAIT_OBJECT_0) return true;
    DWORD code = 0;
    GetExitCodeProcess(handle_, &code);
    exitCode_ = int(code);
    state_.store(1, std::memory_order_release);
    return false;
}

void Process::kill() {
    if (handle_ && alive()) {
        TerminateProcess(handle_, 9);
        WaitForSingleObject(handle_, 2000);
        alive();
    }
}

bool processRunning(int64_t pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!h) return false;
    const bool running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return running;
}

bool killProcess(int64_t pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(pid));
    if (!h) return false;
    const bool ok = TerminateProcess(h, 9) != 0;
    CloseHandle(h);
    return ok;
}

int64_t ownResidentBytes() {
    // (K32GetProcessMemoryInfo, looked up so no import library is needed)
    struct Counters {
        DWORD cb, PageFaultCount;
        SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage,
            QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage;
    } c{};
    c.cb = sizeof c;
    using Fn = BOOL(WINAPI*)(HANDLE, void*, DWORD);
    static Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo")));
    if (!fn || !fn(GetCurrentProcess(), &c, sizeof c)) return -1;
    return int64_t(c.WorkingSetSize);
}
#else
std::unique_ptr<Process> Process::spawn(const std::string& exe, const std::vector<std::string>& args, std::string* error) {
    std::vector<std::string> all{exe};
    all.insert(all.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& a : all) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    // Default signal handling and no blocked signals in the host, whatever this thread has.
    sigset_t none, all_signals;
    sigemptyset(&none);
    sigfillset(&all_signals);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all_signals);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, exe.c_str(), nullptr, &attr, argv.data(), environ);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        if (error) *error = "cannot start " + exe + ": " + std::strerror(rc);
        return nullptr;
    }
    std::unique_ptr<Process> p(new Process);
    p->pid_ = pid;
    return p;
}

Process::~Process() { kill(); }

bool Process::alive() {
    if (state_.load(std::memory_order_acquire) == 1) return false;
    std::lock_guard lock(reapM_);
    if (state_.load(std::memory_order_acquire) == 1) return false;
    int status = 0;
    const pid_t r = waitpid(pid_t(pid_), &status, WNOHANG);
    if (r == 0) return true;
    if (r == pid_t(pid_)) {
        if (WIFEXITED(status)) exitCode_ = WEXITSTATUS(status);
        if (WIFSIGNALED(status)) signal_ = WTERMSIG(status);
    }
    // (another thread reaped it first: ECHILD, ended all the same)
    state_.store(1, std::memory_order_release);
    return false;
}

void Process::kill() {
    if (pid_ <= 0 || !alive()) return;
    ::kill(pid_t(pid_), SIGKILL);
    for (int i = 0; i < 400 && alive(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

bool processRunning(int64_t pid) { return ::kill(pid_t(pid), 0) == 0 || errno == EPERM; }

bool killProcess(int64_t pid) { return pid > 0 && ::kill(pid_t(pid), SIGKILL) == 0; }

int64_t ownResidentBytes() {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return -1;
    return int64_t(info.resident_size);
#else
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return -1;
    long size = 0, resident = 0;
    const int n = std::fscanf(f, "%ld %ld", &size, &resident);
    std::fclose(f);
    return n == 2 ? int64_t(resident) * int64_t(sysconf(_SC_PAGESIZE)) : -1;
#endif
}
#endif

std::string Process::howItEnded() {
    if (alive()) return "still running";
    if (signal_) return "ended by signal " + std::to_string(signal_);
    if (exitCode_ >= 0) return "exited with code " + std::to_string(exitCode_);
    return "ended";
}

int Process::exitCode() { return alive() ? -1 : exitCode_; }

}  // namespace montage::plugins::ipc
