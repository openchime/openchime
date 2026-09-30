#!/usr/bin/env bash
# The terminal client signs in with a device code, end to end (AUTH.md §8.11).
#
# A daemon WITHOUT the test knob, the TUI in tmux with the workspace named and
# Remember me off (nothing reaches this machine's keyring). The TUI shows a code,
# the URL and a QR code; curl plays the person's phone -- the /device page, the
# code, the password, Approve -- and the TUI goes on into the workspace. The
# waiting screen is kept as text and as a PNG under $OUT.
#
#   scripts/tui_device.sh            # after `make openchimed tui`
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${OC_TUI_DEV_PORT:-8561}"
D="$(mktemp -d /tmp/oc-tui-device.XXXXXX)"
OUT="${TUI_DEVICE_OUT:-$D/out}"
SESSION="oc-tui-device-$$"
mkdir -p "$OUT" "$D/home" "$D/blobs"

fail() { echo "FAIL: $*" >&2; cleanup; exit 1; }
cleanup() {
    tmux kill-session -t "$SESSION" 2>/dev/null || true
    for p in $(pgrep -x openchimed || true); do
        tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qx "OPENCHIME_PROTO_PORT=$PORT" && kill "$p" 2>/dev/null
    done
    [ -z "${KEEP:-}" ] && rm -rf "$D/db"* "$D/home" "$D/blobs" "$D/"*.pem
    return 0
}
screen() { tmux capture-pane -t "$SESSION" -p; }
await() {   # a regex the screen must come to show
    for _ in $(seq 1 100); do screen | grep -qE "$1" && return 0; sleep 0.1; done
    return 1
}

# --- the daemon, as shipped ---------------------------------------------------
env -u OPENCHIME_TEST_PASSWORD_AUTH OPENCHIME_DB_PATH="$D/db" OPENCHIME_TLS_CERT="$D/cert.pem" \
    OPENCHIME_TLS_KEY="$D/key.pem" OPENCHIME_BLOB_DIR="$D/blobs" OPENCHIME_PROTO_PORT="$PORT" \
    OPENCHIME_HEALTH_PORT=0 OPENCHIME_WORKSPACE_NAME="Device Code" OPENCHIME_BOOTSTRAP_USERS="dee:pw-dee:owner" \
    setsid "$HERE/openchimed" > "$D/daemon.log" 2>&1 < /dev/null &
disown
for _ in $(seq 1 50); do (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && { exec 3<&- 3>&-; break; }; sleep 0.2; done
grep -q 'OPENCHIME_TEST_PASSWORD_AUTH' "$D/daemon.log" && fail "the daemon has the test knob on"

# --- the TUI -------------------------------------------------------------------
tmux new-session -d -s "$SESSION" -x 110 -y 50 "env HOME=$D/home $HERE/build/openchime-tui 127.0.0.1:$PORT"
await 'Sign in to OpenChime' || fail "no sign-in dialog"
tmux send-keys -t "$SESSION" Space                   # Remember me off (focus starts on it)
await '\[ \] Remember me' || fail "Remember me did not turn off"
tmux send-keys -t "$SESSION" Enter
await 'enter the code' || fail "no device code shown: $(screen | head -20)"
sleep 0.5
screen > "$OUT/tui-device.txt"
URL="$(grep -oE 'https://[^ ]+/device\?code=[A-Z-]+' "$OUT/tui-device.txt" | head -1)"
CODE="$(sed -n 's/.*enter the code *\([A-Z]\{4\}-[A-Z]\{4\}\).*/\1/p' "$OUT/tui-device.txt" | head -1)"
[ -n "$URL" ] && [ -n "$CODE" ] || fail "could not read the URL and code"
[ "$URL" = "https://127.0.0.1:$PORT/device?code=$CODE" ] || fail "unexpected URL $URL"
grep -q '▀\|▄' "$OUT/tui-device.txt" || fail "no QR code drawn"
FP="$(openssl x509 -in "$D/cert.pem" -noout -fingerprint -sha256 | sed 's/.*=//')"
grep -qF "$FP" "$OUT/tui-device.txt" || fail "the daemon's fingerprint is not shown"
echo "ok: the TUI shows $CODE, the URL, a QR code and the fingerprint"
# The QR code, as drawn, decodes to the URL -- where OpenCV is here to read it.
if python3 -c 'import cv2' 2>/dev/null; then
    GOT="$(python3 - "$OUT/tui-device.txt" "$OUT/qr.png" <<'PY'
import sys, cv2, numpy as np
rows = [l[4:] for l in open(sys.argv[1], encoding='utf-8').read().split('\n') if any(c in l for c in '█▀▄')]
img = np.zeros((len(rows) * 2, max(len(r) for r in rows)), np.uint8)
for y, r in enumerate(rows):
    for x, c in enumerate(r):
        if c in '█▀': img[2 * y, x] = 255
        if c in '█▄': img[2 * y + 1, x] = 255
img = cv2.copyMakeBorder(img, 4, 4, 4, 4, cv2.BORDER_CONSTANT, value=255)
img = cv2.resize(img, (img.shape[1] * 10, img.shape[0] * 10), interpolation=cv2.INTER_NEAREST)
cv2.imwrite(sys.argv[2], img)
print(cv2.QRCodeDetector().detectAndDecode(img)[0])
PY
)"
    [ "$GOT" = "$URL" ] || fail "the QR code decodes to '$GOT', not $URL"
    echo "ok: the QR code decodes to the URL"
fi

# --- the phone: the page, the code, the password, Approve --------------------
ORIGIN="https://127.0.0.1:$PORT"
curl -sk "$URL" | grep -q "$CODE" || fail "the page does not know the code"
R="$(curl -sk -X POST -H "Origin: $ORIGIN" --data "code=$CODE&username=dee&password=wrong&action=approve" "$ORIGIN/device")"
grep -q "isn&#39;t right" <<<"$R" || fail "a wrong password was not refused"
R="$(curl -sk -X POST -H "Origin: $ORIGIN" --data "code=$CODE&username=dee&password=pw-dee&action=approve" "$ORIGIN/device")"
grep -q 'Go back to your terminal' <<<"$R" || fail "approval failed: $R"
await '#general|general' || fail "the TUI did not reach the workspace: $(screen | head -20)"
screen > "$OUT/tui-signed-in.txt"
echo "ok: approved on the page; the TUI signed in"

cleanup
echo "PASS: device-code sign-in in the terminal (screens in $OUT)"
