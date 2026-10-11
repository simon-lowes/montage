# Research

- `phase2-roadmap.md`: the feature-gap analysis against Premiere Pro, DaVinci Resolve, Final Cut Pro and Avid Media Composer (October 2026), and the ranked roadmap that `PROGRESS.md` tracks as Phase 2.
- `phase3-roadmap.md` and `phase4-roadmap.md`: the second and third gap analyses (October 2026), tracked as Phases 3 and 4.
- `notes-*.md`: the raw research notes behind it, one file per research track, with sources.

The notes are kept as written. Where they conflict with facts checked directly while building Montage, the roadmap wins. For example, one note says FFmpeg 9.0 does not exist; FFmpeg 9.0.2 is on ffmpeg.org and Homebrew, and Montage is tested against it.
- `notes-hdr10plus.md`: the facts behind HDR10+ export (ST 2094-40 syntax and units, FFmpeg's handling, HEVC SEI and AV1 OBU carriage, hdr10plus_tool's JSON), checked against primary sources in October 2026.
- `notes-mackie-control.md`: the Mackie Control protocol as hosts speak it (fader, touch, V-Pot, button, display, meter and handshake messages), cross-checked against Ardour's, Reaper's and Tracktion's code in October 2026.
