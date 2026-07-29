#!/usr/bin/env bash
# AEF event-chain router for tiny-mq.
#
# Trigger: Claude Code Stop / SubagentStop hook (see .claude/settings.json).
# Effect:  when a stage writes its handoff package (handoffs/<spec>/<stage>.json),
#          route to the next stage per the AEF orchestration protocol (§5.1) and
#          conflict resolution (§5.3). Contract: .claude/chain/HANDOFF.md.
#
# Dispatch is OFF by default (prints the command). Set CHAIN_EXEC=1 to actually
# launch the next agent as a headless `claude -p` on its bound model (via the
# claude-<model> functions in ~/.zshrc, loaded through `zsh -ic`).
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CHAIN_DIR="${CHAIN_DIR:-$ROOT/handoffs}"
MAX_ITER="${CHAIN_MAX_ITER:-2}"           # producer<->reviewer rounds before human (§5.3, Appendix C)
MAX_DISPATCH="${CHAIN_MAX_DISPATCH:-12}"  # global circuit breaker
EXEC="${CHAIN_EXEC:-0}"

command -v jq >/dev/null 2>&1 || { echo "[chain] jq required, skipping" >&2; exit 0; }
[ -d "$CHAIN_DIR" ] || exit 0

# Newest handoff package that has not been routed yet.
pkg=""
while IFS= read -r f; do
  [ -e "$f.routed" ] || { pkg="$f"; break; }
done < <(ls -t "$CHAIN_DIR"/*/*.json 2>/dev/null)
[ -n "$pkg" ] || exit 0

# Validate the §5.2 core — a package without it is "not ready", not an error.
for k in spec status artifact evidence provenance; do
  jq -e "has(\"$k\")" "$pkg" >/dev/null 2>&1 || { echo "[chain] $pkg missing '$k' — not ready" >&2; exit 0; }
done

spec="$(jq -r '.spec' "$pkg")"
status="$(jq -r '.status' "$pkg")"
iter="$(jq -r '.iteration // 1' "$pkg")"
sdd="$(jq -r '.sdd_ref // ""' "$pkg")"
outdir="$CHAIN_DIR/$spec"

mark_routed() { touch "$pkg.routed"; }

dispatch() {  # $1 = claude-<model> function, $2 = next stage label, $3 = prompt
  mark_routed
  local n; n=$(( $(cat "$CHAIN_DIR/.dispatches" 2>/dev/null || echo 0) + 1 ))
  echo "$n" > "$CHAIN_DIR/.dispatches"
  if [ "$n" -gt "$MAX_DISPATCH" ]; then
    echo "[chain] dispatch ceiling $MAX_DISPATCH reached — stop (circuit breaker)" >&2; exit 0
  fi
  echo "[chain] $status @ $(basename "$pkg") -> $2 on $1 (spec $spec, iter $iter)" >&2
  if [ "$EXEC" = "1" ]; then
    ( cd "$ROOT" && zsh -ic "$1 -p \"$3\"" ) >/dev/null 2>&1 &
  else
    echo "[chain] (dry-run) $1 -p \"$3\"" >&2
  fi
}

case "$status" in
  produced)
    dispatch "claude-minimax-m3" "Reviewer" \
"Ты jms-reviewer (AEF Standard 12, независимое кросс-модельное ревью). Прочитай handoff-пакет $pkg и спеку $sdd. Выполни процедуру .claude/skills/cross-model-review против текущего git-диффа. Запиши вердикт в $outdir/reviewer.json с полями spec=$spec, stage=reviewer, status=approved|rejected|escalated, iteration=$iter, artifact, evidence, provenance={model:MiniMax-M3,role:Reviewer,autonomy:R2}."
    ;;
  rejected)
    if [ "$iter" -ge "$MAX_ITER" ]; then
      echo "[chain] rejected at iteration $iter >= $MAX_ITER -> ESCALATE to human (§5.3 default-deny)" >&2
      mark_routed
    else
      dispatch "claude-claude-sonnet-4-6" "Producer(revise)" \
"Ты jms-producer. Ревьюер вернул rejected — см. $pkg. Исправь по замечаниям (скилл .claude/skills/jms-spec-implement), прогони cpp-verify и perf-check. Запиши $outdir/producer.json со status=produced и iteration=$((iter+1)), приложив artifact и evidence."
    fi
    ;;
  approved)
    dispatch "claude-glm-5-2" "DocWriter" \
"Ты doc-writer (AEF Standard 5/6, роль Knowledge). Ревью пройдено (approved): $pkg. По спеке $sdd и принятой реализации напиши/обнови документацию функциональности в docs/features/$spec-*.md (что делает · семантика · как пользоваться · ограничения · проверяемость по Test plan). Следуй .claude/skills/doc-write. Запиши $outdir/docwriter.json со status=documented, iteration=$iter, ядром artifact/evidence/provenance."
    ;;
  documented)
    echo "[chain] DOCUMENTED -> human/orchestrator gate (R1): milestone-status + commit. Not auto-run (Standard 19)." >&2
    mark_routed
    ;;
  escalated)
    echo "[chain] ESCALATED -> human decision point (§5.3)." >&2
    mark_routed
    ;;
  *)
    echo "[chain] unknown status '$status' -> default-deny, stop." >&2
    mark_routed
    ;;
esac
