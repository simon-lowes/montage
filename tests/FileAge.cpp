// Montage tests — setting a file's or a folder's modification time back, for tests of what is pruned by age.
// (std::filesystem::last_write_time cannot set a folder's on Windows: its runtime opens it without
// FILE_FLAG_BACKUP_SEMANTICS. Kept apart from the tests so <windows.h>'s macros stay out of them.)
#include <QDir>
#include <QString>

#include <chrono>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

bool makeOlder(const QString& path, int hours) {
#ifdef _WIN32
    HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER t;
    t.LowPart = now.dwLowDateTime;
    t.HighPart = now.dwHighDateTime;
    t.QuadPart -= ULONGLONG(hours) * 3600 * 10000000;  // (100 ns units)
    FILETIME then;
    then.dwLowDateTime = t.LowPart;
    then.dwHighDateTime = t.HighPart;
    const bool ok = SetFileTime(h, nullptr, nullptr, &then) != 0;
    CloseHandle(h);
    return ok;
#else
    std::error_code ec;
    std::filesystem::last_write_time(std::filesystem::path(path.toStdString()),
                                     std::filesystem::file_time_type::clock::now() - std::chrono::hours(hours), ec);
    return !ec;
#endif
}
