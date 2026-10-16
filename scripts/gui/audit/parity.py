#!/usr/bin/env python3
"""Compare one scene as the Win32 client and the web client painted it.

    parity.py <win32.tsv> <web.tsv> [--tol PX] [--quiet]

The web client is the Win32 application compiled for the browser: the same
layout code draws both, so the two paint ledgers of one scene, at one size,
theme, scale and text size, should hold the same primitives in the same order
at the same places. Where they do not, one of the platforms differs -- a text
measured by another engine, a service the browser lacks, a size the page
decides -- and that is what this is for (scripts/gui/parity.sh).

The ledgers are aligned on their sequence of operations, so one primitive drawn
on one side only shows as itself rather than shifting every row after it. Of
the aligned pairs, a box that moved or changed size by more than the tolerance,
a different ink, or a different alpha is reported. Text is the one primitive
whose width is the platform's (DirectWrite on Windows, the browser's canvas on
the web), so text widths are reported apart from layout, with the largest
first: a few pixels of text width is rendering; a box that moved is layout.

Exit status: 0 when nothing but text widths differs beyond the tolerance, 1
otherwise.
"""

import difflib
import sys

GEOM = ("x", "y", "w", "h")


def load(path):
    head, rows = "", []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#"):
                head = line.strip()
                continue
            r = line.rstrip("\n").split("\t")
            if r[0] == "seq" or len(r) < 13:
                continue
            rows.append({"op": r[1], "x": float(r[2]), "y": float(r[3]), "w": float(r[4]),
                         "h": float(r[5]), "rgb": r[10], "a": float(r[11]),
                         "tag": r[13] if len(r) > 13 else ""})
    return head, rows


def key(o):
    # What a primitive IS, for the alignment: its kind and where, coarsely, so a
    # text a pixel wider still pairs with its counterpart.
    return (o["op"], round(o["x"] / 8), round(o["y"] / 8))


def where(o):
    return "%s at %.0f,%.0f %.0fx%.0f%s" % (o["op"], o["x"], o["y"], o["w"], o["h"],
                                           " [" + o["tag"] + "]" if o["tag"] else "")


def compare(a, b, tol):
    out = {"only_win": [], "only_web": [], "moved": [], "ink": [], "text": []}
    sm = difflib.SequenceMatcher(None, [key(o) for o in a], [key(o) for o in b], autojunk=False)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal" or (tag == "replace" and i2 - i1 == j2 - j1):
            pairs = zip(a[i1:i2], b[j1:j2])
        else:
            out["only_win"] += a[i1:i2]
            out["only_web"] += b[j1:j2]
            continue
        for p, q in pairs:
            if p["op"] != q["op"]:
                out["only_win"].append(p)
                out["only_web"].append(q)
                continue
            d = {g: q[g] - p[g] for g in GEOM}
            if p["op"] == "text":
                # Where it sits is layout; how wide it is, the text engine's. A
                # text is placed by its centre: DirectWrite's ledger row is the
                # inked run, the canvas backend's the line box it was aligned
                # in, and a centred label is the same label either way.
                dc = (q["x"] + q["w"] / 2) - (p["x"] + p["w"] / 2)
                dm = (q["y"] + q["h"] / 2) - (p["y"] + p["h"] / 2)
                if (abs(d["x"]) > tol and abs(dc) > tol + abs(d["w"]) / 2) or abs(dm) > tol + 1:
                    out["moved"].append((p, q, d))
                elif abs(d["w"]) > tol or abs(d["h"]) > tol:
                    out["text"].append((p, q, d))
            elif any(abs(v) > tol for v in d.values()):
                out["moved"].append((p, q, d))
            if p["rgb"] != q["rgb"] or abs(p["a"] - q["a"]) > 0.02:
                if "transient" not in p["tag"]:
                    out["ink"].append((p, q))
    return out


def main(argv):
    tol, quiet, files = 1.0, False, []
    it = iter(argv[1:])
    for x in it:
        if x == "--tol":
            tol = float(next(it))
        elif x == "--quiet":
            quiet = True
        else:
            files.append(x)
    if len(files) != 2:
        sys.stderr.write(__doc__)
        return 2
    ha, a = load(files[0])
    hb, b = load(files[1])
    # A caret's blink is a phase, not a rendering: two captures a moment apart
    # differ in it on one platform as much as between two.
    a = [o for o in a if not o["tag"].startswith("transient")]
    b = [o for o in b if not o["tag"].startswith("transient")]
    r = compare(a, b, tol)
    n = sum(len(v) for v in r.values())
    print("parity: win32 %d ops, web %d ops; %d drawn on one side only, %d moved, %d ink, %d text widths"
          % (len(a), len(b), len(r["only_win"]) + len(r["only_web"]), len(r["moved"]), len(r["ink"]), len(r["text"])))
    if ha.split(" ops=")[0] != hb.split(" ops=")[0]:
        print("  sizes differ: win32 '%s', web '%s'" % (ha, hb))
    if not quiet:
        for o in r["only_win"]:
            print("  win32 only: " + where(o))
        for o in r["only_web"]:
            print("  web only:   " + where(o))
        for p, q, d in r["moved"]:
            print("  moved:      %s -> %+.0f,%+.0f %+.0fx%+.0f" % (where(p), d["x"], d["y"], d["w"], d["h"]))
        for p, q in r["ink"]:
            print("  ink:        %s: %s/%.2f vs %s/%.2f" % (where(p), p["rgb"], p["a"], q["rgb"], q["a"]))
        for p, q, d in sorted(r["text"], key=lambda t: -abs(t[2]["w"]))[:12]:
            print("  text width: %s: %+.1f px" % (where(p), d["w"]))
    # Text widths are reported, not failed: they are the two text engines, and
    # the picture (scripts/gui/parity.sh) is what says whether they read alike.
    return 0 if n - len(r["text"]) == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
