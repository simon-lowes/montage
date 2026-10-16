// Montage — files for bmx by UTF-8 path. libMXF opens files with fopen, which on Windows reads a path in the ANSI
// code page, so a file under C:\Users\José could not be read or written; these open it by its wide name there.
#pragma once

#include <string>

#include <bmx/mxf_helper/MXFFileFactory.h>

namespace montage {

class Utf8MxfFileFactory : public bmx::MXFFileFactory {
public:
    mxfpp::File* OpenNew(std::string filename) override;
    mxfpp::File* OpenRead(std::string filename) override;
    mxfpp::File* OpenModify(std::string filename) override;
};

// A name an fopen-based reader can open an existing file by: on Windows its short (8.3) name when the path is not
// ASCII ("" if it has none), elsewhere the path itself.
std::string fopenPath(const std::string& utf8);

}  // namespace montage
