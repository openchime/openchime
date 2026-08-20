---
description: Start a project — find or create its GitHub issue, cut a feature branch, load context
argument-hint: <issue number | short description of the work>
---

Begin a new project on this repo. The argument is either an existing issue
number or a short description of new work: $ARGUMENTS

Follow these steps in order. Stop and ask the user if any step finds something
unexpected (a dirty tree with unrelated changes, a diverged `main`, an issue
that is already closed or already has a branch).

## 1. Preflight

- `git status` must be clean. If it is not, show the user what is there and ask
  before touching anything.
- Be on `main` and current: `git checkout main && git pull --ff-only origin main`.
  A non-fast-forward pull means local `main` has drifted — stop and ask.

## 2. The issue

Every project has exactly one GitHub issue. The tracker is the project's only
issue list and the place priority is expressed.

- **If the argument is a number:** `gh issue view <number>` and read the whole
  body. If it is closed, stop and ask.
- **If the argument is a description:** search first —
  `gh issue list --state open --search "<key words>"` — so a duplicate is not
  created. If nothing covers it, create one:
  - The title is written in the tracker's declarative voice: it states what is
    wrong or missing as a fact ("The daemon exits 0, silently, when it cannot
    start"), not as a task ("Fix daemon exit code").
  - The body says what is missing and what it costs, and cites `REQ-N` /
    `ARCH-N` where the requirement or decision exists.
  - Apply an existing label pair only where one genuinely fits (`win32`,
    `daemon`, `testing`, `packaging-release` × `missing-feature`, `defect`,
    `verification-gap`). Workflow/tooling work takes no label.

Remember the issue number for commit messages — and only commit messages. **No
file may cite an issue by number** (title, body, code, docs, scripts — nothing);
`make check-refs` enforces this and runs in CI. In files, say the thing or cite
`REQ-N` / `ARCH-N`.

## 3. The branch

- **Docs-only work goes straight to `main`** — no branch, even multi-file.
  Everything else gets a branch.
- Name it kebab-case, descriptively, for the **change** — never for the issue
  number: `message-management`, `tuikit-phase-4`. One logical change per branch.
- `git checkout -b <name>` from the current `main`.

## 4. Context before code

Before writing anything, read what the issue touches:

- the `REQ-N` sections in docs/REQUIREMENTS.md and `ARCH-N` entries in
  docs/ARCHITECTURE.md that the issue names or implies;
- the subsystem doc that owns the area (PROTOCOL.md for wire changes, SCHEMA.md
  for migrations, CLIENT.md for frontend work, TESTING.md for what must be
  proven, AUTH.md, AUDIO.md, VIDEO.md as applicable);
- the code the change will land in.

Two standing traps to load now rather than trip later:

- A frame **layout** change (new field, reorder, optional→required) bumps
  `OC_PROTOCOL_VERSION`; adding a frame does not. Reassigning an opcode counts
  as a layout change.
- Schema changes are new numbered migrations, never edits to an existing one.

## 5. Report

Tell the user: the issue (number and title), the branch name, what you read,
and a short plan of the work. Then begin.
