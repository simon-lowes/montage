#include "CameraRaw.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>

#ifdef MONTAGE_WITH_LIBRAW
#include <libraw/libraw.h>
#endif

namespace montage {

bool rawAvailable() {
#ifdef MONTAGE_WITH_LIBRAW
    return true;
#else
    return false;
#endif
}

bool isRawPath(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    static const char* const kExts[] = {"cr2", "cr3", "crw", "nef", "nrw", "arw", "srf", "sr2", "raf", "rw2", "orf", "pef",
                                        "dng", "srw", "3fr", "fff", "iiq", "mos", "mef", "mrw", "x3f", "erf", "kdc", "rwl", "raw"};
    return std::any_of(std::begin(kExts), std::end(kExts), [&](const char* e) { return ext == e; });
}

#ifdef MONTAGE_WITH_LIBRAW
namespace {

std::unique_ptr<LibRaw> openRaw(const std::string& path, std::string* error) {
    auto raw = std::make_unique<LibRaw>();
    // As most raw developers do: the camera's white balance, AHD demosaicing,
    // Rec.709 primaries and curve (LibRaw's sRGB output with its default BT.709 gamma), 16 bits.
    auto& p = raw->imgdata.params;
    p.use_camera_wb = 1;
    p.use_camera_matrix = 1;
    p.output_color = 1;
    p.output_bps = 16;
    p.user_qual = 3;
    const int rc = raw->open_file(path.c_str());
    if (rc != LIBRAW_SUCCESS) {
        if (error) *error = "Cannot read " + path + " as a camera raw file: " + libraw_strerror(rc);
        return nullptr;
    }
    return raw;
}

std::string cameraName(const LibRaw& raw) {
    const auto& id = raw.imgdata.idata;
    std::string make = id.make, model = id.model;
    // Most models already start with the maker's name.
    if (!make.empty() && model.rfind(make, 0) != 0) return make + " " + model;
    return model.empty() ? make : model;
}

}  // namespace
#endif

bool probeRaw(const std::string& path, RawInfo& info, std::string* error) {
#ifdef MONTAGE_WITH_LIBRAW
    auto raw = openRaw(path, error);
    if (!raw) return false;
    raw->adjust_sizes_info_only();  // the developed size, turned upright
    info.width = raw->imgdata.sizes.iwidth;
    info.height = raw->imgdata.sizes.iheight;
    info.camera = cameraName(*raw);
    if (info.width <= 0 || info.height <= 0) {
        if (error) *error = path + " has no picture LibRaw can develop";
        return false;
    }
    return true;
#else
    if (error) *error = "This build of Montage cannot read camera raw files (built without LibRaw)";
    (void)path, (void)info;
    return false;
#endif
}

bool developRaw(const std::string& path, RawImage& out, std::string* error) {
#ifdef MONTAGE_WITH_LIBRAW
    auto raw = openRaw(path, error);
    if (!raw) return false;
    int rc = raw->unpack();
    if (rc == LIBRAW_SUCCESS) rc = raw->dcraw_process();
    if (rc != LIBRAW_SUCCESS) {
        if (error) *error = "Cannot develop " + path + ": " + libraw_strerror(rc);
        return false;
    }
    libraw_processed_image_t* img = raw->dcraw_make_mem_image(&rc);
    if (!img || rc != LIBRAW_SUCCESS) {
        if (error) *error = "Cannot develop " + path + ": " + libraw_strerror(rc);
        if (img) LibRaw::dcraw_clear_mem(img);
        return false;
    }
    std::unique_ptr<libraw_processed_image_t, void (*)(libraw_processed_image_t*)> hold(img, LibRaw::dcraw_clear_mem);
    if (img->type != LIBRAW_IMAGE_BITMAP || img->bits != 16 || (img->colors != 3 && img->colors != 1)) {
        if (error) *error = "Unexpected developed image from " + path;
        return false;
    }
    out.width = img->width;
    out.height = img->height;
    out.rgb.resize(size_t(out.width) * size_t(out.height) * 3);
    const auto* src = reinterpret_cast<const uint16_t*>(img->data);
    if (img->colors == 3) std::memcpy(out.rgb.data(), src, out.rgb.size() * sizeof(uint16_t));
    else
        for (size_t i = 0; i < size_t(out.width) * size_t(out.height); ++i) out.rgb[i * 3] = out.rgb[i * 3 + 1] = out.rgb[i * 3 + 2] = src[i];
    return true;
#else
    if (error) *error = "This build of Montage cannot read camera raw files (built without LibRaw)";
    (void)path, (void)out;
    return false;
#endif
}

}  // namespace montage
