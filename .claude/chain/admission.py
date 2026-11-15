"""SDD 0004: разбор и сравнение допуска. Нет записи на диск и запуска процессов."""
import json
from pathlib import Path, PurePosixPath
import re


# Общий разбор Markdown для допуска, trace и OpenSpec. Без ввода-вывода.
SECTION = re.compile(r"^##\s+(.+?)\s*$")


REQ_HDR = re.compile(r"^###\s*Requirement:\s*(.+?)\s*#*\s*$", re.I)


SCN_HDR = re.compile(r"^####\s*(?:Scenario:\s*)?(.+?)\s*#*\s*$", re.I)


ID_LINE = re.compile(r"^\s*(?:[-*]\s+)?ID:\s*`?([^`\s]+)`?\s*$")


FENCE = re.compile(r"^\s*(```|~~~)")


def text_lines(text):
    """Строки текста по правилам lines_of."""
    out, fenced, comment = [], False, False
    for ln in re.sub(r"\r\n", "\n", text).split("\n"):
        if not fenced and not comment and ln.lstrip().startswith("<!--"):
            comment = "-->" not in ln.lstrip()[4:]
            out.append("")
            continue
        if comment:
            comment = "-->" not in ln
            out.append("")
            continue
        if FENCE.match(ln):
            fenced = not fenced
            out.append("")
            continue
        out.append("" if fenced else ln)
    return out


def section(lines, name):
    """Lines of '## <name>' up to the next '## '."""
    start = next((i for i, ln in enumerate(lines) if SECTION.match(ln) and SECTION.match(ln).group(1).lower() == name.lower()), -1)
    if start == -1:
        return None
    end = next((i for i in range(start + 1, len(lines)) if SECTION.match(lines[i])), len(lines))
    return lines[start + 1:end]


def norm(name):
    """Requirement or scenario name: inner whitespace collapsed, trailing # dropped."""
    return re.sub(r"\s+", " ", name.strip().rstrip("#").strip())


def is_subsection(ln):
    """A `## ` or `### ` header that is not a requirement: it ends a requirement block."""
    return bool(SECTION.match(ln)) or ln.startswith("### ")


def requirement_blocks(lines, start=0, end=None):
    """Requirement blocks of lines[start:end], lines as text_lines gives them: [(name, header, stop)]. The header
    is lines[header], the body lines[header + 1:stop]. A block ends at the next requirement or at any other
    `## `/`### ` header. One parser for trace.py and openspec.py (M-18)."""
    end = len(lines) if end is None else end
    out, cur = [], None
    for i in range(start, end):
        m = REQ_HDR.match(lines[i])
        if m or is_subsection(lines[i]):
            if cur:
                out.append((cur[0], cur[1], i))
            cur = (norm(m.group(1)), i) if m else None
    if cur:
        out.append((cur[0], cur[1], end))
    return out


def block_id(body):
    """The `ID:` of a requirement block, before its first scenario, or None."""
    for ln in body:
        if SCN_HDR.match(ln) and ln.startswith("####"):
            return None
        m = ID_LINE.match(ln)
        if m:
            return m.group(1)
    return None


def _json(text):
    def pairs(items):
        out = {}
        for key, value in items:
            if key in out:
                raise ValueError(f"повтор ключа {key}")
            out[key] = value
        return out
    return json.loads(text, object_pairs_hook=pairs, parse_constant=lambda x: (_ for _ in ()).throw(ValueError(x)))


def read_plan(text):
    """Один aef-admission в Stop-conditions. ID берутся только из Acceptance Criteria."""
    section_name, fence, info, body, blocks, comment = "", "", "", [], [], False
    for line in text.splitlines():
        stripped = line.strip()
        if fence:
            if re.fullmatch(re.escape(fence[0]) + "{" + str(len(fence)) + ",}\\s*", stripped):
                if info == "aef-admission":
                    blocks.append(_json("\n".join(body)))
                fence, info, body = "", "", []
            elif info == "aef-admission":
                body.append(line)
            continue
        if comment:
            comment = "-->" not in line
            continue
        if stripped.startswith("<!--"):
            comment = "-->" not in stripped[4:]
            continue
        m = re.match(r"^(`{3,}|~{3,})(.*)$", stripped)
        if m:
            fence, info = m.group(1), m.group(2).strip()
            if info == "aef-admission" and section_name != "Stop-conditions":
                raise ValueError("aef-admission допустим только в Stop-conditions")
            continue
        m = re.match(r"^##\s+(.+?)\s*$", line)
        if m:
            section_name = m.group(1)
    if fence and info == "aef-admission":
        raise ValueError("незакрытый блок aef-admission")
    if not blocks:
        return None
    if len(blocks) != 1:
        raise ValueError("нужен ровно один блок aef-admission")
    criteria = section(text_lines(text), "Acceptance Criteria") or []
    ids = [block_id(criteria[start + 1:end]) for _, start, end in requirement_blocks(criteria)]
    if not ids or any(value is None for value in ids):
        raise ValueError("каждый блок Requirement должен иметь ID до первого Scenario")
    plan = blocks[0]
    if not isinstance(plan, dict) or set(plan) != {"allowed", "blocked", "stop_when"}:
        raise ValueError("aef-admission: нужны только allowed, blocked, stop_when")
    allowed, blocked = plan["allowed"], plan["blocked"]
    if not string_set(allowed) or not isinstance(blocked, dict) or not blocked:
        raise ValueError("allowed и blocked должны быть непустыми")
    if any(not isinstance(q, str) or not q.strip() or not string_set(v) for q, v in blocked.items()):
        raise ValueError("blocked: каждый вопрос имеет непустой список уникальных ID")
    excluded = set().union(*map(set, blocked.values()))
    if len(ids) != len(set(ids)) or set(allowed) & excluded or set(allowed) | excluded != set(ids):
        raise ValueError("allowed и blocked должны точно разделять все ID требований SDD")
    if not isinstance(plan["stop_when"], str) or not plan["stop_when"].strip():
        raise ValueError("stop_when должен быть непустым")
    return plan


def string_set(value):
    return (isinstance(value, list) and bool(value)
            and all(isinstance(v, str) and bool(v.strip()) for v in value) and len(set(value)) == len(value))


def safe_path(value):
    return (isinstance(value, str) and bool(value) and not value.startswith("/")
            and not value.endswith("/") and "\\" not in value and not re.search(r"[\x00-\x1f*?\[\]]", value)
            and all(p not in ("", ".", "..") for p in value.split("/"))
            and not PurePosixPath(value).is_absolute())


def check_targets(root, targets, forbidden=()):
    if not string_set(targets):
        raise ValueError("набор файлов допуска должен быть непустым и без повторов")
    base = Path(root).resolve()
    protected = [(base / f).resolve() for f in forbidden if f]
    protected.append((base / ".git").resolve())
    for name in targets:
        path = (base / name).resolve() if safe_path(name) else None
        if (path is None or not path.is_relative_to(base) or path.is_dir()
                or any(path == f or path.is_relative_to(f)
                       or path.exists() and f.exists() and path.samefile(f) for f in protected)):
            raise ValueError(f"небезопасный файл допуска: {name}")
    return targets


DOC_KEYS = {"ref", "path", "pinned"}
RECORD_KEYS = {"package", "sdd", "sdd_snapshot", "intent", "contracts", "sources", "admission"}
ADMISSION_KEYS = {"allowed", "blocked", "stop_when", "target_files", "scope_sha256"}


def record_ok(rec):
    def doc(d):
        return isinstance(d, dict) and set(d) == DOC_KEYS and all(isinstance(d[k], str) and d[k] for k in DOC_KEYS)
    return (isinstance(rec, dict) and set(rec) == RECORD_KEYS
            and all(isinstance(rec[k], str) and rec[k] for k in ("package", "sdd", "sdd_snapshot"))
            and (rec["intent"] is None or doc(rec["intent"]))
            and all(isinstance(rec[k], list) and all(doc(d) for d in rec[k]) for k in ("contracts", "sources"))
            and (rec["admission"] is None or isinstance(rec["admission"], dict)
                 and set(rec["admission"]) == ADMISSION_KEYS))


def record_issue(rec, sdd, snapshot, data, scope):
    """Первое расхождение. head всегда вычислен заново, а не взят из записи."""
    if not record_ok(rec):
        return "record"
    if rec["sdd"] != sdd or rec["sdd_snapshot"] != snapshot:
        return "sdd"
    if rec["admission"] != scope:
        return "admission"
    for key in ("intent", "contracts", "sources"):
        old = [rec[key]] if key == "intent" and rec[key] else ([] if key == "intent" else rec[key])
        new_value = data.get(key)
        new = [new_value] if key == "intent" and new_value else ([] if key == "intent" else new_value or [])
        if len(old) != len(new):
            return "sdd"
        for a, b in zip(old, new):
            if a["ref"] != b.get("ref") or a["path"] != b.get("path"):
                return "sdd"
            if a["pinned"] != b.get("pinned") or a["pinned"] != b.get("head"):
                return "intent" if key == "intent" else f"{key[:-1]}:{a['path']}"
    return None
