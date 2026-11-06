# scripts/test/lib/common.sh: sourced by every test script, never run.
#
# The tests come in two suites (docs/TESTING.md): the fast one, scripts in
# scripts/test/fast/, run by `make test` before every push; and the full one,
# scripts in scripts/test/full/, run by `make test-all` in CI, once per
# release-candidate. A make target calls the scripts; the workflows call only
# the targets. Each script starts with the same header: what it proves, its
# arguments, what it needs, and the target that runs it. Which suites of
# build/tests belong to which is SUITES[] in tests/main.c, read back with
# `build/tests --list fast|full|threads`.

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../../.."
TEST=$(basename "$0" .sh)

# suites fast|full|threads <test binary command...>: the names the binary
# lists (`--list`), comma-separated, as OC_TEST_ONLY takes them.
suites() {
    local which=$1 out; shift
    out=$("$@" --list "$which") || fail "$* --list $which failed"
    [ -n "$out" ] || fail "$* --list $which names no suite"
    tr '\n' ',' <<<"$out" | sed 's/,$//'
}

# error <message>: say what failed -- as an annotation in GitHub Actions, as a
# line on stderr anywhere else -- and go on. fail <message>: the same, then
# exit 1. ok <message>: the test's one line of success.
error() {
    if [ -n "${GITHUB_ACTIONS:-}" ]; then echo "::error::$TEST: $*"; else echo "$TEST: FAIL: $*" >&2; fi
}
fail() { error "$@"; exit 1; }
ok() { echo "$TEST: ok: $*"; }

# need <command> <what provides it>: stop at once, saying what is missing,
# rather than fail later in a way that reads like a bug.
need() {
    command -v "$1" >/dev/null 2>&1 && return 0
    echo "$TEST: needs $1 ($2)" >&2
    exit 2
}

# have <file> <what makes it>: the same, for something a build makes.
have() {
    [ -e "$1" ] && return 0
    echo "$TEST: $1 is missing ($2)" >&2
    exit 2
}

# runnable <binary>: whether this machine can execute <binary>. An arm64
# package built on an x86 runner is checked by what reads it (readelf, the
# package tools) and not run; the caller says so and skips that part.
runnable() {
    local m
    m=$(readelf -h "$1" 2>/dev/null | sed -n 's/^ *Machine: *//p')
    case "$(uname -m):$m" in
        x86_64:*X86-64*|aarch64:*AArch64*) return 0 ;;
        *) return 1 ;;
    esac
}

# wait_healthz <url> [seconds] [pid]: 0 once <url> answers OK, 1 after
# <seconds> (60), or at once when <pid> is given and has exited.
wait_healthz() {
    local url=$1 secs=${2:-60} pid=${3:-} i out
    for ((i = 1; i <= secs; i++)); do
        out=$(curl -fsS "$url" 2>/dev/null || true)
        if [ "$out" = OK ]; then echo "$TEST: healthz OK after ${i} s"; return 0; fi
        if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then return 1; fi
        sleep 1
    done
    return 1
}

# image_runnable <image>: whether this machine can run <image>'s architecture.
image_runnable() {
    local a
    a=$(podman image inspect --format '{{.Architecture}}' "$1" 2>/dev/null || true)
    case "$(uname -m):$a" in
        x86_64:amd64|aarch64:arm64) return 0 ;;
        *) return 1 ;;
    esac
}
