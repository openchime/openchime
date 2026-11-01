#!/usr/bin/env bash
# heard_matches.sh "<what was said>" "<what the recognizer heard>"
#
# Whether speech recognition heard what read-aloud said, judged on words and
# not on the exact string: both are lowercased and stripped of punctuation,
# and at least two thirds of the said words must appear among the heard ones.
# A recognizer that renders "release" as "release," or drops an article is
# working; one that hears nothing, or something else, is not. Exit 0 or 1, and
# prints the comparison either way.
set -euo pipefail
norm() { tr '[:upper:]' '[:lower:]' <<<"$1" | tr -c 'a-z0-9\n' ' ' | tr -s ' '; }
said=$(norm "${1:?what was said}")
heard=$(norm "${2-}")
read -r -a want <<<"$said"
read -r -a got <<<"$heard"
[ "${#want[@]}" -gt 0 ] || { echo "heard_matches: nothing was said" >&2; exit 2; }
hits=0
for w in "${want[@]}"; do
  for g in "${got[@]}"; do
    if [ "$w" = "$g" ]; then hits=$((hits + 1)); break; fi
  done
done
echo "heard_matches: ${hits} of ${#want[@]} words (said: ${said}; heard: ${heard})"
[ $((hits * 3)) -ge $((${#want[@]} * 2)) ]
