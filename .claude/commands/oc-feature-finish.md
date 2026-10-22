---
description: Finish a feature — build and test clean, one commit, push, and raise the PR into staging with an empty body
argument-hint: (none — operates on the current feature branch)
---

Finish the current feature branch and raise its pull request. Refuse — report
and do nothing else — if any step below fails.

This is the whole local sequence, and it is budgeted at 2 minutes. Run
**exactly** these steps, in this order, and nothing else. In particular: **no
`make test-ci`, no `make CC=clang …`, no Windows builds, no `make clean`, no
`make check-release-cc`, no re-runs of the suite.** CI runs the sanitizer, the
release compiler and the Windows builds on the pull request, and staging requires them to pass before a
merge (docs/CONTRIBUTING.md). Duplicating them here only spends time.

## 1. Where you are

- The current branch must be `feature/oc-<number>-…`. Anything else: refuse.
- `git fetch origin --prune`.

## 2. Build and test locally — required, before the commit

```
scripts/check/test.sh
```

It runs `make && make test` and on a pass records the tree it passed on; the
pre-push hook (`.githooks/pre-push`) refuses to push a tree it did not record.
Read the full output, not the exit code. Zero warnings (the tree builds
`-Werror`), zero failed checks, and the last line `OK: all suites passed`.
Anything else: refuse, quote the failing output, fix it, and run this again —
**never push to find out.** CI is the confirmation, not where a failure is
found: every CI run is billed by the minute, and a local run costs nothing. A
change after this step means running it again before the commit. Only a
change to documentation alone (`*.md`, `.gitignore`, `.claude/`) may skip it;
CI does not build one either.

## 3. The title

```
scripts/check/pr.sh "OC-<number>: <Capitalised imperative summary>"
```

It must pass: the house format, and at most 72 characters once GitHub appends
` (#<pr>)`.

## 4. One commit

The branch lands as exactly **one commit** ahead of `origin/staging`. If it has
more, squash:

```
git reset --soft $(git merge-base origin/staging HEAD)
git commit -s -m "OC-<number>: <the title from step 3>"
```

The body is the `Signed-off-by:` trailer and nothing else: no description, no
`Closes #<number>`, no other trailer, nothing the attribution guard would catch.

## 5. Clean and pushed

- `git status --porcelain` prints nothing.
- No run for the branch is still going: a push would cancel it part way, billed
  for what it did. The pre-push hook refuses it; wait, and put every fix in one
  push.
- After a CI failure: reproduce it locally, fix it, run step 2 again, then push
  once. Never push a guess.
- Push. `git push --force-with-lease` only if step 4 rewrote history already
  pushed — the one case, and the one branch class, where a force-push is
  acceptable; `staging` and `main` are never force-pushed.
- The remote tip equals the local tip, exactly one commit ahead of
  `origin/staging`.

## 6. The pull request

```
gh pr create --base staging --head <branch> --title "<the commit's subject>" --body ""
```

Base `staging`, the title the commit's subject verbatim, the body empty.

## 7. Report

One line each: the issue number and title, the commit's subject, the test
result. Never the PR's number. CI's result is reported when it finishes; it is
not waited for here.
