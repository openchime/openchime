#!/usr/bin/env bash
# The configuration registry (daemon/config_table.c) is the one source of every
# OPENCHIME_* name: this holds the rest of the tree to it. Run by `make test`
# with the other source checks. `--write` regenerates what is generated.
#
#   a. getenv() is called in daemon/ only by config.c, and in client/ only by
#      client/core/env.c and the platform path lookups (HOME, APPDATA, ...).
#   b. Every OPENCHIME_* name in code, tests, scripts, workflows, packaging and
#      docs is a row (or a pattern row) of the registry or a name it lists as
#      no longer read; and every row's name appears in CONFIG.md.
#   c. Every OC_TEST_* name the scripts use is read by scripts/gui/lib.sh or a
#      driver (scripts/gui/drive.sh, scripts/web/drive.sh).
#   d. CONFIG.md's generated block and packaging/openchimed.env are what the
#      daemon prints (--config-reference, --config-env), byte for byte.
set -uo pipefail
cd "$(dirname "$0")/../.."
BIN=./openchimed
[ -x "$BIN" ] || { echo "config: ./openchimed is not built (make)" >&2; exit 2; }
fail=0
say() { echo "config: $*" >&2; }

# a. getenv
bad=$(grep -rn 'getenv("' daemon --include=*.c --include=*.h | grep -v '^daemon/config.c:' || true)
[ -z "$bad" ] && bad=$(grep -rn 'getenv("' client shared --include=*.c --include=*.h | grep -v '^client/core/env.c:' \
    | grep -vE 'getenv\("(HOME|APPDATA|LOCALAPPDATA|XDG_CONFIG_HOME|TEMP|TMP|TMPDIR|PATH)"\)' || true)
if [ -n "$bad" ]; then say "getenv outside the loader:"; echo "$bad" | sed 's/^/  /' >&2; fail=1; fi

# b. names
rows=$("$BIN" --config-reference | grep -oE '`OPENCHIME_[A-Za-z0-9_<>]+`' | tr -d '`' | sed 's/<n>/N/' | sort -u)
pattern_prefixes=$(echo "$rows" | grep '_N$' | sed 's/N$//')
# tests/test_config.c names wrong settings on purpose (the unknown-name check).
used=$(git ls-files -- daemon client shared tests scripts packaging docs README.md Dockerfile entrypoint.sh .github mk Makefile \
       | grep -vE '\.(png|ico|bin|ort|npz|wav|bmp)$|^tests/test_config\.c$' | xargs grep -ohE 'OPENCHIME_[A-Z0-9_]+' 2>/dev/null \
       | grep -vE '_H$|^OPENCHIME_TEST_$|^OPENCHIME_S3_$|^OPENCHIME_$|_$' | sort -u)
missing=""
for n in $used; do
    if echo "$rows" | grep -qx "$n"; then continue; fi
    ok=0
    for p in $pattern_prefixes; do case "$n" in "$p"[0-9]*) ok=1;; esac; done
    [ $ok = 1 ] || missing="$missing $n"
done
# The build's own, not settings.
missing=$(echo "$missing" | tr ' ' '\n' | grep -vE '^$|^OPENCHIME_DATA_DIR$' | tr '\n' ' ')
if [ -n "${missing// /}" ]; then say "named somewhere but not a setting in the registry:$missing"; fail=1; fi
for n in $(echo "$rows" | grep -v '_N$'); do grep -q "\`$n\`" docs/CONFIG.md || { say "$n is a row but not in CONFIG.md"; fail=1; }; done

# c. the harness's names
hv=$(grep -ohE 'OC_TEST_[A-Z_]+' scripts/gui/*.sh scripts/web/*.sh scripts/web/*.mjs scripts/demo/*.sh scripts/*.sh 2>/dev/null | sort -u)
for n in $hv; do
    grep -qE "\\$\{$n(:|\}|-)|process\.env\.$n|getenv\(\"$n\"" scripts/gui/*.sh scripts/web/* scripts/gui/lib.sh tests/main.c tests/test_ttskit.c 2>/dev/null \
        || { say "$n is set by a script but read by nothing"; fail=1; }
done

# d. the generated files
gen_ref=$(mktemp); "$BIN" --config-reference > "$gen_ref"
cur_ref=$(mktemp); awk '/<!-- config-reference -->/{f=1;next} /<!-- \/config-reference -->/{f=0} f' docs/CONFIG.md > "$cur_ref"
gen_env=$(mktemp); "$BIN" --config-env > "$gen_env"
if [ "${1:-}" = --write ]; then
    python3 - "$gen_ref" <<'PY'
import sys,re
ref=open(sys.argv[1]).read().rstrip('\n')
p='docs/CONFIG.md'; s=open(p).read()
s=re.sub(r'<!-- config-reference -->\n.*?<!-- /config-reference -->', lambda m: '<!-- config-reference -->\n'+ref+'\n<!-- /config-reference -->', s, flags=re.S)
open(p,'w').write(s)
PY
    cp "$gen_env" packaging/openchimed.env
    echo "config: CONFIG.md's reference and packaging/openchimed.env regenerated"
else
    if ! diff -q "$cur_ref" "$gen_ref" >/dev/null; then say "docs/CONFIG.md's reference block is not what the daemon prints (scripts/check/config.sh --write)"; diff "$cur_ref" "$gen_ref" | head -10 >&2; fail=1; fi
    if ! diff -q packaging/openchimed.env "$gen_env" >/dev/null; then say "packaging/openchimed.env is not what the daemon prints (scripts/check/config.sh --write)"; fail=1; fi
fi
rm -f "$gen_ref" "$cur_ref" "$gen_env"
[ $fail = 0 ] && echo "config: one registry; $(echo "$rows" | wc -l) settings, the page and the env file match"
exit $fail
