#!/usr/bin/env bash
# scripts/test/fast/actionlint.sh
#
# The workflows are what actionlint accepts: expressions, job graphs, inputs
# and the shell they run. A workflow runs first when it is dispatched, after
# it has merged, so this is the one place it is checked before then.
# actionlint is fetched pinned, its tarball checked against the SHA-256 below,
# into ~/.cache/openchime/actionlint-<version>/ on first use; nothing is
# installed. ShellCheck's pass is off (-shellcheck=): the steps' shell is held
# to the tree's own rules by tests_lint.sh.
#
# Needs: curl and sha256sum on first use; network then.
# Run by: make test.
. "$(dirname "$0")/../lib/common.sh"
v=1.7.12
case "$(uname -m)" in
    x86_64)  arch=amd64 sum=8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8 ;;
    *) echo "$TEST: no pinned actionlint for $(uname -m)" >&2; exit 2 ;;
esac
dir="${XDG_CACHE_HOME:-$HOME/.cache}/openchime/actionlint-$v"
if [ ! -x "$dir/actionlint" ]; then
    need curl "curl"
    mkdir -p "$dir"
    curl -fsSL -o "$dir/a.tgz" "https://github.com/rhysd/actionlint/releases/download/v$v/actionlint_${v}_linux_${arch}.tar.gz" \
        || fail "could not fetch actionlint $v"
    echo "$sum  $dir/a.tgz" | sha256sum -c --quiet - || { rm -f "$dir/a.tgz"; fail "actionlint $v's tarball does not match its pinned SHA-256"; }
    tar -xzf "$dir/a.tgz" -C "$dir" actionlint && rm -f "$dir/a.tgz"
fi
"$dir/actionlint" -shellcheck= || fail "actionlint rejects the workflows (above)"
ok "the workflows pass actionlint $v"
