# scripts/test/lib.sh: sourced by every test in scripts/test/, never run.
#
# Every test CI runs is one script in this directory, called by a make target;
# the workflows call only the targets (docs/TESTING.md §4). Each script starts
# with the same header: what it proves, its arguments, what it needs, and the
# target that runs it.

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../../.."
TEST=$(basename "$0" .sh)

# The suites whose code runs threads -- the loop and its I/O threads, the
# writer and readers, the worker pools, the client, the recorder and player.
# `make test-merge` runs them under ThreadSanitizer (merge/tsan.sh) and the
# others plainly (merge/suites.sh), so each runs once there. A suite with no
# thread cannot race, and instrumenting it only makes it slower.
TSAN_SUITES=run_netloop_tests,run_client_core_tests,run_dbwriter_tests,run_ioloop_tests,run_authpool_tests,run_xferpool_tests,run_slow_blob_tests,run_storage_tests,run_blob_s3_tests,run_blob_cache_tests,run_media_tests,run_video_media_tests,run_tts_worker_tests,run_stt_tests,run_push_tests,run_invite_mail_tests,run_enroll_tests,run_tls_tests,run_signin_tests,run_acme_tests,run_relaykeys_tests,run_summary_tests

# The suites too slow for the minute `make test` has (measured; the times
# make test prints at the end, slowest first). They run in make test-merge on
# every pull request; locally, local/suites.sh skips them.
SLOW_SUITES=run_netloop_tests,run_client_core_tests,run_dbwriter_tests,run_media_tests

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
