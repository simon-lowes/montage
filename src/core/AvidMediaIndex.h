// Montage — finding the media an interchange file names (internal to the AAF and Media Composer bin readers): a
// file where its locator says or beside the file naming it, and Media Composer's own MXF media by their package IDs.
#pragma once

#include <QStringList>
#include <map>
#include <string>
#include <vector>

namespace montage {

// The file a URL or path names, if it can be found: where it says, else beside `dir` or in a folder next to it; else
// the path as given.
std::string findInterchangeFile(const std::string& url, const std::string& dir);

// The package IDs an MXF file's header names (each package's PackageUID, local tag 4401h), as mob IDs: read from the
// start of the file, not decoded.
std::vector<std::string> mxfPackageIds(const std::string& path);

// Media Composer's own media: MXF files in the numbered folders of a drive's "Avid MediaFiles/MXF", found by their
// package IDs (a file mob's ID is its file's), as Media Composer finds them. Looked for, when a file is not where its
// locator says, first at the same place under each "Avid MediaFiles" (a drive moved or mounted elsewhere), then among
// all the files of those beside and above `dir` and at the top of each local drive.
class AvidMediaIndex {
public:
    explicit AvidMediaIndex(const std::string& dir) : dir_(dir) {}
    // The file of mob ID `mobId` (an AAF mob ID's 32 bytes), its locator saying `path`; "" if none is found.
    std::string find(const std::string& mobId, const std::string& path);

private:
    void findRoots();
    void scan();
    std::string dir_;
    QStringList roots_;
    bool scanned_ = false;
    std::map<std::string, std::string> files_;
};

}  // namespace montage
