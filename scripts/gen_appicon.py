#!/usr/bin/env python3
"""Generate the Windows application icon.

Drawn rather than downloaded: the app already has a mark — the accent rounded
square the rail and sign-in card use — and a bell from the Lucide set that is
already vendored under ISC (third_party/lucide/LICENSE). "OpenChime" is a
chime, so the bell is the obvious glyph, and reusing the in-app icon set keeps
the taskbar consistent with the rail rather than introducing a second visual
language from a stock-icon site.

Everything is rendered at 8x and downsampled, which is what keeps the small
sizes legible.
"""
from PIL import Image, ImageDraw
import math, sys

ACCENT = (61, 139, 255)          # OC_COL_ACCENT
ACCENT_DEEP = (26, 92, 196)      # a slight vertical gradient so it is not flat
WHITE = (255, 255, 255)
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
SS = 8                            # supersample factor


def bell_path(s):
    """Lucide 'bell' in a 24x24 viewBox, scaled to `s` and returned as polylines."""
    def P(x, y):
        return (x * s / 24.0, y * s / 24.0)

    def cubic(p0, p1, p2, p3, n=24):
        out = []
        for i in range(n + 1):
            t = i / n
            u = 1 - t
            x = u*u*u*p0[0] + 3*u*u*t*p1[0] + 3*u*t*t*p2[0] + t*t*t*p3[0]
            y = u*u*u*p0[1] + 3*u*u*t*p1[1] + 3*u*t*t*p2[1] + t*t*t*p3[1]
            out.append((x, y))
        return out

    # M3.262 15.326A1 1 0 0 0 4 17h16a1 1 0 0 0 .74-1.673C19.41 13.956 18 12.499 18 8A6 6 0 0 0 6 8c0 4.499-1.411 5.956-2.738 7.326
    body = []
    body += cubic(P(3.262, 15.326), P(2.7, 15.9), P(3.1, 17.0), P(4, 17))     # left lip
    body += [P(20, 17)]                                                        # rim
    body += cubic(P(20, 17), P(20.9, 17.0), P(21.3, 15.9), P(20.74, 15.327))   # right lip
    body += cubic(P(20.74, 15.327), P(19.41, 13.956), P(18, 12.499), P(18, 8))
    body += cubic(P(18, 8), P(18, 4.686), P(15.314, 2), P(12, 2))
    body += cubic(P(12, 2), P(8.686, 2), P(6, 4.686), P(6, 8))
    body += cubic(P(6, 8), P(6, 12.499), P(4.589, 13.956), P(3.262, 15.326))
    # M10.268 21a2 2 0 0 0 3.464 0  (the clapper)
    clap = cubic(P(10.268, 21), P(10.62, 21.61), P(13.38, 21.61), P(13.732, 21))
    return body, clap


def render(size):
    s = size * SS
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # Rounded square, matching the rail avatar's proportions (12/36 radius).
    r = int(s * 12 / 36)
    grad = Image.new("RGBA", (s, s))
    gd = ImageDraw.Draw(grad)
    for y in range(s):
        t = y / max(1, s - 1)
        c = tuple(int(ACCENT[i] + (ACCENT_DEEP[i] - ACCENT[i]) * t) for i in range(3))
        gd.line([(0, y), (s, y)], fill=c + (255,))
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], radius=r, fill=255)
    img.paste(grad, (0, 0), mask)

    # The bell, inset and stroked like Lucide (2/24 stroke, round caps/joins).
    inset = s * 0.22
    gs = s - 2 * inset
    body, clap = bell_path(gs)
    w = max(1, int(round(gs * 2.0 / 24.0)))
    off = lambda pts: [(x + inset, y + inset) for (x, y) in pts]
    d.line(off(body), fill=WHITE, width=w, joint="curve")
    d.line(off(clap), fill=WHITE, width=w, joint="curve")
    for (x, y) in [body[0], body[-1], clap[0], clap[-1]]:      # round caps
        d.ellipse([x + inset - w/2, y + inset - w/2, x + inset + w/2, y + inset + w/2], fill=WHITE)

    return img.resize((size, size), Image.LANCZOS)


def main(out):
    imgs = [render(n) for n in SIZES]
    imgs[-1].save(out, format="ICO", sizes=[(n, n) for n in SIZES])
    print("wrote", out, "sizes", SIZES)


# --- MSIX tile assets ---------------------------------------------------------
# The Store package needs PNGs, not an .ico, at a fixed set of names. They are
# drawn from the same render() as the taskbar icon rather than exported by hand,
# so the tile and the taskbar cannot drift apart.
#
# Each logo exists at five display scales; makepri picks one at runtime from the
# `.scale-N` suffix. The targetsize variants are the small-icon case (taskbar,
# Alt-Tab), where `altform-unplated` means Windows draws the image alone instead
# of stamping it onto a solid accent-coloured plate -- correct here, because the
# icon already IS a rounded accent square and would otherwise be plated twice.
SCALES = [100, 125, 150, 200, 400]
TARGETSIZES = [16, 24, 32, 48, 256]
LOGOS = {"StoreLogo": 50, "Square150x150Logo": 150, "Square44x44Logo": 44}


def main_msix(outdir):
    import os
    os.makedirs(outdir, exist_ok=True)
    n = 0
    for name, base in LOGOS.items():
        for scale in SCALES:
            px = -(-base * scale // 100)          # ceil, which is what MSIX expects
            suffix = "" if scale == 100 else ".scale-%d" % scale
            render(px).save(os.path.join(outdir, "%s%s.png" % (name, suffix)))
            n += 1
    for px in TARGETSIZES:
        render(px).save(os.path.join(
            outdir, "Square44x44Logo.targetsize-%d_altform-unplated.png" % px))
        n += 1
    print("wrote", n, "assets to", outdir)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--msix":
        main_msix(sys.argv[2] if len(sys.argv) > 2 else "packaging/windows/msix/assets")
    else:
        main(sys.argv[1] if len(sys.argv) > 1 else "client/gui/win32/res/openchime.ico")
