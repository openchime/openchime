#!/usr/bin/env python3
"""Absolute checks over one captured scene — no reference render involved.

The audit this belongs to used to work by diffing the SDL render against the
Direct2D binary it replaced. That rig found what it could find and is finished:
a diff sees a DIVERGENCE, so it is blind by construction to anything both
renderers did the same way, and the defects left are exactly those. A label
clipped to nothing, ink the colour of the surface behind it, two strings drawn
over each other — all of them produce a picture a diff calls identical.

So the reference is gone and these take its place. Each one is a property the
scene must hold on its own terms, read off the app's own account of what it
drew (the paint ledger, `gfx_ledger_dump`) and its own account of what it
published (the state dump). A check here can be run against one binary, with
nothing to compare to, which is the only reason the audit can continue at all.

    oracles.py <ledger.tsv> <dump.txt> [--json]

Exits 0 when every check passes, 1 when any fails, 2 on bad input.
"""

import json
import sys

# ---------------------------------------------------------------------------
# reading


class Op:
    __slots__ = ("seq", "op", "x", "y", "w", "h", "cx", "cy", "cw", "ch",
                 "rgb", "a", "radius", "tag")

    def __init__(self, row):
        (seq, op, x, y, w, h, cx, cy, cw, ch, rgb, a, radius) = row[:13]
        self.seq = int(seq)
        self.op = op
        self.x, self.y, self.w, self.h = map(float, (x, y, w, h))
        self.cx, self.cy, self.cw, self.ch = map(float, (cx, cy, cw, ch))
        # "-" means the colour is genuinely not knowable from a texture's
        # pixels. Not the same as black, and never silently treated as it.
        self.rgb = None if rgb == "-" else int(rgb, 16)
        self.a = float(a)
        self.radius = float(radius)
        self.tag = row[13] if len(row) > 13 else ""

    @property
    def r(self):
        return (self.x, self.y, self.x + self.w, self.y + self.h)

    @property
    def clip(self):
        if self.cw < 0:
            return None
        return (self.cx, self.cy, self.cx + self.cw, self.cy + self.ch)

    def where(self):
        t = f" [{self.tag}]" if self.tag else ""
        return f"#{self.seq} {self.op}{t} at {self.x:.0f},{self.y:.0f} " \
               f"{self.w:.0f}x{self.h:.0f}"


def read_ledger(path):
    meta, ops = {}, []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("#"):
                for kv in line[1:].split():
                    if "=" in kv:
                        k, v = kv.split("=", 1)
                        meta[k] = v
                continue
            row = line.split("\t")
            if row[0] == "seq" or len(row) < 13:
                continue
            ops.append(Op(row))
    return meta, ops


def read_a11y(path):
    """The published accessibility tree: (id, rect) per element, from the dump."""
    out = []
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if not line.startswith("a11yitem "):
                    continue
                p = line.split()
                if len(p) >= 6:
                    out.append((p[1], tuple(float(v) for v in p[2:6])))
    except OSError:
        pass
    return out


def read_palette(path):
    """The theme in force, token name -> rgb, from the state dump."""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("palette "):
                    out = {}
                    for kv in line.split()[1:]:
                        k, _, v = kv.partition("=")
                        if len(v) == 6:
                            try:
                                out[k] = int(v, 16)
                            except ValueError:
                                pass
                    return out
    except OSError:
        pass
    return {}


# ---------------------------------------------------------------------------
# geometry and colour


def isect(a, b):
    return (max(a[0], b[0]), max(a[1], b[1]),
            min(a[2], b[2]), min(a[3], b[3]))


def empty(r):
    return r[2] - r[0] <= 0.01 or r[3] - r[1] <= 0.01


def contains(outer, inner, slack=0.5):
    """`inner` sits inside `outer`. The slack is half a pixel: a clip is stored
    as whole pixels (floor/ceil) and a rect is not, so an exact containment test
    reports a rounding difference as a clipped label."""
    return (inner[0] >= outer[0] - slack and inner[1] >= outer[1] - slack and
            inner[2] <= outer[2] + slack and inner[3] <= outer[3] + slack)


def overlap(a, b):
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


def is_colour_glyph(o):
    """A raster whose pixels are not the ink it was asked for -- a colour emoji.
    Neither its colour nor its advance width describes what is on screen, so the
    two checks that read those skip it."""
    return o.tag == "content:emoji"


def on_screen(o):
    """Did any of this actually reach the screen? A clip that intersects the
    rect to nothing means no."""
    return o.clip is None or not empty(isect(o.r, o.clip))


def centre(r):
    return ((r[0] + r[2]) / 2.0, (r[1] + r[3]) / 2.0)


def holds(r, p):
    return r[0] <= p[0] <= r[2] and r[1] <= p[1] <= r[3]


def curtain(ops, meta):
    """The last op that covers the whole output, and therefore the point after
    which nothing below is being looked at.

    A MODAL OWNS THE WINDOW. The app already reasons this way -- while a card is
    up the shell's rows are unreachable by pointer, and the accessibility
    publisher skips them entirely rather than offering a screen reader something
    the user cannot see. The ledger has to reason the same way or it reports the
    shell and the card as though they shared a surface: a sidebar label
    "overprinted" by a modal row two hundred pixels away, and a dimmed shell
    graded for contrast it was never meant to be read at. The scrim is where one
    layer ends and the next begins, and it is drawn precisely to say so."""
    last = -1
    try:
        ow, oh = (float(v) for v in meta.get("out", "0x0").split("x"))
    except ValueError:
        return last
    if not ow or not oh:
        return last
    for i, o in enumerate(ops):
        if o.op in ("fill", "fill_round") and o.clip is None:
            if o.x <= 0.5 and o.y <= 0.5 and o.w >= ow - 1 and o.h >= oh - 1:
                last = i
    return last


# WHAT A TEXT ROW'S HEIGHT MEANS, and why two checks below are asymmetric.
#
# The ledger records a text draw as the rect the raster occupies, and sdltext
# rasterizes DirectWrite's layout metrics: the WIDTH is the advance width of the
# glyphs, but the HEIGHT is the LINE BOX -- ascent, descent and leading, pinned
# per size token so that every weight of one size shares it (ARCH-108). The line
# box is deliberately taller than the ink, and callers seat it from the baseline
# rather than fitting it to the rect they pass, so a line box routinely extends
# a couple of pixels past its seat and consecutive lines' boxes touch.
#
# So horizontally these rects mean what they look like and a rect running past
# its clip is a truncated string -- text does not wrap sideways, so a string
# wider than its clip is cut and unreadable, full stop.
#
# Vertically they mean nothing of the kind, and no threshold rescues them: a
# line box legitimately overhangs its seat, and a row half below the fold of a
# scrolling list is ordinary rather than broken. A partial vertical cut is
# therefore not reported at all. The vertical defect that IS real -- a label
# with no room whatever -- is caught as "clipped away entirely" and needs no
# fraction to decide it.
#
# For the same reason a clip that removes an element ENTIRELY is not reported
# either. That is how every scrolling list in the app works -- rows outside the
# viewport are drawn and clipped away -- and from a ledger row there is nothing
# to distinguish a scrolled-out row from a stray one. The instrument that CAN
# tell them apart is the accessibility-derived fit check in the client, which
# only ever sees elements that are reachable; this one stays with the question
# it can answer honestly.
#
# A threshold would have been the easier thing to write and the wrong thing to
# keep: it fires on healthy scrolling lists, and a check that fires on correct
# code is one people learn to skip.
VERT_TOUCHES = 0.5   # how much of two line boxes must overlap to be overprint


def lum(rgb):
    def ch(v):
        v = v / 255.0
        return v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4
    return (0.2126 * ch((rgb >> 16) & 255) + 0.7152 * ch((rgb >> 8) & 255) +
            0.0722 * ch(rgb & 255))


def contrast(a, b):
    la, lb = lum(a), lum(b)
    if la < lb:
        la, lb = lb, la
    return (la + 0.05) / (lb + 0.05)


# What can serve as "the surface behind this". A disc is as much a surface as a
# rectangle -- an avatar's initial sits on one, and the first version of this
# list left ellipses out, so every initial in the app was reported as white ink
# on the sidebar three layers below it. Textures are here because a photo
# avatar is a surface too; its colour is unknowable, which is a different
# answer from "nothing is behind this" and is why lookups can return NO_COLOUR.
OPAQUE_FILL = ("fill", "fill_round", "clear", "ellipse")
OPAQUE_TEX = ("tex", "tex_shaped")

NO_COLOUR = -1   # something is behind it, but what colour cannot be known


def surface_under(ops, i):
    """The colour a person sees behind op `i`: the last opaque fill, before it
    in draw order, that covers its rect. Walking backwards is the whole trick —
    the painter's algorithm means the LAST one wins, and the first version of
    this walked forwards and reported the window background for every label in
    the app."""
    # Probed at the text's CENTRE rather than by containment. A label's line box
    # is taller than its glyphs and an avatar's initial is seated in a disc
    # barely larger than one -- asking for containment there found nothing under
    # the initial and reported white-on-white against the panel three layers
    # below it. The centre is where the glyphs are.
    p = centre(ops[i].r)
    for j in range(i - 1, -1, -1):
        o = ops[j]
        if o.a < 0.99:
            continue
        if o.op == "clear":
            return o.rgb
        if o.op in OPAQUE_TEX:
            box = o.r if o.clip is None else isect(o.r, o.clip)
            if not empty(box) and holds(box, p):
                return NO_COLOUR
            continue
        if o.op not in OPAQUE_FILL or o.rgb is None:
            continue
        box = o.r
        if o.clip is not None:
            box = isect(box, o.clip)
            if empty(box):
                continue
        if holds(box, p):
            return o.rgb
    return None


# ---------------------------------------------------------------------------
# the checks
#
# Each returns a list of findings. A finding is a dict so the JSON form and the
# printed form cannot drift apart.


def f(check, msg, op=None):
    d = {"check": check, "detail": msg}
    if op is not None:
        d["where"] = op.where()
        d["seq"] = op.seq
    return d


def check_degenerate(ops, meta, pal):
    """Rects that cannot be seen: zero-area, negative, or off the output."""
    out = []
    try:
        ow, oh = (float(v) for v in meta.get("out", "0x0").split("x"))
    except ValueError:
        ow = oh = 0
    for o in ops:
        if o.op == "clear":
            continue
        if o.w < 0 or o.h < 0:
            out.append(f("degenerate", "negative extent", o))
        elif o.w == 0 or o.h == 0:
            out.append(f("degenerate", "zero area", o))
        if o.x != o.x or o.y != o.y:            # NaN
            out.append(f("degenerate", "not a number", o))
        if ow and oh and o.w > 0 and o.h > 0:
            if o.x >= ow or o.y >= oh or o.x + o.w <= 0 or o.y + o.h <= 0:
                # A row scrolled past the fold is ordinary; the clip says so.
                if o.clip is None or not empty(isect(o.r, o.clip)):
                    out.append(f("offscreen",
                                 f"drawn entirely outside the {ow:.0f}x{oh:.0f} "
                                 "output", o))
    return out


def check_clipped_text(ops, meta, pal):
    """A string whose raster does not fit the clip it is drawn under is cut,
    and nothing on screen says so — the reader simply gets less of the sentence
    than the app believes it showed. This is the single highest-yield check
    here, because truncation is invisible to every other one."""
    out = []
    for o in ops:
        if o.op != "text" or o.clip is None or is_colour_glyph(o):
            continue
        cut = isect(o.r, o.clip)
        if empty(cut):
            continue          # not drawn at all; see the note above
        lost_x = o.w - (cut[2] - cut[0])
        # A pixel is rounding. Two is a missing glyph.
        if lost_x > 1.5:
            out.append(f("text-clipped",
                         f"{lost_x:.0f}px of the string is cut off the "
                         f"{'right' if cut[2] < o.r[2] else 'left'}", o))
    return out


def check_overprint(ops, meta, pal):
    """Two strings sharing pixels, with no opaque fill drawn between them to
    say the first was meant to be replaced. Reads as a smear."""
    out = []
    cut = curtain(ops, meta)
    texts = [(i, o) for i, o in enumerate(ops)
             if o.op == "text" and o.a > 0.01 and i > cut and on_screen(o)]
    for a in range(len(texts)):
        ia, oa = texts[a]
        ra = oa.r if oa.clip is None else isect(oa.r, oa.clip)
        if empty(ra):
            continue
        for b in range(a + 1, len(texts)):
            ib, ob = texts[b]
            rb = ob.r if ob.clip is None else isect(ob.r, ob.clip)
            if empty(rb) or not overlap(ra, rb):
                continue
            # Consecutive lines' boxes touch by design (see VERT_SURVIVES
            # above), so adjacency is not overprinting. A real one buries a
            # substantial part of the earlier string.
            ov = isect(ra, rb)
            shorter = min(ra[3] - ra[1], rb[3] - rb[1])
            if (ov[3] - ov[1]) < shorter * VERT_TOUCHES:
                continue
            # Is the SHARED region covered by something opaque drawn between
            # them? The overlap, not the whole earlier string: a floating panel
            # -- the emoji picker, a popover -- covers the part of the transcript
            # it sits on and no more, so asking whether the whole message was
            # hidden answered "no" and reported every cell in the picker as
            # overprinting the message underneath it. What is buried is what
            # matters; what still shows is by definition not overlapped.
            hidden = False
            for j in range(ia + 1, ib):
                m = ops[j]
                if m.op in OPAQUE_FILL and m.a > 0.99:
                    box = m.r if m.clip is None else isect(m.r, m.clip)
                    if not empty(box) and contains(box, ov, slack=0.0):
                        hidden = True
                        break
            if not hidden:
                out.append(f("overprint",
                             f"shares pixels with {ob.where()} and nothing "
                             "opaque was drawn between them", oa))
    return out


def check_contrast(ops, meta, pal):
    """Ink against the surface behind it. WCAG asks 4.5:1 of body text and 3:1
    of large text; anything under 3:1 is unreadable at any size, so that is
    where this fails — the aim is to catch ink that is simply wrong for the
    theme, not to grade the palette."""
    out = []
    FLOOR = 3.0
    cut = curtain(ops, meta)
    for i, o in enumerate(ops):
        if i <= cut or o.op != "text" or o.rgb is None or o.a < 0.9:
            continue
        # Text the clip removed is not on screen, and asking what colour it is
        # against gets an answer about the surface it was never drawn on. A
        # scrolled-away button label came back as white-on-white this way.
        if not on_screen(o) or is_colour_glyph(o):
            continue
        bg = surface_under(ops, i)
        if bg is None or bg == NO_COLOUR:
            continue
        c = contrast(o.rgb, bg)
        if c < FLOOR:
            out.append(f("contrast",
                         f"ink {o.rgb:06X} on {bg:06X} is {c:.2f}:1, under "
                         f"{FLOOR:.0f}:1", o))
    return out


def check_fractional_text(ops, meta, pal):
    """Text rasters are placed on the pixel grid or the linear filter softens
    every stem. This held once and is kept as an invariant rather than a
    memory: half-DIP positions are the norm at 150% scale, so the bug is one
    careless placement away at all times."""
    out = []
    for o in ops:
        if o.op != "text":
            continue
        fx, fy = abs(o.x - round(o.x)), abs(o.y - round(o.y))
        if fx > 0.01 or fy > 0.01:
            out.append(f("fractional-text",
                         f"raster placed at a fractional pixel "
                         f"({o.x:.2f},{o.y:.2f})", o))
    return out


def check_unthemed(ops, meta, pal):
    """Every colour on screen should be one the theme names. A literal that
    resolves to no token is a surface that will not follow a theme change —
    which is a bug that only shows up in the theme nobody was looking at."""
    out = []
    if not pal:
        return out
    known = set(pal.values())
    # White and black are load-bearing outside the palette: white ink on an
    # accent-filled button, and a scrim is black at low alpha. Both are
    # deliberate and neither is a theme token.
    known |= {0xFFFFFF, 0x000000}
    seen = set()
    for o in ops:
        # A `content:` tag is the call site declaring that this colour is DATA,
        # not a theme choice: an avatar disc derived from a user id, a swatch
        # showing a scheme you have not picked. Neither should follow the theme,
        # so neither is a palette bug. It is a rule rather than a list of
        # exceptions, so a new one declares itself instead of being added here.
        if o.tag.startswith("content:"):
            continue
        if o.rgb is None or o.rgb in known or o.rgb in seen:
            continue
        seen.add(o.rgb)
        out.append(f("unthemed", f"{o.rgb:06X} is not a palette token", o))
    return out


def check_phantom_items(ops, meta, pal, tree=()):
    """Every published element must have something DRAWN inside it.

    The published tree and the paint ledger are two independent accounts of the
    same frame, and the interesting question is where they disagree. An element
    the tree offers at coordinates nothing painted is a phantom: a screen reader
    is invited to activate something that is not there, and every check built
    from the tree alone -- the client's own fit check included -- compares it
    against real elements and reports collisions that cannot be seen.

    That is not hypothetical. The transcript's rows were published in views that
    draw no transcript, at the coordinates they held in the view that did, and
    the fit check dutifully reported them overlapping the rows actually on
    screen. Nothing internal to the tree could have caught it, because the tree
    was consistent with itself. Only the other account knows."""
    out = []
    if not tree:
        return out
    for aid, (l, t, r, b) in tree:
        if r - l <= 0 or b - t <= 0:
            continue
        rect = (l, t, r, b)
        painted = False
        for o in ops:
            if o.op == "clear" or o.w <= 0 or o.h <= 0:
                continue
            box = o.r if o.clip is None else isect(o.r, o.clip)
            if empty(box):
                continue
            if overlap(rect, box):
                painted = True
                break
        if not painted:
            out.append({"check": "phantom-item",
                        "detail": f"'{aid}' is published at "
                                  f"{l:.0f},{t:.0f} {r - l:.0f}x{b - t:.0f} "
                                  "but nothing was drawn there",
                        "where": f"a11y:{aid}"})
    return out


CHECKS = [
    ("degenerate", check_degenerate),
    ("text-clipped", check_clipped_text),
    ("overprint", check_overprint),
    ("contrast", check_contrast),
    ("fractional-text", check_fractional_text),
    ("unthemed", check_unthemed),
]


def run(ledger_path, dump_path):
    meta, ops = read_ledger(ledger_path)
    pal = read_palette(dump_path) if dump_path else {}
    tree = read_a11y(dump_path) if dump_path else []
    findings = []
    for _, fn in CHECKS:
        findings += fn(ops, meta, pal)
    findings += check_phantom_items(ops, meta, pal, tree)
    return meta, ops, findings


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    as_json = "--json" in argv
    args = [a for a in argv[1:] if not a.startswith("--")]
    ledger = args[0]
    dump = args[1] if len(args) > 1 else None
    try:
        meta, ops, findings = run(ledger, dump)
    except OSError as e:
        sys.stderr.write(f"oracles: {e}\n")
        return 2

    if as_json:
        print(json.dumps({"meta": meta, "ops": len(ops),
                          "findings": findings}, indent=1))
    else:
        # Grouped, with a count, because a hundred findings of one kind is one
        # defect and reading them as a hundred lines hides that.
        by = {}
        for d in findings:
            by.setdefault(d["check"], []).append(d)
        print(f"{len(ops)} primitives, {len(findings)} findings")
        for name, group in sorted(by.items()):
            print(f"  {name}: {len(group)}")
            for d in group[:5]:
                print(f"    {d.get('where', '')} — {d['detail']}")
            if len(group) > 5:
                print(f"    ... and {len(group) - 5} more")
    if meta.get("lost", "0") != "0":
        sys.stderr.write(f"oracles: {meta['lost']} primitives were dropped — "
                         "the ledger filled up and this scene is INCOMPLETE\n")
        return 1
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
