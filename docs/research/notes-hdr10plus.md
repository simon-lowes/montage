# HDR10+ (SMPTE ST 2094-40, application 4): implementation notes

Checked in October 2026 against the primary sources listed at the end: FFmpeg release branches 6.1 to 9.0, SMPTE ST 2094-40:2020 and ST 2094-1:2016 (free on pub.smpte.org), ATSC A/341 Amendment S34-301r2, the AOM HDR10+ AV1 specification, the AV1 specification, ITU-T H.265 (09/2023), hdr10plus_tool and x265.

## The message (ITU-T T.35 payload)
Fields, MSB first:
- Header (6 bytes): `itu_t_t35_country_code` 0xB5, `terminal_provider_code` 0x003C, `terminal_provider_oriented_code` 0x0001, `application_identifier` 4.
- `application_version` u8 = 1. CTA-861 Annex S calls it application_mode, and the AV1 HDR10+ spec fixes it at 1. hdr10plus_tool rejects anything else, and FFmpeg's writer always writes 1. A 2018 ATSC draft that says 0 is stale.
- `num_windows` u2 = 1 (version 1 allows one window only, ST 2094-40 §9.4).
- `targeted_system_display_maximum_luminance` u27, in whole cd/m²; 0 = none (profile A). Then `targeted_system_display_actual_peak_luminance_flag` u1 = 0.
- Per window:
  - `maxscl[3]` u17 each, then `average_maxrgb` u17;
  - `num_distribution_maxrgb_percentiles` u4 = 9, then 9 × (`percentage` u7, `percentile` u17);
  - `fraction_bright_pixels` u10, where 0 means "not calculated" (what x265 and hdr10plus_tool write).
- `mastering_display_actual_peak_luminance_flag` u1 = 0.
- Per window: `tone_mapping_flag` u1. Profile A sends 0. Profile B sends 1, a target of about 400 cd/m², a knee point and 1–9 Bézier anchors. Then `color_saturation_mapping_flag` u1 = 0.
- Zero-padded to a byte. Profile A comes to 49 bytes including the header.

**Units.** maxscl, average_maxrgb and the percentiles are linearised values (the ST 2084 EOTF's output, ST 2094-40 §4.4) in 0..1 steps of 0.00001, where 1 = 10 000 cd/m². So the integer counts 0.1 cd/m² steps. maxscl is the maximum of each of linear R, G and B separately. average_maxrgb and the percentiles are taken over each pixel's linear max(R, G, B).

**The distribution (§8.5.4).** Exactly nine entries, at percentages 1, 5, 10, 25, 50, 75, 90, 95 and 99, where 99 stands for 99.98 %. When the 2nd and 3rd entries are 5 and 10, "the vector elements for V1 and V2 are not part of the CFD, V1 shall be 0.00000, V2 shall be 0.00255": the raw values 0 and 255. Samsung's tools have been seen writing other values there, and libplacebo reads them as the 99.99 % luminance and the share of pixels at or under 100 nits; that reading comes from a talk, not the standard. Montage follows the standard.

## FFmpeg
- `av_dynamic_hdr_plus_to_t35` / `_from_t35`: lavu 58.5.100, so in every release from 6.1 to 9.0. Both start at application_version and leave out the 6-byte header (the comment's "48 bytes" means bits). Denominators:
  - maxscl, average and percentiles: 100000;
  - fraction_bright_pixels: 1000;
  - knee point: 4095;
  - anchors: 1023;
  - target luminance: 1.
- **HEVC decoding.** Prefix SEIs (NAL type 39) with payload type 4 are recognised by the header and exported as `AV_FRAME_DATA_DYNAMIC_HDR_PLUS`. The value is sticky: it stays on later frames until the next message arrives or the decoder is flushed. H.264 ignores HDR10+.
- **AV1 decoding.** libdav1d exports HDR10+ from T.35 metadata OBUs from FFmpeg 6.1.
- **ffprobe.** `-show_frames` prints side data "HDR Dynamic Metadata SMPTE2094-40 (HDR10+)".
- **Encoders.** No HEVC encoder in FFmpeg writes HDR10+ from frame side data, up to master: not libx265, nvenc, videotoolbox, VAAPI, QSV or AMF. x265 writes it only from a JSON file (`dhdr10-info`), and only when built with ENABLE_HDR10_PLUS, which the common packages are not. libaom writes it from FFmpeg 8.1 on, but libsvtav1 does not. So Montage inserts the messages into the packets itself.

## Carriage
- **HEVC.** A prefix SEI NAL: header `4E 01`, payload type 4, payload size (0xFF runs, then a final byte), the T.35 message, `rbsp_trailing_bits` 0x80, with emulation prevention (an 03 after any 00 00 that is followed by 00–03). It goes after the parameter sets and before the access unit's first slice, once per access unit, on every frame, with identical values within a scene (the HDR10+ whitepaper; hdr10plus_tool and x265 do the same).
- **AV1.** An OBU_METADATA (header 0x2A: type 5 with a size field), leb128 size, `metadata_type` 4 (leb128), the T.35 message, trailing 0x80. Exactly one per shown frame, before that frame's frame header. In a temporal unit the shown frame comes last, so Montage puts it before the unit's last frame or frame-header OBU.
- **Requirements.** The whitepaper also requires PQ, BT.2020, 10-bit or more, and the mastering display SEI (which HDR10 exports already carry).

## JSON (hdr10plus_tool's "LLC" format; x265 reads `SceneInfo`)
- Top level: `JSONInfo` {`HDR10plusProfile`, `Version`}, `SceneInfo` (one entry per frame), `SceneInfoSummary` {`SceneFirstFrameIndex`, `SceneFrameNumbers`}, and `ToolInfo`.
- Each entry has `LuminanceParameters`:
  - `AverageRGB`;
  - `MaxScl` [3];
  - `LuminanceDistributions` {`DistributionIndex`, `DistributionValues`}.
- Each entry also has `NumberOfWindows`, `TargetedSystemDisplayMaximumLuminance`, `SceneFrameIndex`, `SceneId` and `SequenceFrameIndex`, plus `BezierCurveData` for profile B.
- Values are the raw syntax-element integers.

## Not confirmed from a primary source
- CTA-861-H Annex S itself, seen here only through the AV1 spec and FFmpeg.
- HDR10+ LLC's own profile A/B definitions, which are not public; those above are hdr10plus_tool's.
- Any public delivery requirement from Amazon, Netflix or Samsung for HDR10+. Prime Video's content guide v6 asks for HDR10 static metadata only.

## Sources
- FFmpeg (release/6.1 … 9.0): `libavutil/hdr_dynamic_metadata.{h,c}`, `doc/APIchanges`, `libavcodec/h2645_sei.c`, `hevc_sei.c`, `libdav1d.c`, `libaomenc.c` (release/8.1), `libvpxenc.c`, `libavformat/matroskaenc.c`, `fftools/ffprobe.c` — https://github.com/FFmpeg/FFmpeg
- SMPTE ST 2094-40:2020 — https://pub.smpte.org/pub/st2094-40/st2094-40-2020.pdf ; ST 2094-1:2016 — https://pub.smpte.org/pub/st2094-1/st2094-1-2016.pdf
- ATSC A/341 Amendment — https://www.atsc.org/wp-content/uploads/2018/02/S34-301r2-A341-Amendment-2094-40.pdf
- AV1 HDR10+ — https://aomediacodec.github.io/av1-hdr10plus/ ; AV1 — https://aomediacodec.github.io/av1-spec/
- H.265 — https://www.itu.int/rec/T-REC-H.265
- hdr10plus_tool — https://github.com/quietvoid/hdr10plus_tool
- x265 — https://x265.readthedocs.io/en/master/cli.html
- HDR10+ whitepaper — https://hdr10plus.org/wp-content/uploads/2023/11/HDR10_WhitePaper.pdf ; FAQ — https://hdr10plus.org/hdr10-faq/
- Prime Video content guide v6 — https://m.media-amazon.com/images/G/01/CooperWebsite/dvp/downloads/Prime-Video-Global-Content-Guide-v6.pdf
