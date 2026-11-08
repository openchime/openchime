#!/usr/bin/env bash
# scripts/test/fast/words.sh
#
# OpenChime has two deployment models, self-hosted and managed (ARCH-76): a
# self-hosted box connects to the OpenChime services or not, and turns each one
# on by its own setting. The words for the three models there once were --
# standalone, stand-alone, a federated deployment, the hosted tier -- stay out
# of the tree, so the old model cannot come back by a sentence at a time.
# "Federated services" stays: it names the services, not a model. Vendored code
# and data (third_party/, ttskit/data/, tests/data/) are not ours to word.
#
# First the matcher is shown a planted line, so a pattern that has stopped
# matching anything fails here rather than passing the tree unchecked.
#
# Needs: git.
# Run by: scripts/test/fast/source.sh (make test; make test-all).
. "$(dirname "$0")/../lib/common.sh"

pattern='\bstand-?alone\b|federated (deployment|model|box|workspace|server)s?\b|self-hosted federated|(?<!self-)\bhosted (tier|suffix|workspace|tenant|box|plan)s?\b'
excluded=(':!third_party' ':!ttskit/data' ':!tests/data' ':!scripts/test/fast/words.sh')

planted='A stand-alone box still reads its own settings.'
grep -qiP "$pattern" <<<"$planted" || fail "the pattern no longer matches a planted 'stand-alone'"
grep -qiP "$pattern" <<<'A hosted workspace moved.' || fail "the pattern no longer matches a planted 'hosted workspace'"
! grep -qiP "$pattern" <<<'A self-hosted workspace stays.' || fail "the pattern matches 'self-hosted workspace'"

found=$(git grep -n -I -i -P "$pattern" -- . "${excluded[@]}" || true)
if [ -n "$found" ]; then
    while IFS= read -r line; do error "a retired model word (ARCH-76): $line"; done <<<"$found"
    exit 1
fi
ok "no retired deployment-model words outside vendored code"
