#!/usr/bin/env python3
"""Draw the summary evaluation set (docs/SUMMARIES.md §7) from a workspace
database: spans picked at random, with a recorded seed, and classed only by
size -- never by channel or by who posts in it.

  sumeval_set.py <db> <seed> <out.tsv> [per_class] [held_out_per_class] [max_tokens]

Classes, by the span's words and estimated tokens (4 bytes a token, as the
summarizer counts):
  small   -- at most 400 words (a short summary);
  medium  -- more, but fits one prompt (6,000 tokens);
  large   -- more than one prompt, up to max_tokens (default 60,000), so a run
             stays within hours on one CPU core; the largest spans are run on
             their own (sumeval_bakeoff.sh --largest).
Each line: id, class, set (eval|held), channel, start_ms, end_ms, messages,
words, tokens.
"""
import random
import sqlite3
import sys

DAY = 86400000
SHORT_WORDS, INPUT_TOKENS = 400, 6000


def main():
    db, seed, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    per = int(sys.argv[4]) if len(sys.argv) > 4 else 10
    held = int(sys.argv[5]) if len(sys.argv) > 5 else 4
    max_tokens = int(sys.argv[6]) if len(sys.argv) > 6 else 60000
    d = sqlite3.connect(db)
    chans = [r for r in d.execute(
        "SELECT c.id, c.name, MIN(m.created_at_ms), MAX(m.created_at_ms) FROM channels c JOIN messages m "
        "ON m.channel_id=c.id WHERE m.deleted_at_ms IS NULL AND m.kind=0 GROUP BY c.id")]
    rnd = random.Random(seed)
    want = {"small": per + held, "medium": per + held, "large": per + held}
    got = {"small": [], "medium": [], "large": []}
    seen = set()
    for _ in range(200000):
        if all(len(got[k]) >= want[k] for k in got):
            break
        cid, name, lo, hi = rnd.choice(chans)
        days = rnd.choice([1, 2, 3, 7, 14, 30])
        first, last = lo // DAY, hi // DAY
        start = rnd.randint(first, last) * DAY
        end = start + days * DAY
        key = (cid, start, end)
        if key in seen:
            continue
        seen.add(key)
        rows = d.execute("SELECT COALESCE(NULLIF(m.author_name,''), u.display_name, ''), m.body FROM messages m "
                         "LEFT JOIN users u ON u.id=m.author_id WHERE m.channel_id=? AND m.created_at_ms>=? AND "
                         "m.created_at_ms<? AND m.deleted_at_ms IS NULL AND m.kind=0", (cid, start, end)).fetchall()
        if not rows:
            continue
        words = sum(len(f"{a}: {b if isinstance(b, str) else b.decode('utf-8', 'replace')}".split()) for a, b in rows)
        nbytes = sum(len(a) + 2 + len(b if isinstance(b, bytes) else b.encode()) + 8 for a, b in rows)
        tokens = nbytes // 4 + 1
        cls = "small" if words <= SHORT_WORDS else "medium" if tokens <= INPUT_TOKENS else "large"
        if cls == "large" and tokens > max_tokens:
            continue
        if len(got[cls]) < want[cls]:
            got[cls].append((name, start, end, len(rows), words, tokens))
    with open(out, "w") as f:
        f.write(f"# seed {seed}\tper_class {per}\theld_out {held}\tmax_tokens {max_tokens}\n")
        n = 0
        for cls in ("small", "medium", "large"):
            for i, (name, start, end, msgs, words, tokens) in enumerate(got[cls]):
                n += 1
                which = "eval" if i < per else "held"
                f.write(f"{n}\t{cls}\t{which}\t{name}\t{start}\t{end}\t{msgs}\t{words}\t{tokens}\n")
    for k, v in got.items():
        print(k, len(v), "of", want[k])


if __name__ == "__main__":
    main()
