#!/usr/bin/env bash
# Runtime smoke test for the TUI (ARCH-75) against a live daemon — the headless
# check the project lacked (the TUI was only ever build-verified).
#
# Drives the real interactive client in a tmux pane: connect + auth (alice), send
# a message from the composer, receive a broadcast from a second user (bob, via
# demo_client), and assert both render in the transcript. No manual interaction.
#
# Prereqs: tmux, and this repo's toolchain (builds the daemon, TUI, demo_client).
#
# Usage: scripts/demo-tui.sh
set -euo pipefail

command -v tmux >/dev/null || { echo "tmux is required"; exit 2; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
PROTO=18445; HEALTH=18082; SESSION=oc-tui-smoke
D_PID=""
trap 'tmux kill-session -t "$SESSION" 2>/dev/null || true; kill "$D_PID" 2>/dev/null || true; rm -rf "$WORK"' EXIT

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

say "Build daemon + TUI + demo_client"
( cd "$ROOT" && make openchimed >/dev/null && make tui >/dev/null && make demo-client >/dev/null )

say "Start a local daemon (bootstrap users alice/bob)"
mkdir -p "$WORK/blobs"
OPENCHIME_DB_PATH="$WORK/tui.db" OPENCHIME_TLS_CERT="$WORK/cert.pem" OPENCHIME_TLS_KEY="$WORK/key.pem" \
OPENCHIME_BLOB_DIR="$WORK/blobs" OPENCHIME_PROTO_PORT="$PROTO" OPENCHIME_HEALTH_PORT="$HEALTH" \
OPENCHIME_BOOTSTRAP_USERS="alice:pw:owner,bob:pw:member" \
"$ROOT/openchimed" >"$WORK/daemon.log" 2>&1 &
D_PID=$!
for _ in $(seq 1 40); do curl -sf "http://localhost:$HEALTH/healthz" >/dev/null 2>&1 && break; sleep 0.3; done

say "Launch the TUI (auto-login as alice) in a tmux pane"
tmux kill-session -t "$SESSION" 2>/dev/null || true
HOME="$WORK/home" tmux new-session -d -s "$SESSION" -x 140 -y 40 \
  "cd '$ROOT' && HOME='$WORK/home' ./build/openchime-tui 127.0.0.1 $PROTO alice:pw"
sleep 4
tmux capture-pane -p -t "$SESSION" | grep -q "connected" || { echo "TUI did not connect"; tmux capture-pane -p -t "$SESSION"; exit 1; }

say "alice sends from the composer; bob sends via demo_client"
tmux send-keys -t "$SESSION" "hello from alice in the tui" Enter
sleep 2
"$ROOT/build/demo_client" 127.0.0.1 "$PROTO" bob pw send 1 "hi alice, this is bob" >/dev/null
sleep 2

say "Transcript"
PANE="$(tmux capture-pane -p -t "$SESSION")"
echo "$PANE" | grep -iE "hello from alice|hi alice, this is bob" | sed 's/[[:space:]]*$//'
echo "$PANE" | grep -q "hello from alice in the tui" || { echo "FAIL: sent message not rendered"; exit 1; }
echo "$PANE" | grep -q "hi alice, this is bob"       || { echo "FAIL: received broadcast not rendered"; exit 1; }
echo
echo "OK: the TUI connected, authenticated, sent, and received a broadcast against a live daemon."
