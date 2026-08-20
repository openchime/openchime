---
description: Finish a project — green CI, fast-forward merge (or PR), close the issue, delete the branch
argument-hint: [pr — open a pull request for a review record instead of merging directly]
---

Land the current project. Optional mode from the user: $ARGUMENTS

The landing sequence is: everything committed → checks green locally → the
closing commit cites the issue → CI green on the branch → fast-forward merge →
issue closed → branch deleted. Stop and ask the user before the merge step if
anything on the way was red, skipped, or surprising.

## 1. Nothing left behind

- `git status` clean — every intended change is committed, and nothing
  unintended is. Warn the user if work would be abandoned.
- The documents match the code: REQUIREMENTS.md status markers, the ARCH entry,
  and the owning subsystem doc all say what is now true. A merged feature whose
  requirement still reads *(Not built)* — or the reverse — is a defect.

## 2. Full local proof

- `make test` (runs check-refs and check-opcodes as prerequisites) and read the
  output.
- Win32 chrome changed? `scripts/gui_smoke.sh` — CI cannot run it, so this is
  its only gate.

## 3. The closing commit cites the issue

The commit that resolves the issue carries `Closes #<number>` in its message,
so landing on `main` closes it. Check **before** the final push — amending
after a push needs a force-push, which this repo's linear history should never
see. If the resolving commit already went up without it, close the issue by
hand in step 6 instead; do not rewrite pushed history.

Commit rules still apply at the finish: `area: imperative summary`, sign-off
(`-s`), and nothing in the message that trips the attribution guard — no
attribution trailers, and never the name of the directory these commands live
in (its path contains a banned word; the guard deletes offending branches from
the remote).

## 4. Green CI on the branch

- `git push`, then `gh run list --branch <branch>` and watch every triggered
  workflow to completion — the build jobs and the guard.
- **Gate the merge on CI.** A fast-forward puts the identical commit on `main`,
  so a green branch is what keeps `main` green. Red means fix, re-push, and
  re-watch; it never means merge anyway.

## 5. Merge

PRs are optional here. Default to the direct fast-forward; use a PR when the
user asked for one, said `pr`, or the change wants a review record.

**Direct (default):**

```
git checkout main
git pull --ff-only origin main
git merge --ff-only <branch>
git push origin main
```

`--ff-only` failing means `main` moved since the branch was cut: rebase the
branch onto fresh `main`, re-run CI on it (the rebased commits are new), and
try again. No merge commits, ever — history stays linear.

**PR (`pr` mode):** `gh pr create --fill`, wait for its checks, then land it
with `gh pr merge --rebase --delete-branch` — rebase keeps `main` linear where
a merge commit would not. Merge only after checks pass.

## 6. Close out

- Confirm the issue closed (`gh issue view <number>`); if the closing keyword
  was missed, `gh issue close <number> --comment "<what landed, and the merge
  commit's subject>"`.
- Delete the branch, local and remote, unless the PR merge already did:
  `git branch -d <branch> && git push origin --delete <branch>`.
- `git log --oneline -3` on `main` to confirm the result is what you meant.

## 7. Report

Tell the user: the merge commit(s) now on `main`, the CI result, the closed
issue, and that the branch is gone. If anything was skipped or is still open —
a follow-up worth its own issue, a doc not updated — say so plainly and offer
to file it.
