# Loaded into every bash step by BASH_ENV (each workflow's top-level env), and
# so into every bash script a step runs, make's included: no network the
# pipeline uses is reliable enough to assume a call succeeds, or fails quickly.
#
# Every command that reaches the network is wrapped here or tuned by its own
# environment variables below: git, gh, ssh-keyscan (GitHub); skopeo, docker,
# buildah, flyctl (Fly's registry, API and builder); curl (the deployed sites,
# vendors, downloads); rclone (object storage); npm and dotnet restore (package
# feeds). apt is scripts/build/apt.sh's, which retries and times out itself.
#
# Each attempt is bounded, so a stalled connection fails instead of hanging,
# and a failure is retried with backoff only when it was the connection: a
# timeout, a dropped, reset or refused connection, a 5xx. A real refusal (a
# rejected push, a missing ref or image, a 4xx, a failed health check) fails at
# once, as it always did. A command's output is passed on once, from the
# attempt that counted, so a retried call never prints twice.
#
# NET_RETRY_ATTEMPTS (default 4) and NET_RETRY_WAIT (default 5 seconds, then
# doubling) tune it.

# The tools that retry themselves, told to retry harder and give up on a stall.
export RCLONE_RETRIES=5 RCLONE_LOW_LEVEL_RETRIES=20 RCLONE_RETRIES_SLEEP=10s
export RCLONE_CONTIMEOUT=30s RCLONE_TIMEOUT=5m
export NPM_CONFIG_FETCH_RETRIES=5 NPM_CONFIG_FETCH_RETRY_MINTIMEOUT=10000
export NPM_CONFIG_FETCH_RETRY_MAXTIMEOUT=120000 NPM_CONFIG_FETCH_TIMEOUT=300000
export NUGET_ENABLE_ENHANCED_HTTP_RETRY=true NUGET_ENHANCED_MAX_NETWORK_TRY_COUNT=6
export NUGET_ENHANCED_NETWORK_RETRY_DELAY_MILLISECONDS=2000

__net_transport='Could not read from remote repository|Connection (timed out|reset|refused|closed)|connect to host|kex_exchange_identification|banner exchange|Broken pipe|early EOF|unexpected EOF|unexpected disconnect|The remote end hung up|RPC failed|Could not resolve host|could not resolve|unable to access|Operation timed out|timed out|TLS handshake|i/o timeout|connection reset by peer|no such host|network is unreachable|HTTP 50[0-9]|HTTP/[0-9.]+ 50[0-9]|returned error: 50[0-9]|unexpected (HTTP )?status( code)?:? 50[0-9]|50[0-9] (Bad Gateway|Service Unavailable|Gateway Time-?out|Internal Server Error)|Internal Server Error|Server Error|secondary rate limit|NU1301'

# __net_run <name> <timeout seconds> <command...>: run, retrying a transport
# failure. stdin is read once and replayed only for a command that takes a
# secret or a body on it (--password-stdin, --input -); otherwise it is left
# alone, so a loop feeding its own stdin keeps it.
__net_run() {
  local name="$1" secs="$2" n=1 max="${NET_RETRY_ATTEMPTS:-4}" wait="${NET_RETRY_WAIT:-5}"
  local rc out err in="" a prev="" curl=0
  shift 2
  [ "$1" = curl ] && curl=1
  for a in "$@"; do
    if [ "$a" = --password-stdin ] || { [ "$prev" = --input ] && [ "$a" = - ]; }; then
      in=$(mktemp); cat >"$in"; break
    fi
    prev="$a"
  done
  out=$(mktemp); err=$(mktemp)
  while :; do
    rc=0
    if [ -n "$in" ]; then timeout "$secs" "$@" <"$in" >"$out" 2>"$err" || rc=$?
    else timeout "$secs" "$@" >"$out" 2>"$err" || rc=$?; fi
    retry=0
    if [ "$rc" != 0 ] && [ "$n" -lt "$max" ]; then
      if [ "$rc" = 124 ] || grep -qiE "$__net_transport" "$err"; then retry=1; fi
      # curl says what failed by its exit code even when told to be silent:
      # resolve, connect, timeout, TLS, nothing or a broken reply, send, receive.
      if [ "$curl" = 1 ]; then case "$rc" in 5|6|7|28|35|52|55|56) retry=1 ;; esac; fi
    fi
    if [ "$retry" = 0 ]; then
      cat "$out"; cat "$err" >&2
      rm -f "$out" "$err" ${in:+"$in"}
      return "$rc"
    fi
    cat "$err" >&2
    echo "::warning::${name}: the connection failed (attempt ${n} of ${max}, exit ${rc}); retrying in ${wait}s" >&2
    sleep "$wait"; n=$((n + 1)); wait=$((wait * 2))
  done
}

git() {
  local a sub=""
  for a in "$@"; do case "$a" in -*) ;; *) sub="$a"; break ;; esac; done
  case "$sub" in
    push|fetch|ls-remote|clone|pull)
      GIT_SSH_COMMAND="${GIT_SSH_COMMAND:-ssh} -o ConnectTimeout=20 -o ServerAliveInterval=15 -o ServerAliveCountMax=4" \
        __net_run "git ${sub}" 1800 git -c http.lowSpeedLimit=1000 -c http.lowSpeedTime=60 "$@" ;;
    *) command git "$@" ;;
  esac
}

gh() { __net_run "gh ${1:-}" 900 gh "$@"; }

skopeo() { __net_run "skopeo ${1:-}" 1800 skopeo --command-timeout 25m "$@"; }

flyctl() { __net_run "flyctl ${1:-}" 2400 flyctl "$@"; }
fly() { flyctl "$@"; }

curl() { __net_run "curl" 900 curl --connect-timeout 20 --speed-limit 1 --speed-time 60 "$@"; }

docker() {
  case "${1:-}" in
    login|pull|push|manifest|buildx) __net_run "docker ${1}" 1800 docker "$@" ;;
    *) command docker "$@" ;;
  esac
}

buildah() {
  case "${1:-}" in
    login|pull|push|manifest|from|bud|build) __net_run "buildah ${1}" 1800 buildah "$@" ;;
    *) command buildah "$@" ;;
  esac
}

dotnet() {
  case "${1:-}" in
    restore) __net_run "dotnet restore" 1800 dotnet "$@" ;;
    *) command dotnet "$@" ;;
  esac
}

ssh-keyscan() {
  local n=1 out
  while :; do
    out=$(command ssh-keyscan -T 15 "$@" 2>/dev/null) || true
    if [ -n "$out" ] || [ "$n" -ge "${NET_RETRY_ATTEMPTS:-4}" ]; then printf '%s\n' "$out"; [ -n "$out" ]; return; fi
    echo "::warning::ssh-keyscan: no answer (attempt ${n}); retrying" >&2
    sleep "$((n * ${NET_RETRY_WAIT:-5}))"; n=$((n + 1))
  done
}
