#!/usr/bin/env python3
"""Минимальный валидатор и архиватор, совместимый с OpenSpec (кит оснастки AEF).

Замена официального CLI (@fission-ai/openspec) без зависимостей для сред, где нет npm.
Реализует то подмножество правил, на которое опирается цепочка AEF, по образцу
src/core/parsers/*.ts и src/core/validation/validator.ts из OpenSpec 1.13.1, чтобы раскладку
читал и настоящий CLI, когда его можно будет поставить:

  openspec/specs/<capability>/spec.md            спецификация-факт (что система делает сейчас)
  openspec/changes/<id>/specs/<capability>/spec.md дельта (ADDED/MODIFIED/REMOVED/RENAMED)
  openspec/changes/<id>/{proposal.md,design.md,tasks.md,.openspec.yaml}
  openspec/changes/archive/<YYYY-MM-DD>-<id>/     изменение, перенесённое в архив

Запуск:
  openspec.py validate [<change-id>]    # все спецификации-факты (и дельты одного изменения)
  openspec.py archive <change-id>       # слить дельты в спецификации-факты, перенести изменение в архив

Идентификаторы требований (AEF, Том III, Стандарт 3, трассируемость требования): после утверждения
SHALL у требования стоит строка `ID:`, и идентификатор переживает перенос в спецификацию-факт.
Ошибки: ADDED или MODIFIED без `ID:`, ADDED с идентификатором, уже занятым в спецификации-факте,
MODIFIED, меняющий идентификатор, и повтор идентификатора между спецификациями-фактами. Требование
спецификации-факта без `ID:` — предупреждение: написанному до практики идентификатор проставляют
задним числом, а проверку оно не валит.

Каталог — из переменной окружения OPENSPEC_DIR относительно корня проекта, без неё — из .claude/chain/chain.env,
без значения там — openspec. Неисполнимый chain.env — код 2.

Код выхода 1 при любой ошибке (ERROR). Это политика как код (AEF, Стандарт 19): инструмент запускает
оснастка, а заявление агента «спецификация валидна» свидетельством не является.
"""
import os
import re
import shutil
import sys
from datetime import date
from pathlib import Path

sys.dont_write_bytecode = True  # route.py импортируется без __pycache__ в .claude/chain
sys.path.insert(0, str(Path(__file__).resolve().parent))
from route import load_env  # один загрузчик chain.env с маршрутизатором (M-13)
from trace import REQ_HDR, block_id, block_scenarios, norm, requirement_blocks, text_lines  # один разбор (M-18)
from trace import chain_env_value, KbError, critic_input, SOURCE_LINE
from route import Context, fingerprint
import admission

ROOT = Path(__file__).resolve().parents[2]


def openspec_dir():
    """Каталог спецификаций-фактов проекта (CHAIN-CONFIG-1): OPENSPEC_DIR из окружения, иначе из
    .claude/chain/chain.env, исполненного загрузчиком маршрутизатора load_env, иначе openspec. Процесс стадии
    получает только экспортированные переменные chain.env, поэтому инструмент исполняет файл сам. Неисполнимый
    chain.env — код 2."""
    if "OPENSPEC_DIR" in os.environ:
        return os.environ["OPENSPEC_DIR"]
    env = ROOT / ".claude" / "chain" / "chain.env"
    if not env.is_file():
        return "openspec"
    try:
        value = load_env(str(env), dict(os.environ))[0].get("OPENSPEC_DIR", "")
    except (OSError, RuntimeError) as e:
        print(f".claude/chain/chain.env не исполняется: {e}", file=sys.stderr)
        sys.exit(2)
    return "openspec" if not value or "{{" in value else value


OS_DIR = SPECS = CHANGES = ARCHIVE = None  # init_dirs(): после проверки команды, справка chain.env не исполняет


def init_dirs():
    global OS_DIR, SPECS, CHANGES, ARCHIVE
    OS_DIR = ROOT / openspec_dir()
    SPECS = OS_DIR / "specs"
    CHANGES = OS_DIR / "changes"
    ARCHIVE = CHANGES / "archive"

REQ_SECTION = re.compile(r"^##\s+Requirements\s*$", re.I)
TOP_SECTION = re.compile(r"^##\s+")
DELTA_HDR = re.compile(r"^##\s+(ADDED|MODIFIED|REMOVED|RENAMED)\s+Requirements\s*$", re.I)
PURPOSE = re.compile(r"^##\s+Purpose\s*$", re.I)
SHALL = re.compile(r"\b(SHALL|MUST)\b")


def read_spec(path):
    """(structure, raw) of a spec file: structure from text_lines (code blocks and HTML comments blanked), raw
    lines of the same text for the content that archive writes back. Both have the same line numbers."""
    text = path.read_text(encoding="utf-8")
    return text_lines(text), text.replace("\r\n", "\n").split("\n")


def has_scenario(body):
    """At least one scenario with a non-empty body (a bare header does not count)."""
    return any(has_body for _, _, has_body in block_scenarios(body))


def subsection_errors(path, lines, start, end, where):
    """A `### ` header that is not a requirement inside a requirements section: archive would drop its text."""
    return [f"{path}:{i+1}: заголовок внутри {where} не требование: {lines[i].strip()}"
            for i in range(start, end) if lines[i].startswith("### ") and not REQ_HDR.match(lines[i])]


def validate_main(path, errors, ids=None, warnings=None):
    lines, _ = read_spec(path)
    if not any(PURPOSE.match(ln) for ln in lines):
        errors.append(f"{path}: нет '## Purpose'")
    req_idx = next((i for i, ln in enumerate(lines) if REQ_SECTION.match(ln)), -1)
    if req_idx == -1:
        errors.append(f"{path}: нет '## Requirements'")
        return
    req_end = next((i for i in range(req_idx + 1, len(lines)) if TOP_SECTION.match(lines[i])), len(lines))
    seen = set()
    errors.extend(subsection_errors(path, lines, req_idx + 1, req_end, "'## Requirements'"))
    for i, ln in enumerate(lines):
        if DELTA_HDR.match(ln):
            errors.append(f"{path}:{i+1}: заголовок дельты в спецификации-факте: {ln.strip()}")
        m = REQ_HDR.match(ln)
        if m and not (req_idx < i < req_end):
            errors.append(f"{path}:{i+1}: требование вне '## Requirements': {ln.strip()}")
    for name, hl, stop in requirement_blocks(lines, req_idx, req_end):
        raw = lines[hl + 1:stop]
        if name in seen:
            errors.append(f"{path}:{hl+1}: требование '{name}' повторяется")
        seen.add(name)
        if not has_scenario(raw):
            errors.append(f"{path}:{hl+1}: у требования '{name}' нет сценария с телом")
        text = "\n".join(raw)
        if not SHALL.search(text):
            errors.append(f"{path}:{hl+1}: в требовании '{name}' нет SHALL или MUST")
        rid = block_id(raw)
        if ids is not None:
            if rid is None:
                if warnings is not None:
                    warnings.append(f"{path}:{hl+1}: у требования '{name}' нет строки ID: (проставить задним числом)")
            elif rid in ids:
                errors.append(f"{path}:{hl+1}: ID требования {rid} повторяется (ещё в {ids[rid]})")
            else:
                ids[rid] = f"{path}:{hl+1}"
    return len(seen)


def parse_delta(path):
    """-> dict op -> [(name, raw_lines, body_lines)] plus errors list. body_lines are the block from text_lines
    for the checks, raw_lines the same lines of the text for archive."""
    lines, raw = read_spec(path)
    ops, errors, sections = {}, [], []
    for i, ln in enumerate(lines):  # every `## ` header closes the open section, a delta header opens the next
        d = DELTA_HDR.match(ln)
        if d or TOP_SECTION.match(ln):
            if sections and sections[-1][2] is None:
                sections[-1][2] = i
            if d:
                sections.append([d.group(1).upper(), i + 1, None])
    if sections and sections[-1][2] is None:
        sections[-1][2] = len(lines)
    for i, ln in enumerate(lines):
        m = REQ_HDR.match(ln)
        if m and not any(a <= i < b for _, a, b in sections):
            errors.append(f"{path}:{i+1}: требование '{norm(m.group(1))}' вне раздела дельты")
    for op, start, end in sections:
        ops.setdefault(op, [])
        errors.extend(subsection_errors(path, lines, start, end, f"раздела {op}"))
        blocks = requirement_blocks(lines, start, end)
        for name, h, stop in blocks:
            ops[op].append([name, raw[h + 1:stop], lines[h + 1:stop]])
        if op == "REMOVED":
            inside = {i for _, h, stop in blocks for i in range(h, stop)}
            for i in range(start, blocks[0][1] if blocks else end):
                b = re.match(r"^\s*[-*]\s+(?:`?)(.+?)(?:`?)\s*$", lines[i])
                if b and i not in inside:
                    ops["REMOVED"].append([norm(b.group(1)), [], []])
    if not ops:
        errors.append(f"{path}: нет разделов дельты (## ADDED/MODIFIED/REMOVED/RENAMED Requirements)")
    names = {op: [n for n, _, _ in v] for op, v in ops.items()}
    for op in ("ADDED", "MODIFIED"):
        for name, _, body in ops.get(op, []):
            if not has_scenario(body):
                errors.append(f"{path}: в {op} '{name}' должен быть хотя бы один сценарий")
            if not SHALL.search("\n".join(body)):
                errors.append(f"{path}: в {op} '{name}' нет SHALL или MUST")
        dup = {n for n in names.get(op, []) if names[op].count(n) > 1}
        for n in dup:
            errors.append(f"{path}: требование повторяется в {op}: '{n}'")
    for a, b in (("MODIFIED", "REMOVED"), ("MODIFIED", "ADDED"), ("ADDED", "REMOVED")):
        for n in set(names.get(a, [])) & set(names.get(b, [])):
            errors.append(f"{path}: требование есть и в {a}, и в {b}: '{n}'")
    return ops, errors


def change_dir(change_id):
    d = CHANGES / change_id
    if not d.is_dir():
        sys.exit(f"изменения нет: {d}")
    return d


def validate(change_id=None):
    errors, warnings, ids, n_main = [], [], {}, 0
    for spec in sorted(SPECS.glob("*/spec.md")) if SPECS.is_dir() else []:
        n = validate_main(spec, errors, ids, warnings)
        n_main += n or 0
    n_delta = 0
    if change_id:
        d = change_dir(change_id)
        for req in ("proposal.md",):
            if not (d / req).is_file():
                errors.append(f"{d}: нет {req}")
        meta = d / ".openspec.yaml"
        if not meta.is_file():
            errors.append(f"{d}: нет .openspec.yaml (schema:, created: YYYY-MM-DD)")
        refs = re.findall(r"^task_ref:\s*(.*?)\s*$", meta.read_text(encoding="utf-8"), re.M) if meta.is_file() else []
        sdd_delta = len(refs) == 1 and re.fullmatch(r"[0-9]+-[a-z0-9-]+\.md", Path(refs[0].strip("'\"")).name)
        deltas = sorted(d.glob("specs/*/spec.md"))
        if (d / "specs" / "spec.md").is_file():
            errors.append(f"{d}: дельта в specs/spec.md не учитывается. Положи её в specs/<capability>/spec.md")
        if not deltas:
            errors.append(f"{d}: нет дельт в specs/<capability>/spec.md")
        for delta in deltas:
            ops, errs = parse_delta(delta)
            errors.extend(errs)
            main = SPECS / delta.parent.name / "spec.md"
            existing, existing_ids = set(), {}
            if main.is_file():
                ml, _ = read_spec(main)
                ri = next((i for i, ln in enumerate(ml) if REQ_SECTION.match(ln)), -1)
                if ri != -1:
                    re_ = next((i for i in range(ri + 1, len(ml)) if TOP_SECTION.match(ml[i])), len(ml))
                    for n, h, stop in requirement_blocks(ml, ri, re_):
                        existing.add(n)
                        existing_ids[n] = block_id(ml[h + 1:stop])
            for op in ("ADDED", "MODIFIED"):
                for name, _, body in ops.get(op, []):
                    rid = block_id(body)
                    if sdd_delta and not any(SOURCE_LINE.match(line) for line in body):
                        errors.append(f"{delta}: у {op} '{name}' нет Источник: — основание должно сохраниться в факте")
                    if rid is None:
                        errors.append(f"{delta}: у {op} '{name}' нет строки ID: — по идентификатору инцидент находит требование (Стандарт 3)")
                    elif op == "ADDED" and rid in ids:
                        errors.append(f"{delta}: ADDED '{name}' повторно использует ID {rid}, уже занятый в {ids[rid]}")
                    elif op == "MODIFIED" and existing_ids.get(name) and rid != existing_ids[name]:
                        errors.append(f"{delta}: MODIFIED '{name}' меняет ID {existing_ids[name]} -> {rid}, а идентификатор должен пережить изменение")
            for op in ("MODIFIED", "REMOVED"):
                for name, _, _ in ops.get(op, []):
                    if name not in existing:
                        errors.append(f"{delta}: {op} '{name}' не найдено в {main}")
            for name, _, _ in ops.get("ADDED", []):
                if name in existing:
                    errors.append(f"{delta}: ADDED '{name}' уже есть в {main} (нужен MODIFIED)")
            n_delta += sum(len(v) for v in ops.values())
    for w in warnings:
        print(f"WARN  {w}")
    for e in errors:
        print(f"ERROR {e}")
    print(f"openspec: требований в спецификациях-фактах {n_main}, записей дельт {n_delta}, "
          f"ошибок {len(errors)}, предупреждений {len(warnings)}")
    return 0 if not errors else 1


def shown(path):
    """Путь для сообщения: от корня проекта, если каталог внутри него, иначе полностью (OPENSPEC_DIR вне корня)."""
    try:
        return path.relative_to(ROOT)
    except ValueError:
        return path


def render_block(name, raw):
    body = "\n".join(raw).strip("\n")
    return f"### Requirement: {name}\n{body}\n"


def check_archive_admission(change_id):
    """До записи: постановка, закрытая область и тот же свежий допуск, что у маршрутизатора."""
    meta = (change_dir(change_id) / ".openspec.yaml").read_text(encoding="utf-8")
    values = re.findall(r"^task_ref:\s*(.*?)\s*$", meta, re.M)
    if len(values) != 1:
        raise ValueError("нужна одна строка task_ref: <путь постановки> в .openspec.yaml")
    ref = values[0].strip("'\"")
    path = (ROOT / ref).resolve()
    if not admission.safe_path(ref) or not path.is_relative_to(ROOT.resolve()) or not path.is_file() or path.stem != change_id:
        raise ValueError("task_ref должен указывать на постановку этого change-id внутри компонента")
    from workflow_contract import native
    if native(path):
        raise ValueError("native archive requires trusted runtime: .claude/chain/aef.py archive " + change_id)
    text = path.read_text(encoding="utf-8")
    headings = {line[3:].strip() for line in text_lines(text) if line.startswith("## ")}
    if re.fullmatch(r"M-[0-9]+-[a-z0-9-]+", path.stem):
        if not {"Spec ref", "Done criteria"} <= headings:
            raise ValueError("задаче сопровождения нужны Spec ref и Done criteria")
        return
    if not re.fullmatch(r"[0-9]+-[a-z0-9-]+", path.stem) or not {"Problem", "Acceptance Criteria", "Stop-conditions"} <= headings:
        raise ValueError("task_ref не распознаётся как SDD или задача сопровождения")
    if admission.read_plan(text) is not None:
        raise ValueError("ограниченный допуск не разрешает archive всей SDD")
    hd = chain_env_value("HANDOFF_DIR")
    if not hd or "{{" in hd:
        raise ValueError("HANDOFF_DIR не задан. Укажи каталог записей допуска")
    record_path = ROOT / hd / path.stem.split("-", 1)[0] / "critic.input.json"
    try:
        rec = admission._json(record_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise ValueError(f"нет читаемой записи полного допуска: {record_path}") from e
    data, errors = critic_input(path)
    if errors:
        raise ValueError("вход критика не прочитан: " + ". ".join(errors))
    ctx = Context(str(ROOT), {}, dict(os.environ))
    ok, snap = fingerprint(ctx, paths=[ref])
    if not ok:
        raise ValueError("снимок SDD не вычисляется")
    problem = admission.record_issue(rec, ref, snap, data, None)
    if problem:
        raise ValueError(f"нужен свежий полный clean критика: {problem}")


def merged_text(main, ops, cap):
    """Рассчитать результат слияния без записи и без приёмки. Вызывается после validate."""
    if main.is_file():
        lines, raw = read_spec(main)
        ri = next(i for i, ln in enumerate(lines) if REQ_SECTION.match(ln))
        re_ = next((i for i in range(ri + 1, len(lines)) if TOP_SECTION.match(lines[i])), len(lines))
        found = requirement_blocks(lines, ri, re_)
        # The section's introduction, before its first requirement, is kept as written (review r1 M-18, B1).
        head, tail = raw[: found[0][1] if found else re_], raw[re_:]
        blocks = [(n, raw[h + 1:stop]) for n, h, stop in found]
    else:
        title = cap.replace("-", " ").title()
        purpose = f"# {title}\n\n## Purpose\n\nПока не описано — заполняется садовником при первом archive.\n"
        head, tail, blocks = (purpose + "\n## Requirements").split("\n"), [], []
    by = {n: raw for n, raw in blocks}
    order = [n for n, _ in blocks]
    for name, _, _ in ops.get("REMOVED", []):
        by.pop(name, None)
        order = [n for n in order if n != name]
    for name, body, _ in ops.get("MODIFIED", []):
        by[name] = body
    for name, body, _ in ops.get("ADDED", []):
        by[name] = body
        order.append(name)
    body = "\n".join(render_block(n, by[n]) for n in order)
    out = "\n".join(head).rstrip("\n") + "\n\n" + body.rstrip("\n") + "\n"
    if tail:
        out += "\n" + "\n".join(tail).rstrip("\n") + "\n"
    return out, len(order)


def archive(change_id):
    if validate(change_id) != 0:
        sys.exit("archive отклонён: есть ошибки валидации")
    try:
        check_archive_admission(change_id)
    except (ValueError, OSError, KbError) as e:
        sys.exit(f"archive отклонён: {e}")
    d = change_dir(change_id)
    for delta in sorted(d.glob("specs/*/spec.md")):
        cap = delta.parent.name
        main = SPECS / cap / "spec.md"
        ops, _ = parse_delta(delta)
        out, count = merged_text(main, ops, cap)
        main.parent.mkdir(parents=True, exist_ok=True)
        main.write_text(out, encoding="utf-8")
        print(f"слито {shown(delta)} -> {shown(main)} (требований: {count})")
    ARCHIVE.mkdir(parents=True, exist_ok=True)
    dest = ARCHIVE / f"{date.today().isoformat()}-{change_id}"
    if dest.exists():
        sys.exit(f"каталог архива уже существует: {dest}")
    shutil.move(str(d), str(dest))
    print(f"перенесено в архив -> {shown(dest)}")
    return validate()


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args or args[0] not in ("validate", "archive"):
        sys.exit(__doc__)
    if args[0] == "archive" and len(args) < 2:
        sys.exit("запуск: openspec.py archive <change-id>")
    init_dirs()
    if args[0] == "validate":
        sys.exit(validate(args[1] if len(args) > 1 else None))
    sys.exit(archive(args[1]))
