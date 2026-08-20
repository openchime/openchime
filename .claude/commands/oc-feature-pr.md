---
description: Raise the feature PR — tests clean with no warnings, branch squashed to one commit and pushed, PR with empty body
argument-hint: (none — operates on the current feature branch)
---

Raise the pull request for the current feature branch. Refuse — report and do
nothing — if any check below fails.

## 1. Where you are

- The current branch must be a `feature/oc-<number>-…` branch. Anything else:
  refuse.
- `git fetch origin --prune` so every judgement below is against current
  remote state.

## 2. Tests pass cleanly — no warnings, no errors

- Run `make` and `make test` and **read the full output**, not the exit code:
  zero compiler warnings (the tree builds `-Wall -Wextra`), zero test
  failures, zero check failures. A warning is a failure here.
- Anything non-clean: refuse, quote the offending output, and stop.

## 3. One commit on the branch

The branch lands as exactly **one commit** ahead of `origin/staging`. If it
has more, squash them:

```
git reset --soft $(git merge-base origin/staging HEAD)
git commit -s -m "<area>: <imperative summary>"
```

The single commit's message follows the house format — `area: imperative
summary`, sentence case, no trailing period, sign-off, the issue cited as
`Closes #<number>` — and must contain nothing that trips the attribution
guard (no attribution trailers, never the name of the directory these
commands live in).

## 4. Clean and pushed

- `git status --porcelain` must print nothing.
- Push the branch. If the squash rewrote already-pushed history, use
  `git push --force-with-lease` — the **only** situation and the **only**
  branch class where a force-push is ever acceptable; `staging` and `main`
  are never force-pushed.
- Verify the remote tip now equals the local tip and is exactly one commit
  ahead of `origin/staging`.

## 5. The PR

```
gh pr create --base staging --head <branch> --title "<the commit's subject>" --body ""
```

- Base is `staging`, always.
- The title is the single commit's subject line, verbatim.
- **The body is empty** — no commit message, no extended description, no
  summary, no checklist. The issue and the commit already say everything.

## 6. Report

Give the user the PR URL, the single commit's subject, and the test result —
one line each.
