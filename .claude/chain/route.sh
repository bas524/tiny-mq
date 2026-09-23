#!/usr/bin/env bash
# AEF event-chain router for tiny-mq.
#
# Trigger: Claude Code Stop / SubagentStop hook (see .claude/settings.json).
# Effect:  when a stage writes its handoff package (<HANDOFF_DIR>/<spec>/<stage>.json),
#          route to the next stage per the AEF orchestration protocol (Vol IV §5.1)
#          and conflict resolution (§5.3). Contract: HANDOFF.md. Thresholds and
#          rationale: CALIBRATION.md. Executable bindings: chain.env.
#
# Dispatch is OFF by default (prints the command). Set CHAIN_EXEC=1 to actually
# launch the next agent as a headless `claude -p` on its bound model (via the
# wrapper functions named in chain.env, loaded through `zsh -ic`).
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck disable=SC1091
. "$ROOT/.claude/chain/chain.env"
CHAIN_DIR="${CHAIN_DIR:-$ROOT/${HANDOFF_DIR:-handoffs}}"
MAX_ITER="${CHAIN_MAX_ITER:-5}"           # producer<->reviewer rounds before human (§5.3, CALIBRATION.md)
MAX_DISPATCH="${CHAIN_MAX_DISPATCH:-12}"  # per-spec circuit breaker
EXEC="${CHAIN_EXEC:-0}"

command -v jq >/dev/null 2>&1 || { echo "[chain] jq required, skipping" >&2; exit 0; }
[ -d "$CHAIN_DIR" ] || exit 0

# Newest handoff package that has not been routed yet.
pkg=""
while IFS= read -r f; do
  case "$f" in */chain.log) continue ;; esac
  [ -e "$f.routed" ] || [ -e "$f.notready" ] || { pkg="$f"; break; }
done < <(ls -t "$CHAIN_DIR"/*/*.json 2>/dev/null)
[ -n "$pkg" ] || exit 0

outdir="$(dirname "$pkg")"
# Append-only router journal (Standard 20). Written by the harness, never by an
# agent: an agent's own JSON is a *claim*, this is *evidence*. On divergence the
# harness record wins and the divergence is logged as its own event.
CHAIN_LOG="$outdir/chain.log"
jlog() { mkdir -p "$outdir"; printf '%s | %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" >> "$CHAIN_LOG"; }

# Validate the §5.2 core — a package without it is "not ready", not an error.
# It is marked `.notready` so it does not block the scan for newer packages
# (a legacy package without a core used to stop every later hook run).
for k in spec status artifact evidence provenance; do
  jq -e "has(\"$k\")" "$pkg" >/dev/null 2>&1 || {
    echo "[chain] $pkg missing '$k' — not ready" >&2
    jlog "NOT-READY pkg=$(basename "$pkg") missing=$k"
    touch "$pkg.notready"
    exit 0
  }
done

spec="$(jq -r '.spec' "$pkg")"
status="$(jq -r '.status' "$pkg")"
iter="$(jq -r '.iteration // 1' "$pkg")"
# Not every stage copies sdd_ref forward, so fall back to .spec rather than
# handing the next agent an empty spec reference.
sdd="$(jq -r '.sdd_ref // .spec // ""' "$pkg")"

# Which role produced this package. Older packages omit `stage`, so fall back to
# the file name (<spec>/producer.json -> producer).
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
# A prose string here is rejected outright — a tolerated exception is how prose
# stays forever.
# ---------------------------------------------------------------------------
check_evidence() {
  local kind; kind="$(jq -r '.evidence | type' "$pkg")"
  if [ "$kind" != "array" ]; then
    stop "BAD-EVIDENCE pkg=$(basename "$pkg") type=$kind" \
         "evidence must be an array of paths to real run logs, not '$kind' (Standard 15) -> stop"
  fi
  if [ "$(jq -r '.evidence | length' "$pkg")" -eq 0 ]; then
    stop "BAD-EVIDENCE pkg=$(basename "$pkg") empty" \
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
# ---------------------------------------------------------------------------
check_scope_lock() {
  command -v git >/dev/null 2>&1 || return 0
  local changed; changed="$(cd "$ROOT" && git diff --name-only HEAD 2>/dev/null; cd "$ROOT" && git ls-files --others --exclude-standard 2>/dev/null)"
  [ -n "$changed" ] || return 0

  local declared; declared="$(jq -r '(.target_files // []) | .[]' "$pkg" 2>/dev/null)"
  # Layering (Vol IV §5.2): the working tree legitimately carries the producer's
  # UNCOMMITTED work (the producer never commits — that is the R1 human gate).
  # So every later stage is checked against the producer's declared set as its
  # baseline: read-only stages must add nothing to it, modifying stages
  # (docwriter) add their own set ON TOP of it.
  if [ "$stage" != "producer" ] && [ -s "$outdir/producer.json" ]; then
    declared="$(printf '%s\n%s' "$declared" "$(jq -r '(.target_files // []) | .[]' "$outdir/producer.json" 2>/dev/null)")"
  fi

  local f out=""
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    # Chain bookkeeping (packages, journal, review reports) is not the stage's work.
    case "$f" in "${HANDOFF_DIR:-handoffs}"/*|"${REVIEWS_DIR:-docs/reviews}"/*) continue ;; esac
    if [ -z "$declared" ] || ! printf '%s\n' "$declared" | grep -qxF "$f"; then
      out="$out $f"
    fi
  done < <(printf '%s\n' "$changed")

  if [ -n "$out" ]; then
    stop "SCOPE-VIOLATION pkg=$(basename "$pkg") stage=$stage files=$out" \
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
# ---------------------------------------------------------------------------
spec_owner() {
  local f; case "$sdd" in /*) f="$sdd" ;; *) f="$ROOT/$sdd" ;; esac
  [ -f "$f" ] || { echo "(Owner не найден: $sdd)"; return; }
  local o; o="$(awk '/^## Owner/{f=1;next} f&&/^## /{exit} f&&/^- /{sub(/^- /,"");print;exit}' "$f")"
  case "$o" in ""|*"<"*">"*|*"{{"*) echo "(Owner не заполнен — спека невалидна по Standard 2)";;
                 *) echo "$o";; esac
}
consult_role() {
  local files; files="$(jq -r '(.target_files // []) | .[]' "$pkg" 2>/dev/null | tr '\n' ' ')"
  case "$stage" in
    security) echo "Information Security Owner"; return ;;
    perf)     echo "Engineering Team Lead"; return ;;
    critic|docwriter) echo "Product Owner"; return ;;
  esac
  local pat
  for pat in $PLATFORM_FILES; do
    case " $files " in *"$pat"*) echo "Agentic Platform Owner"; return ;; esac
  done
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

# Model wrappers are shell functions (claude-<model>). A name that does not exist
# dies inside the backgrounded subshell with no trace; check before dispatching.
wrapper_exists() { zsh -ic "typeset -f $1 >/dev/null" 2>/dev/null; }

# Prompts go through a file: they contain quotes, `$` and JSON, which an inline
# argument mangles. No `#` and no backticks inside $( … ) heredocs below.
write_prompt() {  # $1 = stage name; body on stdin; echoes the file path
  mkdir -p "$outdir"
  cat > "$outdir/$1.prompt.md"
  echo "$outdir/$1.prompt.md"
}

HEADLESS_RULE="Ты headless-процесс: никаких фоновых команд; сессия завершена только когда записан пакет."

dispatch() {  # $1 = wrapper function, $2 = next stage label, $3 = prompt file
  mark_routed
  # Per-spec counter: the breaker guards one chain against looping; a global
  # counter trips on the accumulated history of every past spec.
  local n; n=$(( $(cat "$outdir/.dispatches" 2>/dev/null || echo 0) + 1 ))
  echo "$n" > "$outdir/.dispatches"
  if [ "$n" -gt "$MAX_DISPATCH" ]; then
    jlog "CEILING dispatches=$n max=$MAX_DISPATCH"
    echo "[chain] dispatch ceiling $MAX_DISPATCH reached — stop (circuit breaker)" >&2; exit 0
  fi
  if [ -z "$1" ] || [ "${1#\{\{}" != "$1" ]; then
    jlog "NO-WRAPPER wrapper='$1' next=$2"
    echo "[chain] wrapper for '$2' is not bound in chain.env -> stop, human decides (default-deny)" >&2
    return 0
  fi
  if ! wrapper_exists "$1"; then
    jlog "NO-WRAPPER wrapper=$1 next=$2"
    echo "[chain] wrapper '$1' is not defined in the shell -> stop, human decides (default-deny)" >&2
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

# Specialist gates run in the order of SPECIALIST_GATES; after the last one the
# chain goes to the doc-writer. `next_gate <current>` prints the gate after
# <current> ("" = reviewer -> first gate), or nothing if <current> was the last.
next_gate() {
  local cur="$1" g found=0
  [ -z "$cur" ] && { set -- $SPECIALIST_GATES; echo "${1:-}"; return; }
  for g in $SPECIALIST_GATES; do
    if [ "$found" = 1 ]; then echo "$g"; return; fi
    [ "$g" = "$cur" ] && found=1
  done
}

dispatch_gate() {  # $1 = gate name (nfr|conformance|security)
  local wrapper model p
  case "$1" in
    perf)        wrapper="$WRAPPER_PERF";        model="$MODEL_PERF" ;;
    conformance) wrapper="$WRAPPER_CONFORMANCE"; model="$MODEL_CONFORMANCE" ;;
    security)    wrapper="$WRAPPER_SECURITY";    model="$MODEL_SECURITY" ;;
    *) stop "UNKNOWN-GATE gate=$1" "unknown specialist gate '$1' in SPECIALIST_GATES -> default-deny, stop." ;;
  esac
  p="$(write_prompt "$1" <<EOF
Ты $1-specialist (AEF, Specialist gate). Роль — .claude/agents/$1-specialist.md, процедура — соответствующий скилл.
Предыдущая стадия пройдена: $pkg. Спека: $sdd.
Если изменение вне твоей области — верни status=approved с пометкой N/A и объяснением.
Standard 15: вердикт строится на ТВОЁМ прогоне, сохранённом в $outdir/logs/. Ссылка на чужой замер или отчёт вердиктом не является.
Код не правь. Запиши отчёт в $REVIEWS_DIR/ и handoff в $outdir/$1.json со stage=$1,
status=approved|rejected, iteration=$iter, artifact, evidence = массив путей к логам,
provenance={model:$model,role:Specialist,autonomy:R2}.
$HEADLESS_RULE
EOF
)"
  dispatch "$wrapper" "Specialist($1)" "$p"
}

dispatch_docwriter() {
  local cid p; cid="$(basename "$sdd" .md)"
  p="$(write_prompt docwriter <<EOF
Ты doc-writer (AEF Standard 6/7, роль Knowledge). Роль — .claude/agents/doc-writer.md, процедура — .claude/skills/doc-write.
Ревью и гейты пройдены: $pkg. Спека: $sdd.
Перед созданием нового файла выполни каскад REUSE > EXTEND > JUSTIFY > ESCALATE (Standard 16 п.9).
Напиши дельту принятой реализации в capability-спеку (правила — $OPENSPEC_DIR/README.md): каталог $OPENSPEC_DIR/changes/$cid/ с .openspec.yaml, proposal.md, design.md и specs/<capability>/spec.md (секции ADDED/MODIFIED/REMOVED Requirements; требование → заголовок «Requirement:» с SHALL; сценарий → заголовок «Scenario:» WHEN/THEN + строка «- Test: <идентификатор>» с реальным тестом из диффа; точный формат — в README).
Внимание: в этом промпте нет символов решётки намеренно — это ограничение оболочки, формат бери из README.
Документируй принятую реализацию, а не замысел: если они расходятся, опиши фактическое поведение и отметь расхождение.
Прогони python3 .claude/chain/openspec.py validate $cid → 0 ошибок; лог в $outdir/logs/openspec-validate.log — это твой evidence. archive НЕ делай (рубеж человека).
target_files — только файлы внутри $OPENSPEC_DIR/changes/$cid/. Запиши $outdir/docwriter.json со stage=docwriter,
status=documented, iteration=$iter, ядром artifact/evidence/provenance + target_files, provenance.model=$MODEL_DOCWRITER.
$HEADLESS_RULE
EOF
)"
  dispatch "$WRAPPER_DOCWRITER" "DocWriter" "$p"
}

dispatch_producer() {  # $1 = iteration, $2 = reason line, $3 = stage label
  local p
  p="$(write_prompt producer <<EOF
Ты jms-producer. Роль — .claude/agents/jms-producer.md, процедура — .claude/skills/jms-spec-implement.
Спека: $sdd. $2
Реализуй ровно скоуп «Test plan», не расширяя его.
Перед стартом объяви target_files (scope-lock, Standard 18) — правки вне набора роутер остановит; расширение набора — через оркестратора, не молча.
Прогони cpp-verify; perf-check — только если тронут горячий путь. Логи прогонов сохрани в $outdir/logs/.
При неоднозначности, подпадающей под Stop-conditions спеки, НЕ выбирай умолчание: пиши status=paused с полем question (адресат — Owner спеки; роль для консультации подскажет роутер).
Запиши $outdir/producer.json со stage=producer, status=produced, iteration=$1,
evidence = массив путей к логам (не проза), target_files, artifact, provenance.model=$MODEL_PRODUCER.
$HEADLESS_RULE
EOF
)"
  dispatch "$WRAPPER_PRODUCER" "$3" "$p"
}

dispatch_explainer() {
  local p cid; cid="$(basename "$sdd" .md)"
  p="$(write_prompt explainer <<EOF
Ты стадия объяснения (AEF Standard 4, Том IV §5.1). Роль — .claude/agents/explainer.md, процедура — .claude/skills/change-brief.
Ревью, гейты и дельта готовы: $pkg. Спека: $sdd.
Твой адресат — ЧЕЛОВЕК, принимающий решение (Owner спеки или назначенный рецензент), а не следующий агент.
Ты НЕ проверяешь корректность и НЕ выносишь вердикт: это уже сделали гейты. Ты объясняешь, ЧТО изменилось в продукте.
Источники строго в порядке: дельта $OPENSPEC_DIR/changes/$cid/, отчёты $REVIEWS_DIR/, ADR, журнал и пакеты $outdir/, и только потом дифф (для ссылок на места в коде).
Заполни семь разделов по templates/change-brief.md; разделы 2, 3 и 7 — по применимости, неприменимый удали с пометкой почему.
Каждое утверждение обязано нести ссылку на источник: требование дельты, файл со строкой, идентификатор теста, отчёт гейта, ADR. Утверждение без источника удали или найди ему источник.
Схемы — текстовым исходником, не изображением. Дифф не пересказывай.
Перед сдачей пройди по собственному тексту и проверь, что каждая ссылка ведёт туда, где написано именно то, что ты утверждаешь.
Запиши объяснение в $REVIEWS_DIR/$cid.brief.md и handoff $outdir/explainer.json со stage=explainer,
status=explained, iteration=$iter, artifact = путь к объяснению, evidence = массив путей к источникам трассировки,
provenance={model:$MODEL_EXPLAINER,role:Producer,autonomy:R2}.
$HEADLESS_RULE
EOF
)"
  dispatch "$WRAPPER_EXPLAINER" "Explanation" "$p"
}

# Attribution (Standard 20): the model is recorded by the harness, not only claimed
# by the agent. For the critic this line is also the Standard 3 record — the
# identifier of the agent that analysed the acceptance criteria.
model="$(jq -r '.provenance.model // "?"' "$pkg")"
jlog "SEEN pkg=$(basename "$pkg") stage=$stage status=$status iter=$iter model=$model"
check_scope_lock

# Stages that merely hand a decision to a human are exempt from the file check.
case "$status" in
  produced|approved|rejected|criticized|documented|explained) check_evidence ;;
esac

# Routing is by the pair stage x status, not by status alone: `approved` from the
# reviewer and `approved` from a specialist lead to different places.
case "$status" in
  criticized)
    verdict="$(jq -r '.verdict // "unclear"' "$pkg")"
    nq="$(jq -r '(.questions // []) | length' "$pkg")"
    if [ "$verdict" = "clean" ] && [ "$nq" -gt 0 ]; then
      # A "clean" verdict with open questions contradicts itself; the questions win.
      jlog "CRITIC contradiction verdict=clean questions=$nq -> needs-work"
      verdict="needs-work"
    fi
    if [ "$verdict" = "clean" ]; then
      dispatch_producer 1 "Критик намерения признал её замкнутой и однозначной ($pkg)." "Producer"
    else
      jq -r '(.questions // [])[] | "[chain]   \(.id): \(.question) -> \(.answer_goes_to // "?") | options: \((.options // []) | join(" / "))"' "$pkg" >&2 2>/dev/null
      [ "$verdict" = "critical" ] && consult_hint ESCALATED
      stop "CRITIC verdict=$verdict questions=$nq spec=$spec" \
           "critic verdict '$verdict' ($nq вопросов) -> спека недоопределена: оркестратор отвечает правкой секций answer_goes_to, не в чате (§5.3)"
    fi
    ;;
  produced)
    # Standard 13, context axis: the reviewer gets the spec, the diff and the
    # evidence paths to RE-RUN — deliberately not the producer's own artifact
    # description or evidence_summary.
    ev="$(jq -r '.evidence[]' "$pkg" | tr '\n' ' ')"
    p="$(write_prompt reviewer <<EOF
Ты jms-reviewer (AEF Standard 13, независимое кросс-модельное ревью). Роль — .claude/agents/jms-reviewer.md.
Спека (критерии приёмки): $sdd. Процедура: .claude/skills/cross-model-review.

Твой вход — спека, git-дифф относительно основной ветки и пути к логам прогонов Producer:
  $ev
Разбор и самооценку Producer'а ты намеренно НЕ получаешь (Standard 13, ось контекста).

Standard 15: логи выше нужны, чтобы ты СВЕРИЛ их с собственным прогоном, а не принял на веру.
Собери и прогони тесты сам. Расхождение твоего прогона с логом Producer'а — блокер
категории «сфабрикованная проверка», а не замечание.

Вердикт по правилу default-deny. Код не правь — роутер поймает это по scope-lock.
Запиши развёрнутое ревью в $REVIEWS_DIR/ и handoff в $outdir/reviewer.json:
spec=$spec, stage=reviewer, status=approved|rejected|escalated, iteration=$iter,
artifact, evidence = массив путей к ТВОИМ логам прогонов, provenance={model:$MODEL_REVIEWER,role:Reviewer,autonomy:R2}.
$HEADLESS_RULE
EOF
)"
    dispatch "$WRAPPER_REVIEWER" "Reviewer" "$p"
    ;;
  rejected)
    if [ "$iter" -ge "$MAX_ITER" ]; then
      jlog "ESCALATE iter=$iter max=$MAX_ITER spec=$spec"
      echo "[chain] rejected at iteration $iter >= $MAX_ITER -> ESCALATE to human (§5.3 default-deny)" >&2
      consult_hint ESCALATED
      mark_routed
    else
      dispatch_producer $((iter+1)) "Стадия $stage вернула rejected — см. $pkg (замечания в .artifact/.evidence). Исправь строго по замечаниям." "Producer(revise)"
    fi
    ;;
  approved)
    case "$stage" in
      reviewer)
        g="$(next_gate "")"
        if [ -n "$g" ]; then dispatch_gate "$g"; else dispatch_docwriter; fi
        ;;
      perf|conformance|security)
        g="$(next_gate "$stage")"
        if [ -n "$g" ]; then dispatch_gate "$g"; else dispatch_docwriter; fi
        ;;
      *)
        stop "UNEXPECTED approved from stage=$stage" \
             "approved from unexpected stage '$stage' -> default-deny, stop."
        ;;
    esac
    ;;
  paused)
    # Standard 2 / Vol IV §5.2: the agent hit a Stop-conditions clause and stopped
    # instead of silently picking a default. Not an error and not a rejection.
    q="$(jq -r '.question // .artifact // "(вопрос не указан)"' "$pkg")"
    jlog "PAUSED spec=$spec stage=$stage question=$q"
    echo "[chain] PAUSED (Stop-conditions, Standard 2) -> вопрос человеку: $q" >&2
    echo "[chain] это НЕ ошибка: сделанное корректно, работа возобновляема с этой точки." >&2
    consult_hint PAUSED
    mark_routed
    ;;
  documented)
    # The delta is written, so the explanation stage now has its input (Vol IV §5.1):
    # it is built from the delta and the gate reports, not from the code again.
    dispatch_explainer
    ;;
  explained)
    # Standard 4: the human decision has its input — green gates AND an explanation of
    # what changed. The chain stops here; what follows is the human's, not the router's.
    jlog "EXPLAINED spec=$spec brief=$(jq -r '.artifact // "?"' "$pkg")"
    echo "[chain] EXPLAINED -> human/orchestrator gate (R1): прочитать объяснение, затем openspec archive + milestone-status + commit." >&2
    echo "[chain] объяснение готовит решение и ничего не подтверждает: проверяй его выборочно по ссылкам (Standard 4, Appendix A N18)." >&2
    mark_routed
    ;;
  escalated)
    jlog "ESCALATED spec=$spec stage=$stage"
    echo "[chain] ESCALATED -> human decision point (§5.3)." >&2
    consult_hint ESCALATED
    mark_routed
    ;;
  *)
    stop "UNKNOWN-STATUS status=$status" \
         "unknown status '$status' -> default-deny, stop."
    ;;
esac
