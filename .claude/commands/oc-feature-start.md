---
description: Start a feature — verify everything is clean, find or create the issue, branch feature/oc-<number>-<kebab> from staging
argument-hint: <issue number | short description of the work>
---

Begin a feature on this repo. The argument is either an existing issue number
or a short description of new work: $ARGUMENTS

This command is for feature work. Docs-only changes keep going straight to
`main` and do not use it.

## 1. Preflight — refuse, do not repair

Every check below that fails means **stop, report exactly what was found, and
do nothing else**. This command never stashes, resets, force-updates, or works
around a dirty state.

- `git fetch origin --prune` first, so every judgement below is against
  current remote state.
- **Local must be clean.** `git status --porcelain` must print nothing —
  no staged, modified, or untracked files. Anything present: refuse and show
  it to the user.
- **`staging` must exist**, both locally and as `origin/staging`. If it does
  not, refuse — creating the integration branch is a repo decision a human
  makes once, not a side effect of starting a feature.
- **`staging` must be clean.** Local `staging` may not be ahead of or diverged
  from `origin/staging` — refuse if it is. Merely *behind* is the one state
  this command fixes itself, because "make sure it is current" is the point:
  `git checkout staging && git pull --ff-only origin staging`. If the
  fast-forward fails, refuse.
- **No branch already carries this issue.** If any local or remote branch is
  named `feature/oc-<this issue's number>-…`, refuse and point at it — the
  work may already be in flight.

## 2. The issue

Every feature has exactly one GitHub issue; its number goes into the branch
name below.

- **If the argument is a number:** `gh issue view <number>` and read the whole
  body. Closed issue: refuse.
- **If the argument is a description:** search first —
  `gh issue list --state open --search "<key words>"` — so a duplicate is not
  created. If nothing covers it, create one: title in the tracker's
  declarative voice (states what is wrong or missing as a fact, not a task),
  body saying what is missing and what it costs, citing `REQ-N` / `ARCH-N`
  where they exist, an existing label pair only where one genuinely fits
  (`win32`, `daemon`, `testing`, `packaging-release` × `missing-feature`,
  `defect`, `verification-gap`).

The issue number may appear in the **branch name** and in **commit messages**
— never in a file (`make check-refs` enforces it; say the thing or cite
`REQ-N` / `ARCH-N` instead).

## 3. The branch

From the tip of the now-current `staging`:

```
git checkout -b feature/oc-<number>-<short-kebab-description> staging
```

`oc` is this issue board's prefix and is always present, lowercase. The
description is a few kebab-case words naming the **change**, usually distilled
from the issue title — for the issue "The daemon exits 0, silently, when it
cannot start", the branch is `feature/oc-144-daemon-exit-nonzero`. One logical
change per branch.

## 4. Context before code

Before writing anything, read what the issue touches:

- the `REQ-N` sections in docs/REQUIREMENTS.md and `ARCH-N` entries in
  docs/ARCHITECTURE.md that the issue names or implies;
- the subsystem doc that owns the area (PROTOCOL.md for wire changes,
  SCHEMA.md for migrations, CLIENT.md for frontend work, TESTING.md for what
  must be proven, AUTH.md, AUDIO.md, VIDEO.md as applicable);
- the code the change will land in.

Two standing traps to load now rather than trip later:

- A frame **layout** change (new field, reorder, optional→required) bumps
  `OC_PROTOCOL_VERSION`; adding a frame does not. Reassigning an opcode counts
  as a layout change.
- Schema changes are new numbered migrations, never edits to an existing one.

## 5. Report

Tell the user: the issue (number and title), the branch name, that it was cut
from a clean, current `staging`, what you read, and a short plan of the work.
Then begin.
