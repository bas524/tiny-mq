#!/usr/bin/env python3
"""Minimal OpenSpec-compatible validator and archiver for tiny-mq.

The official CLI (@fission-ai/openspec) needs npm, which the corporate proxy
refuses (403). This script implements the subset of its rules that the
tiny-mq harness relies on, mirroring src/core/parsers/*.ts and
src/core/validation/validator.ts of OpenSpec 1.13.1 so the layout stays
readable by the real CLI once it can be installed:

  openspec/specs/<capability>/spec.md            current truth (main spec)
  openspec/changes/<id>/specs/<capability>/spec.md delta (ADDED/MODIFIED/REMOVED/RENAMED)
  openspec/changes/<id>/{proposal.md,design.md,tasks.md,.openspec.yaml}
  openspec/changes/archive/<YYYY-MM-DD>-<id>/     archived change

Usage:
  openspec.py validate [<change-id>]    # all main specs (+ one change's deltas)
  openspec.py archive <change-id>       # merge deltas into main specs, move change

Exit code 1 on any ERROR. This is policy-as-code (AEF Standard 19): the
harness runs it, an agent's claim that "the spec is valid" is not evidence.
"""
import re
import shutil
import sys
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OS_DIR = ROOT / "openspec"
SPECS = OS_DIR / "specs"
CHANGES = OS_DIR / "changes"
ARCHIVE = CHANGES / "archive"

REQ_SECTION = re.compile(r"^##\s+Requirements\s*$", re.I)
TOP_SECTION = re.compile(r"^##\s+")
DELTA_HDR = re.compile(r"^##\s+(ADDED|MODIFIED|REMOVED|RENAMED)\s+Requirements\s*$", re.I)
REQ_HDR = re.compile(r"^###\s*Requirement:\s*(.+?)\s*#*\s*$", re.I)
SCN_HDR = re.compile(r"^####\s*(?:Scenario:\s*)?(.+?)\s*#*\s*$", re.I)
PURPOSE = re.compile(r"^##\s+Purpose\s*$", re.I)
SHALL = re.compile(r"\b(SHALL|MUST)\b")
FENCE = re.compile(r"^\s*(```|~~~)")


def strip_fences(lines):
    """Blank out fenced code so '### Requirement:' in examples is ignored."""
    out, fenced = [], False
    for ln in lines:
        if FENCE.match(ln):
            fenced = not fenced
            out.append("")
            continue
        out.append("" if fenced else ln)
    return out


def norm(name):
    return re.sub(r"\s+", " ", name.strip().rstrip("#").strip())


def parse_blocks(lines, start, end):
    """Requirement blocks between start..end -> [(name, header_line, raw_lines)]."""
    blocks, cur = [], None
    for i in range(start, end):
        m = REQ_HDR.match(lines[i])
        if m:
            cur = [norm(m.group(1)), i, []]
            blocks.append(cur)
        elif cur is not None:
            cur[2].append(lines[i])
    return blocks


def scenarios(raw):
    """Scenario names with non-empty bodies (a bare header does not count)."""
    found, name, body = [], None, []
    for ln in raw:
        m = SCN_HDR.match(ln)
        if m:
            if name and any(s.strip() for s in body):
                found.append(name)
            name, body = norm(m.group(1)), []
        elif name is not None:
            body.append(ln)
    if name and any(s.strip() for s in body):
        found.append(name)
    return found


def validate_main(path, errors):
    lines = strip_fences(path.read_text(encoding="utf-8").replace("\r\n", "\n").split("\n"))
    if not any(PURPOSE.match(ln) for ln in lines):
        errors.append(f"{path}: missing '## Purpose'")
    req_idx = next((i for i, ln in enumerate(lines) if REQ_SECTION.match(ln)), -1)
    if req_idx == -1:
        errors.append(f"{path}: missing '## Requirements'")
        return
    req_end = next((i for i in range(req_idx + 1, len(lines)) if TOP_SECTION.match(lines[i])), len(lines))
    seen = set()
    for i, ln in enumerate(lines):
        if DELTA_HDR.match(ln):
            errors.append(f"{path}:{i+1}: delta header in main spec: {ln.strip()}")
        m = REQ_HDR.match(ln)
        if m and not (req_idx < i < req_end):
            errors.append(f"{path}:{i+1}: requirement outside '## Requirements': {ln.strip()}")
    for name, hl, raw in parse_blocks(lines, req_idx, req_end):
        if name in seen:
            errors.append(f"{path}:{hl+1}: duplicate requirement '{name}'")
        seen.add(name)
        if not scenarios(raw):
            errors.append(f"{path}:{hl+1}: requirement '{name}' has no scenario with a body")
        text = "\n".join(raw)
        if not SHALL.search(text):
            errors.append(f"{path}:{hl+1}: requirement '{name}' has no SHALL/MUST")
    return len(seen)


def parse_delta(path):
    """-> dict op -> [(name, raw_lines)] plus errors list."""
    lines = strip_fences(path.read_text(encoding="utf-8").replace("\r\n", "\n").split("\n"))
    ops, errors, cur_op, cur = {}, [], None, None
    for i, ln in enumerate(lines):
        d = DELTA_HDR.match(ln)
        if d:
            cur_op, cur = d.group(1).upper(), None
            ops.setdefault(cur_op, [])
            continue
        if TOP_SECTION.match(ln):
            cur_op, cur = None, None
            continue
        m = REQ_HDR.match(ln)
        if m:
            if cur_op is None:
                errors.append(f"{path}:{i+1}: requirement '{norm(m.group(1))}' outside a delta section")
                cur = None
                continue
            cur = [norm(m.group(1)), []]
            ops[cur_op].append(cur)
            continue
        if cur_op == "REMOVED" and cur is None:
            b = re.match(r"^\s*[-*]\s+(?:`?)(.+?)(?:`?)\s*$", ln)
            if b:
                ops["REMOVED"].append([norm(b.group(1)), []])
            continue
        if cur is not None:
            cur[1].append(ln)
    if not ops:
        errors.append(f"{path}: no delta sections (## ADDED/MODIFIED/REMOVED/RENAMED Requirements)")
    names = {op: [n for n, _ in v] for op, v in ops.items()}
    for op in ("ADDED", "MODIFIED"):
        for name, raw in ops.get(op, []):
            if not scenarios(raw):
                errors.append(f"{path}: {op} '{name}' must include at least one scenario")
            if not SHALL.search("\n".join(raw)):
                errors.append(f"{path}: {op} '{name}' has no SHALL/MUST")
        dup = {n for n in names.get(op, []) if names[op].count(n) > 1}
        for n in dup:
            errors.append(f"{path}: duplicate requirement in {op}: '{n}'")
    for a, b in (("MODIFIED", "REMOVED"), ("MODIFIED", "ADDED"), ("ADDED", "REMOVED")):
        for n in set(names.get(a, [])) & set(names.get(b, [])):
            errors.append(f"{path}: requirement present in both {a} and {b}: '{n}'")
    return ops, errors


def change_dir(change_id):
    d = CHANGES / change_id
    if not d.is_dir():
        sys.exit(f"no such change: {d}")
    return d


def validate(change_id=None):
    errors, n_main = [], 0
    for spec in sorted(SPECS.glob("*/spec.md")) if SPECS.is_dir() else []:
        n = validate_main(spec, errors)
        n_main += n or 0
    n_delta = 0
    if change_id:
        d = change_dir(change_id)
        for req in ("proposal.md",):
            if not (d / req).is_file():
                errors.append(f"{d}: missing {req}")
        meta = d / ".openspec.yaml"
        if not meta.is_file():
            errors.append(f"{d}: missing .openspec.yaml (schema:, created: YYYY-MM-DD)")
        deltas = sorted(d.glob("specs/*/spec.md"))
        if (d / "specs" / "spec.md").is_file():
            errors.append(f"{d}: delta at specs/spec.md is ignored — put it under specs/<capability>/spec.md")
        if not deltas:
            errors.append(f"{d}: no delta specs under specs/<capability>/spec.md")
        for delta in deltas:
            ops, errs = parse_delta(delta)
            errors.extend(errs)
            main = SPECS / delta.parent.name / "spec.md"
            existing = set()
            if main.is_file():
                ml = strip_fences(main.read_text(encoding="utf-8").split("\n"))
                ri = next((i for i, ln in enumerate(ml) if REQ_SECTION.match(ln)), -1)
                if ri != -1:
                    re_ = next((i for i in range(ri + 1, len(ml)) if TOP_SECTION.match(ml[i])), len(ml))
                    existing = {n for n, _, _ in parse_blocks(ml, ri, re_)}
            for op in ("MODIFIED", "REMOVED"):
                for name, _ in ops.get(op, []):
                    if name not in existing:
                        errors.append(f"{delta}: {op} '{name}' not found in {main}")
            for name, _ in ops.get("ADDED", []):
                if name in existing:
                    errors.append(f"{delta}: ADDED '{name}' already exists in {main} (use MODIFIED)")
            n_delta += sum(len(v) for v in ops.values())
    for e in errors:
        print(f"ERROR {e}")
    print(f"openspec: {n_main} main requirement(s), {n_delta} delta entr{'y' if n_delta == 1 else 'ies'}, {len(errors)} error(s)")
    return 0 if not errors else 1


def render_block(name, raw):
    body = "\n".join(raw).strip("\n")
    return f"### Requirement: {name}\n{body}\n"


def archive(change_id):
    if validate(change_id) != 0:
        sys.exit("archive refused: validation errors")
    d = change_dir(change_id)
    for delta in sorted(d.glob("specs/*/spec.md")):
        cap = delta.parent.name
        main = SPECS / cap / "spec.md"
        ops, _ = parse_delta(delta)
        if main.is_file():
            text = main.read_text(encoding="utf-8")
            lines = text.split("\n")
            ri = next(i for i, ln in enumerate(lines) if REQ_SECTION.match(ln))
            re_ = next((i for i in range(ri + 1, len(lines)) if TOP_SECTION.match(lines[i])), len(lines))
            head, tail = lines[: ri + 1], lines[re_:]
            blocks = [(n, raw) for n, _, raw in parse_blocks(lines, ri, re_)]
        else:
            title = cap.replace("-", " ").title()
            purpose = f"# {title}\n\n## Purpose\n\nTBD — заполняется садовником при первом archive.\n"
            head, tail, blocks = (purpose + "\n## Requirements").split("\n"), [], []
            main.parent.mkdir(parents=True, exist_ok=True)
        by = {n: raw for n, raw in blocks}
        order = [n for n, _ in blocks]
        for name, _ in ops.get("REMOVED", []):
            by.pop(name, None)
            order = [n for n in order if n != name]
        for name, raw in ops.get("MODIFIED", []):
            by[name] = raw
        for name, raw in ops.get("ADDED", []):
            by[name] = raw
            order.append(name)
        body = "\n".join(render_block(n, by[n]) for n in order)
        out = "\n".join(head).rstrip("\n") + "\n\n" + body.rstrip("\n") + "\n"
        if tail:
            out += "\n" + "\n".join(tail).rstrip("\n") + "\n"
        main.write_text(out, encoding="utf-8")
        print(f"merged {delta.relative_to(ROOT)} -> {main.relative_to(ROOT)} ({len(order)} requirement(s))")
    ARCHIVE.mkdir(parents=True, exist_ok=True)
    dest = ARCHIVE / f"{date.today().isoformat()}-{change_id}"
    if dest.exists():
        sys.exit(f"archive target exists: {dest}")
    shutil.move(str(d), str(dest))
    print(f"archived -> {dest.relative_to(ROOT)}")
    return validate()


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args or args[0] not in ("validate", "archive"):
        sys.exit(__doc__)
    if args[0] == "validate":
        sys.exit(validate(args[1] if len(args) > 1 else None))
    if len(args) < 2:
        sys.exit("archive <change-id>")
    sys.exit(archive(args[1]))
