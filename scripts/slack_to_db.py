#!/usr/bin/env python3
"""Load a Slack export into a daemon database, for evaluating summaries
(docs/SUMMARIES.md §7). Development only; the data never enters the repository.

    scripts/slack_to_db.py <export dir> <db made by `sumeval init`>

Users, channels, members and messages, with thread replies pointing at their
root as the daemon stores them; Slack's markup for mentions, links and channel
names becomes the plain text the daemon would hold. Bot and integration posts
are stored as integration posts (author_name set), which summaries leave out.
"""

import glob
import json
import os
import re
import sqlite3
import sys


def main(src, dbpath):
    db = sqlite3.connect(dbpath)
    users = json.load(open(os.path.join(src, "users.json")))
    uid = {}
    for n, u in enumerate(users, 1):
        name = (u.get("profile", {}).get("real_name") or u.get("real_name") or u.get("name") or "someone")
        uid[u["id"]] = n
        db.execute("INSERT INTO users(id, subject, display_name, created_at_ms) VALUES(?,?,?,0)",
                   (n, "slack|" + u["id"], name))
    names = {u["id"]: (u.get("profile", {}).get("real_name") or u.get("name") or "someone") for u in users}

    def clean(t):
        t = re.sub(r"<@(\w+)(\|[^>]*)?>", lambda m: "@" + names.get(m.group(1), "someone"), t)
        t = re.sub(r"<!subteam\^\w+\|([^>]*)>", r"\1", t)
        t = re.sub(r"<!date\^\d+\^[^|]*\|([^>]*)>", r"\1", t)
        t = re.sub(r"<!(here|channel|everyone)>", r"@\1", t)
        t = re.sub(r"<#\w+\|([^>]*)>", r"#\1", t)
        t = re.sub(r"<(https?://[^|>]+)\|([^>]*)>", r"\2", t)
        t = re.sub(r"<(https?://[^>]+)>", r"\1", t)
        return t.replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">")

    def text_of(m):
        if m.get("text"):
            return clean(m["text"])
        parts = []
        for a in m.get("attachments") or []:
            parts.append(a.get("pretext") or a.get("text") or a.get("fallback") or "")
        return clean("\n".join(p for p in parts if p))

    ch_id = 0
    msg_id = 0
    for ch in sorted(d for d in os.listdir(src) if os.path.isdir(os.path.join(src, d))):
        msgs = []
        for f in glob.glob(os.path.join(src, ch, "*.json")):
            msgs += json.load(open(f))
        msgs = [m for m in msgs if m.get("subtype") not in ("channel_join", "channel_leave", "channel_purpose",
                                                             "channel_topic")]
        if not msgs:
            continue
        ch_id += 1
        db.execute("INSERT INTO channels(id, kind, name, is_public, created_at_ms) VALUES(?,?,?,1,0)",
                   (ch_id, "channel", ch))
        members = set()
        by_ts = {}
        msgs.sort(key=lambda m: float(m["ts"]))
        # Roots first: a reply needs its root's id.
        roots = [m for m in msgs if not (m.get("thread_ts") and m["thread_ts"] != m["ts"])]
        replies = [m for m in msgs if m.get("thread_ts") and m["thread_ts"] != m["ts"]]
        for m in roots + replies:
            msg_id += 1
            bot = bool(m.get("bot_id")) or m.get("subtype") == "bot_message"
            author = uid.get(m.get("user"), 0)
            if not author and not bot and m.get("user"):
                # Someone from another workspace in a shared channel: not in
                # users.json, but the message carries their profile.
                prof = m.get("user_profile") or {}
                name = prof.get("real_name") or prof.get("display_name") or prof.get("name") or "someone"
                author = len(uid) + 1
                uid[m["user"]] = author
                names[m["user"]] = name
                db.execute("INSERT INTO users(id, subject, display_name, created_at_ms) VALUES(?,?,?,0)",
                           (author, "slack|" + m["user"], name))
            if not author:
                author = 1
                bot = True
            parent = None
            if m in replies:
                parent = by_ts.get(m["thread_ts"])
                if parent is None:
                    # The root predates the export: the first reply stands in for it.
                    parent = None
            body = text_of(m)
            ms = int(float(m["ts"]) * 1000)
            db.execute(
                "INSERT INTO messages(id, channel_id, author_id, body, created_at_ms, parent_id, author_name, kind) "
                "VALUES(?,?,?,?,?,?,?,0)",
                (msg_id, ch_id, author, body.encode(), ms, parent,
                 (m.get("username") or (m.get("bot_profile") or {}).get("name") or "integration") if bot else None))
            if m not in replies:
                by_ts[m["ts"]] = msg_id
            elif parent is None:
                by_ts.setdefault(m["thread_ts"], msg_id)
            if not bot:
                members.add(author)
        for u in members:
            db.execute("INSERT OR IGNORE INTO channel_members(channel_id, user_id, joined_at_ms) VALUES(?,?,0)",
                       (ch_id, u))
    db.commit()
    print(f"{ch_id} channels, {msg_id} messages")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
