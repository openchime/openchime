#!/usr/bin/env python3
"""Prove every audit check can FAIL.

    selftest.py [capture.bmp]

A check that has never failed has not been shown to check anything. That is not
a slogan here: the client's own layout fit check spent months reporting phantom
overlaps that nobody read, and the first run of the ledger oracles reported
sixty-five findings on a healthy screen. Both were discovered by looking, not by
the checks saying anything about themselves.

So each check gets a case it must report. The ledger cases are synthetic
scenes -- a handful of rows describing exactly the defect. The pixel cases are
harder to fake and are the point: they hand a REAL capture a ledger that lies
about it in the specific way each check exists to catch, which is the same shape
as the real bug (the app asked for a shape, the renderer did not produce it).

Run it after touching anything in this directory. It needs no client.
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
HDR = "# scale=1.0000 out=1024x768 ops=0 lost=0\n" \
      "seq\top\tx\ty\tw\th\tcx\tcy\tcw\tch\trgb\ta\tradius\ttag\n"


def row(seq, op, x, y, w, h, clip=None, rgb="000000", a=1.0, rad=0.0, tag=""):
    c = clip or (0, 0, -1, -1)
    return (f"{seq}\t{op}\t{x:.2f}\t{y:.2f}\t{w:.2f}\t{h:.2f}\t"
            f"{c[0]:.2f}\t{c[1]:.2f}\t{c[2]:.2f}\t{c[3]:.2f}\t"
            f"{rgb}\t{a:.3f}\t{rad:.2f}\t{tag}\n")


def write(rows, suffix=".tsv"):
    f = tempfile.NamedTemporaryFile("w", suffix=suffix, delete=False)
    f.write(HDR + "".join(rows))
    f.close()
    return f.name


def run(script, *args):
    p = subprocess.run([sys.executable, os.path.join(HERE, script), *args],
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


# ---------------------------------------------------------------------------
# ledger cases: (name, expected substring, rows)
#
# Each is the smallest scene that is unambiguously the defect. A background
# fill first, so the checks that ask "what is behind this" have an answer.

BG = row(1, "clear", 0, 0, 0, 0, rgb="FFFFFF")
PANEL = row(2, "fill", 0, 0, 1024, 768, rgb="FFFFFF")

LEDGER_CASES = [
    ("text-clipped", "cut off the",
     [BG, PANEL,
      # a 200px string in a 100px box: half the sentence is not on screen
      row(3, "text", 100, 100, 200, 20, clip=(100, 100, 100, 20), rgb="000000")]),

    ("overprint", "shares pixels",
     [BG, PANEL,
      row(3, "text", 100, 100, 120, 20, rgb="000000"),
      row(4, "text", 140, 100, 120, 20, rgb="000000")]),

    ("contrast", "under 3:1",
     [BG, PANEL,
      row(3, "fill", 90, 90, 200, 40, rgb="FFFFFF"),
      row(4, "text", 100, 100, 80, 20, rgb="FEFEFE")]),

    ("offscreen", "outside the",
     [BG, PANEL, row(3, "fill", 1200, 100, 80, 20, rgb="112233")]),

    ("degenerate", "zero area",
     [BG, PANEL, row(3, "fill", 100, 100, 0, 20, rgb="112233")]),

    ("unthemed", "not a palette token",
     [BG, PANEL, row(3, "fill", 100, 100, 40, 40, rgb="AB12CD")]),

    # The published tree claims an element at 600,600; the ledger paints
    # nothing past 200,100. This is the shape of the stale-transcript defect.
    ("phantom-item", "nothing was drawn there",
     [BG, row(2, "fill", 0, 0, 200, 100, rgb="FFFFFF")]),
]

# The palette the contrast and palette checks read, as the state dump writes it.
DUMP = ("palette mode=1 light=1 n=18 accent=1264A3 accent_dim=0B4C7C "
        "rail=21324F sidebar=F3F4F6 base=FFFFFF header=FFFFFF input=FFFFFF "
        "select=C1D4E3 hover=E8E8E8 border=D5D8DE rail_icon=AAB4C8 "
        "text=1D1C1D muted=868A91 faint=9B9EA3 danger=B33A3A notice=8B5A00 "
        "online=2BAC76 away=E8A33D\n"
        "a11yitem message.7 600 600 800 640\n"
        "a11yitem rail.home 0 0 60 60\n")


def ledger_tests():
    dump = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False)
    dump.write(DUMP)
    dump.close()
    bad = 0
    for name, want, rows in LEDGER_CASES:
        path = write(rows)
        code, out = run("oracles.py", path, dump.name)
        if code == 1 and want in out:
            print(f"  ok   {name} is reported")
        else:
            print(f"  FAIL {name} was NOT reported (exit {code})")
            print("       " + out.strip().replace("\n", "\n       "))
            bad += 1
        os.unlink(path)
    os.unlink(dump.name)
    return bad


def pair_test():
    a = write([BG, PANEL, row(3, "text", 100, 100, 80, 20, rgb="000000")])
    b = write([BG, PANEL, row(3, "text", 101, 100, 84, 20, rgb="000000")])
    code, out = run("pair.py", a, b)
    ok = code == 1 and "MOVED" in out
    print(f"  {'ok  ' if ok else 'FAIL'} a label that shifts between states is reported")
    os.unlink(a)
    os.unlink(b)
    return 0 if ok else 1


def flat_block(img, side=46):
    """A patch of the capture with exactly one colour in it, so a synthetic
    shape placed there is measured against a known surround."""
    # Coarse first, then EVERY pixel. A patch that is uniform on a 4px lattice
    # can still have a hairline through it, and a hairline is enough to give a
    # synthetic disc a real edge to find -- which reports the check as broken
    # when it was the fixture that was.
    for y in range(60, img.h - side - 10, 8):
        for x in range(60, img.w - side - 10, 8):
            c = img.px(x, y)
            if c is None:
                continue
            if any(img.px(x + dx, y + dy) != c
                   for dy in range(0, side, 4) for dx in range(0, side, 4)):
                continue
            if any(img.px(x + dx, y + dy) != c
                   for dy in range(side) for dx in range(side)):
                continue
            return x, y, c
    return None


def pixel_tests(bmp):
    sys.path.insert(0, HERE)
    import pixels as P
    img = P.Img(bmp)
    spot = flat_block(img)
    if not spot:
        print("  SKIP the capture has no flat patch to place a synthetic shape in")
        return 0
    x, y, _ = spot
    bad = 0
    cases = [
        ("fill-gap", "interior samples differ",
         row(3, "fill_round", x + 2, y + 2, 40, 40, rgb="FF00FF", rad=4)),
        ("icon-blank", "drew nothing",
         row(3, "icon", x + 5, y + 5, 24, 24, rgb="000000")),
        ("hard-edge", "no antialiased rim",
         row(3, "ellipse", x + 5, y + 5, 30, 30, rgb="FF0000", rad=15)),
    ]
    for name, want, r in cases:
        path = write([r])
        code, out = run("pixels.py", path, bmp)
        if code == 1 and want in out:
            print(f"  ok   {name} is reported")
        else:
            print(f"  FAIL {name} was NOT reported (exit {code})")
            print("       " + out.strip().replace("\n", "\n       "))
            bad += 1
        os.unlink(path)
    return bad


def main(argv):
    print("== the ledger checks")
    bad = ledger_tests()
    print("== the state-pair check")
    bad += pair_test()
    print("== the pixel checks")
    if len(argv) > 1 and os.path.exists(argv[1]):
        bad += pixel_tests(argv[1])
    else:
        # Any capture the audit left behind will do: these cases describe a
        # shape the picture does not contain, so the picture's content does not
        # matter beyond having one flat patch in it.
        import glob
        found = sorted(glob.glob("/tmp/oc-audit/*.bmp"))
        if found:
            bad += pixel_tests(found[0])
        else:
            print("  SKIP no capture given and none in /tmp/oc-audit "
                  "(run gui_audit.sh first)")
    print()
    print("selftest: OK" if not bad else f"selftest: {bad} CHECK(S) DID NOT FIRE")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
