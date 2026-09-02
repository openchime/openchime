#!/usr/bin/env python3
"""Compare two captures of the same surface in two states.

    pair.py <before.tsv> <after.tsv> [--moved-only]

Some defects are invisible in one frame and obvious in two. A label that shifts
a pixel when its row is selected, a control that moves when the pointer enters
it, a whole column that slides when a badge appears -- each frame on its own
looks correct, and it is the DIFFERENCE that reads as a twitch.

The property asserted here is that changing a STATE must not change a POSITION.
Selecting a row swaps its label to the semibold format; that changes how wide
the string is and must not change where it starts. The two weights of a size
lay out to the same line box by construction (ARCH-108), so the seat is a
function of the rect and the size token alone -- and this is how that stays
true, rather than being true on the day it was arranged.

Rows are matched by their y and their kind, since x is the thing under test and
cannot also be the key.
"""

import sys

TOL = 0.5      # half a pixel: below this is rounding, not movement


def rows(path):
    out = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            r = line.rstrip("\n").split("\t")
            if len(r) < 13 or r[0] in ("seq",) or line.startswith("#"):
                continue
            out.append({"op": r[1], "x": float(r[2]), "y": float(r[3]),
                        "w": float(r[4]), "h": float(r[5]),
                        "rgb": r[10], "tag": r[13] if len(r) > 13 else ""})
    return out


def main(argv):
    if len(argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    a, b = rows(argv[1]), rows(argv[2])

    # Index by (op, rounded y, tag). Two rows of one kind on one line are rare
    # and, when they happen, ambiguous -- so those are dropped rather than
    # guessed at, which is the difference between "no finding" and a wrong one.
    def index(rs):
        seen = {}
        for r in rs:
            k = (r["op"], round(r["y"]), r["tag"])
            seen.setdefault(k, []).append(r)
        return {k: v[0] for k, v in seen.items() if len(v) == 1}

    ia, ib = index(a), index(b)
    moved = []
    for k in sorted(set(ia) & set(ib), key=lambda k: k[1]):
        ra, rb = ia[k], ib[k]
        dx = rb["x"] - ra["x"]
        if abs(dx) > TOL:
            moved.append((k, ra, rb, dx))

    for k, ra, rb, dx in moved:
        tag = f" [{ra['tag']}]" if ra["tag"] else ""
        print(f"MOVED {k[0]}{tag} at y={ra['y']:.0f}: x {ra['x']:.1f} -> "
              f"{rb['x']:.1f} ({dx:+.1f}px), width {ra['w']:.1f} -> {rb['w']:.1f}")
    if "--moved-only" not in argv:
        print(f"{len(ia)} matched primitives, {len(moved)} moved")
    return 1 if moved else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
