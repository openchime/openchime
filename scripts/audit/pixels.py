#!/usr/bin/env python3
"""Checks that need the PIXELS, not just the account of what was asked for.

    pixels.py <ledger.tsv> <capture.bmp>

The ledger says what the app asked the renderer to draw. These ask whether the
renderer did it. That gap is real and has been expensive: the stroke
tessellator once put a round-join disc at every flattened bezier point, blew the
vertex budget on nine of the nineteen icons, and DISCARDED THEM WHOLE -- the
draw call happened, the ledger would have recorded it, and nothing appeared.
Only the picture knows.

Three questions, each asked only of shapes NOTHING WAS DRAWN OVER afterwards.
That filter is what makes the answers trustworthy: a fill with a label on top of
it is supposed to differ from its own colour, and a check that did not know
which those were would report every button in the app.

No reference image is involved here either. Each shape is compared against what
its own ledger row says it should be.
"""

import struct
import sys


# ---------------------------------------------------------------------------
# a bottom-up 24/32-bit BMP, which is what the capture verb writes


class Img:
    def __init__(self, path):
        d = open(path, "rb").read()
        off = struct.unpack_from("<I", d, 10)[0]
        hdr = struct.unpack_from("<I", d, 14)[0]
        w, h = struct.unpack_from("<ii", d, 18)
        bpp = struct.unpack_from("<H", d, 28)[0]
        self.w, self.h = w, abs(h)
        self.flip = h > 0                      # positive height = bottom-up
        self.bpp = bpp // 8
        self.stride = ((w * bpp + 31) // 32) * 4
        self.d = d
        self.off = off + (hdr - 40 if hdr > 40 else 0)

    def px(self, x, y):
        if not (0 <= x < self.w and 0 <= y < self.h):
            return None
        row = (self.h - 1 - y) if self.flip else y
        i = self.off + row * self.stride + x * self.bpp
        b, g, r = self.d[i], self.d[i + 1], self.d[i + 2]
        return (r << 16) | (g << 8) | b


def near(a, b, tol=12):
    return (abs(((a >> 16) & 255) - ((b >> 16) & 255)) <= tol and
            abs(((a >> 8) & 255) - ((b >> 8) & 255)) <= tol and
            abs((a & 255) - (b & 255)) <= tol)


def lum(c):
    return 0.299 * ((c >> 16) & 255) + 0.587 * ((c >> 8) & 255) + 0.114 * (c & 255)


# ---------------------------------------------------------------------------


def read(path):
    ops, meta = [], {}
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("#"):
            for kv in line[1:].split():
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    meta[k] = v
            continue
        r = line.rstrip("\n").split("\t")
        if len(r) < 13 or r[0] == "seq":
            continue
        ops.append({"i": len(ops), "op": r[1],
                    "x": float(r[2]), "y": float(r[3]),
                    "w": float(r[4]), "h": float(r[5]),
                    "cw": float(r[8]), "ch": float(r[9]),
                    "rgb": None if r[10] == "-" else int(r[10], 16),
                    "a": float(r[11]), "radius": float(r[12]),
                    "tag": r[13] if len(r) > 13 else ""})
    return meta, ops


def box(o):
    return (o["x"], o["y"], o["x"] + o["w"], o["y"] + o["h"])


def hits(a, b):
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


def on_image(img, o):
    """Is the whole shape inside the capture? Sampling outside it returns
    nothing, and "nothing" read as "one flat colour" reported every icon drawn
    off the window edge as an icon that failed to draw -- true in a sense, and
    the wrong sense: that is the offscreen finding, already reported once, and
    saying it twice in different words makes a list harder to act on."""
    return (o["x"] >= 0 and o["y"] >= 0 and
            o["x"] + o["w"] <= img.w and o["y"] + o["h"] <= img.h)


def untouched(ops, i):
    """Nothing after op `i` overlaps it. Only these can be measured against
    their own declared colour -- everything else is supposed to have changed."""
    r = box(ops[i])
    for j in range(i + 1, len(ops)):
        o = ops[j]
        if o["w"] <= 0 or o["h"] <= 0:
            continue
        if hits(r, box(o)):
            return False
    return True


# ---------------------------------------------------------------------------
# the checks


def check_fill_gaps(img, ops, out):
    """A filled shape should BE its colour. Holes in one mean the tessellation
    left the interior uncovered -- the faceted-corner and dropped-triangle
    class."""
    for i, o in enumerate(ops):
        if o["op"] not in ("fill_round", "fill") or o["a"] < 0.99:
            continue
        if o["rgb"] is None or o["w"] < 8 or o["h"] < 8:
            continue
        if not on_image(img, o) or not untouched(ops, i):
            continue
        # Inset past the corner arcs: the corners are supposed to be
        # background, so sampling them would report every rounded rect.
        pad = max(2.0, o["radius"] + 1.0)
        x0, y0 = int(o["x"] + pad), int(o["y"] + pad)
        x1, y1 = int(o["x"] + o["w"] - pad), int(o["y"] + o["h"] - pad)
        if x1 - x0 < 3 or y1 - y0 < 3:
            continue
        bad = 0
        total = 0
        for y in range(y0, y1, max(1, (y1 - y0) // 12)):
            for x in range(x0, x1, max(1, (x1 - x0) // 12)):
                p = img.px(x, y)
                if p is None:
                    continue
                total += 1
                if not near(p, o["rgb"]):
                    bad += 1
        if total and bad > total * 0.1:
            out.append(f"fill-gap: #{i + 1} {o['op']} at {o['x']:.0f},{o['y']:.0f} "
                       f"{o['w']:.0f}x{o['h']:.0f} declared {o['rgb']:06X} but "
                       f"{bad}/{total} interior samples differ")


def check_icon_ink(img, ops, out):
    """An icon that drew nothing leaves its box exactly as it found it. This is
    the check the vertex-budget bug needed and did not have."""
    for i, o in enumerate(ops):
        if o["op"] != "icon" or o["w"] < 6 or o["h"] < 6:
            continue
        if not on_image(img, o) or not untouched(ops, i):
            continue
        x0, y0 = int(o["x"]), int(o["y"])
        x1, y1 = int(o["x"] + o["w"]), int(o["y"] + o["h"])
        seen = set()
        for y in range(y0, y1):
            for x in range(x0, x1):
                p = img.px(x, y)
                if p is not None:
                    seen.add(p)
                if len(seen) > 3:
                    break
            if len(seen) > 3:
                break
        if len(seen) <= 1:
            out.append(f"icon-blank: #{i + 1} icon at {o['x']:.0f},{o['y']:.0f} "
                       f"{o['w']:.0f}x{o['h']:.0f} drew nothing — its box is one "
                       "flat colour")


def check_disc_edges(img, ops, out):
    """A disc's rim should be a RAMP. SDL_RenderGeometry has no antialiasing, so
    a circle built from chords comes out a staircase -- which is why discs draw
    from a coverage texture. If a rim is a hard step, something has fallen back
    to raw geometry, and it will read as a jagged dot beside a face."""
    for i, o in enumerate(ops):
        if o["op"] != "ellipse" or o["a"] < 0.99 or o["rgb"] is None:
            continue
        r = min(o["w"], o["h"]) / 2.0
        if r < 5 or not on_image(img, o) or not untouched(ops, i):
            continue
        cx, cy = o["x"] + o["w"] / 2.0, o["y"] + o["h"] / 2.0
        inside = o["rgb"]
        # Walk out along four diagonals; a ramp shows at least one sample that
        # is neither the fill nor the surround.
        ramps = 0
        probes = 0
        for dx, dy in ((0.7071, 0.7071), (-0.7071, 0.7071),
                       (0.7071, -0.7071), (-0.7071, -0.7071)):
            outer = img.px(int(cx + dx * (r + 3)), int(cy + dy * (r + 3)))
            if outer is None or near(outer, inside):
                continue
            probes += 1
            for t in (-1.0, -0.5, 0.0, 0.5, 1.0):
                p = img.px(int(cx + dx * (r + t)), int(cy + dy * (r + t)))
                if p is None:
                    continue
                if not near(p, inside, 8) and not near(p, outer, 8):
                    ramps += 1
                    break
        if probes >= 3 and ramps == 0:
            out.append(f"hard-edge: #{i + 1} ellipse at {o['x']:.0f},{o['y']:.0f} "
                       f"r={r:.0f} has no antialiased rim — every sampled edge "
                       "steps straight from fill to background")


def main(argv):
    if len(argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    meta, ops = read(argv[1])
    try:
        img = Img(argv[2])
    except (OSError, struct.error) as e:
        sys.stderr.write(f"pixels: {argv[2]}: {e}\n")
        return 2

    # The capture is the whole WINDOW; the ledger is the client area. When they
    # disagree the ledger's coordinates do not address the picture, and every
    # sample would be taken from the wrong place -- so say so instead of
    # reporting nonsense.
    out_w = meta.get("out", "").split("x")[0]
    if out_w and abs(int(float(out_w)) - img.w) > 2:
        sys.stderr.write(f"pixels: capture is {img.w}px wide but the ledger "
                         f"describes {out_w}px — not comparable\n")
        return 2

    findings = []
    check_fill_gaps(img, ops, findings)
    check_icon_ink(img, ops, findings)
    check_disc_edges(img, ops, findings)
    for line in findings[:8]:
        print(line)
    if len(findings) > 8:
        print(f"... and {len(findings) - 8} more")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
