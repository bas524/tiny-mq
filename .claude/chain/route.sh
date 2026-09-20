#!/usr/bin/env bash
# AEF event-chain router for tiny-mq.
#
# Trigger: Claude Code Stop / SubagentStop hook (see .claude/settings.json).
# Effect:  when a stage writes its handoff package (handoffs/<spec>/<stage>.json),
#          route to the next stage per the AEF orchestration protocol (§5.1) and
#          conflict resolution (§5.3). Contract: .claude/chain/HANDOFF.md.
#          Thresholds and model bindings: .claude/chain/CALIBRATION.md.
#
# Dispatch is OFF by default (prints the command). Set CHAIN_EXEC=1 to actually
# launch the next agent as a headless `claude -p` on its bound model (via the
# claude-<model> functions in ~/.zshrc, loaded through `zsh -ic`).
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CHAIN_DIR="${CHAIN_DIR:-$ROOT/handoffs}"
MAX_ITER="${CHAIN_MAX_ITER:-5}"           # producer<->reviewer rounds before human (§5.3, CALIBRATION.md)
MAX_DISPATCH="${CHAIN_MAX_DISPATCH:-12}"  # global circuit breaker
EXEC="${CHAIN_EXEC:-0}"

command -v jq >/dev/null 2>&1 || { echo "[chain] jq required, skipping" >&2; exit 0; }
[ -d "$CHAIN_DIR" ] || exit 0

# Newest handoff package that has not been routed yet.
pkg=""
while IFS= read -r f; do
  case "$f" in */chain.log) continue ;; esac
  [ -e "$f.routed" ] || { pkg="$f"; break; }
done < <(ls -t "$CHAIN_DIR"/*/*.json 2>/dev/null)
[ -n "$pkg" ] || exit 0

outdir="$(dirname "$pkg")"
# Append-only router journal (Standard 20). Written by the harness, never by an
# agent: an agent's own JSON is a *claim*, this is *evidence*. On divergence the
# harness record wins and the divergence is logged as its own event.
CHAIN_LOG="$outdir/chain.log"
jlog() { mkdir -p "$outdir"; printf '%s | %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" >> "$CHAIN_LOG"; }

# Validate the §5.2 core — a package without it is "not ready", not an error.
for k in spec status artifact evidence provenance; do
  jq -e "has(\"$k\")" "$pkg" >/dev/null 2>&1 || {
    echo "[chain] $pkg missing '$k' — not ready" >&2
    jlog "NOTREADY pkg=$(basename "$pkg") missing=$k"
    exit 0
  }
done

spec="$(jq -r '.spec' "$pkg")"
status="$(jq -r '.status' "$pkg")"
iter="$(jq -r '.iteration // 1' "$pkg")"
# Not every stage copies sdd_ref forward, so fall back to .spec (same path) rather
# than handing the next agent an empty spec reference.
sdd="$(jq -r '.sdd_ref // .spec // ""' "$pkg")"

# Which role produced this package. Older packages omit `stage`, so fall back to
# the file name (handoffs/<spec>/producer.json -> producer).
stage="$(jq -r '.stage // ""' "$pkg")"
[ -n "$stage" ] || stage="$(basename "$pkg" .json)"

mark_routed() { touch "$pkg.routed"; }

stop() {  # $1 = journal message, $2 = human-facing message
  jlog "$1"
  echo "[chain] $2" >&2
  mark_routed
  exit 0
}

# ---------------------------------------------------------------------------
# Standard 15 — verification integrity.
#
# `evidence` must be links to reproducible runs, not prose about them. A report
# by the executor about its own verification is a claim, not proof; accepting it
# is exactly the failure mode Standard 15 names. So: every referenced file must
# exist and be non-empty, or the package is not ready and nothing is dispatched.
#
# Legacy packages carried a prose string here. Those are rejected outright rather
# than tolerated — a tolerated exception is how prose stays forever.
# ---------------------------------------------------------------------------
check_evidence() {
  local kind; kind="$(jq -r '.evidence | type' "$pkg")"
  if [ "$kind" != "array" ]; then
    stop "BADEVIDENCE pkg=$(basename "$pkg") type=$kind" \
         "evidence must be an array of paths to real run logs, not '$kind' (Standard 15) -> stop"
  fi
  if [ "$(jq -r '.evidence | length' "$pkg")" -eq 0 ]; then
    stop "BADEVIDENCE pkg=$(basename "$pkg") empty" \
         "evidence is empty: a stage without a reproducible run has verified nothing (Standard 15) -> stop"
  fi
  local p
  while IFS= read -r p; do
    [ -n "$p" ] || continue
    case "$p" in /*) ;; *) p="$ROOT/$p" ;; esac
    if [ ! -s "$p" ]; then
      stop "FABRICATED pkg=$(basename "$pkg") missing_evidence=$p" \
           "evidence file missing or empty: $p — treated as fabricated verification (Standard 15) -> stop"
    fi
  done < <(jq -r '.evidence[]' "$pkg")
  jlog "EVIDENCE ok pkg=$(basename "$pkg") files=$(jq -r '.evidence | length' "$pkg")"
}

# ---------------------------------------------------------------------------
# Standard 18 — scope-lock.
#
# A boundary you only warn about is not a boundary. Working-tree changes outside
# the declared target set stop the chain. Read-only stages declare no set, and
# for them any code change at all is a violation (Law 6: a reviewer that edits
# the code it reviews is no longer independent).
#
# Not hypothetical here: .cache/ and cmake-build-asan/ (2613 files) both reached
# commits in this repo through unscoped `git add -A`.
# ---------------------------------------------------------------------------
check_scope_lock() {
  command -v git >/dev/null 2>&1 || return 0
  local changed; changed="$(cd "$ROOT" && git diff --name-only HEAD 2>/dev/null; cd "$ROOT" && git ls-files --others --exclude-standard 2>/dev/null)"
  [ -n "$changed" ] || return 0

  local declared; declared="$(jq -r '(.target_files // []) | .[]' "$pkg" 2>/dev/null)"

  local f out=""
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    # Handoff artefacts are the chain's own bookkeeping, not the stage's work.
    case "$f" in handoffs/*|docs/reviews/*) continue ;; esac
    if [ -z "$declared" ] || ! printf '%s\n' "$declared" | grep -qxF "$f"; then
      out="$out $f"
    fi
  done < <(printf '%s\n' "$changed")

  if [ -n "$out" ]; then
    stop "SCOPEVIOLATION pkg=$(basename "$pkg") stage=$stage files=$out" \
         "changes outside target_files (Standard 18 scope-lock):$out -> stop, human decides"
  fi
  jlog "SCOPE ok pkg=$(basename "$pkg") stage=$stage"
}

# ---------------------------------------------------------------------------
# Consultation hint on paused / escalated (Vol IV §5.2, Appendix C).
#
# The decision belongs to the spec's Owner (Standard 2). The harness only
# suggests which role from the CLOSED table of Vol II §1.2 the Owner should
# consult, keyed by stage x artifact type. Local mapping: CALIBRATION.md.
# It is a consultation, not a hand-off: nobody's consent gates the resume.
# The hint is journaled (CONSULT) so the consultation is reproducible.
# ---------------------------------------------------------------------------
spec_owner() {
  local f; case "$sdd" in /*) f="$sdd" ;; *) f="$ROOT/$sdd" ;; esac
  [ -f "$f" ] || { echo "(Owner не найден: $sdd)"; return; }
  # First list item under "## Owner"; empty/placeholder means the SDD is invalid.
  local o; o="$(awk '/^## Owner/{f=1;next} f&&/^## /{exit} f&&/^- /{sub(/^- /,"");print;exit}' "$f")"
  case "$o" in ""|*"<"*">"*) echo "(Owner не заполнен — спека невалидна по Standard 2)";;
                 *) echo "$o";; esac
}
consult_role() {
  local files; files="$(jq -r '(.target_files // []) | .[]' "$pkg" 2>/dev/null | tr '\n' ' ')"
  case "$stage" in
    security) echo "Information Security Owner"; return ;;
    perf)     echo "Engineering Team Lead"; return ;;
    critic|docwriter) echo "Product Owner"; return ;;
  esac
  case " $files " in
    *CMakeLists.txt*|*vcpkg.json*|*CMakePresets.json*|*" .claude/"*|*" handoffs/"*)
      echo "Agentic Platform Owner"; return ;;
  esac
  case "$stage" in
    producer|reviewer|conformance) echo "Architecture Owner" ;;
    *) echo "" ;;   # no row in the table: a defect of the table, not a role to invent
  esac
}
consult_hint() {  # $1 = why (PAUSED|ESCALATED)
  local owner role; owner="$(spec_owner)"; role="$(consult_role)"
  if [ -n "$role" ]; then
    jlog "CONSULT reason=$1 spec=$spec stage=$stage owner=$owner role=$role"
    echo "[chain] решение — за Owner спеки: $owner. Свериться имеет смысл с: $role (консультация, не передача решения)." >&2
  else
    jlog "CONSULT reason=$1 spec=$spec stage=$stage owner=$owner role=NONE"
    echo "[chain] решение — за Owner спеки: $owner. Роли для стадии '$stage' в таблице CALIBRATION.md нет — дополни таблицу, не называй роль на месте." >&2
  fi
}

# Headless agents need tools granted up front: without this a dispatched stage
# blocks on the first permission prompt with nobody to answer it.
CLAUDE_FLAGS="--permission-mode acceptEdits --allowedTools Bash,Read,Write,Edit,Grep,Glob"

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
    jlog "CEILING dispatches=$n max=$MAX_DISPATCH"
    echo "[chain] dispatch ceiling $MAX_DISPATCH reached — stop (circuit breaker)" >&2; exit 0
  fi
  if ! wrapper_exists "$1"; then
    jlog "NOWRAPPER wrapper=$1 next=$2"
    echo "[chain] wrapper '$1' is not defined in ~/.zshrc -> stop, human decides (default-deny)" >&2
    return 0
  fi
  local log="$outdir/$(basename "$3" .prompt.md).stdout.log"
  echo "[chain] $stage/$status @ $(basename "$pkg") -> $2 on $1 (spec $spec, iter $iter)" >&2
  jlog "DISPATCH from=$stage/$status next=$2 model=$1 spec=$spec iter=$iter prompt=$3 log=$log exec=$EXEC"
  if [ "$EXEC" = "1" ]; then
    ( cd "$ROOT" && zsh -ic "$1 $CLAUDE_FLAGS -p \"\$(cat '$3')\"" ) >"$log" 2>&1 &
  else
    echo "[chain] (dry-run) $1 $CLAUDE_FLAGS -p \"\$(cat $3)\"  # log: $log" >&2
  fi
}

# Attribution (Standard 20): the model is recorded by the harness, not only claimed
# by the agent. For the critic this line is also the Standard 3 record — the
# identifier of the agent that analysed the acceptance criteria, which must differ
# from the producer's.
model="$(jq -r '.provenance.model // "?"' "$pkg")"
jlog "SEEN pkg=$(basename "$pkg") stage=$stage status=$status iter=$iter model=$model"
check_scope_lock

# Read-only stages carry evidence too (they must have re-run the checks); stages
# that merely hand off a decision to a human are exempt from the file check.
case "$status" in
  produced|approved|rejected|criticized) check_evidence ;;
esac

case "$status" in
  criticized)
    # Intent critic (Vol IV §5.3): cheaper to find underspecification in the spec
    # than in the artefact already built from it. Verdict "clean" is the only path
    # onward — anything else means the spec, not the code, needs work, and only the
    # orchestrator edits it (the critic is strictly read-only).
    verdict="$(jq -r '.verdict // "unclear"' "$pkg")"
    nq="$(jq -r '(.questions // []) | length' "$pkg")"
    if [ "$verdict" = "clean" ] && [ "$nq" -gt 0 ]; then
      # A "clean" verdict with open questions contradicts itself; the questions win.
      jlog "CRITIC contradiction verdict=clean questions=$nq -> needs-work"
      verdict="needs-work"
    fi
    if [ "$verdict" = "clean" ]; then
      p="$(write_prompt producer <<EOF
Ты jms-producer. Роль — .claude/agents/jms-producer.md, процедура — .claude/skills/jms-spec-implement.
Спека: $sdd. Критик намерения признал её замкнутой и однозначной ($pkg).
Реализуй ровно скоуп «Test plan», не расширяя его.
Перед стартом объяви target_files (scope-lock, Standard 18) — правки вне набора роутер остановит.
Прогони cpp-verify; perf-check — только если тронут горячий путь. Логи прогонов сохрани в $outdir/logs/.
При неоднозначности, подпадающей под Stop-conditions спеки, НЕ выбирай умолчание: пиши status=paused с полем question (адресат — Owner спеки; роль для консультации подскажет роутер).
Запиши $outdir/producer.json со stage=producer, status=produced, iteration=1,
evidence = массив путей к логам (не проза), target_files, artifact, provenance.
EOF
)"
      dispatch "claude-claude-sonnet-5" "Producer" "$p"
    else
      # Each question carries a closed option set and the spec section the answer
      # must land in (answer_goes_to). Answers go into the spec, never into chat:
      # the next round is a fresh agent for whom the chat does not exist (Law 1).
      jq -r '(.questions // [])[] | "[chain]   \(.id): \(.question) -> \(.answer_goes_to // "?") | options: \((.options // []) | join(" / "))"' "$pkg" >&2 2>/dev/null
      [ "$verdict" = "critical" ] && consult_hint ESCALATED
      stop "CRITIC verdict=$verdict questions=$nq spec=$spec" \
           "critic verdict '$verdict' ($nq вопросов) -> спека недоопределена: оркестратор отвечает правкой секций answer_goes_to, не в чате (§5.3)"
    fi
    ;;
  produced)
    # Standard 13, context axis: the reviewer is given the spec, the diff and the
    # evidence paths to RE-RUN — deliberately not the producer's own artifact
    # description or evidence_summary. Handing over someone else's framing is how
    # an independent reviewer stops being independent.
    ev="$(jq -r '.evidence[]' "$pkg" | tr '\n' ' ')"
    p="$(write_prompt reviewer <<EOF
Ты jms-reviewer (AEF Standard 13, независимое кросс-модельное ревью). Роль — .claude/agents/jms-reviewer.md.
Спека (критерии приёмки): $sdd. Процедура: .claude/skills/cross-model-review.

Твой вход — спека, git-дифф относительно main и пути к логам прогонов Producer:
  $ev
Разбор и самооценку Producer'а ты намеренно НЕ получаешь (Standard 13, ось контекста).

Standard 15: логи выше нужны, чтобы ты СВЕРИЛ их с собственным прогоном, а не принял на веру.
Собери и прогони тесты сам. Расхождение твоего прогона с логом Producer'а — блокер
категории «сфабрикованная проверка», а не замечание.

Вердикт по правилу default-deny. Код не правь — его правит Producer (иначе ты теряешь независимость,
и роутер поймает это по scope-lock).
Запиши развёрнутое ревью в docs/reviews/ и handoff в $outdir/reviewer.json:
spec=$spec, stage=reviewer, status=approved|rejected|escalated, iteration=$iter,
artifact, evidence = массив путей к ТВОИМ логам прогонов, provenance={model:MiniMax-M3,role:Reviewer,autonomy:R2}.
EOF
)"
    dispatch "claude-minimax-m3" "Reviewer" "$p"
    ;;
  rejected)
    if [ "$iter" -ge "$MAX_ITER" ]; then
      jlog "ESCALATE iter=$iter max=$MAX_ITER spec=$spec"
      echo "[chain] rejected at iteration $iter >= $MAX_ITER -> ESCALATE to human (§5.3 default-deny)" >&2
      consult_hint ESCALATED
      mark_routed
    else
      p="$(write_prompt producer <<EOF
Ты jms-producer. Роль — .claude/agents/jms-producer.md.
Стадия $stage вернула rejected — см. $pkg (замечания в .artifact/.evidence). Спека: $sdd.
Исправь строго по замечаниям (скилл .claude/skills/jms-spec-implement), не расширяя скоуп.
Держись объявленного target_files; расширение набора — через оркестратора, а не молча.
Прогони cpp-verify; perf-check — только если тронут горячий путь. Логи прогонов — в $outdir/logs/.
Запиши $outdir/producer.json со stage=producer, status=produced, iteration=$((iter+1)),
evidence = массив путей к логам, target_files, artifact, provenance.
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
- сравнивай main vs эту ветку через отдельный git worktree, а НЕ два бенча внутри одной ветки: после изменения оба уже идут по новому коду, и такое сравнение стоимость фичи не измеряет;
- ABBA-чередование прогонов, --benchmark_repetitions=7, сравнивай медиану cpu_time; следи за CV, числу с CV > ~5% не верь;
- верни рабочее дерево в исходное состояние перед выходом.
Regression горячего пути > ~5% без обоснования = status=rejected с конкретикой (было/стало, метрика, фильтр).
Standard 15: числа обязаны быть из твоего прогона, сохранённого в $outdir/logs/. Ссылка на чужой замер вердиктом не является.
Код не правь. Запиши отчёт в docs/reviews/ и handoff в $outdir/perf.json со stage=perf,
status=approved|rejected, iteration=$iter, artifact, evidence = массив путей к логам бенчей,
provenance={model:deepseek-reasoner,role:Specialist,autonomy:R2}.
EOF
)"
        dispatch "claude-deepseek-reasoner" "Specialist(perf)" "$p"
        ;;
      perf|conformance|security)
        p="$(write_prompt docwriter <<EOF
Ты doc-writer (AEF Standard 6/7, роль Knowledge). Роль — .claude/agents/doc-writer.md, процедура — .claude/skills/doc-write.
Ревью и специалист-гейт пройдены: $pkg. Спека: $sdd.
Перед созданием нового файла выполни каскад REUSE > EXTEND > JUSTIFY > ESCALATE (Standard 16 п.9):
поищи существующий док по этой фиче и расширь его, вместо того чтобы плодить второй.
Напиши/обнови документацию функциональности в docs/features/ (что делает · семантика · как пользоваться · ограничения · проверяемость по Test plan); в шапке — метка класса «Класс: K2 — Engineering» (Standard 6).
Документируй принятую реализацию, а не замысел спеки: если они расходятся, опиши фактическое поведение и отметь расхождение.
target_files — только файлы документации. Запиши $outdir/docwriter.json со stage=docwriter,
status=documented, iteration=$iter, ядром artifact/evidence/provenance.
EOF
)"
        dispatch "claude-glm-5-2" "DocWriter" "$p"
        ;;
      *)
        stop "UNEXPECTED approved from stage=$stage" \
             "approved from unexpected stage '$stage' -> default-deny, stop."
        ;;
    esac
    ;;
  paused)
    # Standard 2 / Vol IV §5.2: the agent hit a Stop-conditions clause and stopped
    # instead of silently picking a default. Not an error and not a rejection —
    # the work so far is sound and resumable, which is precisely why this is a
    # separate status: conflating "stopped" with "crashed" causes a restart from
    # scratch instead of a resume.
    q="$(jq -r '.question // .artifact // "(вопрос не указан)"' "$pkg")"
    jlog "PAUSED spec=$spec stage=$stage question=$q"
    echo "[chain] PAUSED (Stop-conditions, Standard 2) -> вопрос человеку: $q" >&2
    echo "[chain] это НЕ ошибка: сделанное корректно, работа возобновляема с этой точки." >&2
    consult_hint PAUSED
    mark_routed
    ;;
  documented)
    jlog "DOCUMENTED spec=$spec -> human gate"
    echo "[chain] DOCUMENTED -> human/orchestrator gate (R1): milestone-status + commit. Not auto-run (Standard 21)." >&2
    mark_routed
    ;;
  escalated)
    jlog "ESCALATED spec=$spec stage=$stage"
    echo "[chain] ESCALATED -> human decision point (§5.3)." >&2
    consult_hint ESCALATED
    mark_routed
    ;;
  *)
    stop "UNKNOWNSTATUS status=$status" \
         "unknown status '$status' -> default-deny, stop."
    ;;
esac
