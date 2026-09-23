#!/usr/bin/env bash
# Dry-run test of the chain router on synthetic packages (CHAIN_EXEC=0).
# Builds a throwaway git repo with the kit's .claude/, fills chain.env with test
# bindings, feeds packages one at a time and asserts journal events.
set -u
KIT="$(cd "$(dirname "$0")/../../.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T" "$T.stderr"' EXIT
fail=0; pass=0
ok()   { pass=$((pass+1)); echo "  ok   $1"; }
bad()  { fail=$((fail+1)); echo "  FAIL $1"; }
expect_log() {  # $1 = spec, $2 = grep pattern, $3 = label
  if grep -qE "$2" "$T/handoffs/$1/chain.log" 2>/dev/null; then ok "$3"; else bad "$3 (no /$2/ in $1/chain.log)"; fi
}
run_router() { (cd "$T" && CHAIN_EXEC=0 CHAIN_MAX_ITER="${CHAIN_MAX_ITER:-5}" CHAIN_MAX_DISPATCH="${CHAIN_MAX_DISPATCH:-12}" \
  ZDOTDIR="$T/zsh" bash .claude/chain/route.sh 2>>"$T.stderr"); }

# --- fixture ---------------------------------------------------------------
mkdir -p "$T/zsh" "$T/docs/jms-spec" "$T/handoffs"
cp -R "$KIT/.claude" "$T/.claude"
cat > "$T/zsh/.zshrc" <<'EOF'
claude-test-a() { :; }
claude-test-b() { :; }
EOF
cat > "$T/.claude/chain/chain.env" <<'EOF'
WRAPPER_CRITIC="claude-test-a"; WRAPPER_PRODUCER="claude-test-a"; WRAPPER_REVIEWER="claude-test-b"
WRAPPER_PERF="claude-test-b"; WRAPPER_CONFORMANCE="claude-test-b"; WRAPPER_SECURITY="claude-test-b"; WRAPPER_DOCWRITER="claude-test-a"; WRAPPER_EXPLAINER="claude-test-b"
MODEL_CRITIC="m-a"; MODEL_PRODUCER="m-a"; MODEL_REVIEWER="m-b"; MODEL_PERF="m-b"; MODEL_CONFORMANCE="m-b"; MODEL_SECURITY="m-b"; MODEL_DOCWRITER="m-a"; MODEL_EXPLAINER="m-b"
SPECIALIST_GATES="perf"
SPEC_DIR="docs/jms-spec"; HANDOFF_DIR="handoffs"; REVIEWS_DIR="docs/reviews"; OPENSPEC_DIR="openspec"
PLATFORM_FILES="build.cfg .claude/ handoffs/"
EOF
cat > "$T/docs/jms-spec/01-demo.md" <<'EOF'
# Demo

## Owner

- Test Owner

## Semantics
EOF
printf 'handoffs/\n' > "$T/.gitignore"
(cd "$T" && git init -q && git add -A && git -c user.email=t@t -c user.name=t commit -qm init)

pkg() {  # $1 = spec, $2 = file name, $3 = json body
  mkdir -p "$T/handoffs/$1/logs"; printf '%s' "$3" > "$T/handoffs/$1/$2"
}
core='"sdd_ref":"docs/jms-spec/01-demo.md","artifact":"x","provenance":{"model":"m-a","role":"Producer","autonomy":"R2"}'

echo "route.sh dry-run"

# 1. package without core -> NOT-READY
pkg n1 producer.json '{"spec":"n1","status":"produced"}'
run_router; expect_log n1 'NOT-READY .*missing=artifact' "package without core -> NOT-READY"

# 2. evidence path missing -> FABRICATED
pkg n2 producer.json "{\"spec\":\"n2\",\"stage\":\"producer\",\"status\":\"produced\",\"evidence\":[\"handoffs/n2/logs/verify.log\"],\"target_files\":[],$core}"
run_router; expect_log n2 'FABRICATED' "missing evidence -> FABRICATED"

# 3. evidence present -> DISPATCH Reviewer on reviewer wrapper
pkg n3 producer.json "{\"spec\":\"n3\",\"stage\":\"producer\",\"status\":\"produced\",\"evidence\":[\"handoffs/n3/logs/verify.log\"],\"target_files\":[],$core}"
echo "PASSED" > "$T/handoffs/n3/logs/verify.log"
run_router; expect_log n3 'DISPATCH from=producer/produced next=Reviewer model=claude-test-b' "produced -> Reviewer"
[ -f "$T/handoffs/n3/reviewer.prompt.md" ] && ok "reviewer prompt written" || bad "reviewer prompt missing"
grep -q "headless" "$T/handoffs/n3/reviewer.prompt.md" && ok "prompt carries headless rule" || bad "headless rule missing in prompt"
grep -q "evidence_summary" "$T/handoffs/n3/reviewer.prompt.md" && bad "reviewer prompt must not leak evidence_summary" || ok "reviewer prompt withholds producer summary (Std 13)"

# 4. reviewer approved -> first gate (perf); perf approved -> DocWriter
pkg n4 reviewer.json "{\"spec\":\"n4\",\"stage\":\"reviewer\",\"status\":\"approved\",\"evidence\":[\"handoffs/n4/logs/r.log\"],$core}"
echo x > "$T/handoffs/n4/logs/r.log"
run_router; expect_log n4 'DISPATCH from=reviewer/approved next=Specialist\(perf\)' "reviewer approved -> perf gate"
pkg n4 perf.json "{\"spec\":\"n4\",\"stage\":\"perf\",\"status\":\"approved\",\"evidence\":[\"handoffs/n4/logs/r.log\"],$core}"
run_router; expect_log n4 'DISPATCH from=perf/approved next=DocWriter' "last gate approved -> DocWriter"
grep -q '#' "$T/handoffs/n4/docwriter.prompt.md" && bad "docwriter prompt contains '#'" || ok "docwriter prompt has no '#'"

# 4b. documented -> Explanation; explained -> human gate
pkg n4c docwriter.json "{\"spec\":\"n4c\",\"stage\":\"docwriter\",\"status\":\"documented\",\"evidence\":[\"handoffs/n4c/logs/v.log\"],\"target_files\":[],$core}"
echo x > "$T/handoffs/n4c/logs/v.log"
run_router; expect_log n4c 'DISPATCH from=docwriter/documented next=Explanation model=claude-test-b' "documented -> Explanation"
grep -q "НЕ выносишь вердикт" "$T/handoffs/n4c/explainer.prompt.md" && ok "explainer prompt forbids a verdict" || bad "explainer prompt must forbid a verdict"
grep -q '#' "$T/handoffs/n4c/explainer.prompt.md" && bad "explainer prompt contains '#'" || ok "explainer prompt has no '#'"
pkg n4c explainer.json "{\"spec\":\"n4c\",\"stage\":\"explainer\",\"status\":\"explained\",\"artifact\":\"docs/reviews/01-demo.brief.md\",\"evidence\":[\"handoffs/n4c/logs/v.log\"],\"sdd_ref\":\"docs/jms-spec/01-demo.md\",\"provenance\":{\"model\":\"m-b\",\"role\":\"Producer\",\"autonomy\":\"R2\"}}"
run_router; expect_log n4c 'EXPLAINED spec=n4c brief=docs/reviews/01-demo.brief.md' "explained -> human gate"
# 4d. explanation without evidence -> FABRICATED (Standard 15 applies to it too)
pkg n4d explainer.json "{\"spec\":\"n4d\",\"stage\":\"explainer\",\"status\":\"explained\",\"evidence\":[\"handoffs/n4d/logs/none.log\"],$core}"
run_router; expect_log n4d 'FABRICATED' "explanation without sources -> FABRICATED"

# 5. paused -> PAUSED + CONSULT with Owner and role
pkg n5 producer.json "{\"spec\":\"n5\",\"stage\":\"producer\",\"status\":\"paused\",\"question\":\"q?\",\"evidence\":[],\"target_files\":[\"src/a.c\"],$core}"
run_router; expect_log n5 'PAUSED spec=n5 stage=producer' "paused -> PAUSED"
expect_log n5 'CONSULT reason=PAUSED .*owner=Test Owner role=Architecture Owner' "consult hint names Owner and role"

# 5b. paused with platform file -> Agentic Platform Owner
pkg n5b producer.json "{\"spec\":\"n5b\",\"stage\":\"producer\",\"status\":\"paused\",\"question\":\"q?\",\"evidence\":[],\"target_files\":[\"build.cfg\"],$core}"
run_router; expect_log n5b 'role=Agentic Platform Owner' "platform file -> Agentic Platform Owner"

# 6. scope violation: untracked file outside target set
echo y > "$T/stray.txt"
pkg n6 producer.json "{\"spec\":\"n6\",\"stage\":\"producer\",\"status\":\"produced\",\"evidence\":[\"handoffs/n6/logs/v.log\"],\"target_files\":[\"src/a.c\"],$core}"
echo x > "$T/handoffs/n6/logs/v.log"
run_router; expect_log n6 'SCOPE-VIOLATION .*stray.txt' "file outside target_files -> SCOPE-VIOLATION"
# 6b. reviewer with producer's uncommitted file in tree -> SCOPE ok (layering)
mv "$T/stray.txt" "$T/src_a.c" 2>/dev/null || true
pkg n6b producer.json "{\"spec\":\"n6b\",\"stage\":\"producer\",\"status\":\"produced\",\"evidence\":[\"handoffs/n6b/logs/v.log\"],\"target_files\":[\"src_a.c\"],$core}"
echo x > "$T/handoffs/n6b/logs/v.log"
run_router
pkg n6b reviewer.json "{\"spec\":\"n6b\",\"stage\":\"reviewer\",\"status\":\"approved\",\"evidence\":[\"handoffs/n6b/logs/v.log\"],$core}"
run_router; expect_log n6b 'SCOPE ok pkg=reviewer.json' "reviewer inherits producer's target set (layering)"
rm -f "$T/src_a.c"

# 7. rejected at max iteration -> ESCALATE
pkg n7 reviewer.json "{\"spec\":\"n7\",\"stage\":\"reviewer\",\"status\":\"rejected\",\"iteration\":5,\"evidence\":[\"handoffs/n7/logs/r.log\"],$core}"
echo x > "$T/handoffs/n7/logs/r.log"
run_router; expect_log n7 'ESCALATE iter=5 max=5' "rejected at limit -> ESCALATE"

# 8. ceiling per spec
pkg n8 producer.json "{\"spec\":\"n8\",\"stage\":\"producer\",\"status\":\"produced\",\"evidence\":[\"handoffs/n8/logs/v.log\"],\"target_files\":[],$core}"
echo x > "$T/handoffs/n8/logs/v.log"
CHAIN_MAX_DISPATCH=1 run_router
pkg n8 reviewer.json "{\"spec\":\"n8\",\"stage\":\"reviewer\",\"status\":\"approved\",\"evidence\":[\"handoffs/n8/logs/v.log\"],$core}"
CHAIN_MAX_DISPATCH=1 run_router; expect_log n8 'CEILING dispatches=2 max=1' "dispatch ceiling is per spec"

# 9. critic: clean with questions -> contradiction -> needs-work
pkg n9 critic.json "{\"spec\":\"n9\",\"stage\":\"critic\",\"status\":\"criticized\",\"verdict\":\"clean\",\"questions\":[{\"id\":\"Q1\",\"question\":\"?\",\"options\":[\"A\",\"other\"],\"answer_goes_to\":\"Semantics\"}],\"evidence\":[\"handoffs/n9/logs/c.md\"],$core}"
echo x > "$T/handoffs/n9/logs/c.md"
run_router; expect_log n9 'CRITIC contradiction verdict=clean questions=1' "clean+questions -> needs-work"
# 9b. critic clean -> Producer
pkg n9b critic.json "{\"spec\":\"n9b\",\"stage\":\"critic\",\"status\":\"criticized\",\"verdict\":\"clean\",\"questions\":[],\"evidence\":[\"handoffs/n9b/logs/c.md\"],$core}"
echo x > "$T/handoffs/n9b/logs/c.md"
run_router; expect_log n9b 'DISPATCH from=critic/criticized next=Producer' "critic clean -> Producer"

# 10. unknown status -> default-deny
pkg n10 producer.json "{\"spec\":\"n10\",\"stage\":\"producer\",\"status\":\"done\",\"evidence\":[],$core}"
run_router; expect_log n10 'UNKNOWN-STATUS status=done' "unknown status -> default-deny"

# 11. unbound wrapper placeholder -> NO-WRAPPER
sed -i.bak 's/WRAPPER_REVIEWER="claude-test-b"/WRAPPER_REVIEWER="{{WRAPPER_REVIEWER}}"/' "$T/.claude/chain/chain.env" && rm -f "$T/.claude/chain/chain.env.bak"
(cd "$T" && git add -A && git -c user.email=t@t -c user.name=t commit -qm rebind)
pkg n11 producer.json "{\"spec\":\"n11\",\"stage\":\"producer\",\"status\":\"produced\",\"evidence\":[\"handoffs/n11/logs/v.log\"],\"target_files\":[],$core}"
echo x > "$T/handoffs/n11/logs/v.log"
run_router; expect_log n11 'NO-WRAPPER' "unfilled wrapper slot -> NO-WRAPPER (default-deny)"

echo
echo "passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
