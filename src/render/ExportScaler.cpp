#include "ExportScaler.h"

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace montage {

SwsContext* exportPictureScaler(SwsContext* sws, int srcW, int srcH, int dstW, int dstH, AVPixelFormat dst, AVChromaLocation loc,
                                AVColorSpace cs, AVColorRange range) {
    const AVPixelFormat srcFmt = AV_PIX_FMT_RGBA64LE;
    const int flags = SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INP;
    if (!sws && (loc == AVCHROMA_LOC_TOPLEFT || loc == AVCHROMA_LOC_LEFT)) {
        // Chroma made where the encoder's format sites it (otherwise FFmpeg's scaler centres it, as the decoder reads
        // untagged 4:2:0 pictures). The cached context below keeps this one, as it matches.
        sws = sws_alloc_context();
        if (sws) {
            av_opt_set_int(sws, "srcw", srcW, 0);
            av_opt_set_int(sws, "srch", srcH, 0);
            av_opt_set_int(sws, "src_format", srcFmt, 0);
            av_opt_set_int(sws, "dstw", dstW, 0);
            av_opt_set_int(sws, "dsth", dstH, 0);
            av_opt_set_int(sws, "dst_format", dst, 0);
            av_opt_set_int(sws, "sws_flags", flags, 0);
            av_opt_set_int(sws, "dst_h_chr_pos", 0, 0);
            // (in 1/256 luma pixels: left, and top or centred between the lines it spans, as the decoder takes it)
            const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(dst);
            const int spans = d ? 1 << d->log2_chroma_h : 1;
            av_opt_set_int(sws, "dst_v_chr_pos", loc == AVCHROMA_LOC_TOPLEFT ? 0 : (spans - 1) * 128, 0);
            if (sws_init_context(sws, nullptr, nullptr) < 0) {
                sws_freeContext(sws);
                sws = nullptr;
            }
        }
    }
    sws = sws_getCachedContext(sws, srcW, srcH, srcFmt, dstW, dstH, dst, flags, nullptr, nullptr, nullptr);
    if (!sws) return nullptr;
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(dst);
    if (desc && !(desc->flags & AV_PIX_FMT_FLAG_RGB))
        sws_setColorspaceDetails(sws, sws_getCoefficients(SWS_CS_DEFAULT), 1,
                                 sws_getCoefficients(cs == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020 : SWS_CS_ITU709),
                                 range == AVCOL_RANGE_JPEG ? 1 : 0, 0, 1 << 16, 1 << 16);
    return sws;
}

}  // namespace montage
