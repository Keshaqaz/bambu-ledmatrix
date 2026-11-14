#!/usr/bin/env python3
"""Render the README preview of the 16x16 matrix as an animated SVG.

The glyphs are parsed from F_MICRO in src/app.cpp; colours, the progress
ramp, the preparing breath (beatsin8(10, 72, 220)) and the 500 ms blink of
paused/error/finished mirror showClockTemp(). The layout is the one the
owner's matrix runs: clock at ct6ClockY=1, auto-centred temperature at
ct6TempY=10, so both keep a one-pixel gap from the overlay on rows 7-8.

Panels:
  1. a print lifecycle loop: preparing -> printing 0..100% -> finished -> idle
  2. paused (yellow blink)
  3. error (red blink)

    python3 tools/render_matrix_preview.py [output.svg]
    python3 tools/render_matrix_preview.py --at 9.5 frame.svg   # static frame
"""

import math
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO = ROOT.parent
DEFAULT_OUT = REPO / "docs" / "assets" / "matrix-preview.svg"

MW = MH = 16
CLOCK_Y, TEMP_Y, BAR_ROWS = 1, 10, (7, 8)
CLOCK = (255, 128, 0)          # ct6Clock default
FILL = (0, 220, 70)            # printer fill
PAUSE, ERROR = (255, 200, 0), (255, 0, 0)
COLON_BRIGHT = 90              # scales the overlay track
OFF = (29, 31, 35)             # unlit LED in the picture

# Lifecycle loop of panel 1, seconds.
PREP, PRINT, FINISH, IDLE = 6.0, 10.0, 4.0, 2.0
LOOP = PREP + PRINT + FINISH + IDLE
BLINK = 0.5                    # overlayPhase = (millis() / 500) & 1
EPS = 0.001


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


TRACK = scale8(CLOCK, COLON_BRIGHT)
FILL_MIN = scale8(FILL, 56)


def static_frame(hh, mm, temp):
    frame = [[None] * MW for _ in range(MH)]

    def draw(x, y, text, col):
        for i, ch in enumerate(text):
            for row, bits in enumerate(glyph(ch)):
                for k in range(3):
                    if bits & (1 << (2 - k)) and 0 <= x + i * 4 + k < MW:
                        frame[y + row][x + i * 4 + k] = col

    draw(0, CLOCK_Y, hh, CLOCK)
    draw(9, CLOCK_Y, mm, CLOCK)
    a = abs(round(temp))
    text = ("-" if temp < 0 else "+") + (f"{a}" if a < 10 else f"{a:02d}")
    draw(max(0, (MW - (len(text) * 4 - 1)) // 2), TEMP_Y, text, temp_color(temp))
    for y in BAR_ROWS:
        assert not any(frame[y]), "clock/temperature overlap the overlay rows"
    return frame


# ── keyframes: [(t, rgb)], linear in between; a jump is two keys EPS apart ──

def blink(color, t0, t1, keys):
    t, on = t0, True
    while t < t1 - EPS:
        c = color if on else OFF
        keys += [(t, c), (min(t + BLINK, t1) - EPS, c)]
        t, on = t + BLINK, not on


def lifecycle_keys(x):
    keys = []
    # preparing: column 0 breathes, the rest is the dim track
    if x == 0:
        n = 24
        for i in range(n + 1):
            t = PREP * i / n
            s = 72 + (220 - 72) * (math.sin(2 * math.pi * t / 6.0) + 1) / 2
            keys.append((min(t, PREP - EPS), scale8(FILL, int(s))))
    else:
        keys += [(0.0, TRACK), (PREP - EPS, TRACK)]
    # printing: progress 0..100 % over PRINT seconds
    lo, hi = PREP + PRINT * x / MW, PREP + PRINT * (x + 1) / MW
    if x == 0:
        keys += [(PREP, FILL_MIN), (hi, FILL)]
    elif x < MW - 1:   # the active column ramps fillMin -> fill
        keys += [(PREP, TRACK), (lo, TRACK), (lo + EPS, FILL_MIN), (hi, FILL)]
    else:              # the last column has no ramp, it lights at 100 %
        keys += [(PREP, TRACK), (hi - EPS, TRACK)]
    # finished: the full bar blinks green, then idle
    blink(FILL, PREP + PRINT, PREP + PRINT + FINISH, keys)
    keys += [(PREP + PRINT + FINISH, OFF), (LOOP, OFF)]
    assert all(a[0] <= b[0] for a, b in zip(keys, keys[1:])), f"column {x}: keys not sorted"
    return keys


def blink_keys(color, period=2 * BLINK):
    keys = []
    blink(color, 0.0, period, keys)
    keys.append((period, keys[-1][1]))
    return keys, period


def color_at(keys, t):
    for (t0, c0), (t1, c1) in zip(keys, keys[1:]):
        if t0 <= t <= t1:
            f = 0 if t1 == t0 else (t - t0) / (t1 - t0)
            return tuple(round(a + (b - a) * f) for a, b in zip(c0, c1))
    return keys[-1][1]


# ── SVG ──

PITCH, LED, PAD, GAP, LABEL_H = 14, 10, 14, 28, 34
FONT_ATTR = ('font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif" '
             'font-size="15" fill="#8b949e" text-anchor="middle"')


def hexc(c):
    return "#%02x%02x%02x" % c


def animate_fill(keys, dur):
    times = ";".join(f"{t / dur:.5f}" for t, _ in keys)
    values = ";".join(hexc(c) for _, c in keys)
    return (f'<animate attributeName="fill" dur="{dur}s" repeatCount="indefinite" '
            f'keyTimes="{times}" values="{values}"/>')


def label_switch(texts, dur, cx, y):
    """Discrete opacity animation that shows one label per [start, end) window."""
    out = []
    for text, start, end in texts:
        vals, times = [], []
        if start > 0:
            vals.append("0"); times.append(0.0)
        vals.append("1"); times.append(start / dur)
        if end < dur:
            vals.append("0"); times.append(end / dur)
        out.append(
            f'<text x="{cx}" y="{y}" {FONT_ATTR} opacity="0">{text}'
            f'<animate attributeName="opacity" dur="{dur}s" repeatCount="indefinite" '
            f'calcMode="discrete" keyTimes="{";".join(f"{t:.5f}" for t in times)}" '
            f'values="{";".join(vals)}"/></text>')
    return out


def panel(ox, frame, bar_keys, dur, labels, at):
    side = MW * PITCH + 2 * PAD
    out = [f'<rect x="{ox}" y="0" width="{side}" height="{side}" rx="14" fill="#111214"/>']
    lit = []
    for y in range(MH):
        for x in range(MW):
            cx = ox + PAD + x * PITCH + PITCH / 2
            cy = PAD + y * PITCH + PITCH / 2
            circle = f'<circle cx="{cx}" cy="{cy}" r="{LED / 2}"'
            if y in BAR_ROWS:
                keys = bar_keys(x)
                if at is None:
                    lit.append(f'{circle} fill="{hexc(keys[0][1])}">{animate_fill(keys, dur)}</circle>')
                else:
                    lit.append(f'{circle} fill="{hexc(color_at(keys, at % dur))}"/>')
            elif frame[y][x]:
                lit.append(f'{circle} fill="{hexc(frame[y][x])}"/>')
            else:
                out.append(f'{circle} fill="{hexc(OFF)}"/>')
    out.append('<g filter="url(#glow)">' + "".join(lit) + "</g>")
    if at is None:
        out += label_switch(labels, dur, ox + side / 2, side + 24)
    else:
        t = at % dur
        text = next(l for l, s, e in labels if s <= t < e or e == dur and t >= s)
        out.append(f'<text x="{ox + side / 2}" y="{side + 24}" {FONT_ATTR}>{text}</text>')
    return out


def svg(at=None):
    frame = static_frame("21", "47", 7)
    paused, pdur = blink_keys(PAUSE)
    error, edur = blink_keys(ERROR)
    panels = [
        (lambda x: lifecycle_keys(x), LOOP,
         [("preparing", 0, PREP), ("printing", PREP, PREP + PRINT),
          ("finished", PREP + PRINT, PREP + PRINT + FINISH), ("idle", PREP + PRINT + FINISH, LOOP)]),
        (lambda x: paused, pdur, [("paused", 0, pdur)]),
        (lambda x: error, edur, [("error", 0, edur)]),
    ]
    side = MW * PITCH + 2 * PAD
    w = len(panels) * side + (len(panels) - 1) * GAP
    h = side + LABEL_H
    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
        f'viewBox="0 0 {w} {h}" role="img" aria-label="LED matrix preview">',
        '<defs><filter id="glow" x="-50%" y="-50%" width="200%" height="200%">'
        '<feGaussianBlur stdDeviation="2.2" result="b"/>'
        '<feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>'
        "</filter></defs>",
    ]
    for i, (keys, dur, labels) in enumerate(panels):
        out += panel(i * (side + GAP), frame, keys, dur, labels, at)
    out.append("</svg>")
    return "\n".join(out) + "\n"


def main():
    args = sys.argv[1:]
    at = None
    if args[:1] == ["--at"]:
        at, args = float(args[1]), args[2:]
    out = Path(args[0]) if args else DEFAULT_OUT
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(svg(at), encoding="utf-8")
    print(out)


if __name__ == "__main__":
    main()
