#!/usr/bin/env python3
"""Contact sheets, organised BY CLASS rather than by screen.

    sheets.py [capture-dir] [-o out-dir]

Some defects no check will ever find. "A 13px marker on an 18px tile looks bad"
is not a property; it is a judgement, and it needs eyes. The four defects logged
against this audit by hand were all of that kind, and they were found by someone
scrolling through screenshots noticing that one thing looked unlike its
siblings.

So this does not try to replace the eye. It changes what the eye is asked to do.
Reviewing 190 screenshots one at a time is how a difference between two of them
gets missed; the same material cut into "every icon at every size on one page"
and "every avatar composite at every tile size on another" turns the same
question into a glance, because a thing that is wrong is now sitting next to
eleven things that are right.

The crops come from the paint ledger, so a sheet knows what each tile IS -- an
icon box, a disc, a rounded corner -- rather than being a grid of guesses.

Sheets produced:
  survey-<state>.png   every scene in one state, whole windows, labelled
  icons.png            every icon, magnified, grouped by drawn size
  avatars.png          every avatar composite and its presence marker
  corners.png          the top-left corner of every rounded rect, magnified
"""

import glob
import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.stderr.write("sheets: needs Pillow (pip install pillow)\n")
    sys.exit(2)

PAD = 6
LABEL = 11


def read_ledger(path):
    ops = []
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("#") or line.startswith("seq"):
            continue
        r = line.rstrip("\n").split("\t")
        if len(r) < 13:
            continue
        ops.append({"op": r[1], "x": float(r[2]), "y": float(r[3]),
                    "w": float(r[4]), "h": float(r[5]),
                    "rgb": r[10], "radius": float(r[12]),
                    "tag": r[13] if len(r) > 13 else ""})
    return ops


def captures(d):
    """(state, scene, image path, ledger path) for everything the audit left."""
    out = []
    for bmp in sorted(glob.glob(os.path.join(d, "*.bmp"))):
        base = bmp[:-4]
        led = base + ".ledger.tsv"
        if not os.path.exists(led):
            continue
        name = os.path.basename(base)
        state, _, scene = name.partition(".")
        out.append((state, scene, bmp, led))
    return out


def grid(tiles, cols, title, bg=(24, 24, 27), fg=(230, 230, 235)):
    """`tiles` is a list of (label, PIL image)."""
    if not tiles:
        return None
    tw = max(t[1].width for t in tiles)
    th = max(t[1].height for t in tiles)
    rows = (len(tiles) + cols - 1) // cols
    W = cols * (tw + PAD) + PAD
    H = rows * (th + PAD + LABEL) + PAD + 20
    sheet = Image.new("RGB", (W, H), bg)
    d = ImageDraw.Draw(sheet)
    d.text((PAD, 5), title, fill=fg)
    for i, (label, im) in enumerate(tiles):
        cx = PAD + (i % cols) * (tw + PAD)
        cy = 20 + PAD + (i // cols) * (th + PAD + LABEL)
        sheet.paste(im, (cx, cy))
        d.text((cx, cy + th + 1), label[:max(8, tw // 6)], fill=fg)
    return sheet


def crop(img, o, pad=2, scale=1, corner=None):
    """The shape, or -- when `corner` is a size -- just that much of its
    top-left. A card is a rounded rect too, and magnifying a 720x620 modal four
    times produces a tile nothing can open; what a reviewer is looking at on a
    corner sheet is the arc, which is a few pixels across whatever it belongs
    to."""
    x0 = max(0, int(o["x"]) - pad)
    y0 = max(0, int(o["y"]) - pad)
    if corner:
        x1 = min(img.width, x0 + corner)
        y1 = min(img.height, y0 + corner)
    else:
        x1 = min(img.width, int(o["x"] + o["w"]) + pad)
        y1 = min(img.height, int(o["y"] + o["h"]) + pad)
    if x1 - x0 < 2 or y1 - y0 < 2:
        return None
    c = img.crop((x0, y0, x1, y1))
    if scale > 1:
        # NEAREST on purpose: this is for judging edges and alignment, and a
        # smooth resample would invent the antialiasing the reviewer is here to
        # look for.
        c = c.resize((c.width * scale, c.height * scale), Image.NEAREST)
    return c


def sheet_survey(caps, outdir):
    by_state = {}
    for state, scene, bmp, _ in caps:
        by_state.setdefault(state, []).append((scene, bmp))
    made = []
    for state, items in sorted(by_state.items()):
        tiles = []
        for scene, bmp in sorted(items):
            im = Image.open(bmp).convert("RGB")
            im.thumbnail((300, 300))
            tiles.append((scene, im))
        s = grid(tiles, 5, f"survey — {state}")
        if s:
            p = os.path.join(outdir, f"survey-{state}.png")
            s.save(p)
            made.append(p)
    return made


def sheet_class(caps, outdir, name, want, cols=12, scale=3, cap=180,
                corner=None, maxpx=80):
    """One sheet of every primitive `want` accepts, from every capture.

    `maxpx` keeps a tile a tile. These sheets are for judging small detail --
    an arc, a marker, a stroke weight -- and one 600px element magnified beside
    forty 20px ones is not a comparison, it is a wall."""
    tiles = []
    for state, scene, bmp, led in caps:
        img = None
        for o in read_ledger(led):
            if not want(o):
                continue
            if not corner and (o["w"] > maxpx or o["h"] > maxpx):
                continue
            if img is None:
                img = Image.open(bmp).convert("RGB")
            c = crop(img, o, pad=3, scale=scale, corner=corner)
            if c is None:
                continue
            tiles.append((f"r{o['radius']:.0f} {int(o['w'])}px"
                          if corner else
                          f"{int(o['w'])}px {state.split('-')[0]}", c))
            if len(tiles) >= cap:
                break
        if len(tiles) >= cap:
            break
    # Grouped by drawn size, so like sits beside like -- that adjacency is the
    # whole reason the sheet exists.
    tiles.sort(key=lambda t: t[0])
    s = grid(tiles, cols, f"{name} — {len(tiles)} of them, magnified {scale}x")
    if not s:
        return None
    p = os.path.join(outdir, f"{name}.png")
    s.save(p)
    return p


def main(argv):
    d = "/tmp/oc-audit"
    outdir = None
    args = [a for a in argv[1:]]
    if args and not args[0].startswith("-"):
        d = args.pop(0)
    if "-o" in args:
        outdir = args[args.index("-o") + 1]
    outdir = outdir or os.path.join(d, "sheets")
    os.makedirs(outdir, exist_ok=True)

    caps = captures(d)
    if not caps:
        sys.stderr.write(f"sheets: no captures in {d} — run gui_audit.sh first\n")
        return 2

    made = sheet_survey(caps, outdir)
    for p in (
        sheet_class(caps, outdir, "icons",
                    lambda o: o["op"] == "icon", cols=14, scale=3),
        sheet_class(caps, outdir, "avatars",
                    lambda o: o["tag"] == "content:avatar" and o["op"] != "text",
                    cols=12, scale=4, cap=120, maxpx=48),
        sheet_class(caps, outdir, "corners",
                    lambda o: o["op"] == "fill_round" and o["radius"] >= 4
                    and o["w"] >= 16 and o["h"] >= 16, cols=12, scale=5,
                    corner=18),
    ):
        if p:
            made.append(p)

    for p in made:
        print(p)
    print(f"{len(made)} sheets from {len(caps)} captures")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
