#!/usr/bin/env python3
"""Draws the Montage app icon and writes it in every format the packages need.

    python3 scripts/make-icons.py      (needs Pillow)

Outputs packaging/icons/montage.png (1024 px), packaging/macos/Montage.icns
and packaging/windows/montage.ico. The design is three timeline clips
(video, title, audio) under a playhead, in the app's theme colours.
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
SS = 4  # supersampling factor
N = 1024 * SS


def s(v):
    return int(round(v * SS))


def draw():
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))

    # Rounded-square plate on the macOS icon grid (824 px inside 1024), with a soft shadow.
    plate = (s(100), s(100), s(924), s(924))
    shadow = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle(
        (plate[0], plate[1] + s(14), plate[2], plate[3] + s(14)), radius=s(185), fill=(0, 0, 0, 110))
    img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(s(18))))

    grad = Image.new("RGBA", (N, N))
    gd = ImageDraw.Draw(grad)
    top, bottom = (0x2f, 0x32, 0x3a), (0x17, 0x18, 0x1c)
    for y in range(N):
        u = y / (N - 1)
        gd.line([(0, y), (N, y)], fill=tuple(int(a + (b - a) * u) for a, b in zip(top, bottom)) + (255,))
    mask = Image.new("L", (N, N), 0)
    ImageDraw.Draw(mask).rounded_rectangle(plate, radius=s(185), fill=255)
    img.paste(grad, (0, 0), mask)

    d = ImageDraw.Draw(img)
    # Track lanes.
    lanes = [s(300), s(470), s(640)]
    lane_h = s(120)
    for y in lanes:
        d.rounded_rectangle((s(170), y, s(854), y + lane_h), radius=s(22), fill=(0x24, 0x26, 0x2b, 255))

    # Clips: (lane, x0, x1, colour)
    clips = [
        (0, 190, 520, (0x3f, 0x6f, 0xb5)),
        (0, 540, 834, (0x4f, 0x86, 0xd6)),
        (1, 330, 690, (0x8a, 0x5c, 0xc9)),
        (2, 190, 834, (0x3c, 0x9a, 0x6e)),
    ]
    for lane, x0, x1, col in clips:
        y = lanes[lane]
        d.rounded_rectangle((s(x0), y + s(14), s(x1), y + lane_h - s(14)), radius=s(16), fill=col + (255,))

    # Audio waveform on the bottom clip.
    import math
    y_mid = lanes[2] + lane_h // 2
    for i in range(0, 61):
        x = s(214 + i * 10)
        amp = (0.35 + 0.65 * abs(math.sin(i * 0.47) * math.cos(i * 0.13))) * s(38)
        d.rounded_rectangle((x - s(3), y_mid - amp, x + s(3), y_mid + amp), radius=s(3), fill=(0xd8, 0xf5, 0xe6, 200))

    # Playhead.
    px = s(600)
    d.rounded_rectangle((px - s(9), s(250), px + s(9), s(790)), radius=s(9), fill=(0xff, 0x4d, 0x4d, 255))
    d.polygon([(px - s(40), s(215)), (px + s(40), s(215)), (px + s(40), s(250)), (px, s(285)), (px - s(40), s(250))],
              fill=(0xff, 0x4d, 0x4d, 255))

    return img.resize((1024, 1024), Image.LANCZOS)


def main():
    icon = draw()
    out_png = ROOT / "packaging/icons/montage.png"
    out_icns = ROOT / "packaging/macos/Montage.icns"
    out_ico = ROOT / "packaging/windows/montage.ico"
    for p in (out_png, out_icns, out_ico):
        p.parent.mkdir(parents=True, exist_ok=True)
    icon.save(out_png, optimize=True)
    icon.save(out_icns)
    icon.save(out_ico, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print("wrote", out_png, out_icns, out_ico)


if __name__ == "__main__":
    main()
