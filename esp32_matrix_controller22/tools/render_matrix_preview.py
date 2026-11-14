#!/usr/bin/env python3
"""Render the README preview of the 16x16 matrix as an SVG.

The glyphs are parsed from F_MICRO in src/app.cpp and the layout, colours
and progress-bar maths mirror showClockTemp(), so the picture shows what the
firmware draws with default settings (clock at y=2, auto-centred temperature
at y=9, printer overlay on rows 7-8).

    python3 tools/render_matrix_preview.py [output.svg]
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO = ROOT.parent
DEFAULT_OUT = REPO / "docs" / "assets" / "matrix-preview.svg"

MW = MH = 16
CLOCK = (255, 128, 0)          # ct6Clock default
FILL = (0, 220, 70)            # printer fill
PAUSE = (255, 200, 0)
COLON_BRIGHT = 90              # scales the overlay track


def load_font():
    src = (ROOT / "src" / "app.cpp").read_text(encoding="utf-8")
    body = re.search(r"F_MICRO\[13\]\[5\][^=]*=\s*\{(.*?)\};", src, re.S).group(1)
    rows = [[int(v) for v in g.split(",")] for g in re.findall(r"\{([\d,\s]+)\}", body)]
    assert len(rows) == 13, "F_MICRO layout changed"
    return rows


FONT = load_font()


def glyph(c):
    if c.isdigit():
        return FONT[int(c)]
    return FONT[{"-": 10, "+": 11}.get(c, 12)]


def scale8(c, s):
    return tuple(v * s // 256 for v in c)


def blend(a, b, mix):
    return tuple(x + ((y - x) * mix) // 255 for x, y in zip(a, b))


def temp_color(t):
    if t <= -20: return (0, 0, 255)
    if t <= 0:   return (0, int(200 * (t + 20) / 20), 255)
    if t <= 20:  return (0, 220, int(255 * (1 - t / 20)))
    if t <= 30:  return (int(255 * (t - 20) / 10), 220, 0)
    if t <= 40:  return (255, int(220 * (1 - (t - 30) / 10)), 0)
    return (255, 0, 0)


def draw_str(frame, x, y, text, col):
    for i, ch in enumerate(text):
        cx = x + i * 4
        for row, bits in enumerate(glyph(ch)):
            for k in range(3):
                if bits & (1 << (2 - k)) and 0 <= cx + k < MW and 0 <= y + row < MH:
                    frame[y + row][cx + k] = col


def render(hh, mm, temp, lifecycle=None, progress=0):
    frame = [[None] * MW for _ in range(MH)]
    draw_str(frame, 0, 2, hh, CLOCK)
    draw_str(frame, 9, 2, mm, CLOCK)
    a = abs(round(temp))
    text = ("-" if temp < 0 else "+") + (f"{a}" if a < 10 else f"{a:02d}")
    width = len(text) * 4 - 1
    draw_str(frame, max(0, (MW - width) // 2), 9, text, temp_color(temp))

    track = scale8(CLOCK, COLON_BRIGHT)
    for x in range(MW):
        c = None
        if lifecycle == "printing":
            lo, hi = 100 * x / MW, 100 * (x + 1) / MW
            c = track
            if progress >= hi:
                c = FILL
            elif progress > lo and x < MW - 1:
                c = blend(scale8(FILL, 56), FILL, round((progress - lo) / (hi - lo) * 255))
        elif lifecycle == "paused":
            c = PAUSE
        if c:
            frame[7][x] = frame[8][x] = c
    return frame


PITCH, LED, PAD, GAP, LABEL_H = 14, 10, 14, 28, 34


def svg(frames):
    side = MW * PITCH + 2 * PAD
    w = len(frames) * side + (len(frames) - 1) * GAP
    h = side + LABEL_H
    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
        f'viewBox="0 0 {w} {h}" role="img" aria-label="LED matrix preview">',
        '<defs><filter id="glow" x="-50%" y="-50%" width="200%" height="200%">'
        '<feGaussianBlur stdDeviation="2.2" result="b"/>'
        '<feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>'
        "</filter></defs>",
    ]
    for i, (label, frame) in enumerate(frames):
        ox = i * (side + GAP)
        out.append(f'<rect x="{ox}" y="0" width="{side}" height="{side}" rx="14" fill="#111214"/>')
        lit = []
        for y in range(MH):
            for x in range(MW):
                cx = ox + PAD + x * PITCH + PITCH / 2
                cy = PAD + y * PITCH + PITCH / 2
                c = frame[y][x]
                if c is None:
                    out.append(f'<circle cx="{cx}" cy="{cy}" r="{LED / 2}" fill="#1d1f23"/>')
                else:
                    lit.append(f'<circle cx="{cx}" cy="{cy}" r="{LED / 2}" fill="rgb{c}"/>')
        out.append('<g filter="url(#glow)">' + "".join(lit) + "</g>")
        out.append(
            f'<text x="{ox + side / 2}" y="{side + 24}" text-anchor="middle" '
            'font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif" '
            f'font-size="15" fill="#8b949e">{label}</text>'
        )
    out.append("</svg>")
    return "\n".join(out) + "\n"


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_OUT
    frames = [
        ("clock + weather", render("21", "47", 7)),
        ("printing 62%", render("21", "47", 7, "printing", 62)),
        ("paused (blinks)", render("21", "47", 7, "paused")),
    ]
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(svg(frames), encoding="utf-8")
    print(out)


if __name__ == "__main__":
    main()
