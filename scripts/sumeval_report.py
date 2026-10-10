#!/usr/bin/env python3
"""The bake-off report (docs/SUMMARIES.md §7): every span's summaries side by
side, under labels that do not name the model, for a person to grade; and, per
model, what code can measure -- whether every span finished, how long and how
much memory it took, whether each summary keeps to the standard's caps, and
whether every citation is a message of the span.

  sumeval_report.py <db> <set.tsv> <bake dir> <report.md> <key.md>

The key (which label is which model) goes to its own file, to open after
grading.
"""
import json
import os
import random
import sqlite3
import sys

# The standard (SUMMARIES.md §1, daemon/sum_core.h): sized to the span.
WORDS_PER_WORD, MIN_WORDS, MAX_WORDS = 10, 100, 600
OVERVIEW, PARA, DETAIL, MAX_TOPICS, DETAILS, ATTENTION, MORE = 60, 80, 40, 6, 3, 5, 10


def caps_for(input_words):
    total = max(MIN_WORDS, min(MAX_WORDS, input_words // WORDS_PER_WORD))
    total = min(total, max(1, input_words))
    topics = max(2, min(MAX_TOPICS, total // 100))
    return dict(total=total, overview=OVERVIEW, para=PARA, detail=DETAIL, topics=topics, details=DETAILS,
                attention=ATTENTION, more=MORE if total >= 200 else 0)


def words(s):
    return len((s or "").split())


def check(summary, ids_in_span, input_words):
    """What breaks the standard, as a list of strings (empty when none)."""
    input_words = summary.get("words", input_words)   # the daemon's own count, whole threads included
    c = caps_for(input_words)
    bad = []
    s = summary
    total = words(s["overview"]["text"])
    if words(s["overview"]["text"]) > c["overview"]:
        bad.append("overview over its cap")
    if len(s["topics"]) > c["topics"]:
        bad.append("too many topics")
    refs = list(s["overview"]["refs"])
    for t in s["topics"]:
        total += words(t["title"]) + words(t["text"])
        if words(t["text"]) > c["para"]:
            bad.append(f"topic '{t['title']}' account over its cap")
        if len(t["details"]) > c["details"]:
            bad.append(f"topic '{t['title']}' has too many details")
        refs += t["refs"]
        for d in t["details"]:
            total += words(d["text"])
            if words(d["text"]) > c["detail"]:
                bad.append("a detail over its cap")
            refs += d["refs"]
            if not d["refs"]:
                bad.append("a detail cites nothing")
    for a in s["attention"]:
        total += words(a["text"])
        refs += a["refs"]
    if len(s["attention"]) > c["attention"]:
        bad.append("too much needs attention")
    if len(s["more"]) > c["more"]:
        bad.append("too many more topics")
    if total > c["total"]:
        bad.append(f"{total} words, over {c['total']}")
    outside = [r for r in refs if r not in ids_in_span]
    if outside:
        bad.append(f"{len(outside)} citation(s) outside the span")
    return bad, total, f"{c['total']}-word"


def render(s):
    out = []
    if s["overview"]["text"]:
        out.append(f"**{s['overview']['text']}**")
    for t in s["topics"]:
        meta = ", ".join(t.get("people", [])[:3])
        out.append(f"\n**{t['title']}** _({meta}{' · ' if meta else ''}{t.get('count', 0)} messages)_  ")
        if t["text"]:
            out.append(t["text"])
        for d in t["details"]:
            out.append(f"- {d['text']}")
    if s["attention"]:
        out.append("\n_Needs attention:_")
        for a in s["attention"]:
            out.append(f"- {a['kind'].capitalize()}: {a['text']}")
    if s["more"]:
        out.append("\n_More topics:_ " + ", ".join(s["more"]))
    return "\n".join(out) if out else "_(nothing to summarize)_"


def main():
    dbp, set_tsv, bake, report, keyfile = sys.argv[1:6]
    db = sqlite3.connect(dbp)
    spans = [l.rstrip("\n").split("\t") for l in open(set_tsv) if not l.startswith("#")]
    spans = [s for s in spans if s[2] == "eval"]
    models = sorted(d for d in os.listdir(bake) if os.path.isdir(os.path.join(bake, d)))
    labels = list("ABCDEFGH")[:len(models)]
    rnd = random.Random(609)
    order = models[:]
    rnd.shuffle(order)
    label_of = {m: labels[i] for i, m in enumerate(order)}
    meas = {}
    for m in models:
        meas[m] = {}
        p = os.path.join(bake, m, "measure.tsv")
        if os.path.exists(p):
            for l in open(p):
                f = l.rstrip("\n").split("\t")
                meas[m][f[0]] = f
    stats = {m: dict(done=0, failed=0, compliant=0, wall=0.0, cpu=0.0, calls=0, rss=0, words=[], dropped=0) for m in models}
    out = ["# Summary bake-off\n",
           "Each span's summaries under labels that do not name the model; the key is in a separate file. "
           "Grade each with the rubric in docs/SUMMARIES.md §7.\n"]
    for sp in spans:
        sid, cls, _, chan, start, end, msgs, w, toks = sp
        # the set names a channel, or gives its id
        row = db.execute("SELECT id, name FROM channels WHERE id=?", (chan,)).fetchone() if chan.isdigit() else \
              db.execute("SELECT id, name FROM channels WHERE name=?", (chan,)).fetchone()
        cid, chan = row
        ids = {r[0] for r in db.execute("SELECT id FROM messages WHERE channel_id=? AND created_at_ms>=? AND "
                                         "created_at_ms<?", (cid, int(start) - 30 * 86400000, int(end)))}
        out.append(f"\n## Span {sid}: #{chan}, {cls} ({msgs} messages, {w} words)\n")
        for m in sorted(models, key=lambda x: label_of[x]):
            ms = meas[m].get(sid)
            jp = os.path.join(bake, m, f"{sid}.json")
            out.append(f"\n### {label_of[m]}\n")
            if not ms:
                out.append("_not run yet_\n")
                continue
            st = stats[m]
            st["done"] += 1
            st["calls"] += int(ms[4]); st["cpu"] += float(ms[5]); st["wall"] += float(ms[6])
            st["dropped"] += int(ms[10]) if len(ms) > 10 and ms[10].strip().lstrip("-").isdigit() else 0
            st["rss"] = max(st["rss"], int(ms[8]))
            if ms[3] != "ok" or not os.path.exists(jp):
                st["failed"] += 1
                out.append(f"_failed: {ms[9] if len(ms) > 9 else ''}_\n")
                continue
            body = json.load(open(jp))
            s = body["summary"]
            bad, total, form = check(s, ids, int(w))
            st["words"].append(total)
            if not bad:
                st["compliant"] += 1
            out.append(render(s) + "\n")
            dropped = f", {ms[10].strip()} dropped by the checks" if len(ms) > 10 and ms[10].strip() not in ("", "0") else ""
            out.append(f"\n<sub>{form}, {total} words, {ms[4]} calls, {float(ms[6]):.0f} s{dropped}"
                       f"{'; breaks the standard: ' + '; '.join(bad) if bad else ''}</sub>\n")
    out.append("\n## Measured, per label\n\n| Label | Spans run | Failed | Within the standard | Model calls | "
               "CPU s | Wall s | Peak RSS MB | Mean words | Dropped by the checks |\n|---|---|---|---|---|---|---|---|---|---|\n")
    for m in sorted(models, key=lambda x: label_of[x]):
        st = stats[m]
        mw = sum(st["words"]) / len(st["words"]) if st["words"] else 0
        out.append(f"| {label_of[m]} | {st['done']} | {st['failed']} | {st['compliant']} | {st['calls']} | "
                   f"{st['cpu']:.0f} | {st['wall']:.0f} | {st['rss']} | {mw:.0f} | {st['dropped']} |\n")
    open(report, "w").write("".join(out))
    open(keyfile, "w").write("# Bake-off key\n\n" + "".join(f"- {label_of[m]}: {m}\n" for m in sorted(models, key=lambda x: label_of[x])))


if __name__ == "__main__":
    main()
