#!/usr/bin/env python3
"""Training pairs for a small summary model, from sumeval traces (docs/TUNING.md).

    scripts/sumtune_pairs.py <out.jsonl> <tree.md or results dir> ...

Every model call sumeval traces is a prompt, the answer as the model wrote
it, and what was stored from it -- or "(failed)" when the parser and the
checks kept nothing. A call whose answer was stored in full is a pair the
student learns from: the daemon's one system prompt, the exact user prompt,
and the teacher's answer. A call that failed, or whose answer the checks cut
(the stored notes fewer than the answer's lines), is written as a rejected
pair for preference tuning, with `kept` 0.

Pairs are written as JSON lines {"system", "prompt", "answer", "kind",
"kept", "source"}; `kind` is notes, final, shorten or rewrite, from the
prompt. Development only; traces of real workspaces never enter the
repository, and the pairs made from them are as private as the traces.
"""
import glob
import json
import os
import re
import sys

SYSTEM = ("You take notes on a team's chat for someone catching up on it. You write only what the lines "
          "you are given say.")

CALL = re.compile(r"^## Call (\d+)\n\n### Prompt\n\n```\n(.*?)```\n\n### Answer\n\n```\n(.*?)\n```\n\n### Stored\n\n```\n(.*?)\n```\n",
                  re.S | re.M)


def kind_of(prompt):
    if prompt.startswith("This summary is"):
        return "shorten"
    if prompt.startswith("This is ") and "Rewrite it in at most" in prompt:
        return "rewrite"
    if "Summarize these lines for someone catching up" in prompt:
        return "final"
    return "notes"


def kept_fully(kind, answer, stored):
    """1 when the stored body holds everything the answer wrote: no note and no
    sentence dropped by the checks. A notes answer's lines against the stored
    notes; a final's lines against the stored parts."""
    if stored.strip() == "(failed)":
        return 0
    try:
        body = json.loads(stored)
    except ValueError:
        return 0
    lines = [l for l in answer.splitlines() if l.strip()]
    if kind == "notes":
        wrote = sum(1 for l in lines if l.lstrip().startswith("- ["))
        return 1 if wrote and len(body.get("notes", [])) == wrote else 0
    if kind == "final":
        parts = 1 if body.get("overview", {}).get("text") else 0
        for t in body.get("topics", []):
            parts += 1 + (1 if t.get("text") else 0) + len(t.get("details", []))
        parts += len(body.get("attention", []))
        wrote = sum(1 for l in lines if not l.startswith("Needs attention") and not l.startswith("More topics"))
        return 1 if parts and parts >= wrote else 0
    return 1


def main(out, paths):
    files = []
    for p in paths:
        files += glob.glob(os.path.join(p, "*.tree.md")) if os.path.isdir(p) else [p]
    n = kept = 0
    with open(out, "w") as o:
        for f in sorted(files):
            text = open(f, encoding="utf-8", errors="replace").read()
            for m in CALL.finditer(text):
                _, prompt, answer, stored = m.groups()
                kind = kind_of(prompt)
                k = kept_fully(kind, answer, stored)
                o.write(json.dumps({"system": SYSTEM, "prompt": prompt, "answer": answer.strip() + "\n",
                                    "kind": kind, "kept": k, "source": os.path.basename(f)}) + "\n")
                n += 1
                kept += k
    print(f"{n} pairs, {kept} kept in full, {n - kept} rejected, from {len(files)} traces -> {out}")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__.strip(), file=sys.stderr)
        sys.exit(2)
    main(sys.argv[1], sys.argv[2:])
