---
description: Finish a feature — build and test clean, one commit, push, and raise the PR into staging with an empty body
argument-hint: (none — operates on the current feature branch)
---

Finish the current feature branch and raise its pull request. Refuse — report
and do nothing else — if any step below fails.

This is the whole local sequence, and it is budgeted at 2 minutes. Run
**exactly** these steps, in this order, and nothing else. In particular: **no
`make test-ci`, no `make CC=clang …`, no Windows builds, no `make clean`, no
`make check-release-cc`, no re-runs of the suite.** Nothing builds or tests the
pull request: the sanitizer, the release compiler and the Windows builds run in
the staging build, dispatched by hand on `staging` once changes have merged
(docs/CONTRIBUTING.md). Duplicating them here only spends
time.

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
`-Werror`), zero failed checks, and the suite's `OK: all suites passed`,
followed by `tested: tree … recorded`.
Anything else: refuse, quote the failing output, fix it, and run this again —
**never push to find out.** This is the only build and test the change gets
before it merges: the pull request runs only its policy checks, and the staging
build that follows is the confirmation, not where a failure is found — a
failure there holds back everything merged with the change. A change after this step means running it again before the commit. Only
a change to documentation alone (`*.md`, `.gitignore`, `.claude/`) may skip it.

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
- After a policy failure on the pull request (`pr policy`, `guard`): fix it,
  run step 2 again if anything but the title changed, then push once. Never
  push a guess.
- Push. `git push --force-with-lease` only if step 4 rewrote history already
  pushed — the one case, and the one branch class, where this command
  force-pushes. `main` is never force-pushed, and `staging` only by an admin
  in the one case docs/CONTRIBUTING.md "Commits" names, never from here.
- The remote tip equals the local tip, exactly one commit ahead of
  `origin/staging`.

## 6. The pull request

```
gh pr create --base staging --head <branch> --title "<the commit's subject>" --body ""
```

Base `staging`, the title the commit's subject verbatim, the body empty.

## 7. Report

One line each: the issue number and title, the commit's subject, the test
result. Never the PR's number. Nothing is waited for: the pull request's only
checks are `pr policy` and `guard`, and the build and tests come with the next
staging build.
