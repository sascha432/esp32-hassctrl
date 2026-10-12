#!/usr/bin/env python3
"""Builds the Home Assistant logo of the start screen (docs/ui/lvgl/lvgl-dashboard.md section 1.2) from its SVG.

    python tools/build_logo.py

The output is committed, so the build needs neither Pillow nor the network; run this only when
the logo, its size or the runner's track change:

    components/ui/src/logo.inc   two 4-bit alpha masks (the white house, the blue face) and the
                                 track the runner follows round the house

The masks are drawn in a color the screen chooses (LV_IMG_CF_ALPHA_4BIT), so they do not depend on
the byte order of a board's panel. The SVG is the icon of dashboardicons.com
(github.com/homarr-labs/dashboard-icons, svg/home-assistant.svg); its two paths are copied below.

Needs Pillow.
"""

import math
import re
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "components" / "ui" / "src" / "logo.inc"

VIEWBOX = 512.0
# the logo in pixels on every screen
SIZE = 160
# the runner runs this far outside the house's edge, so its glow does not drown in the blue
TRACK_OFFSET = 5.0
# the room round the logo for the track and the glow (runner.cpp: kGlow)
MARGIN = 16
# about this far apart are the points of the track
TRACK_STEP = 3.0
# the masks are drawn this many times larger and scaled down: the anti-aliasing
SUPERSAMPLE = 8

# the whole house (white)
HOUSE = ("M512 473.3c0 17.6-14.4 32-32 32H32c-17.6 0-32-14.4-32-32v-192c0-17.6 10.2-42.2 22.6-54.6"
         "L233.4 16c12.4-12.4 32.8-12.4 45.2 0l210.8 210.8c12.4 12.4 22.6 37 22.6 54.6z")
# the blue face: the house without the circuit tree
FACE = ("M489.4 226.7 278.6 16c-12.4-12.4-32.8-12.4-45.2 0L22.6 226.7C10.2 239.1 0 263.7 0 281.3v192"
        "c0 17.6 14.4 32 32 32h196.8l-86.7-86.7c-4.5 1.5-9.2 2.4-14.2 2.4-24.1 0-43.7-19.6-43.7-43.7"
        "s19.6-43.7 43.7-43.7 43.7 19.6 43.7 43.7c0 5-.9 9.7-2.4 14.2l67.5 67.5V211.8"
        "c-14.5-7.1-24.5-22-24.5-39.2 0-24.1 19.6-43.7 43.7-43.7s43.7 19.6 43.7 43.7"
        "c0 17.2-10 32.1-24.5 39.2v173.4l67.1-67.1c-1.3-4.2-2-8.6-2-13.2 0-24.1 19.6-43.7 43.7-43.7"
        "s43.7 19.6 43.7 43.7-19.6 43.7-43.7 43.7c-5.3 0-10.4-1-15.1-2.8l-93.7 93.7v65.9H480"
        "c17.6 0 32-14.4 32-32v-192c0-17.6-10.2-42.2-22.6-54.7")

CURVE_STEPS = 24
PARAMS = {"M": 2, "L": 2, "H": 1, "V": 1, "C": 6, "S": 4, "Z": 0}


def flatten(path):
    """The path as one polygon (both paths have a single contour): M L H V C S Z, absolute and relative."""
    tokens = re.findall(r"[A-Za-z]|-?(?:\d+\.?\d*|\.\d+)", path)
    points = []
    x = y = 0.0
    last_control = None
    command = None
    i = 0
    while i < len(tokens):
        if tokens[i].isalpha():
            command = tokens[i]
            i += 1
            if command in "Zz":
                last_control = None
                continue
        upper = command.upper()
        relative = command.islower()
        args = [float(t) for t in tokens[i:i + PARAMS[upper]]]
        i += PARAMS[upper]
        ox, oy = (x, y) if relative else (0.0, 0.0)
        if upper == "M":
            x, y = ox + args[0], oy + args[1]
            points.append((x, y))
            # more pairs after a move are lines
            command = "l" if relative else "L"
            last_control = None
        elif upper == "L":
            x, y = ox + args[0], oy + args[1]
            points.append((x, y))
            last_control = None
        elif upper == "H":
            x = (x if relative else 0.0) + args[0]
            points.append((x, y))
            last_control = None
        elif upper == "V":
            y = (y if relative else 0.0) + args[0]
            points.append((x, y))
            last_control = None
        else:
            if upper == "C":
                c1 = (ox + args[0], oy + args[1])
                c2 = (ox + args[2], oy + args[3])
                end = (ox + args[4], oy + args[5])
            else:  # S: the first control point mirrors the last one
                c1 = (2 * x - last_control[0], 2 * y - last_control[1]) if last_control else (x, y)
                c2 = (ox + args[0], oy + args[1])
                end = (ox + args[2], oy + args[3])
            start = (x, y)
            for step in range(1, CURVE_STEPS + 1):
                t = step / CURVE_STEPS
                u = 1 - t
                points.append(tuple(u * u * u * start[k] + 3 * u * u * t * c1[k] + 3 * u * t * t * c2[k] + t * t * t * end[k]
                                    for k in range(2)))
            x, y = end
            last_control = c2
    return points


def mask(polygon):
    """4-bit alpha of the filled polygon, SIZE x SIZE, rows of SIZE / 2 bytes (high nibble first)."""
    scale = SIZE * SUPERSAMPLE / VIEWBOX
    big = Image.new("L", (SIZE * SUPERSAMPLE, SIZE * SUPERSAMPLE), 0)
    ImageDraw.Draw(big).polygon([(px * scale, py * scale) for px, py in polygon], fill=255)
    return big.reduce(SUPERSAMPLE)


def pack4(image):
    data = bytearray()
    pixels = image.load()
    for row in range(SIZE):
        for col in range(0, SIZE, 2):
            high = (pixels[col, row] + 8) // 17
            low = (pixels[col + 1, row] + 8) // 17
            data.append(min(high, 15) << 4 | min(low, 15))
    return data


def resample(points, step):
    """The closed polygon as points `step` apart (the last step is shortened to close it)."""
    closed = points + [points[0]]
    lengths = [math.dist(closed[k], closed[k + 1]) for k in range(len(points))]
    total = sum(lengths)
    count = max(3, round(total / step))
    step = total / count
    out = []
    segment = 0
    walked = 0.0
    for n in range(count):
        target = n * step
        while walked + lengths[segment] < target:
            walked += lengths[segment]
            segment += 1
        t = (target - walked) / lengths[segment] if lengths[segment] else 0.0
        a, b = closed[segment], closed[segment + 1]
        out.append((a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])))
    return out, step


def track():
    """The house's outline in pixels of the logo's box, TRACK_OFFSET outside, clockwise on the screen."""
    scale = SIZE / VIEWBOX
    outline = [(px * scale + MARGIN, py * scale + MARGIN) for px, py in flatten(HOUSE)]
    # drop the repeated closing point
    if math.dist(outline[0], outline[-1]) < 1e-6:
        outline.pop()
    dense, _ = resample(outline, 0.5)
    cx = sum(p[0] for p in dense) / len(dense)
    cy = sum(p[1] for p in dense) / len(dense)
    # the house is convex: every point moves out along the normal of its neighbours' chord
    offset = []
    n = len(dense)
    for k, (px, py) in enumerate(dense):
        ax, ay = dense[k - 4]
        bx, by = dense[(k + 4) % n]
        tx, ty = bx - ax, by - ay
        length = math.hypot(tx, ty)
        nx, ny = ty / length, -tx / length
        if nx * (px - cx) + ny * (py - cy) < 0:
            nx, ny = -nx, -ny
        offset.append((px + nx * TRACK_OFFSET, py + ny * TRACK_OFFSET))
    # clockwise on the screen (y down): positive shoelace sum
    area = sum(offset[k][0] * offset[(k + 1) % n][1] - offset[(k + 1) % n][0] * offset[k][1] for k in range(n))
    if area < 0:
        offset.reverse()
    # start at the top, where the runner on a card starts too
    top = min(range(n), key=lambda k: offset[k][1])
    offset = offset[top:] + offset[:top]
    return resample(offset, TRACK_STEP)


def hex_rows(data, per_row=20):
    return "\n".join("    " + ", ".join(f"0x{b:02x}" for b in data[k:k + per_row]) + ","
                     for k in range(0, len(data), per_row))


def main():
    house = mask(flatten(HOUSE))
    face = mask(flatten(FACE))
    # the white only where the blue is not, so the edges of the two do not add up to a white seam
    white = Image.new("L", (SIZE, SIZE))
    hp, fp, wp = house.load(), face.load(), white.load()
    for row in range(SIZE):
        for col in range(SIZE):
            wp[col, row] = max(0, hp[col, row] - fp[col, row])
    points, step = track()

    text = f"""// Generated by tools/build_logo.py from the Home Assistant icon of dashboardicons.com; do not edit.
// The logo is kLogoSize x kLogoSize at (kLogoMargin, kLogoMargin) of a kLogoBox square box; the
// masks are 4-bit alpha, two pixels per byte. The track lies {TRACK_OFFSET:g} px outside the house, in
// pixels of the box, {step:.3f} px from point to point, clockwise from the top.

constexpr lv_coord_t kLogoSize = {SIZE};
constexpr lv_coord_t kLogoMargin = {MARGIN};
constexpr lv_coord_t kLogoBox = {SIZE + 2 * MARGIN};

// the house where the face leaves it free: the circuit tree
constexpr uint8_t kLogoTree[] = {{
{hex_rows(pack4(white))}
}};

// the blue face
constexpr uint8_t kLogoFace[] = {{
{hex_rows(pack4(face))}
}};

constexpr runner::Point kLogoTrack[] = {{
{chr(10).join(f"    {{{x:.2f}f, {y:.2f}f}}," for x, y in points)}
}};
constexpr float kLogoTrackStep = {step:.4f}f;
"""
    OUTPUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"{OUTPUT.relative_to(ROOT)}: {SIZE}x{SIZE}, track of {len(points)} points, step {step:.2f} px")


if __name__ == "__main__":
    main()
