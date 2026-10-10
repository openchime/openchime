#!/usr/bin/env python3
"""Synthetic team chat, as a Slack export, for training a small summary
model (docs/TUNING.md).

    scripts/sumtune_synth.py <api url> <model> <out dir> <workspaces> [seed]

A large model writes each workspace: a few channels of a made-up company
over a few days, with threads, decisions, open questions, small talk and an
integration posting notifications, in Slack's export layout
(users.json, <channel>/<day>.json), so scripts/slack_to_db.py loads it and
sumeval summarizes it as it would a real workspace. The teacher's summaries
of these spans, through the pipeline, are the training pairs
(scripts/sumtune_pairs.py). Nothing here is real: names, companies and
events are invented by the model from the seed.

<api url> is an OpenAI-style chat-completions base (…/v1), <model> the
model's name there; the key is read from OPENCHIME_SUMMARY_API_KEY, as the
daemon reads it. Development only.
"""
import json
import os
import random
import re
import sys
import time
import urllib.request

DOMAINS = ["customer support for a software product", "field service scheduling", "a sales team with a CRM",
           "software development with a bug tracker", "HR and office operations", "accounting and invoicing",
           "a logistics and dispatch desk", "an inventory audit company", "a marketing team", "IT and devops"]

PROMPT = """Write a realistic Slack workspace for a small company doing {domain}. Invent the company, 6 to 10 people with first and last names and roles, and 3 channels. Cover {days} consecutive working days starting {start}.

Output ONLY JSON, no prose, of this shape:
{{"company": "...", "users": [{{"id": "U1", "name": "First Last", "title": "..."}}, ...],
 "channels": [{{"name": "channel-name", "messages": [{{"user": "U1", "day": 0, "time": "09:14", "text": "...", "thread_of": null}}, ...]}}, ...]}}

Rules:
- 60 to 140 messages per channel in all, spread over the days, in order.
- Real work: decisions made, actions assigned with owners and dates, questions that get answered and some that do not, numbers (amounts, ticket ids, counts, dates), and a little small talk.
- Threads: "thread_of" is the index (0-based, within the channel) of the message replied to, or null. About a third of messages are replies.
- One channel has an integration posting notifications: those messages have "user": "BOT", a "bot": "..." name, and text like a CRM, ticketing or build system would post, several a day, some near-identical.
- People write like people: short, sometimes terse, with typos now and then, @mentions as @First Last.
- No markdown headings, no numbered lists, no quotes around the whole message."""


def chat(url, model, key, user, max_tokens=12000, retries=5):
    body = json.dumps({"model": model, "messages": [{"role": "user", "content": user}], "temperature": 0.9,
                       "max_tokens": max_tokens, "stream": False}).encode()
    req = urllib.request.Request(url.rstrip("/") + "/chat/completions", data=body,
                                 headers={"Content-Type": "application/json", "Authorization": "Bearer " + key})
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(req, timeout=600) as r:
                d = json.loads(r.read())
            return d["choices"][0]["message"]["content"]
        except Exception as e:  # 429s and the like: wait and try again
            wait = 2 << attempt
            print(f"  api: {e}; waiting {wait} s", file=sys.stderr)
            time.sleep(wait)
    raise SystemExit("the API did not answer")


def parse(text):
    text = text.strip()
    m = re.search(r"\{.*\}", text, re.S)
    return json.loads(m.group(0) if m else text)


def write_export(ws, out, base_ts):
    os.makedirs(out, exist_ok=True)
    users = [{"id": u["id"], "name": u["name"].lower().replace(" ", "."), "real_name": u["name"],
              "profile": {"real_name": u["name"], "title": u.get("title", "")}} for u in ws["users"]]
    json.dump(users, open(os.path.join(out, "users.json"), "w"), indent=1)
    json.dump([{"name": c["name"]} for c in ws["channels"]], open(os.path.join(out, "channels.json"), "w"), indent=1)
    for c in ws["channels"]:
        cdir = os.path.join(out, re.sub(r"[^a-z0-9_-]", "-", c["name"].lower()))
        os.makedirs(cdir, exist_ok=True)
        by_day = {}
        ts_of = {}
        last = 0.0
        for i, m in enumerate(c["messages"]):
            h, mi = (m.get("time") or "09:00").split(":")[:2]
            ts = base_ts + int(m.get("day", 0)) * 86400 + int(h) * 3600 + int(mi) * 60 + (i % 59)
            if ts <= last:
                ts = last + 1
            last = ts
            ts_of[i] = ts
            rec = {"type": "message", "ts": f"{ts:.6f}", "text": m.get("text", "")}
            if m.get("user") == "BOT":
                rec.update({"subtype": "bot_message", "bot_id": "B1", "username": m.get("bot") or "integration"})
            else:
                rec["user"] = m.get("user")
            t = m.get("thread_of")
            if t is not None and t in ts_of and t != i:
                rec["thread_ts"] = f"{ts_of[t]:.6f}"
            day = time.strftime("%Y-%m-%d", time.gmtime(ts))
            by_day.setdefault(day, []).append(rec)
        for day, recs in by_day.items():
            json.dump(recs, open(os.path.join(cdir, day + ".json"), "w"), indent=1)


def main(url, model, out, n, seed):
    key = os.environ.get("OPENCHIME_SUMMARY_API_KEY", "")
    rnd = random.Random(seed)
    os.makedirs(out, exist_ok=True)
    for i in range(n):
        d = os.path.join(out, f"ws{i:04d}")
        if os.path.exists(os.path.join(d, "users.json")):
            continue
        domain = rnd.choice(DOMAINS)
        days = rnd.randint(3, 8)
        base = 1735689600 + rnd.randint(0, 300) * 86400   # a day in 2025, by the seed
        start = time.strftime("%A %B %d, %Y", time.gmtime(base))
        print(f"workspace {i}: {domain}, {days} days", file=sys.stderr)
        text = chat(url, model, key, PROMPT.format(domain=domain, days=days, start=start))
        try:
            ws = parse(text)
            if not ws.get("users") or not ws.get("channels"):
                raise ValueError("no users or channels")
        except Exception as e:
            print(f"  skipped: {e}", file=sys.stderr)
            continue
        write_export(ws, d, base)
        print(f"  {sum(len(c['messages']) for c in ws['channels'])} messages in {len(ws['channels'])} channels -> {d}",
              file=sys.stderr)


if __name__ == "__main__":
    if len(sys.argv) < 5:
        print(__doc__.strip(), file=sys.stderr)
        sys.exit(2)
    main(sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]) if len(sys.argv) > 5 else 609)
