# Camera logs and ProRes RAW: research notes (checked 2026-10-10)

How to read these notes: linear values are scene reflectance with 18% grey = 0.18, which is the convention in `src/render/ColorSpace.cpp`. V is the normalised full-range code value, 0..1, as decoded from R'G'B'. Check values marked "computed" are my own arithmetic from the published formulas. No files were changed.

## A1. Apple Log 2 / Apple Gamut
- **Primary source:** the Apple Log 2 white paper (September 2025) at https://developer.apple.com/download/all/?q=Apple%20log%202. It needs an Apple sign-in, so I could not fetch it. The values below come from the ACES vendor transform `apple/CSC.Apple.AppleLog2_to_ACES.ctl` in https://github.com/aces-aswf/aces-input-and-colorspaces, whose README links that paper.
- **Curve:** the same as the original Apple Log. The constants match Montage's `kAl*` exactly.
  - Encode: R ≥ Rt gives V = γ·log2(R+β) + δ. R0 ≤ R < Rt gives V = σ(R−R0)². R < R0 gives 0.
  - Decode: V ≥ Pt gives R = 2^((V−δ)/γ) − β. 0 ≤ V < Pt gives R = √(V/σ) + R0. V < 0 gives R0.
  - Constants: R0 = −0.05641088, Rt = 0.01, σ = 47.28711236, β = 0.00964052, γ = 0.08550479, δ = 0.69336945. Pt = σ(Rt−R0)² = 0.20855532.
  - Check values (computed): 0% → 0.150476, 18% → 0.488272 (colour-science's test gives 0.48827245852686763), 90% → 0.681687.
- **Gamut:** Apple's docs call it "Apple Gamut" (`AVCaptureColorSpace.appleLog2`, iOS 26.0). ACES and OCIO call it "Apple Wide Gamut".
  - xy: R (0.725, 0.301), G (0.221, 0.814), B (0.068, −0.076), white D65 (0.3127, 0.3290).
  - Apple publishes no matrix and no chromatic adaptation (OpenColorIO-Config-ACES PR #179). ACES and OCIO derive the matrix using Bradford.
  - Apple Wide Gamut → XYZ (computed): [0.6514914802, 0.2215531133, 0.0774113336; 0.2704812904, 0.8160372589, −0.0865185493; −0.0233638324, −0.0350875971, 1.1475091803].
  - My Bradford-derived Apple Wide Gamut → AP0 matrix [0.694961049318, 0.241405268785, 0.063633681897; 0.047362746415, 1.004295925054, −0.051658671469; −0.021989789360, −0.028989104971, 1.050978894331] matches OCIO-Config-ACES issue #163 exactly.
  - OCIO names: "Apple Log 2" (`ocio:applelog_applewg_scene`) and "Linear Apple Wide Gamut".
- **Original Apple Log uses Rec.2020: confirmed.** Apple's `appleLog` doc says it "uses BT2020 as the color primaries" (iOS 17). `CSC.Apple.AppleLog_to_ACES.ctl` also uses Rec.2020 primaries.
- **File tagging:** I found no documentation of how Apple Log or Apple Log 2 are signalled in the container. Probe real clips before relying on tags.

## A2. DJI
- **The names exist, but only D-Log has published formulas.**
  - D-Log2 / D-Gamut2 (Osmo Pocket 4P, May 2026): DJI has published no white paper or formula. Resolve has no CST for it. Source: gamut.io, which is secondary.
  - D-Log M: no published formula either.
  - The published curve is D-Log: X7 white paper Rev 1.0, 2017 (https://dl.djicdn.com/downloads/zenmuse+x7/20171010/D-Log_D-Gamut_Whitepaper.pdf). The X9 white paper (2022.02, https://dl.djicdn.com/downloads/DJI_Ronin_4D/X9_D_Log_D_Gamut_Whitepaper.pdf) says the conversion is unchanged and only the clip level varies with EI.
- **D-Log encode:** x ≤ 0.0078 gives V = 6.025x + 0.0929. Otherwise V = 0.256663·log10(0.9892x + 0.0108) + 0.584555.
- **D-Log decode:** V ≤ 0.14 gives x = (V − 0.0929)/6.025. Otherwise x = (10^(3.89616V − 2.27752) − 0.0108)/0.9892.
- **X9 table:** 0% → 95, 18% → 408, 90% → 586 (10-bit). V×1023 gives 95.04, 407.94 and 586.12, so the code values are full range. The clip is 948 at native EI and lower below native (789 at EI 200).
- **D-Gamut:** R (0.71, 0.31), G (0.21, 0.88), B (0.09, −0.08), D65.
  - Published RGB→XYZ: [0.6482, 0.1940, 0.1082; 0.2830, 0.8132, −0.0962; −0.0183, −0.0832, 1.1903].
  - Published XYZ→RGB: [1.7257, −0.4314, −0.1917; −0.6025, 1.3906, 0.1671; −0.0156, 0.0905, 0.8489].
- Montage's existing D-Log / D-Gamut already matches these.

## A3. GoPro
- **Formula for every GoPro curve:** V = ln(L(b−1) + 1)/ln b, inverse L = (b^V − 1)/(b − 1). There is no linear segment. L is normalised sensor linear, with 1 = clip.
- **GP-Log (HERO12 Black, HERO13 Black): b = 400.**
  - Source is GoPro's own Labs docs (https://github.com/gopro/labs/blob/master/docs/control/tech/README.md, the LOGB entry): "Color Flat is Log base 113. Math:out = log(in*(base-1)+1)/log(b) … $LOGB=400 for GPLog equivalent".
  - Gamut is Rec.709. GoPro's support article calls it "GP-Log, a Rec. 709 color profile" (https://community.gopro.com/s/article/HERO13-Black-10-Bit-Log-Encoding). That page renders with JavaScript, so I read it only through a search snippet.
  - GoPro publishes no 18% grey value for GP-Log.
- **GP-Log2 (MISSION 1 series, not HERO13): b = 600, Rec.2020 primaries.**
  - GoPro's white paper: https://github.com/gopro/labs/blob/master/docs/log/README.md. LUT generator: https://gopro.github.io/labs/gplog2/.
  - It states "L = 0.0517 (18% grey) → ~0.542", which is about code 554 of 1023.
  - 0.0517 = 0.18·2^−1.8, so reflectance = 2^1.8·L. This matches the camera protecting about 1.8 stops above reflected white, and the generator's default of +1.8 EV.
  - 90% → V = 0.7892 (computed).
- **GP-Log with the same exposure logic (my inference):** 18% → 0.513. GoPro says all colour modes start from similar exposure.
- **Older curves:** Protune Flat uses b = 113 (colour-science `log_encoding_Protune`, 18% → 0.645623). colour-science's "Protune Native" primaries are reverse-engineered (secondary).

## A4. Leica L-Log
- **Primary source:** the L-Log Reference Manual v1.6 (https://leica-camera.com/sites/default/files/pm-118912-L-Log_Reference_Manual_V1.6.pdf). Leica's server returned 502 every time, so I could not read it.
  - From the manual's indexed text: 18% = 44 IRE = 10-bit 445 = 12-bit 1782, with 15 stops of range.
  - The constants come from ACES `leica/CSC.Leica.LLog_BT2020_to_ACES.ctl` and colour-science `leica_l_log.py`, which cites the manual.
- **Encode:** x ≤ 0.006 gives V = 8x + 0.09. Otherwise V = 0.27·log10(1.3x + 0.0115) + 0.6.
- **Decode:** V ≤ 0.138 gives x = (V − 0.09)/8. Otherwise x = (10^((V−0.6)/0.27) − 0.0115)/1.3.
- **Join point:** the two pieces do not meet exactly at the cut. At x = 0.006 the log segment gives 0.13710 against 0.138, and that is how it is published.
- **Check values:** 0% → 0.09 (92), 18% → 0.435314 (445.3, matching the manual's 445), 90% → 0.619557 (634, computed).
- **Gamut:** Rec.2020, D65. Leica's spec sheets list "Rec. 709/Rec. 2020 (HLG/L-Log)", and the ACES transform is named `LLog_BT2020`. Leica publishes no separate "L-Gamut"; the term appears only in third-party guides.

## B. FFmpeg ProRes RAW decoder
I read FFmpeg master at ba987fe (2026-10-10) and the release tags n8.0, n8.1 and n9.0. The files are `libavcodec/prores_raw.c` (`decode_init`, `decode_frame`, `decode_tile`, `decode_comp`), `prores_raw_parser.c`, `proresdsp.c`, `vulkan_prores_raw.c`, `libavutil/raw_color_params.h` and `libavformat/isom_tags.c`.

- **Identity:** decoder name `prores_raw`, `AV_CODEC_ID_PRORES_RAW` (lavc 62.9.100). Profiles are `AV_PROFILE_PRORES_RAW` = 0 and `_HQ` = 1.
- **MOV fourccs:** `aprn` (RAW) and `aprh` (RAW HQ), in `isom_tags.c` lines 240–241. Any other tag fails with "patch welcome". mov.c parses headers only.
- **Pixel format:** always `AV_PIX_FMT_BAYER_RGGB16`. Any other Bayer pattern in the header is rejected with "patch welcome".
- **CPU path: yes.** The format list is Vulkan (8.0+), then VideoToolbox (9.0+), then Bayer. The default `get_format` picks software, which decodes through `execute2(decode_tiles)` with frame and slice threads. Vulkan is optional, not required.
- **The version matters a lot:**
  - **8.0 (2025-08-22) and 8.1:** the colour header fields and the linearisation curve are skipped (`bytestream2_skip`). Output is `CLIP_12(v) << 4`, i.e. non-linear 12-bit codes. White balance, the colour matrix and the crop are not exported.
  - **9.0 (2026-08-03) and later:** `bits_per_raw_sample` = 16 and `color_trc` = LINEAR. The 8-point linearisation curve is applied in `put_pixel_bayer_lin_curve_12`; when flag bit 4 is clear the default {0, 512, 1024, …, 32768} is used. Output is linear 16-bit.
  - Master also fixes DC-offset and end-of-data bugs that 9.0 still has.
- **Metadata (9.0+):** frame side data `AV_FRAME_DATA_RAW_COLOR_PARAMS` (lavu 60.33.100) holding `AVRawColorParams`. Read it with `av_frame_get_side_data`.
  - Outer struct: `black_level` = 256/65535 (hardcoded), `white_level` = (senselValueRange + 256)/65535, `wb_cct` in Kelvin (0 if not signalled).
  - `codec.prores_raw`: `wb_red`, `wb_blue` (green is 1), `color_matrix[3][3]` (camera RGB → XYZ D65, row-major), and `gain`, a scene-linear multiplier for highlight headroom. All are AVRational.
  - Documented order: normalise with the black/white levels → white balance on the CFA → debayer → matrix → multiply by gain.
- **Crop:** `frame->crop_*` carries the header's RecommendedCrop. libavcodec applies it by default and may round the left crop down for alignment. Set `apply_cropping = 0` and crop after debayering.
- **Not exported:** ISO/EI, shutter and exposure; the vendor `psim` records (mentioned only in a code comment); and MOV sample-description extensions.
- **Test sample:** neither FATE nor fate-suite has a ProRes RAW file.
  - The only free one I found is https://samples.ffmpeg.org/ffmpeg-bugs/trac/ticket7887/Filmplusgear-ProRes-RAW-testfiles-6.mov, which is 1,041,311,744 bytes.
  - I checked it with byte-range requests: `aprh`, 4112×2176, 25 fps, 178 frames of about 5.87 MB each, crop 8/8/8/8 (giving 4096×2160), vendor `appl`, version 0, white level 61568.
  - Its header values: wb_red 1.82421875, wb_blue 2.08203125, gain 11.7412, no curve. The matrix rows sum to about (0.95, 1.00, 1.09), which is the D65 white, consistent with an XYZ D65 matrix.
  - The moov box is at the end of the file. `ffmpeg -i <url> -map 0:v -c copy -frames:v 2 prr.mov` should make a fixture of about 12 MB.

## Implementation implications
1. **Apple Log 2:** add an Apple Wide Gamut entry (D65 white, xy above) and reuse the existing Apple Log curve. Nothing else is needed.
2. **DJI:** only D-Log / D-Gamut is implementable, and it is already in Montage. Don't ship guessed D-Log M or D-Log2 curves; offer LUT import for those instead.
3. **GoPro:** add GP-Log (base 400, Rec.709) and GP-Log2 (base 600, Rec.2020). Scale decoded L by 2^1.8 so 18% lands at 0.18. That scale is GoPro's published figure for GP-Log2 and my assumption for GP-Log.
4. **Leica:** add L-Log with Rec.2020 primaries. Encode with the 0.006 cut and decode with the 0.138 cut.
5. **ProRes RAW:** require FFmpeg 9.0 (lavc ≥ 63.1, lavu ≥ 60.33.100), behind compile-time version guards. The system FFmpeg here is 6.1 (lavc 60.31), which has no ProRes RAW decoder at all. With 8.x the output is non-linear and has no colour metadata, so reject it or warn.
6. **ProRes RAW pipeline:** RGGB16 → black/white levels → white balance → debayer → matrix to XYZ D65 → gain → `primariesToXyz` inverse into the working space. LibRaw's `open_bayer()` might work as the demosaicer; worth checking. Where 18% grey lands after gain is not documented.
7. **Unit tests:** use the check values above.
