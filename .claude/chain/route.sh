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
# Not every stage copies sdd_ref forward, so fall back to .spec (same path) rather
# than handing the next agent an empty spec reference.
sdd="$(jq -r '.sdd_ref // .spec // ""' "$pkg")"
# Next stage writes next to the package it answers. Deriving this from `.spec`
# instead (a path like docs/jms-spec/45-*.md) pointed outputs at a directory that
# does not exist, since packages live in handoffs/<spec-slug>/.
outdir="$(dirname "$pkg")"

# Which role produced this package. Older packages omit `stage`, so fall back to
# the file name (handoffs/<spec>/producer.json -> producer).
stage="$(jq -r '.stage // ""' "$pkg")"
[ -n "$stage" ] || stage="$(basename "$pkg" .json)"

# Headless agents need tools granted up front: without this a dispatched stage
# blocks on the first permission prompt with nobody to answer it.
CLAUDE_FLAGS="--permission-mode acceptEdits --allowedTools Bash,Read,Write,Edit,Grep,Glob"

mark_routed() { touch "$pkg.routed"; }

# Model wrappers are shell functions in ~/.zshrc (claude-<model>). A name that does
# not exist there used to die inside the backgrounded subshell with output sent to
# /dev/null, so the chain stopped with no trace at all. Check before dispatching.
wrapper_exists() { zsh -ic "typeset -f $1 >/dev/null" 2>/dev/null; }

# Prompts go through a file rather than an inline argument: they contain quotes,
# `$`, and JSON, all of which the old `zsh -ic "$1 -p \"$3\""` mangled or expanded.
write_prompt() {  # $1 = stage name; body on stdin; echoes the file path
  mkdir -p "$outdir"
  cat > "$outdir/$1.prompt.md"
  echo "$outdir/$1.prompt.md"
}

dispatch() {  # $1 = claude-<model> function, $2 = next stage label, $3 = prompt file
  mark_routed
  local n; n=$(( $(cat "$CHAIN_DIR/.dispatches" 2>/dev/null || echo 0) + 1 ))
  echo "$n" > "$CHAIN_DIR/.dispatches"
  if [ "$n" -gt "$MAX_DISPATCH" ]; then
    echo "[chain] dispatch ceiling $MAX_DISPATCH reached — stop (circuit breaker)" >&2; exit 0
  fi
  if ! wrapper_exists "$1"; then
    echo "[chain] wrapper '$1' is not defined in ~/.zshrc -> stop, human decides (default-deny)" >&2
    return 0
  fi
  local log="$outdir/$(basename "$3" .prompt.md).stdout.log"
  echo "[chain] $stage/$status @ $(basename "$pkg") -> $2 on $1 (spec $spec, iter $iter)" >&2
  if [ "$EXEC" = "1" ]; then
    ( cd "$ROOT" && zsh -ic "$1 $CLAUDE_FLAGS -p \"\$(cat '$3')\"" ) >"$log" 2>&1 &
  else
    echo "[chain] (dry-run) $1 $CLAUDE_FLAGS -p \"\$(cat $3)\"  # log: $log" >&2
  fi
}

case "$status" in
  produced)
    p="$(write_prompt reviewer <<EOF
Ты jms-reviewer (AEF Standard 12, независимое кросс-модельное ревью). Роль — .claude/agents/jms-reviewer.md.
Прочитай handoff-пакет $pkg и спеку $sdd (раздел «Test plan» = критерии приёмки).
Выполни процедуру .claude/skills/cross-model-review против текущего git-диффа относительно main.
Producer работал на другой модели — рассуждай самостоятельно и перепроверяй его утверждения фактически (собери и прогони тесты сам), а не на слово.
Вердикт по правилу default-deny. Код не правь — его правит Producer.
Запиши развёрнутое ревью в docs/reviews/ и handoff в $outdir/reviewer.json с полями:
spec=$spec, stage=reviewer, status=approved|rejected|escalated, iteration=$iter, artifact, evidence, provenance={model:MiniMax-M3,role:Reviewer,autonomy:R2}.
EOF
)"
    dispatch "claude-minimax-m3" "Reviewer" "$p"
    ;;
  rejected)
    if [ "$iter" -ge "$MAX_ITER" ]; then
      echo "[chain] rejected at iteration $iter >= $MAX_ITER -> ESCALATE to human (§5.3 default-deny)" >&2
      mark_routed
    else
      p="$(write_prompt producer <<EOF
Ты jms-producer. Роль — .claude/agents/jms-producer.md.
Стадия $stage вернула rejected — см. $pkg (замечания в .evidence). Спека: $sdd.
Исправь строго по замечаниям (скилл .claude/skills/jms-spec-implement), не расширяя скоуп.
Прогони cpp-verify; perf-check — только если тронут горячий путь.
Запиши $outdir/producer.json со stage=producer, status=produced, iteration=$((iter+1)), приложив artifact и evidence.
EOF
)"
      dispatch "claude-claude-sonnet-5" "Producer(revise)" "$p"
    fi
    ;;
  approved)
    case "$stage" in
      reviewer)
        # Specialist gate (§5.1) sits between review and docs: an approved review is
        # not an approved perf profile. Skipping it once already let a hot-path change
        # through on debug-build numbers (spec 45).
        p="$(write_prompt perf <<EOF
Ты perf-specialist (AEF) — блокирующий перф-гейт. Роль — .claude/agents/perf-specialist.md, процедура — .claude/skills/perf-check.
Ревью пройдено: $pkg. Спека: $sdd.
Определи, тронут ли горячий путь (routing, delivery, (de)serialization, storage, ack/transaction, сетевой кодек). Если нет — верни status=approved с пометкой N/A.
Если тронут:
- мерь на release/relwithdebinfo (cmake --preset user-release), НЕ на debug;
- сравнивай main vs эту ветку через git stash + checkout, а НЕ два бенча внутри одной ветки: после изменения оба уже идут по новому коду, и такое сравнение стоимость фичи не измеряет;
- несколько повторов (--benchmark_repetitions=5 --benchmark_report_aggregates_only=true), следи за CV; числу с CV > ~5% не верь;
- верни рабочее дерево в исходное состояние перед выходом (та же ветка, стеш применён).
Регрессия горячего пути > ~5% без обоснования = status=rejected с конкретикой.
Код не правь. Запиши отчёт в docs/reviews/ и handoff в $outdir/perf.json со stage=perf, status=approved|rejected, iteration=$iter, artifact, evidence, provenance={model:deepseek-reasoner,role:Specialist,autonomy:R2}.
EOF
)"
        dispatch "claude-deepseek-reasoner" "Specialist(perf)" "$p"
        ;;
      perf|conformance|security)
        p="$(write_prompt docwriter <<EOF
Ты doc-writer (AEF Standard 5/6, роль Knowledge). Роль — .claude/agents/doc-writer.md, процедура — .claude/skills/doc-write.
Ревью и специалист-гейт пройдены: $pkg. Спека: $sdd.
Напиши/обнови документацию функциональности в docs/features/ (что делает · семантика · как пользоваться · ограничения · проверяемость по Test plan).
Документируй принятую реализацию, а не замысел спеки: если они расходятся, опиши фактическое поведение и отметь расхождение.
Запиши $outdir/docwriter.json со stage=docwriter, status=documented, iteration=$iter, ядром artifact/evidence/provenance.
EOF
)"
        dispatch "claude-glm-5-2" "DocWriter" "$p"
        ;;
      *)
        echo "[chain] approved from unexpected stage '$stage' -> default-deny, stop." >&2
        mark_routed
        ;;
    esac
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
