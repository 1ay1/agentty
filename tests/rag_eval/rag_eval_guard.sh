#!/bin/sh
# search_code / search_docs must not get worse. Runs the judged evals in
# tests/rag_eval keyword-only (no embedder, a scratch AGENTTY_HOME) and fails
# if hit@8 or MRR drops below the floors. Floors sit a little under today's
# numbers; raise them when a change makes search better.
#   usage: rag_eval_guard.sh <agentty> <repo root>
set -u
A="$1"; ROOT="$2"
S=$(mktemp -d "${TMPDIR:-/tmp}/agentty_rag_eval_XXXXXX")
trap 'rm -rf "$S"' EXIT
export AGENTTY_HOME="$S/home" AGENTTY_EMBED_BACKEND=disabled AGENTTY_DOCS_DIR="$ROOT/docs"
mkdir -p "$AGENTTY_HOME"
cd "$ROOT" || exit 1

fails=0
# set  hit@8-floor  MRR-floor
while read -r set h8min mrrmin; do
    out=$("$A" rag-eval "tests/rag_eval/$set.json" 2>&1)
    h8=$(printf '%s\n' "$out" | awk '/hit@8/{print $2}')
    mrr=$(printf '%s\n' "$out" | awk '/MRR/{print $2}')
    if [ -z "$h8" ] || [ -z "$mrr" ]; then
        echo "FAIL $set: no result"; printf '%s\n' "$out" | tail -5; fails=$((fails+1)); continue
    fi
    if awk -v a="$h8" -v b="$h8min" -v c="$mrr" -v d="$mrrmin" 'BEGIN{exit !(a>=b && c>=d)}'; then
        echo "ok   $set  hit@8 $h8 (>= $h8min)  MRR $mrr (>= $mrrmin)"
    else
        echo "FAIL $set  hit@8 $h8 (want >= $h8min)  MRR $mrr (want >= $mrrmin)"
        printf '%s\n' "$out" | grep -A1 MISS
        fails=$((fails+1))
    fi
done <<'EOF'
agentty_code          0.875 0.60
agentty_code_heldout  0.80  0.55
agentty_docs          0.85  0.63
agentty_docs_heldout  0.95  0.68
EOF
exit $fails
