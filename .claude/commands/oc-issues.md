---
description: Show all open GitHub issues as a table — number, title, owner, created, status — newest first
argument-hint: [optional filter, e.g. a label or search words]
---

Show the repo's open issues as one table. Optional filter from the user:
$ARGUMENTS

## 1. Fetch

Get every open issue in one call — do not page by hand or trust a default
limit (this tracker holds 100+ open issues):

```
gh issue list --state open --limit 500 \
  --json number,title,assignees,createdAt,state,labels
```

If the user gave a filter, apply it: a bare label name goes to `--label`,
anything else to `--search`.

## 2. Render

One markdown table, **sorted by number, highest first**, one row per issue,
five columns:

| Column | What goes in it |
|---|---|
| **Number** | The issue number. |
| **Title** | Verbatim. Do not truncate — the titles are written to be read. |
| **Owner** | The assignee login(s), comma-separated; `—` when unassigned. |
| **Created** | `createdAt` as `YYYY-MM-DD`. |
| **Status** | `open`, followed by the labels when present (` · ` separated, e.g. `open · win32, defect`). The labels are where this tracker expresses what kind of open an issue is. |

Build the table from the JSON (a `python3`/`jq` one-liner is fine); do not
hand-copy values between fetch and render — a transcription slip in a
100-row table is invisible.

## 3. After the table

Two lines, no more: the total count, and the count per label kind
(`missing-feature` / `defect` / `verification-gap` / unlabeled). Do not
summarize, group, or editorialize beyond that — the table is the deliverable.
