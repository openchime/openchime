#!/usr/bin/env bash
# main is a line of snapshot commits: each move of
# main appends one commit whose parent is main as it was, so main only ever
# fast-forwards, here and on the public copy. staging holds the real
# commits; main holds what was released plus the documentation promoted since.
# Every snapshot carries the trailer "Snapshot: release" or "Snapshot:
# documentation", which is how one is recognised:
#
#   Release N       tree: the candidate's, with the promoted documentation the
#                   candidate does not contain applied on top.
#                   Trailers: Snapshot: release, Candidate: <sha>, and
#                   Docs: <sha> <patch-id> per documentation commit carried.
#   Documentation   tree: main's, with documentation-only staging commits
#                   applied on top. Trailers: Snapshot: documentation, and
#                   Docs: <sha> <patch-id> per commit applied.
#
# A commit is applied as its own change, three-way (git merge-tree with the
# commit's parent as the base), onto the tree so far; nothing is cherry-picked
# and no history is shared. Everything is built in the object store, without
# a worktree, and the new commit is printed, unreferenced, for the caller to
# push with a plain (fast-forward) push. A Docs trailer names the commit and
# its patch-id (git patch-id --stable), so the change is still recognised when
# staging has been rewritten and the commit is gone.
#
#   scripts/release/main_next.sh last [<main>]
#       the last Release snapshot's candidate, or nothing before the first
#   scripts/release/main_next.sh base <commit> [<main>]
#       where the commits a release of <commit> brings begin: the last
#       release's candidate, or main's merge base with <commit> before one
#   scripts/release/main_next.sh release <candidate> <N> [<main>]
#       the Release N snapshot; main itself when its last release already is
#       <candidate> (a re-run). Refused (exit 1) when <candidate> does not
#       descend from the last release's candidate and the newest release-N tag
#       (no downgrade), or when promoted documentation it lacks cannot be
#       carried onto it: it does not apply, or its commit is gone (staging
#       rewritten) and its change is not among the candidate's commits.
#   scripts/release/main_next.sh docs <staging> [<main>]
#       the Documentation snapshot of the documentation-only commits from the
#       last release's candidate to <staging> that main has not taken yet
#       (scripts/release/docs_only.sh), in staging's order; nothing when there
#       are none or they change nothing. One that does not apply, or that
#       names an OPENCHIME_ setting main's daemon/config_table.c does not have,
#       is skipped and named.
#
# <main> is origin/main by default. What it decided goes to stderr. Exit 0
# with an answer, 1 refused, 2 on any other failure, a commit on main the
# pipeline did not write among them.
set -euo pipefail
shopt -s inherit_errexit
here=$(cd "$(dirname "$0")" && pwd)
say() { echo "main_next: $*" >&2; }
die() { say "$*"; exit 2; }
resolve() { git rev-parse -q --verify "${1}^{commit}" || die "cannot resolve ${1}"; }
# The pipeline's identity on a snapshot: built here, not written by anyone.
export GIT_AUTHOR_NAME='github-actions[bot]' GIT_COMMITTER_NAME='github-actions[bot]'
export GIT_AUTHOR_EMAIL='41898282+github-actions[bot]@users.noreply.github.com'
export GIT_COMMITTER_EMAIL="$GIT_AUTHOR_EMAIL"

trailer() {  # <commit> <key>: its values, one per line
  local out
  out=$(git log -1 --format="%(trailers:key=$2,valueonly,separator=%x0A)" "$1") || die "cannot read ${1}"
  [ -z "$out" ] || printf '%s\n' "$out"
}
patch_id() {  # <commit>: its stable patch-id
  local diff id
  diff=$(git show --format= --no-color "$1") || die "cannot read ${1}"
  id=$(printf '%s\n' "$diff" | git patch-id --stable) || die "cannot take the patch-id of ${1}"
  printf '%s\n' "${id%% *}"
}

# The snapshots from main back to the last Release one (it included), newest
# first, "<sha> <kind>" per line. The walk ends there, or at the first commit
# that is not a snapshot when no snapshot is below it (main's history from
# before snapshots). A commit that is not a snapshot with one below it is a
# commit the pipeline did not write: refused, since main moves only by
# snapshots.
snapshots() {  # <main>
  local list sha kind below
  list=$(git rev-list --first-parent "$1") || die "cannot read ${1}"
  for sha in $list; do
    kind=$(trailer "$sha" Snapshot)
    case "$kind" in
      documentation) echo "$sha documentation" ;;
      release) echo "$sha release"; return 0 ;;
      "")
        below=$(git log --first-parent --format='%(trailers:key=Snapshot,valueonly)' "$sha") || die "cannot read ${sha}"
        if [ -n "${below//[[:space:]]/}" ]; then
          die "main has $(git log -1 --format='%h %s' "$sha"), which is not a snapshot the pipeline wrote, above snapshots that are: main moves only by a release or a promote. Put main right by hand before going on"
        fi
        return 0 ;;
      *) die "$(git log -1 --format='%h' "$sha") has an unknown Snapshot trailer '${kind}'" ;;
    esac
  done
}
last_candidate() {  # <main>
  local snaps tail sha kind cand
  snaps=$(snapshots "$1")
  [ -n "$snaps" ] || return 0
  tail=$(printf '%s\n' "$snaps" | tail -n 1)
  sha=${tail%% *}; kind=${tail#* }
  [ "$kind" = release ] || return 0
  cand=$(trailer "$sha" Candidate)
  [ -n "$cand" ] || die "$(git log -1 --format='%h' "$sha") is a Release snapshot with no Candidate trailer"
  printf '%s\n' "$cand" | head -n 1
}
# "<sha> <patch-id>" per Docs trailer of the snapshots since the last release
# (it included), each sha once.
named_docs() {  # <main>
  local snaps sha kind out=""
  snaps=$(snapshots "$1")
  [ -n "$snaps" ] || return 0
  while read -r sha kind; do
    out+=$(trailer "$sha" Docs)$'\n'
  done < <(printf '%s\n' "$snaps" | tac)
  printf '%s' "$out" | awk 'NF && !seen[$1]++'
}
# The patch-ids of the commits <from>..<to>, one per line.
patch_ids() {  # <from> <to>
  local log
  log=$(git log -p --no-merges --format='commit %H' --no-color "${1}..${2}") || die "cannot read ${1}..${2}"
  [ -z "$log" ] || printf '%s\n' "$log" | git patch-id --stable | awk '{print $1}'
}
# apply <commit-so-far> <docs commit>: prints the commit with the change on
# top (a temporary, unreferenced commit); exit 1 when it does not apply.
apply() {
  local out rc=0 tree
  out=$(git merge-tree --write-tree --merge-base="${2}^" "$1" "$2" 2>/dev/null) || rc=$?
  case "$rc" in
    0) tree=$(printf '%s\n' "$out" | head -n 1)
       git commit-tree "$tree" -p "$1" -m "apply ${2}" || die "cannot write a commit" ;;
    1) return 1 ;;
    *) die "cannot merge ${2}" ;;
  esac
}
# Does <docs commit> name an OPENCHIME_ setting <main>'s registry lacks? Prints
# the first such name. A cheap stand-in for scripts/check/config.sh, which
# needs the daemon built: documentation describing a setting reaches main with
# the code that has it.
unknown_setting() {  # <docs commit> <main>
  local table added n
  table=$(git show "${2}:daemon/config_table.c" 2>/dev/null || true)
  added=$(git show --format= --no-color "$1") || die "cannot read ${1}"
  added=$(printf '%s\n' "$added" | sed -n 's/^+//p' | grep -oE 'OPENCHIME_[A-Z0-9_]+' | sort -u || true)
  for n in $added; do
    grep -qF -- "$n" <<<"$table" && continue
    [[ "$n" =~ ^(.*_)[0-9]+$ ]] && { grep -qF -- "${BASH_REMATCH[1]}%d" <<<"$table" || grep -qF -- "${BASH_REMATCH[1]}N" <<<"$table"; } && continue
    printf '%s\n' "$n"; return 0
  done
  return 0
}

verb="${1:-}"; [ "$#" -gt 0 ] && shift
case "$verb" in
  last)
    main=$(resolve "${1:-origin/main}")
    last_candidate "$main"
    ;;
  base)
    [ "$#" -ge 1 ] || die "usage: main_next.sh base <commit> [<main>]"
    c=$(resolve "$1"); main=$(resolve "${2:-origin/main}")
    last=$(last_candidate "$main")
    if [ -n "$last" ]; then echo "$last"; else git merge-base "$main" "$c" || die "main and ${c:0:7} have no common commit"; fi
    ;;
  release)
    [ "$#" -ge 2 ] && [[ "$2" =~ ^[0-9]+$ ]] || die "usage: main_next.sh release <candidate> <N> [<main>]"
    cand=$(resolve "$1"); n="$2"; main=$(resolve "${3:-origin/main}")
    short=$(git rev-parse --short "$cand")
    last=$(last_candidate "$main")
    if [ "$last" = "$cand" ]; then say "main's last release already is ${short}"; echo "$main"; exit 0; fi
    if [ -n "$last" ]; then
      git cat-file -e "${last}^{commit}" 2>/dev/null || die "the last release's candidate ${last:0:7} is not here"
      git merge-base --is-ancestor "$last" "$cand" || {
        say "${short} does not descend from the last release's candidate ${last:0:7}: a release cannot go back"; exit 1; }
    fi
    tags=$(git tag -l 'release-*') || die "cannot list the tags"
    tag=$(printf '%s\n' "$tags" | { grep -E '^release-[0-9]+$' || true; } | sort -t- -k2 -n | tail -n 1)
    if [ -n "$tag" ] && ! git merge-base --is-ancestor "${tag}^{commit}" "$cand"; then
      say "${short} does not descend from ${tag}: a release cannot go back"; exit 1
    fi
    named=$(named_docs "$main")
    since=${last:-$(git merge-base "$main" "$cand")} || die "main and ${short} have no common commit"
    have=""; todo=()
    while read -r d pid; do
      [ -n "${d:-}" ] || continue
      if git cat-file -e "${d}^{commit}" 2>/dev/null; then
        git merge-base --is-ancestor "$d" "$cand" && continue
        todo+=("$d"); continue
      fi
      # The commit is gone: staging was rewritten. Its change is known by its
      # patch-id; the candidate either has it among its own commits, or it
      # cannot be carried.
      [ -n "$have" ] || have=$(patch_ids "$since" "$cand")$'\n'
      if [ -n "${pid:-}" ] && grep -qxF -- "$pid" <<<"$have"; then continue; fi
      say "main has documentation from ${d:0:7}${pid:+ (patch ${pid:0:12})}, a commit no longer in this repository, and ${short} does not contain that change. Land the change on staging again and make a release-candidate of a commit that contains it"
      exit 1
    done <<<"$named"
    cur="$cand"; carried=""
    if [ "${#todo[@]}" -gt 0 ]; then
      # In staging's order, which is the order they were written in.
      order=$(git rev-list --topo-order --reverse "${todo[@]}" "^${since}") || die "cannot order the documentation"
      wanted=$(printf '%s\n' "${todo[@]}")
      for d in $order; do
        grep -qxF -- "$d" <<<"$wanted" || continue
        what=$(git log -1 --format='%h %s' "$d")
        rc=0; next=$(apply "$cur" "$d") || rc=$?
        case "$rc" in
          0) cur="$next" ;;
          1) say "main has ${what}, promoted documentation that does not apply on ${short}. Make a release-candidate of a staging commit that contains it"; exit 1 ;;
          *) exit 2 ;;
        esac
        say "carried: ${what}"
        carried+=$'\n'"Docs: ${d} $(patch_id "$d")"
      done
    fi
    tree=$(git rev-parse "${cur}^{tree}")
    msg="Release ${n}"$'\n\n'"Snapshot: release"$'\n'"Candidate: ${cand}${carried}"
    git commit-tree "$tree" -p "$main" -m "$msg" || die "cannot write the snapshot"
    ;;
  docs)
    [ "$#" -ge 1 ] || die "usage: main_next.sh docs <staging> [<main>]"
    staging=$(resolve "$1"); main=$(resolve "${2:-origin/main}")
    last=$(last_candidate "$main")
    base=${last:-$(git merge-base "$main" "$staging")} || die "main and staging have no common commit"
    named=$(named_docs "$main")
    named_shas=$(printf '%s\n' "$named" | awk 'NF {print $1}')
    named_pids=$(printf '%s\n' "$named" | awk 'NF > 1 {print $2}')
    list=$(git rev-list --reverse --topo-order --no-merges "${base}..${staging}") || die "cannot list ${base:0:7}..${staging:0:7}"
    cur="$main"; applied=""
    for d in $list; do
      grep -qxF -- "$d" <<<"$named_shas" && continue
      what=$(git log -1 --format='%h %s' "$d")
      rc=0; other=$("$here/docs_only.sh" "$d") || rc=$?
      case "$rc" in 0) ;; 1) say "skipped (changes ${other}): ${what}"; continue ;; *) exit 2 ;; esac
      pid=$(patch_id "$d")
      if [ -z "$pid" ]; then say "skipped (changes nothing): ${what}"; continue; fi
      if grep -qxF -- "$pid" <<<"$named_pids"; then say "already on main (the same change): ${what}"; continue; fi
      setting=$(unknown_setting "$d" "$main")
      if [ -n "$setting" ]; then say "skipped (names ${setting}, a setting main does not have yet): ${what}"; continue; fi
      rc=0; next=$(apply "$cur" "$d") || rc=$?
      case "$rc" in
        0) cur="$next"; applied+=$'\n'"Docs: ${d} ${pid}"; say "applied: ${what}" ;;
        1) say "skipped (does not apply to main): ${what}" ;;
        *) exit 2 ;;
      esac
    done
    tree=$(git rev-parse "${cur}^{tree}")
    if [ -z "$applied" ] || [ "$tree" = "$(git rev-parse "${main}^{tree}")" ]; then
      say "no documentation waiting"; exit 0
    fi
    msg="Documentation"$'\n\n'"Snapshot: documentation${applied}"
    git commit-tree "$tree" -p "$main" -m "$msg" || die "cannot write the snapshot"
    ;;
  *) echo "usage: scripts/release/main_next.sh last|base|release|docs ..." >&2; exit 2 ;;
esac
