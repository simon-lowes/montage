#include "MxfFiles.h"

#include <bmx/BMXException.h>
#include <libMXF++/File.h>

#ifdef _WIN32
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace montage {

#ifdef _WIN32
namespace {

std::wstring widePath(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), w.data(), n);
    return w;
}

// libMXF's disk file, on a stream _wfopen opened. The stream itself is the file's private data.
FILE* stream(MXFFileSysData* d) { return reinterpret_cast<FILE*>(d); }

mxfpp::File* openWide(const std::string& path, const wchar_t* mode) {
    FILE* f = _wfopen(widePath(path).c_str(), mode);
    if (!f) throw bmx::BMXException("Failed to open MXF file '%s'", path.c_str());
    auto* m = static_cast<MXFFile*>(calloc(1, sizeof(MXFFile)));  // libMXF frees it with free()
    if (!m) {
        fclose(f);
        throw bmx::BMXException("Out of memory opening '%s'", path.c_str());
    }
    m->close = [](MXFFileSysData* d) { fclose(stream(d)); };
    m->read = [](MXFFileSysData* d, uint8_t* data, uint32_t count) { return data ? uint32_t(fread(data, 1, count, stream(d))) : 0u; };
    m->write = [](MXFFileSysData* d, const uint8_t* data, uint32_t count) { return data ? uint32_t(fwrite(data, 1, count, stream(d))) : 0u; };
    m->get_char = [](MXFFileSysData* d) { return fgetc(stream(d)); };
    m->put_char = [](MXFFileSysData* d, int c) { return fputc(c, stream(d)); };
    m->eof = [](MXFFileSysData* d) { return feof(stream(d)); };
    m->seek = [](MXFFileSysData* d, int64_t offset, int whence) { return int(_fseeki64(stream(d), offset, whence) == 0); };
    m->tell = [](MXFFileSysData* d) { return int64_t(_ftelli64(stream(d))); };
    m->is_seekable = [](MXFFileSysData*) { return 1; };
    m->size = [](MXFFileSysData* d) -> int64_t {
        fflush(stream(d));  // the descriptor's size leaves out what the stream still buffers
        struct _stati64 st;
        return _fstati64(_fileno(stream(d)), &st) == 0 ? int64_t(st.st_size) : -1;
    };
    m->free_sys_data = nullptr;
    m->sysData = reinterpret_cast<MXFFileSysData*>(f);
    return new mxfpp::File(m);
}

}  // namespace

mxfpp::File* Utf8MxfFileFactory::OpenNew(std::string filename) { return openWide(filename, L"w+b"); }
mxfpp::File* Utf8MxfFileFactory::OpenRead(std::string filename) { return openWide(filename, L"rb"); }
mxfpp::File* Utf8MxfFileFactory::OpenModify(std::string filename) { return openWide(filename, L"r+b"); }

std::string fopenPath(const std::string& utf8) {
    bool ascii = true;
    for (unsigned char c : utf8) ascii = ascii && c < 0x80;
    if (ascii) return utf8;
    const std::wstring w = widePath(utf8);
    const DWORD n = GetShortPathNameW(w.c_str(), nullptr, 0);
    if (!n) return {};
    std::wstring s(n, L'\0');
    const DWORD got = GetShortPathNameW(w.c_str(), s.data(), n);
    if (!got || got >= n) return {};
    s.resize(got);
    std::string out;
    for (wchar_t c : s) {
        if (c >= 0x80) return {};  // the volume keeps no short names
        out += char(c);
    }
    return out;
}
#else
mxfpp::File* Utf8MxfFileFactory::OpenNew(std::string filename) { return mxfpp::File::openNew(filename); }
mxfpp::File* Utf8MxfFileFactory::OpenRead(std::string filename) { return mxfpp::File::openRead(filename); }
mxfpp::File* Utf8MxfFileFactory::OpenModify(std::string filename) { return mxfpp::File::openModify(filename); }

std::string fopenPath(const std::string& utf8) { return utf8; }
#endif

}  // namespace montage
