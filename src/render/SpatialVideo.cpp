#include "SpatialVideo.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "media/SpatialAudio.h"

#ifdef MONTAGE_WITH_SPATIAL_VIDEO
#include MONTAGE_X265_HEADER  // (Montage's own x265, with the multiview encoder)
#endif

namespace montage {

namespace {

void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
}
void put16(std::vector<uint8_t>& b, unsigned v) {
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}
// A box of `type` around `body` (a full box when `full`: version 0, no flags).
std::vector<uint8_t> box(const char* type, const std::vector<uint8_t>& body, bool full = false) {
    std::vector<uint8_t> b;
    put32(b, uint32_t(8 + (full ? 4 : 0) + body.size()));
    b.insert(b.end(), type, type + 4);
    if (full) put32(b, 0);
    b.insert(b.end(), body.begin(), body.end());
    return b;
}
std::vector<uint8_t> operator+(std::vector<uint8_t> a, const std::vector<uint8_t>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

}  // namespace

#ifdef MONTAGE_WITH_SPATIAL_VIDEO

bool spatialVideoAvailable() { return true; }

struct SpatialVideoEncoder::Impl {
    x265_param* param = nullptr;
    x265_encoder* encoder = nullptr;
    ~Impl() {
        if (encoder) x265_encoder_close(encoder);
        if (param) x265_param_free(param);
    }
};

namespace {

// A NAL unit's layer (nuh_layer_id) from its header, after the start code.
int nalLayer(const x265_nal& n) {
    const uint32_t at = n.sizeBytes > 4 && n.payload[2] == 1 ? 3 : 4;  // (a three or four byte start code)
    return n.sizeBytes > at + 1 ? ((n.payload[at] & 1) << 5) | (n.payload[at + 1] >> 3) : 0;
}

void collect(const x265_nal* nals, uint32_t count, const x265_picture& pic, std::vector<SpatialVideoEncoder::Packet>& out) {
    if (!count) return;
    SpatialVideoEncoder::Packet p;
    for (uint32_t i = 0; i < count; ++i) p.data.insert(p.data.end(), nals[i].payload, nals[i].payload + nals[i].sizeBytes);
    p.pts = pic.pts;
    p.dts = pic.dts;
    p.key = IS_X265_TYPE_I(pic.sliceType);
    out.push_back(std::move(p));
}

}  // namespace

std::unique_ptr<SpatialVideoEncoder> SpatialVideoEncoder::open(const SpatialVideoSettings& s, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return nullptr;
    };
    if (s.width <= 0 || s.height <= 0 || (s.width & 1) || (s.height & 1)) return fail("Each eye's picture must be an even size");
    std::unique_ptr<SpatialVideoEncoder> e(new SpatialVideoEncoder);
    e->d_ = std::make_unique<Impl>();
    x265_param* p = e->d_->param = x265_param_alloc();
    if (!p) return fail("Out of memory");
    if (x265_param_default_preset(p, s.preset.empty() ? "medium" : s.preset.c_str(), nullptr) < 0) return fail("Unknown x265 preset " + s.preset);
    p->logLevel = X265_LOG_ERROR;
    // Two views given as two pictures (the left the base view, which the stream's 3D reference displays message names
    // the left eye, view 0, and the right view 1).
    p->numViews = 2;
    p->format = 0;
    p->numLayers = 2;
    p->sourceWidth = s.width;
    p->sourceHeight = s.height;
    p->fpsNum = uint32_t(s.fpsNum);
    p->fpsDenom = uint32_t(s.fpsDen);
    p->internalCsp = X265_CSP_I420;
    p->internalBitDepth = 8;
    // Closed GOPs: every keyframe a clean place to start (no leading pictures that reach back past it), as Apple's own
    // spatial video is coded.
    p->bOpenGOP = 0;
    p->keyframeMax = s.keyint > 0 ? s.keyint : std::max(1, int(std::lround(2.0 * s.fpsNum / std::max(1, s.fpsDen))));
    // Parameter sets once, in the sample description (hvcC and lhvC), as an 'hvc1' track keeps them.
    p->bRepeatHeaders = 0;
    p->bAnnexB = 1;
    p->bEmitInfoSEI = 0;
    if (s.bitrate > 0) {
        p->rc.rateControlMode = X265_RC_ABR;
        p->rc.bitrate = int(std::max<int64_t>(1, s.bitrate / 1000));
    } else {
        p->rc.rateControlMode = X265_RC_CRF;
        p->rc.rfConstant = std::clamp(s.crf, 0, 51);
    }
    p->vui.bEnableVideoSignalTypePresentFlag = 1;
    p->vui.videoFormat = 5;  // unspecified
    p->vui.bEnableVideoFullRangeFlag = 0;
    p->vui.bEnableColorDescriptionPresentFlag = 1;
    p->vui.colorPrimaries = s.primaries;
    p->vui.transferCharacteristics = s.transfer;
    p->vui.matrixCoeffs = s.matrix;
    if (!(e->d_->encoder = x265_encoder_open(p))) return fail("x265 cannot code MV-HEVC with these settings");
    x265_nal* nals = nullptr;
    uint32_t count = 0;
    if (x265_encoder_headers(e->d_->encoder, &nals, &count) < 0 || !count) return fail("x265 gave no parameter sets");
    // The base view's VPS, SPS, PPS and SEI messages for the hvcC; the second view's SPS and PPS for the lhvC (its
    // record: version 1, no segmentation or parallelism stated, temporal layers as the base's, 4-byte lengths).
    std::vector<std::vector<uint8_t>> second[2];  // SPS, PPS
    for (uint32_t i = 0; i < count; ++i) {
        const x265_nal& n = nals[i];
        if (nalLayer(n) == 0) {
            e->base_.insert(e->base_.end(), n.payload, n.payload + n.sizeBytes);
        } else if (n.type == NAL_UNIT_SPS || n.type == NAL_UNIT_PPS) {
            const uint32_t at = n.payload[2] == 1 ? 3 : 4;
            second[n.type == NAL_UNIT_PPS].emplace_back(n.payload + at, n.payload + n.sizeBytes);
        }
    }
    if (second[0].empty() || second[1].empty()) return fail("x265 gave no parameter sets for the second view");
    std::vector<uint8_t>& r = e->lhvc_;
    r = {1, 0xf0, 0x00, 0xfc, uint8_t(0xc0 | (1 << 3) | (1 << 2) | 3), 2};
    for (int k = 0; k < 2; ++k) {
        r.push_back(uint8_t(0x80 | (k ? NAL_UNIT_PPS : NAL_UNIT_SPS)));  // (array complete)
        put16(r, unsigned(second[k].size()));
        for (const auto& n : second[k]) {
            put16(r, unsigned(n.size()));
            r.insert(r.end(), n.begin(), n.end());
        }
    }
    return e;
}

SpatialVideoEncoder::~SpatialVideoEncoder() = default;

bool SpatialVideoEncoder::encode(const uint8_t* sbs, size_t size, int64_t pts, std::vector<Packet>& out, std::string* error) {
    const int W = d_->param->sourceWidth, H = d_->param->sourceHeight;
    if (!sbs || size != size_t(3) * W * H) {  // (luma 2W x H, each chroma plane W x H/2)
        if (error) *error = "The picture is not both eyes side by side at the size the spatial video encoder was opened with";
        return false;
    }
    x265_picture in[2], got[2];
    for (int v = 0; v < 2; ++v) {
        x265_picture_init(d_->param, &in[v]);
        x265_picture_init(d_->param, &got[v]);
        // (The planes of the picture both eyes share: luma 2W wide, then chroma W wide; each eye its half.)
        const uint8_t* y = sbs;
        const uint8_t* cb = y + size_t(2 * W) * H;
        const uint8_t* cr = cb + size_t(W) * (H / 2);
        in[v].planes[0] = const_cast<uint8_t*>(y + size_t(v) * W);
        in[v].planes[1] = const_cast<uint8_t*>(cb + size_t(v) * (W / 2));
        in[v].planes[2] = const_cast<uint8_t*>(cr + size_t(v) * (W / 2));
        in[v].stride[0] = 2 * W;
        in[v].stride[1] = in[v].stride[2] = W;
        in[v].bitDepth = 8;
        in[v].colorSpace = X265_CSP_I420;
        in[v].pts = pts;
        in[v].layerID = v;
    }
    x265_nal* nals = nullptr;
    uint32_t count = 0;
    if (x265_encoder_encode(d_->encoder, &nals, &count, in, got) < 0) {
        if (error) *error = "x265 failed to code a picture";
        return false;
    }
    collect(nals, count, got[0], out);
    return true;
}

bool SpatialVideoEncoder::flush(std::vector<Packet>& out, std::string* error) {
    for (;;) {
        x265_picture got[2];
        x265_picture_init(d_->param, &got[0]);
        x265_picture_init(d_->param, &got[1]);
        x265_nal* nals = nullptr;
        uint32_t count = 0;
        const int rc = x265_encoder_encode(d_->encoder, &nals, &count, nullptr, got);
        if (rc < 0) {
            if (error) *error = "x265 failed to finish the stream";
            return false;
        }
        if (rc == 0) return true;
        collect(nals, count, got[0], out);
    }
}

#else

bool spatialVideoAvailable() { return false; }
struct SpatialVideoEncoder::Impl {};
std::unique_ptr<SpatialVideoEncoder> SpatialVideoEncoder::open(const SpatialVideoSettings&, std::string* error) {
    if (error) *error = "This build of Montage has no MV-HEVC encoder: it was built without x265's multiview encoder";
    return nullptr;
}
SpatialVideoEncoder::~SpatialVideoEncoder() = default;
bool SpatialVideoEncoder::encode(const uint8_t*, size_t, int64_t, std::vector<Packet>&, std::string*) { return false; }
bool SpatialVideoEncoder::flush(std::vector<Packet>&, std::string*) { return false; }

#endif

bool writeSpatialVideoBoxes(const std::string& path, const std::vector<uint8_t>& lhvc, double baselineMm, double fieldOfView,
                            std::string* error) {
    if (lhvc.empty()) {
        if (error) *error = "No second view's configuration";
        return false;
    }
    // Apple's stereo video extensions: the eyes present (stri: left and right, in their own views, not reversed),
    // the hero eye (left), the cameras' baseline (micrometres) and the disparity adjustment (none); the horizontal
    // field of view (thousandths of a degree).
    std::vector<uint8_t> v;
    put32(v, uint32_t(std::lround(std::clamp(baselineMm, 0.0, 4.0e6) * 1000)));
    const std::vector<uint8_t> blin = box("blin", v, true);
    std::vector<uint8_t> zero;
    put32(zero, 0);
    const std::vector<uint8_t> eyes =
        box("eyes", box("stri", {0x03}, true) + box("hero", {0x01}, true) + box("cams", blin) + box("cmfy", box("dadj", zero, true)));
    std::vector<uint8_t> fov;
    put32(fov, uint32_t(std::lround(std::clamp(fieldOfView, 0.0, 360.0) * 1000)));
    return addVideoEntryBoxes(path, box("lhvC", lhvc) + box("vexu", eyes) + box("hfov", fov), error);
}

}  // namespace montage
