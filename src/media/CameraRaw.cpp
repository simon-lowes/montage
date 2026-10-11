#include "CameraRaw.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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
    // Rec.709 primaries and curve (LibRaw's sRGB output with its default BT.709 gamma), 16 bits, at the exposure the
    // camera recorded: no automatic brightening, which would differ from frame to frame of a CinemaDNG run.
    auto& p = raw->imgdata.params;
    p.no_auto_bright = 1;
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


// XYZ to the camera's raw values: LibRaw's matrix for the model, else the DNG's ColorMatrix (the one for daylight when
// there are two), else worked back from its camera to sRGB matrix and the daylight multipliers.
bool cameraFromXyz(const LibRaw& raw, float out[3][3]) {
    auto nonzero = [](const float m[][3]) {
        double sum = 0;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) sum += std::fabs(m[r][c]);
        return sum > 1e-6;
    };
    auto copy = [&](const float m[][3]) {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) out[r][c] = m[r][c];
        return true;
    };
    const auto& col = raw.imgdata.color;
    if (nonzero(col.cam_xyz)) return copy(col.cam_xyz);
    const libraw_dng_color_t* pick = nullptr;
    for (const auto& d : col.dng_color)
        if (nonzero(d.colormatrix) && (!pick || d.illuminant == 21 || d.illuminant == 23)) pick = &d;  // D65, D50
    if (pick) return copy(pick->colormatrix);
    // rgb_cam takes white-balanced camera values to sRGB: camera = diag(1 / pre_mul) * inverse(rgb_cam) * sRGB-from-XYZ.
    double m[3][3], inv[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) m[r][c] = col.rgb_cam[r][c];
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (std::fabs(det) < 1e-9 || col.pre_mul[0] <= 0 || col.pre_mul[1] <= 0 || col.pre_mul[2] <= 0) return false;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            inv[c][r] = (m[(r + 1) % 3][(c + 1) % 3] * m[(r + 2) % 3][(c + 2) % 3] - m[(r + 1) % 3][(c + 2) % 3] * m[(r + 2) % 3][(c + 1) % 3]) / det;
    static const double srgbFromXyz[3][3] = {{3.2406, -1.5372, -0.4986}, {-0.9689, 1.8758, 0.0415}, {0.0557, -0.2040, 1.0570}};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            double v = 0;
            for (int k = 0; k < 3; ++k) v += inv[r][k] * srgbFromXyz[k][c];
            out[r][c] = float(v / col.pre_mul[r]);
        }
    return true;
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

bool whiteBalanceMultipliers(const float camFromXyz[3][3], double kelvin, double tint, float out[3]) {
    // The light's chromaticity on the Planckian locus (Kim et al.'s cubic fit, 1667 K to 25000 K).
    const double t = std::clamp(kelvin, 1667.0, 25000.0);
    const double x = t <= 4000 ? -0.2661239e9 / (t * t * t) - 0.2343589e6 / (t * t) + 0.8776956e3 / t + 0.179910
                               : -3.0258469e9 / (t * t * t) + 2.1070379e6 / (t * t) + 0.2226347e3 / t + 0.240390;
    const double y = t <= 2222   ? -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683
                     : t <= 4000 ? -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867
                                 : 3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x - 0.37001483;
    const double xyz[3] = {x / y, 1.0, (1 - x - y) / y};
    double cam[3];
    for (int r = 0; r < 3; ++r) cam[r] = camFromXyz[r][0] * xyz[0] + camFromXyz[r][1] * xyz[1] + camFromXyz[r][2] * xyz[2];
    if (cam[0] <= 1e-6 || cam[1] <= 1e-6 || cam[2] <= 1e-6) return false;
    // What makes that light grey, with green kept at 1; the tint takes green down (magenta) or up (green).
    out[0] = float(cam[1] / cam[0]);
    out[1] = float(std::pow(2.0, -tint / 150.0));
    out[2] = float(cam[1] / cam[2]);
    return true;
}

bool developRaw(const std::string& path, RawImage& out, std::string* error, const RawSettings& settings) {
#ifdef MONTAGE_WITH_LIBRAW
    auto raw = openRaw(path, error);
    if (!raw) return false;
    auto& p = raw->imgdata.params;
    if (settings.exposure != 0) {
        // LibRaw's own exposure shift reaches -2 to +3 stops (rolling brightened highlights off rather than clipping
        // them); the rest is a straight gain before the output curve.
        const double gain = std::pow(2.0, settings.exposure);
        const double shift = std::clamp(gain, 0.25, 8.0);
        p.exp_correc = 1;
        p.exp_shift = float(shift);
        p.exp_preser = settings.exposure > 0 ? 0.5f : 0.0f;
        p.bright = float(gain / shift);
    }
    if (settings.temperature > 0) {
        float camFromXyz[3][3];
        float mul[3];
        if (cameraFromXyz(*raw, camFromXyz) && whiteBalanceMultipliers(camFromXyz, settings.temperature, settings.tint, mul)) {
            p.use_camera_wb = 0;
            p.use_auto_wb = 0;
            p.user_mul[0] = mul[0], p.user_mul[1] = mul[1], p.user_mul[2] = mul[2], p.user_mul[3] = mul[1];
        }
    }
    if (settings.temperature <= 0 && settings.tint != 0) {
        // A tint on the camera's own white balance.
        const float* as = raw->imgdata.color.cam_mul[1] > 0 ? raw->imgdata.color.cam_mul : raw->imgdata.color.pre_mul;
        if (as[1] > 0) {
            p.use_camera_wb = 0;
            p.use_auto_wb = 0;
            const float g = as[1] * float(std::pow(2.0, -settings.tint / 150.0));
            p.user_mul[0] = as[0] / as[1], p.user_mul[1] = g / as[1], p.user_mul[2] = as[2] / as[1], p.user_mul[3] = g / as[1];
        }
    }
    p.highlight = settings.highlights == 1 ? 2 : settings.highlights == 2 ? 5 : 0;
    if (p.highlight != 0) {
        // To keep highlights LibRaw scales by the largest white balance multiplier instead of the smallest, which
        // darkens the whole picture by their ratio (a stop or more); that is given back so the exposure holds.
        const auto& col = raw->imgdata.color;
        const float* mul = p.user_mul[0] > 0 ? p.user_mul : p.use_camera_wb && col.cam_mul[0] > 0 && col.cam_mul[1] > 0 ? col.cam_mul : col.pre_mul;
        float lo = 0, hi = 0;
        for (int c = 0; c < 4; ++c) {
            if (mul[c] <= 0) continue;
            lo = lo > 0 ? std::min(lo, mul[c]) : mul[c];
            hi = std::max(hi, mul[c]);
        }
        if (lo > 0) p.bright *= hi / lo;
    }
    if (settings.half) p.half_size = 1;
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
    (void)path, (void)out, (void)settings;
    return false;
#endif
}

}  // namespace montage
