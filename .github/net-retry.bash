# Loaded into every bash step by BASH_ENV (each workflow's top-level env), and
# so into every script a step runs: GitHub's network is not reliable enough to
# assume a call to it succeeds, or fails quickly.
#
# git's network commands get a connect timeout, keep-alives (a stalled SSH
# connection dies in about a minute instead of hanging) and an HTTPS stall
# limit. git, gh and ssh-keyscan are retried, with backoff, only when what
# failed was the connection: a lost or refused connection, a timeout, a 5xx
# from GitHub. A real refusal (a rejected push, a missing ref, a 4xx) fails at
# once, as before. Output is passed through unchanged.
#
# NET_RETRY_ATTEMPTS (default 4) and NET_RETRY_WAIT (default 5, seconds, then
# doubling) tune it.

__net_transport='Could not read from remote repository|Connection (timed out|reset|refused|closed)|connect to host|kex_exchange_identification|banner exchange|Broken pipe|early EOF|unexpected disconnect|The remote end hung up|RPC failed|Could not resolve host|unable to access|Operation timed out|timed out|TLS handshake|i/o timeout|connection reset by peer|EOF$|HTTP 50[0-9]|HTTP/2 50[0-9]|returned error: 50[0-9]|50[234] (Bad Gateway|Service Unavailable|Gateway Time-out|Gateway Timeout)|Server Error|secondary rate limit'

__net_run() {  # <name> <cmd...>: run, retrying a transport failure
  local name="$1" n=1 max="${NET_RETRY_ATTEMPTS:-4}" wait="${NET_RETRY_WAIT:-5}" rc err
  shift
  err=$(mktemp)
  while :; do
    rc=0
    "$@" 2>"$err" || rc=$?
    cat "$err" >&2
    if [ "$rc" = 0 ] || [ "$n" -ge "$max" ] || ! grep -qiE "$__net_transport" "$err"; then
      rm -f "$err"; return "$rc"
    fi
    echo "::warning::${name}: the connection failed (attempt ${n} of ${max}); retrying in ${wait}s" >&2
    sleep "$wait"; n=$((n + 1)); wait=$((wait * 2))
  done
}

git() {
  local a sub=""
  for a in "$@"; do case "$a" in -*) ;; *) sub="$a"; break ;; esac; done
  case "$sub" in
    push|fetch|ls-remote|clone|pull)
      GIT_SSH_COMMAND="${GIT_SSH_COMMAND:-ssh} -o ConnectTimeout=20 -o ServerAliveInterval=15 -o ServerAliveCountMax=4" \
        __net_run "git ${sub}" command git -c http.lowSpeedLimit=1000 -c http.lowSpeedTime=60 "$@" ;;
    *) command git "$@" ;;
  esac
}

gh() { __net_run "gh ${1:-}" command gh "$@"; }

ssh-keyscan() {
  local n=1 out
  while :; do
    out=$(command ssh-keyscan -T 15 "$@" 2>/dev/null) || true
    if [ -n "$out" ] || [ "$n" -ge "${NET_RETRY_ATTEMPTS:-4}" ]; then printf '%s\n' "$out"; [ -n "$out" ]; return; fi
    echo "::warning::ssh-keyscan: no answer (attempt ${n}); retrying" >&2
    sleep "$((n * ${NET_RETRY_WAIT:-5}))"; n=$((n + 1))
  done
}
