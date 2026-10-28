#!/usr/bin/env bash
# Hold the tests to the rule that makes them worth running (docs/TESTING.md
# "Brittle tests"): a test's result changes only when behaviour a person,
# an operator or an integrator could see changes, and it cannot stay green
# while that behaviour regresses. What a C test cannot see of itself -- its
# own text -- is checked here, before the suite. Run by `make test` and CI's
# build job with the other source checks.
#
# In tests/ (*.c, *.h), but tests/manual/, which a person runs by hand:
#   sleep    usleep, nanosleep, msleep, sleep, SDL_Delay called anywhere but
#            tests/check.h, whose WAIT_UNTIL / oc_test_let_time_pass are the
#            one way a test waits. A fake that models a slow peer on purpose
#            says so on the line or the line before: /* slow-on-purpose: <why> */
#   prose    a comparison (strstr, strcmp, strncmp, strcasecmp, strncasecmp,
#            strcasestr, memcmp, memmem) on a line holding a string literal
#            with a space and lowercase words: a person's words, which may be
#            reworded without anything breaking. A machine contract (a header,
#            a wire format, a documented log line) says why on the line or the
#            line before: /* contract: <why> */
#   tmp      a "/tmp/ literal: a path every run, every suite and every checkout
#            shares. tests/testenv.h's oc_test_mkdtemp gives each its own.
# In .github/workflows/*.yml:
#   pipe     `| grep -q` in a step that runs under pipefail: grep -q exits at
#            its first match, the writer dies on the closed pipe, and pipefail
#            turns a match into a failure. Capture first, then grep the
#            capture (`grep -q ... <<<"$out"`) or a file.
#
# Prints file:line: rule: text for each, and exits 1 if there is any.
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 2

list() {   # NUL-separated paths under $1 matching the -name patterns after it
    local dir=$1; shift
    if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        local pats=()
        for p in "$@"; do pats+=("$dir/$p"); done
        git ls-files -z --cached --others --exclude-standard -- "${pats[@]}"
    else
        local expr=() first=1
        for p in "$@"; do
            [ $first = 1 ] || expr+=(-o)
            expr+=(-name "$p"); first=0
        done
        find "$dir" -type f \( "${expr[@]}" \) -print0
    fi
}

found=0
report() { found=1; printf '%s\n' "$1" >&2; }

# --- the C tests ----------------------------------------------------------------
# One awk pass per file: comments are stripped (block comments across lines
# too) so a comment that mentions sleep() or quotes a page is not a finding,
# while string literals are kept, since they are what the prose rule reads.
c_lint=$(cat <<'AWK'
function strip(s,    out, i, c, n, q) {
    out = ""; n = length(s); q = 0
    for (i = 1; i <= n; i++) {
        c = substr(s, i, 1)
        if (inblock) {
            if (c == "*" && substr(s, i + 1, 1) == "/") { inblock = 0; i++ }
            continue
        }
        if (q) {
            out = out c
            if (c == "\\") { out = out substr(s, i + 1, 1); i++ }
            else if (c == "\"") q = 0
            continue
        }
        if (c == "\"") { q = 1; out = out c; continue }
        if (c == "'") {   # a character literal, whole
            if (substr(s, i + 1, 1) == "\\") { out = out substr(s, i, 4); i += 3 }
            else { out = out substr(s, i, 3); i += 2 }
            continue
        }
        if (c == "/" && substr(s, i + 1, 1) == "*") { inblock = 1; i++; continue }
        if (c == "/" && substr(s, i + 1, 1) == "/") break
        out = out c
    }
    return out
}
# A literal a person reads: a space and lowercase words. Not one that is plainly
# wire format -- it carries CRLF, or is an HTTP request or status line, or a
# header field's "Name: " -- which is a contract by its shape.
function prose(lit) {
    if (lit !~ / / || lit !~ /[a-z][a-z]/) return 0
    if (lit ~ /\\r\\n/) return 0
    if (lit ~ /^(GET|POST|PUT|DELETE|HEAD|OPTIONS|PATCH) \// || lit ~ /^HTTP\/1\.[01] /) return 0
    if (lit ~ /^\\r\\n[A-Z][A-Za-z0-9-]*: ?$/ || lit ~ /^[A-Z][A-Za-z0-9-]*: ?$/) return 0
    return 1
}
# How often `lit` occurs in the file's literals, inside bigger ones too.
function seen(lit,    rest, n, at) {
    n = 0; rest = BIG
    while ((at = index(rest, lit)) > 0) { n++; rest = substr(rest, at + length(lit)) }
    return n
}
# The value of an argument that is nothing but string literals ("a" "b"), or "".
function literal(arg,    v, rest) {
    gsub(/^[ \t]+|[ \t]+$/, "", arg)
    if (arg !~ /^("([^"\\]|\\.)*"[ \t]*)+$/) return ""
    v = ""; rest = arg
    while (match(rest, /"([^"\\]|\\.)*"/)) { v = v substr(rest, RSTART + 1, RLENGTH - 2); rest = substr(rest, RSTART + RLENGTH) }
    return v == "" ? "" : v
}
# The comparison calls in code line k (continued onto the next lines while its
# parentheses are open): each argument that is a prose literal, recorded as
# P[k, n]; how often each literal is compared, in CMP[].
function calls(k,    text, j, rest, pos, i, c, depth, q, arg, lit, n, off) {
    text = C[k]
    for (j = 1; j <= 4 && k + j <= N; j++) text = text " " C[k + j]
    n = 0; off = 0
    rest = text
    while (match(rest, /(^|[^A-Za-z0-9_])(strstr|strcasestr|strcmp|strncmp|strcasecmp|strncasecmp|memcmp|memmem)[ \t]*\(/)) {
        if (off + RSTART > length(C[k]) + 1) break     # only calls that start on line k
        pos = RSTART + RLENGTH                          # just past the "("
        depth = 1; q = 0; arg = ""
        for (i = pos; i <= length(rest) && depth > 0; i++) {
            c = substr(rest, i, 1)
            if (q) { arg = arg c; if (c == "\\") { arg = arg substr(rest, i + 1, 1); i++ } else if (c == "\"") q = 0; continue }
            if (c == "\"") { q = 1; arg = arg c; continue }
            if (c == "(") depth++
            if (c == ")") { depth--; if (depth == 0) break }
            if (c == "," && depth == 1) { lit = literal(arg); if (prose(lit)) { P[k, ++n] = lit; CMP[lit]++ } arg = ""; continue }
            arg = arg c
        }
        lit = literal(arg); if (prose(lit)) { P[k, ++n] = lit; CMP[lit]++ }
        rest = substr(rest, pos); off += pos - 1
    }
    NP[k] = n
}
{
    N = FNR; R[N] = $0; C[N] = strip($0)
    rest = C[N]
    while (match(rest, /"([^"\\]|\\.)*"/)) { BIG = BIG "\001" substr(rest, RSTART + 1, RLENGTH - 2); rest = substr(rest, RSTART + RLENGTH) }
}
END {
    for (k = 1; k <= N; k++) calls(k)
    for (k = 1; k <= N; k++) {
        raw = R[k]; code = C[k]; prev = k > 1 ? R[k - 1] : ""
        contract = (raw ~ /\/\* *contract: *[^ *]/) || (prev ~ /\/\* *contract: *[^ *]/)
        slow = (raw ~ /\/\* *slow-on-purpose: *[^ *]/) || (prev ~ /\/\* *slow-on-purpose: *[^ *]/)
        if (!helper && !slow && code ~ /(^|[^A-Za-z0-9_])(usleep|nanosleep|msleep|sleep|SDL_Delay)[ \t]*\(/)
            printf "%s:%d: sleep: wait on an outcome with WAIT_UNTIL (tests/check.h): %s\n", FILENAME, k, raw
        # A literal the test also writes somewhere other than a comparison --
        # alone, or inside a bigger literal it feeds the code (a JSON body, an
        # HTML page) -- is its own data, given to the code and looked for
        # again: an identity, not the code's words.
        for (i = 1; i <= NP[k]; i++)
            if (!contract && seen(P[k, i]) <= CMP[P[k, i]]) {
                printf "%s:%d: prose: compare a marker, a code or structure, or say /* contract: <why> */: %s\n", FILENAME, k, raw
                break
            }
        if (code ~ /"\/tmp\//)
            printf "%s:%d: tmp: a directory of its own (oc_test_mkdtemp, tests/testenv.h): %s\n", FILENAME, k, raw
    }
}
AWK
)

while IFS= read -r -d '' f; do
    case "$f" in tests/manual/*) continue ;; esac   # run by hand against a real provider or desktop, never by make test or CI
    helper=0
    [ "$f" = tests/check.h ] && helper=1
    out=$(awk -v helper="$helper" "$c_lint" "$f")
    if [ -n "$out" ]; then
        while IFS= read -r line; do report "tests_lint: $line"; done <<<"$out"
    fi
done < <(list tests '*.c' '*.h')

# --- the workflows --------------------------------------------------------------
# A step is the lines from one `- name:` (or `- uses:`, `- run:`, ...) to the next.
# It runs under pipefail when it says so (set -o pipefail, set -...o pipefail)
# or declares `shell: bash`, which GitHub runs as `bash -eo pipefail`; a
# workflow's `defaults: run: shell: bash` puts every step there.
yml_lint='
function flush(   i) {
    if (n && (pf || filepf))
        for (i = 1; i <= n; i++)
            if (body[i] !~ /^[ \t]*#/ && body[i] ~ /\|[ \t]*grep[ \t]+-[A-Za-z]*q/)
                printf "%s:%d: pipe: capture, then grep the capture (pipefail fails a grep -q match): %s\n", FILENAME, at[i], body[i]
    n = 0; pf = 0
}
/^[ \t]*defaults:/ { indef = 1 }
indef && /^[ \t]*shell:[ \t]*bash[ \t]*$/ { filepf = 1 }
/^[a-z]/ { indef = 0 }
/^[ \t]*- (name|uses|run|id|if|shell|env|with|working-directory|timeout-minutes|continue-on-error):/ { flush() }
{
    n++; body[n] = $0; at[n] = FNR
    if ($0 ~ /set[ \t]+-[A-Za-z]*o[ \t]+pipefail/ || $0 ~ /set[ \t]+-o[ \t]+pipefail/ || $0 ~ /^[ \t]*shell:[ \t]*bash[ \t]*$/) pf = 1
}
END { flush() }'

while IFS= read -r -d '' f; do
    out=$(awk "$yml_lint" "$f")
    if [ -n "$out" ]; then
        while IFS= read -r line; do report "tests_lint: $line"; done <<<"$out"
    fi
done < <(list .github/workflows '*.yml' '*.yaml')

if [ $found != 0 ]; then
    echo "tests_lint: the tests wait on outcomes, compare markers rather than words, and share no paths (docs/TESTING.md \"Brittle tests\")" >&2
    exit 1
fi
echo "tests_lint: tests and workflows clean"

