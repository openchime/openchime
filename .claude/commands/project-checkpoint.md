---
description: Mid-project checkpoint — commit in the house format, run the checks, push, then read CI
argument-hint: [optional note about where the work stands]
---

Checkpoint the project in progress. Optional context from the user: $ARGUMENTS

This is the middle-of-project discipline: turn the working tree into clean
commits, prove them locally, push, and — the part that gets skipped — **read
the run**. Report honestly at the end, including anything red or unfinished.

## 1. Take stock

- `git status` and `git diff` — account for every changed file. A file you
  cannot explain does not get committed; ask the user about it.
- Group the changes into **logical commits**, not one bucket. One logical
  change per commit; unrelated fixes found along the way get their own.

## 2. Commit format

- `area: imperative summary` — sentence case, **no trailing period**. The
  prefix names the area (`daemon:`, `GUI:`, `TUI:`, `docs:`, `build:`,
  `tooling:`, …), not a fixed type set.
- Cite decision ids inline where relevant: `ARCH-N`, `REQ-N`.
- The issue number belongs in the **commit message only** — never in a file
  (`make check-refs` enforces it). Cite it as `#<number>`, or save
  `Closes #<number>` for the finishing commit that should close the issue on
  merge.
- Sign off every commit: `git commit -s` (DCO, no CLA).
- **The attribution guard scans author, committer and the whole message on
  every push, case-insensitively, for the assistant's name and its vendor's —
  anywhere, not just trailers.** A match on a non-default branch **deletes the
  branch from the remote** within seconds (local commits survive; `main` is
  protected and blocks instead). Two consequences: no `Co-Authored-By` or
  "Generated with" trailer, ever — and a commit message must never name the
  directory these commands live in, whose path contains the banned word. Say
  "the commands directory". File *contents* are not scanned; commit metadata
  is.

## 3. Prove it locally before pushing

Run what the change class requires, and read the output rather than the exit
banner:

- **Always:** `make check-refs` (no file cites an issue by number).
- **Code changed:** `make test` — the unit + in-process integration binary; it
  runs check-refs and check-opcodes as prerequisites.
- **Protocol touched:** confirm `make check-opcodes` passes and that a frame
  *layout* change bumped `OC_PROTOCOL_VERSION` (adding a frame needs no bump;
  reassigning an opcode does).
- **Win32 chrome touched:** run `scripts/gui_smoke.sh` — it is not in CI, so
  running it here is the only gate it has.

## 4. Push, then read the run

A green local suite is not a substitute for reading CI — the wire once broke
for eleven consecutive pushes while `make test` stayed green.

- `git push -u origin <branch>`
- `gh run list --branch <branch> --limit 5`, then `gh run watch <id>` (or poll)
  until the runs finish. The guard workflow runs on **every** push, including
  docs-only ones; the build jobs skip docs-only changes via `paths-ignore`.
- A red run is this checkpoint's work, not the next one's. Diagnose it now.

## 5. Record state where it belongs

- If the project's shape changed — scope moved, a decision was taken, a
  blocker was found — leave a short comment on the issue
  (`gh issue comment <number>`), so the tracker stays the place priority and
  status live.
- If the change altered behavior a document specifies, update the document in
  the same branch: the status markers in REQUIREMENTS.md, the ARCH entry, the
  subsystem doc. A doc that overstates what ships is the failure the markers
  exist to prevent.

## 6. Report

Tell the user: what was committed (subjects), what was run and its result, the
CI outcome with anything red quoted, and what remains. If tests fail, say so
with the output — never round a partial success up.
