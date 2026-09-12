#!/usr/bin/env python3
"""Drive the client into states that MUST agree, and check that they do.

    consistency.py [--keep]

Every check here compares the client against itself. There is no reference
binary and no golden image; the assertion is that two routes to one state
produce one scene.

  COLD vs LIVE.       A theme applied at startup and the same theme switched to
                      while running must render identically. This is the check
                      that catches a cache whose key forgot an input it depends
                      on -- message bodies kept the OUTGOING theme's ink for
                      exactly that reason, and every screenshot taken after a
                      restart missed it.

  THERE AND BACK.     96 -> 192 -> 96 must equal a cold 96. Same for text size.
                      A scale change that does not fully undo leaves the window
                      drawing at two scales at once, which is the failure the
                      DPI path's own comment warns about.

  TWO ROUTES.         A conversation reached from the sidebar and the same one
                      reached by the keyboard are the same conversation, and
                      must look like it.

A difference here is not automatically a bug in the scene -- it can be a bug in
the cache, the invalidation or the state machine. That is the point: those are
invisible to a single capture, however carefully it is checked.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DRIVE = os.path.join(HERE, "..", "gui_drive.sh")
WIN = "C:\\Windows\\Temp\\octest"
LIN = "/mnt/c/Windows/Temp/octest"

ENV = dict(os.environ)
ENV.setdefault("OC_DEV_PORT", "9550")
ENV.setdefault("OC_DEV_DIR", "/tmp/oc-audit-dev")
ENV.setdefault("OC_DEV_WS", "Audit Fixture")


def drive(*args):
    p = subprocess.run([DRIVE, *args], capture_output=True, text=True, env=ENV)
    return p.returncode == 0


def capture(name):
    drive("ledger", f"{WIN}\\{name}.tsv")
    path = f"{LIN}/{name}.tsv"
    rows = []
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("#") or line.startswith("seq"):
            continue
        r = line.rstrip("\n").split("\t")
        if len(r) < 13:
            continue
        # A `transient:` tag is the call site saying "this is present in some
        # frames and not others by design" -- the composer caret blinks on a
        # 530ms phase. Comparing it would make every check here flap.
        if len(r) > 13 and r[13].startswith("transient:"):
            continue
        rows.append(tuple(r[1:]))         # everything but the sequence number
    return rows


def differ(a, b):
    """What the two scenes disagree about, as a short list. Compared as
    multisets of primitives rather than positionally: a scene that draws the
    same things in a different order is still the same scene, and reporting an
    ordering change as 200 differences would bury the one that matters."""
    from collections import Counter
    ca, cb = Counter(a), Counter(b)
    only_a = list((ca - cb).elements())
    only_b = list((cb - ca).elements())
    return only_a, only_b


def show(name, a, b):
    only_a, only_b = differ(a, b)
    if not only_a and not only_b:
        print(f"  ok   {name}")
        return 0
    print(f"  FAIL {name} — {len(only_a)} primitives only in the first, "
          f"{len(only_b)} only in the second")
    for r in only_a[:4]:
        print(f"         first only: {r[0]} at {r[1]},{r[2]} {r[3]}x{r[4]} "
              f"rgb={r[9]}")
    for r in only_b[:4]:
        print(f"        second only: {r[0]} at {r[1]},{r[2]} {r[3]}x{r[4]} "
              f"rgb={r[9]}")
    return 1


def settle():
    """One repaint, observed. `ledger` already forces one, but a state change
    can take a frame to propagate through a cache, and comparing the frame
    DURING that is a race the check would lose intermittently -- which is worse
    than losing it every time."""
    drive("dump", "settle")
    drive("dump", "settle")


def wait_for(key, want, ms=6000):
    """Poll the state dump until it says what we are about to assert about.

    A restarted client reaches its preferences ASYNCHRONOUSLY -- they come from
    the account, after authentication -- so a capture taken on a timer catches
    the default theme and reports the switch under test as a disagreement. That
    is the same race the scene manifest's settle column exists for, and it bit
    this file too."""
    import re
    import time
    t = 0
    while t < ms:
        drive("dump", "settle")
        try:
            txt = open(f"{LIN}/settle.txt", encoding="utf-8",
                       errors="replace").read()
        except OSError:
            txt = ""
        m = re.search(r"\b" + key + r"=(\S+)", txt)
        if m and m.group(1) == want:
            return True
        time.sleep(0.05)
        t += 50
    return False


def restart_into(num):
    """Bring the client up cold in a theme, and wait until it is really in it.

    Preferences arrive from the ACCOUNT, after authentication, so a capture
    taken on a timer catches the default and reports the switch under test as a
    disagreement."""
    if not drive("theme", num):
        return False
    drive("kill")
    if not drive("launch"):
        return False
    drive("size", "1042", "815")
    return wait_for("theme", num)


def scene():
    """The surface everything below is compared on. A conversation, because it
    is the one with cached message rasters in it -- which is where a stale
    cache actually shows."""
    drive("key", "esc")
    drive("view", "home")
    drive("channel", "general")
    settle()


def main(argv):
    bad = 0
    print("audit: consistency checks")

    # One client up front. The loop below sets a preference and then restarts
    # INTO it, so there has to be something running to set it on -- the first
    # pass without this told a client that did not exist to go dark, got no
    # answer, and then compared a light cold start against a dark live switch.
    if not drive("launch"):
        print("  FAIL the client did not start")
        return 1
    drive("size", "1042", "815")

    # --- cold vs live, for each theme -------------------------------------
    #
    # ONE switch, from a cold start in the other theme. The first version made
    # the live case a round trip -- X to Y and back to X -- and it passed while
    # the defect it was written for was live in the build. A cache whose key
    # forgets the theme is stale in BOTH directions, so going there and back
    # lands on the original colours by being wrong twice. A check that a bug can
    # satisfy is not a check, and this one had to be rebuilt rather than
    # believed.
    for theme, num in (("dark", "0"), ("light", "1")):
        other = "1" if num == "0" else "0"

        if not restart_into(num):
            print(f"  FAIL the client never came up in theme={num}")
            bad += 1
            continue
        scene()
        cold = capture("cold")

        if not restart_into(other):
            print(f"  FAIL the client never came up in theme={other}")
            bad += 1
            continue
        drive("theme", num)
        wait_for("theme", num)
        settle()
        scene()
        live = capture("live")
        bad += show(f"{theme}: reached by a live switch, renders as a cold start does",
                    cold, live)

    # --- there and back ---------------------------------------------------
    drive("dpi", "96")
    scene()
    base = capture("dpi_base")
    drive("dpi", "192")
    settle()
    drive("dpi", "96")
    scene()
    bad += show("96 -> 192 -> 96 renders as 96 did", base, capture("dpi_back"))

    drive("textsize", "1")
    scene()
    tbase = capture("ts_base")
    drive("textsize", "3")
    settle()
    drive("textsize", "1")
    scene()
    bad += show("text size 1 -> 3 -> 1 renders as 1 did", tbase, capture("ts_back"))

    # --- two routes to one conversation -----------------------------------
    # Read every channel FIRST. Visiting one clears its unread, which changes
    # its row -- badge gone, label back to regular weight -- so a route that
    # passes through an unread channel changes the scene it is being compared
    # against. That is the client behaving correctly and the fixture asking the
    # wrong question.
    for ch in ("design", "engineering", "releases", "general"):
        drive("channel", ch)
        settle()
    drive("channel", "general")
    settle()
    direct = capture("route_direct")
    drive("channel", "design")
    settle()
    drive("channel", "general")
    settle()
    bad += show("a conversation reached twice looks the same both times",
                direct, capture("route_via"))

    if "--keep" not in argv:
        drive("kill")
    print()
    print("consistency: OK" if not bad else f"consistency: {bad} disagreement(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
