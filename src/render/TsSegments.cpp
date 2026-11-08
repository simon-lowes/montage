#include "TsSegments.h"

#include <algorithm>
#include <climits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

namespace montage {

namespace {

bool failWith(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

std::string averr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

}  // namespace

bool tsSegmentsCarry(const std::string& c) { return c == "h264" || c == "aac" || c == "ac3" || c == "eac3"; }

TsSegmenter::~TsSegmenter() {
    if (!oc_) return;
    if (oc_->pb) {
        uint8_t* b = nullptr;
        avio_close_dyn_buf(oc_->pb, &b);
        av_free(b);
        oc_->pb = nullptr;
    }
    avformat_free_context(oc_);
}

bool TsSegmenter::open(const AVCodecParameters* par, std::string* error) {
    const char* name = avcodec_get_name(par->codec_id);
    if (!name || !tsSegmentsCarry(name))
        return failWith(error, std::string("MPEG-TS segments carry H.264, AAC, AC-3 or E-AC-3, not ") + (name ? name : "this codec"));
    if (avformat_alloc_output_context2(&oc_, nullptr, "mpegts", nullptr) < 0 || !oc_) return failWith(error, "cannot make an MPEG-TS muxer");
    AVStream* st = avformat_new_stream(oc_, nullptr);
    if (!st || avcodec_parameters_copy(st->codecpar, par) < 0) return failWith(error, "cannot add the stream");
    st->codecpar->codec_tag = 0;
    st->time_base = {1, 90000};
    oc_->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;  // (the times are kept as given, kTsOffset on)
    if (int rc = avio_open_dyn_buf(&oc_->pb); rc < 0) return failWith(error, averr(rc));
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "max_delay", "0", 0);
    const int rc = avformat_write_header(oc_, &opts);
    av_dict_free(&opts);
    if (rc < 0) return failWith(error, "cannot start the transport stream: " + averr(rc));
    // (the header writes nothing a segment needs: each starts with the tables, below)
    uint8_t* b = nullptr;
    avio_close_dyn_buf(oc_->pb, &b);
    av_free(b);
    oc_->pb = nullptr;
    return true;
}

bool TsSegmenter::begin(std::string* error) {
    if (!oc_) return failWith(error, "the transport stream is not open");
    if (started_) return true;
    if (int rc = avio_open_dyn_buf(&oc_->pb); rc < 0) return failWith(error, averr(rc));
    av_opt_set(oc_->priv_data, "mpegts_flags", "+resend_headers", 0);  // (PAT and PMT before the segment's first packet)
    started_ = first_ = true;
    iframeBytes = 0, start = INT64_MAX, finish = INT64_MIN;
    keyFirst = false;
    return true;
}

bool TsSegmenter::write(AVPacket* pkt, int num, int den, std::string* error) {
    if (!started_ && !begin(error)) return false;
    AVStream* st = oc_->streams[0];
    av_packet_rescale_ts(pkt, AVRational{num, den}, st->time_base);
    if (pkt->pts != AV_NOPTS_VALUE) pkt->pts += kTsOffset;
    if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += kTsOffset;
    pkt->stream_index = 0;
    const int64_t pts = pkt->pts, end = pkt->pts + std::max<int64_t>(pkt->duration, 0);
    const bool key = pkt->flags & AV_PKT_FLAG_KEY;
    if (int rc = av_write_frame(oc_, pkt); rc < 0) return failWith(error, "cannot write a sample: " + averr(rc));
    if (pts != AV_NOPTS_VALUE) start = std::min(start, pts), finish = std::max(finish, end);
    if (first_) {
        // (a picture is written as one PES packet at once: the bytes so far are the tables and the keyframe)
        avio_flush(oc_->pb);
        iframeBytes = avio_tell(oc_->pb);
        keyFirst = key;
        first_ = false;
    }
    return true;
}

bool TsSegmenter::end(std::vector<uint8_t>& bytes, std::string* error) {
    bytes.clear();
    if (!started_) return true;
    av_write_frame(oc_, nullptr);  // (the sound held for a fuller PES packet, out now)
    uint8_t* b = nullptr;
    const int n = avio_close_dyn_buf(oc_->pb, &b);
    oc_->pb = nullptr;
    started_ = false;
    if (n < 0) {
        av_free(b);
        return failWith(error, "cannot finish a segment");
    }
    // (the service description FFmpeg sends ahead of the tables is left out, so the segment starts with its PAT; it
    // names the service, which HLS has no use for)
    bytes.reserve(size_t(n));
    int64_t before = 0;
    for (int at = 0; at < n; at += 188) {
        const int len = std::min(188, n - at);
        if (len == 188 && b[at] == 0x47 && ((b[at + 1] & 0x1f) << 8 | b[at + 2]) == 0x11) {
            if (at < iframeBytes) before += 188;
            continue;
        }
        bytes.insert(bytes.end(), b + at, b + at + len);
    }
    iframeBytes -= before;
    av_free(b);
    if (finish < start) start = finish = 0;
    return true;
}

}  // namespace montage
