#!/usr/bin/env python3
"""Трассируемость требования (AEF, Том III, Стандарт 3; Приложение C).

Без зависимостей. Правило книги: план пишется сверху вниз, факт — снизу вверх, и они
сверяются. SDD ссылается вверх на источник (строка `Источник:`) и планирует проверки вниз
(строки `Test:`), а тесты ссылаются вверх на идентификаторы требований. Инструмент
проверяет формат и сверяет план с фактом. Его вывод — свидетельство (Стандарт 15): стадия
прикладывает лог, а заявление агента «с трассируемостью всё в порядке» свидетельством не является.

Запуск:
  trace.py sdd   <sdd.md>                 # шлюз «Спецификация → Архитектура»
  trace.py tests <sdd.md> <каталог тестов> # шлюз «Агентное исполнение → Верификация»
  trace.py hash  <файл>                   # редакция источника, ещё не перенесённого в Git (временно)
  trace.py map   [<компонент>]            # карта компонентов коммита HEAD клона product-kb (Приложение I)
  trace.py intent <ID намерения>          # сверка декомпозиции вниз: план против SDD компонентов (M-8)
  trace.py input <sdd.md>                 # вход разбора критика одним объектом JSON (SDD 0003)

`sdd` проверяет и строку компонента (Стандарт 6): SDD относится ровно к одному компоненту,
единице слияния и отката.

SDD со строкой `Намерение: product-kb:<ID>@<коммит>` (составное намерение, Стандарт 1) `sdd` проверяет вверх
по этой редакции клона PRODUCT_KB_PATH: намерение, его декомпозиция и карта компонентов читаются одним снимком,
пара «компонент, ID SDD» должна быть в декомпозиции, а все строки `Источник: product-kb:…` — разрешаться
по links.source до бизнес-требования. Клон только читается.

`tests` нужен шаблон идентификатора требования: --id-pattern RE или переменная REQ_ID_PATTERN
(chain.env). Если шаблон не задан или не заполнен, проверка останавливается (запрет по умолчанию).

Код выхода 1 при любой ошибке (ERROR). У `intent` свои коды: 0 — план и факт совпадают, 1 — расхождения или
отставание ссылок, 2 — ошибка входа, 3 — сверка неполна. Клоны компонентов берутся из файла привязки
COMPONENT_BINDING, клоны только читаются.

`input` печатает вход разбора критика (Том IV, §5.3): саму SDD, sources каждого требования, намерение из строки `Намерение:` и контракты
из ссылок `product-kb:contracts/<путь>@<коммит>`. У намерения и контракта `pinned` — объект Git файла в коммите
ссылки, `head` — в коммите HEAD клона или null, если файла там нет. Маршрутизатор пишет из этого объекта запись
critic.input.json и с ним же сверяет вход. Непрочитанный документ или неверная ссылка — код 1.
"""
import ast
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True  # route.py импортируется без __pycache__ в .claude/chain
sys.path.insert(0, str(Path(__file__).resolve().parent))
from route import load_env  # один загрузчик chain.env с маршрутизатором (M-13)
from admission import safe_path, text_lines, section, norm, is_subsection, requirement_blocks, block_id
from admission import SECTION, REQ_HDR, SCN_HDR, ID_LINE, FENCE

ROOT = Path(__file__).resolve().parents[2]


def openspec_dir():
    """Каталог спецификаций-фактов проекта (CHAIN-CONFIG-1): OPENSPEC_DIR из окружения, иначе из chain.env
    (chain_env_value), иначе openspec. Неисполнимый chain.env — код 2."""
    if "OPENSPEC_DIR" in os.environ:
        return os.environ["OPENSPEC_DIR"]
    try:
        value = chain_env_value("OPENSPEC_DIR")
    except KbError as e:
        print(e, file=sys.stderr)
        sys.exit(2)
    return "openspec" if not value or "{{" in value else value


# --- знание продукта и карта компонентов (SDD 0002) ------------------------------------------------------
# Читается только Git-объектами клона: cat-file, show, ls-tree, rev-parse. Рабочее дерево, индекс и ссылки
# клона не меняются (Стандарт 18). Формат карты, декомпозиции и строки «Намерение:» — книга, Приложение I.
# Поиск файла намерения по первой строке — профиль кита, а не норма книги.

class KbError(Exception):
    """Вход знания продукта не прочитан: сообщение называет, что именно."""


def chain_env_value(name):
    """Переменная name из окружения, иначе из .claude/chain/chain.env, исполненного загрузчиком маршрутизатора
    load_env (CHAIN-CONFIG-1). Процесс стадии получает только экспортированные переменные chain.env, поэтому
    инструмент исполняет файл сам. Нет файла или переменной — None, неисполнимый файл — KbError."""
    if name in os.environ:
        return os.environ[name]
    env = ROOT / ".claude" / "chain" / "chain.env"
    if not env.is_file():
        return None
    try:
        return load_env(str(env), dict(os.environ))[0].get(name)
    except (OSError, RuntimeError) as e:
        raise KbError(f".claude/chain/chain.env не исполняется: {e}")


def kb_dir():
    """Путь к клону product-kb: PRODUCT_KB_PATH из окружения, иначе из chain.env. Относительный путь — от корня
    проекта. Значение «—», пустое, незаполненный слот или путь не к клону — KbError с именем переменной."""
    value = chain_env_value("PRODUCT_KB_PATH")
    if value is None or not value.strip() or value.strip() == "—" or "{{" in value:
        raise KbError(f"PRODUCT_KB_PATH не задан ({value!r}): проверка вверх читает клон product-kb")
    path = Path(value)
    if not path.is_absolute():
        path = ROOT / path
    path = path.resolve()
    top = git_kb(path, "rev-parse", "--show-toplevel", check=False) if path.is_dir() else None
    if top is None or Path(top.decode("utf-8", "surrogateescape").strip()).resolve() != path:
        raise KbError(f"PRODUCT_KB_PATH={value}: это не клон product-kb")
    return path


def git_kb(kb, *args, check=True):
    """Вывод git в клоне kb (байты) или None при ошибке, если check=False."""
    env = dict(os.environ, GIT_OPTIONAL_LOCKS="0")
    r = subprocess.run(["git", "-C", str(kb), *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    if r.returncode != 0:
        if check:
            raise KbError(f"git {' '.join(args)}: {r.stderr.decode('utf-8', 'replace').strip()}")
        return None
    return r.stdout


_COMMITS = {}


def kb_commit(kb, commit):
    """Полный SHA коммита commit клона kb, один раз на вызов инструмента (M-19). Нет такого коммита — KbError."""
    key = (str(kb), commit)
    if key not in _COMMITS:
        out = git_kb(kb, "rev-parse", "--verify", "--quiet", f"{commit}^{{commit}}", check=False)
        if not out:
            raise KbError(f"коммита {commit} нет в клоне product-kb")
        _COMMITS[key] = out.decode().strip()
    return _COMMITS[key]


def kb_read(kb, commit, path):
    """Содержимое файла path в редакции commit или None, если файла нет. Неизвестный коммит — KbError."""
    sha = kb_commit(kb, commit)
    out = git_kb(kb, "cat-file", "blob", f"{sha}:{path}", check=False)
    return None if out is None else out.decode("utf-8", "replace")


def kb_files(kb, commit, prefix):
    """Пути файлов в редакции commit под каталогом prefix."""
    out = git_kb(kb, "ls-tree", "-r", "-z", "--name-only", kb_commit(kb, commit), "--", prefix)
    return [p.decode("utf-8", "surrogateescape") for p in out.split(b"\0") if p]


FM_SCALAR = re.compile(r"^(id|level):\s*([^\s\"'#]+)\s*(?:#.*)?$")
FM_LINKS = re.compile(r"^links:\s*(?:#.*)?$")
FM_SOURCE = re.compile(r"^  source:\s*\[([^\]\"'#]*)\]\s*(?:#.*)?$")
FM_SOURCE_ERROR = "links.source записан не в форме `source: [A, B]` без кавычек (книга, Приложение I)"


def front_matter(text):
    """Заголовок YAML файла требования в форме книги (Приложение I): `id:` и `level:` одним словом, `links:`
    с `  source: [A, B]`. links.source — только строка с отступом ровно в два пробела, глубже или иначе
    отступленный `source:` им не считается. Иная запись links.source и повторный `source:` не разгадываются:
    поле error называет формат (M-19)."""
    lines = text.replace("\r\n", "\n").split("\n")
    if not lines or lines[0].strip() != "---":
        return {}
    out, links, seen = {"source": []}, False, False
    for ln in lines[1:]:
        if ln.strip() == "---":
            break
        if not ln.strip() or ln.lstrip().startswith("#"):
            continue
        if not ln[0].isspace():
            links = bool(FM_LINKS.match(ln))
            m = FM_SCALAR.match(ln)
            if m:
                out[m.group(1)] = m.group(2)
            continue
        if links and ln.startswith("  source:"):
            m = FM_SOURCE.match(ln)
            if m and not seen:
                out["source"] = [v.strip() for v in m.group(1).split(",") if v.strip()]
            else:
                out["source"], out["error"] = [], FM_SOURCE_ERROR
            seen = True
    return out


_REQUIREMENTS = {}


def requirement_index(kb, sha):
    """`ID` → файлы requirements/** коммита sha: строится один раз на коммит (M-19)."""
    key = (str(kb), sha)
    if key not in _REQUIREMENTS:
        index = {}
        for p in kb_files(kb, sha, "requirements"):
            if p.endswith(".md"):
                index.setdefault(front_matter(kb_read(kb, sha, p) or "").get("id"), []).append(p)
        _REQUIREMENTS[key] = index
    return _REQUIREMENTS[key]


def kb_find_requirement(kb, commit, ident):
    """Файл в requirements/**, чей заголовок YAML несёт id: ident. Ровно один, иначе KbError."""
    hits = requirement_index(kb, kb_commit(kb, commit)).get(ident, [])
    if len(hits) != 1:
        raise KbError(f"требование {ident} в редакции {commit}: найдено файлов requirements/** с id: {ident} — {len(hits)}")
    return hits[0]


def kb_find_intent(kb, commit, ident):
    """Файл intents/*.md, чья первая строка начинается с «# ident ». Ровно один, иначе KbError."""
    hits = []
    for p in kb_files(kb, commit, "intents"):
        if p.count("/") == 1 and p.endswith(".md"):
            first = (kb_read(kb, commit, p) or "").split("\n", 1)[0]
            if first.startswith(f"# {ident} "):
                hits.append(p)
    if len(hits) != 1:
        raise KbError(f"намерение {ident} в редакции {commit}: найдено файлов intents/*.md с первой строкой "
                      f"«# {ident} » — {len(hits)}")
    return hits[0]


INTENT_FIELDS = ("Цель", "Ограничения", "Критерии успеха", "Критерии отмены", "Ожидаемый бизнес-эффект")
META_FIELDS = ("Owner", "Version", "Review Date", "Examples", "Machine Readability", "Cross References")


def metadata_errors(text):
    lines = text_lines(text)
    first_section = next((i for i, line in enumerate(lines) if SECTION.match(line)), len(lines))
    values = {}
    for name in META_FIELDS:
        found = [m.group(1).strip() for line in lines[:first_section]
                 if (m := re.match(re.escape(name) + r":\s*(.*?)\s*$", line))]
        if name == "Owner" and not found:
            owner = section(lines, "Owner")
            found = ["\n".join(owner).strip()] if owner else []
        if len(found) != 1 or not found[0] or PLACEHOLDER.search(found[0]):
            return [f"метаданные: нужно одно заполненное поле {name}"]
        values[name] = found[0]
    try:
        if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", values["Review Date"]):
            raise ValueError()
        age = (datetime.date.today() - datetime.date.fromisoformat(values["Review Date"])).days
        if not 0 <= age <= 90:
            raise ValueError()
    except ValueError:
        return ["Review Date: нужна прошедшая дата YYYY-MM-DD не старше 90 дней (Приложение C, Стандарт 7)"]
    return []


def full_intent_errors(text):
    lines = text_lines(text)
    errors = metadata_errors(text)
    for name in INTENT_FIELDS:
        body = section(lines, name)
        if not body or not any(line.strip() for line in body):
            errors.append(f"полный INT: не заполнено поле {name}")
    return errors


def kb_source_path(kb, commit, ident):
    """ID не должен быть неоднозначным между INT и прежними requirements/**."""
    sha = kb_commit(kb, commit)
    hits = list(requirement_index(kb, sha).get(ident, []))
    hits += [p for p in kb_files(kb, sha, "intents") if p.count("/") == 1 and p.endswith(".md")
             and (kb_read(kb, sha, p) or "").split("\n", 1)[0].startswith(f"# {ident} ")]
    if len(hits) != 1:
        raise KbError(f"источник {ident}@{commit}: найдено документов {len(hits)}, нужен один")
    return hits[0]


def fragment_addresses(text):
    addresses, counts = [], {}
    for line in text_lines(text):
        m = ID_LINE.match(line)
        if m:
            addresses.append(m.group(1))
        m = re.match(r"^#{1,6}\s+(.+?)\s*#*\s*$", line)
        if m:
            title = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", m.group(1))
            base = re.sub(r"[^\w\s-]", "", title.lower())
            base = re.sub(r"\s+", "-", base.strip())
            n = counts.get(base, 0)
            counts[base] = n + 1
            addresses.append(base + (f"-{n}" if n else ""))
    return addresses


def resolve_source(sdd_path, ref):
    m = re.fullmatch(r"(repo|product-kb):([^@#\s]+)@([0-9a-fA-F]{7,40})#([^\s#]+)", ref)
    if not m:
        raise KbError(f"источник {ref}: ожидается repo|product-kb:<документ>@<commit>#<фрагмент>")
    origin, ident, commit, fragment = m.groups()
    if origin == "repo":
        top = git_kb(Path(sdd_path).resolve().parent, "rev-parse", "--show-toplevel", check=False)
        if not top:
            raise KbError("источник repo: SDD должна находиться в Git-репозитории компонента")
        kb, path = Path(top.decode().strip()), ident
    else:
        kb = kb_dir()
        path = ident if "/" in ident or ident.endswith(".md") else kb_source_path(kb, commit, ident)
    if not safe_path(path) or not path.endswith(".md"):
        raise KbError(f"источник {ref}: нужен относительный путь Markdown без выхода из репозитория")
    sha = kb_commit(kb, commit)
    text = kb_read(kb, sha, path)
    if text is None:
        raise KbError(f"источник {ref}: документа нет в закреплённой редакции")
    if fragment_addresses(text).count(fragment) != 1:
        raise KbError(f"источник {ref}: фрагмент не найден или неоднозначен")
    pinned = git_kb(kb, "rev-parse", f"{sha}:{path}", check=False)
    head = git_kb(kb, "rev-parse", f"HEAD:{path}", check=False)
    return {"ref": ref, "path": path, "pinned": pinned.decode().strip(),
            "head": head.decode().strip() if head else None}


def requirement_sources(sdd_path, criteria):
    out, errors, seen = [], [], set()
    for name, start, end in requirement_blocks(criteria):
        body = criteria[start + 1:end]
        first_scenario = next((i for i, line in enumerate(body) if SCN_HDR.match(line)), len(body))
        source_lines = [line for line in body[:first_scenario] if re.match(r"^\s*(?:[-*]\s+)?Источник:", line)]
        if not source_lines:
            errors.append(f"у требования '{name}' нет Источник: рядом с ID до первого Scenario")
        for line in source_lines:
            m = SOURCE_LINE.match(line)
            if not m:
                errors.append(f"у требования '{name}' неверная строка источника: {line}")
                continue
            ref = f"{m.group(1)}@{m.group(2)}"
            if ref in seen:
                continue
            seen.add(ref)
            try:
                out.append(resolve_source(sdd_path, ref))
            except KbError as e:
                errors.append(f"у требования '{name}': {e}")
    return out, errors


MAP_KEYS = ("name", "repo", "path", "spec_dir")
MAP_SCALAR = re.compile(r"""^[^\s"'#&*!|>\[\]{}%@`,][^\s"'#]*$""")


def read_map(text):
    """Карта компонентов (Приложение I): ([{name, repo|path, spec_dir}], [ошибки]). Подмножество YAML: ключ
    components и список отображений из полей name, repo или path, spec_dir со скалярными значениями."""
    comps, errors, cur, item_indent = [], [], None, None
    lines = text.replace("\r\n", "\n").split("\n")
    seen_root = False
    for n, ln in enumerate(lines, 1):
        body = re.sub(r"\s+#.*$", "", ln).rstrip()
        if not body.strip() or body.lstrip().startswith("#"):
            continue
        if not seen_root:
            if body != "components:":
                return [], [f"components.yaml, строка {n}: ввод вне подмножества YAML карты — ожидается «components:»"]
            seen_root = True
            continue
        m = re.match(r"^(\s+)-\s+([a-z_]+):\s*(.*)$", body)
        if m and (item_indent is None or len(m.group(1)) == item_indent):
            item_indent = len(m.group(1))
            cur = {}
            comps.append(cur)
            key, val = m.group(2), m.group(3)
        else:
            m = re.match(r"^(\s+)([a-z_]+):\s*(.*)$", body)
            if not m or cur is None or item_indent is None or len(m.group(1)) != item_indent + 2:
                return [], [f"components.yaml, строка {n}: ввод вне подмножества YAML карты: {ln.strip()}"]
            key, val = m.group(2), m.group(3)
        if key not in MAP_KEYS:
            errors.append(f"components.yaml, строка {n}: неизвестное поле {key}")
            continue
        # Значение — одно слово без кавычек и «#», без «:» в конце (SDD 0002, Constraints).
        if not MAP_SCALAR.match(val) or val.endswith(":"):
            return [], [f"components.yaml, строка {n}: ввод вне подмножества YAML карты — значение {key}: {val!r}"]
        if key in cur:
            errors.append(f"components.yaml, строка {n}: поле {key} повторено")
        cur[key] = val
    if not seen_root:
        return [], ["components.yaml: ввод вне подмножества YAML карты — нет «components:»"]
    if not comps:
        return [], ["components.yaml: нет списка компонентов после «components:» — ввод вне подмножества YAML карты"]
    names = {}
    for i, c in enumerate(comps, 1):
        who = c.get("name", f"№{i}")
        if "name" not in c:
            errors.append(f"components.yaml: у компонента №{i} нет name")
        if "spec_dir" not in c:
            errors.append(f"components.yaml: у компонента {who} нет spec_dir")
        if ("repo" in c) == ("path" in c):
            errors.append(f"components.yaml: у компонента {who} должно быть ровно одно из repo и path")
        if "name" in c:
            if c["name"] in names:
                errors.append(f"components.yaml: дубликат имени компонента {c['name']}")
            names[c["name"]] = c
    return comps, errors


# Формы связи в колонке «Зависит от» (книга, Приложение I; SDD 0003). Разделитель нескольких связей «;» — профиль кита.
DEPENDENCY_FORMS = (
    ("технически", re.compile(r"^технически:\s+(стык «[^«»]+»)$")),
    ("технически", re.compile(r"^технически:\s+([^\s/]+/\d+)$")),
    ("по усилию", re.compile(r"^по усилию:\s+([^\s/]+/\d+)$")),
)
FORMS_HINT = "«технически: стык «<имя>»», «технически: <компонент>/<ID SDD>» или «по усилию: <компонент>/<ID SDD>»"


def read_dependencies(cell):
    """Ячейка «Зависит от»: ([(вид, цель)], [ошибки]). «—» или пусто — связей нет."""
    cell = cell.strip()
    if cell in ("", "—"):
        return [], []
    links, errors = [], []
    for part in (x.strip() for x in cell.split(";")):
        if not part:
            errors.append("пустая связь между «;» или после последней «;»")
        elif part == "—":
            errors.append("«—» вместе с другими связями")
        else:
            m = next(((kind, rx.match(part)) for kind, rx in DEPENDENCY_FORMS if rx.match(part)), None)
            if m is None:
                errors.append(f"связь «{part}» не по форме: ожидается {FORMS_HINT}")
            elif (m[0], m[1].group(1)) in links:
                errors.append(f"связь «{part}» повторена")
            else:
                links.append((m[0], m[1].group(1)))
    return links, errors


def dependency_errors(rows):
    """Ошибки связей строк SDD: [(строка, пара, [(вид, цель)])] -> [ошибки]. Цель-пара должна быть в декомпозиции
    и не совпадать со своей строкой, технические связи между SDD не образуют цикл."""
    pairs = {f"{c}/{n}" for _, (c, n), _ in rows}
    errors, edges = [], {}
    for row, (c, n), links in rows:
        me = f"{c}/{n}"
        for kind, target in links:
            if target.startswith("стык «"):
                continue
            if target == me:
                errors.append(f"строка декомпозиции «{row}»: связь строки на себя ({kind}: {target})")
            elif target not in pairs:
                errors.append(f"строка декомпозиции «{row}»: связь на пару {target}, которой нет в декомпозиции")
            elif kind == "технически":
                edges.setdefault(me, []).append(target)
    state, cycles = {}, []

    def walk(node, path):
        state[node] = 1
        for nxt in edges.get(node, []):
            if state.get(nxt) == 1:
                cycles.append(path[path.index(nxt):] + [nxt])
            elif nxt not in state:
                walk(nxt, path + [nxt])
        state[node] = 2

    for node in sorted(edges):
        if node not in state:
            walk(node, [node])
    errors += [f"цикл технических связей между SDD: {' -> '.join(c)}" for c in cycles]
    return errors


def read_decomposition(text, cmap):
    """Пары SDD «компонент, ID SDD» из таблицы раздела «## Декомпозиция»: ([(компонент, ID)], [ошибки]).
    В пары попадают только строки класса SDD, ID — строка с начальными нулями. Строки других классов не сверяются.
    Если в таблице есть колонка «Зависит от», связи строк SDD проверяются по виду и графу (SDD 0003)."""
    names = {c.get("name") for c in cmap}
    sec = section(text.replace("\r\n", "\n").split("\n"), "Декомпозиция")
    if sec is None:
        return [], ["в намерении нет раздела «## Декомпозиция»"]
    rows = [ln.strip() for ln in sec if ln.strip().startswith("|")]
    if len(rows) < 2:
        return [], ["в разделе «## Декомпозиция» нет таблицы"]
    cells = lambda r: [c.strip().strip("`").strip() for c in r.strip("|").split("|")]
    head = cells(rows[0])
    missing = [c for c in ("Класс", "Компонент", "ID SDD") if c not in head]
    if missing:
        return [], [f"в таблице декомпозиции нет колонок: {', '.join(missing)}"]
    k, c, i = head.index("Класс"), head.index("Компонент"), head.index("ID SDD")
    # Колонка связей узнаётся по началу имени: «Зависит от» книги и «Зависит от (вид зависимости)» шаблона (ревью r1 B1).
    d = next((j for j, h in enumerate(head) if h.startswith("Зависит от")), None)
    pairs, errors, deps = [], [], []
    for row in rows[2:]:
        r = cells(row)
        if len(r) <= max(k, c, i) or r[k] != "SDD":
            continue
        comp, num = r[c], r[i]
        if not comp or not num or comp == "—" or num == "—":
            errors.append(f"строка декомпозиции «{row}»: у SDD нет компонента или ID")
        elif comp not in names:
            errors.append(f"строка декомпозиции «{row}»: компонента {comp} нет в карте")
        elif (comp, num) in pairs:
            errors.append(f"строка декомпозиции «{row}»: дубликат пары {comp}/{num}")
        else:
            pairs.append((comp, num))
            if d is not None:
                links, link_errors = read_dependencies(r[d] if len(r) > d else "")
                errors += [f"строка декомпозиции «{row}»: колонка «Зависит от»: {e}" for e in link_errors]
                deps.append((row, (comp, num), links))
    return pairs, errors + dependency_errors(deps)


INTENT_REF = re.compile(r"^product-kb:([^@\s]+)@([^@\s]+)$")
SDD_NUMBER = re.compile(r"^(\d+)-")


def check_chain(kb, ident, commit):
    """Все ветки links.source от ident в редакции commit доходят до level: business. Ошибки — списком."""
    errors, done = [], set()

    def walk(rid, stack):
        if rid in stack:
            errors.append(f"цикл в links.source редакции {commit}: {' → '.join(stack[stack.index(rid):] + [rid])}")
            return
        if rid in done:
            return
        try:
            path = kb_source_path(kb, commit, rid)
            text = kb_read(kb, commit, path) or ""
            if path.startswith("intents/"):
                errors.extend(full_intent_errors(text))
                done.add(rid)
                return
            req = front_matter(text)
        except KbError as e:
            way = " → ".join(stack + [rid])
            errors.append(f"цепочка {way} оборвана: {e}")
            return
        if req.get("error"):
            errors.append(f"цепочка {' → '.join(stack + [rid])} оборвана: у {rid} {req['error']}")
            return
        if req.get("level") == "business":
            done.add(rid)
            return
        if not req.get("source"):
            errors.append(f"у требования {rid} в редакции {commit} нет links.source до бизнес-уровня "
                          f"(цепочка {' → '.join(stack + [rid])})")
            return
        for src in req["source"]:
            walk(src, stack + [rid])
        done.add(rid)

    walk(ident, [])
    return errors


def check_upward(sdd_path, lead):
    """Проверка вверх SDD со строкой «Намерение:» (SDD 0002, CMAP-3, CMAP-4). Намерение, его декомпозиция и карта
    читаются одним снимком — коммитом из строки «Намерение:»; текущая карта не читается."""
    errors = []
    refs = [INTENT_LINE.match(ln).group(1) for ln in lead if INTENT_LINE.match(ln)]
    refs = [r for r in refs if not PLACEHOLDER.search(r)]
    if not refs:
        return errors
    comp = next((COMPONENT_LINE.match(ln).group(1) for ln in lead if COMPONENT_LINE.match(ln)), None)
    num = SDD_NUMBER.match(Path(sdd_path).name)
    bad = [r for r in refs if not INTENT_REF.match(r)]
    for r in bad:
        errors.append(f"{sdd_path}: строка «Намерение: {r}» не в формате product-kb:<ID>@<коммит>")
    if bad:
        return errors
    try:
        kb = kb_dir()
    except KbError as e:
        return [f"{sdd_path}: {e}"]
    if not num:
        errors.append(f"{sdd_path}: имя файла SDD не начинается с номера <NN>-, ID SDD не получить")
    for ref in refs:
        ident, commit = INTENT_REF.match(ref).groups()
        try:
            sha = kb_commit(kb, commit)
            intent = kb_read(kb, sha, kb_find_intent(kb, sha, ident))
            text = kb_read(kb, sha, "components.yaml")
            if text is None:
                raise KbError(f"в редакции {commit} нет components.yaml")
        except KbError as e:
            errors.append(f"{sdd_path}: {e}")
            continue
        cmap, map_errors = read_map(text)
        errors += [f"{sdd_path}: редакция {commit}: {e}" for e in map_errors]
        errors += [f"{sdd_path}: {ident}: {e}" for e in full_intent_errors(intent)]
        has_decomposition = section(text_lines(intent), "Декомпозиция") is not None
        pairs, dec_errors = read_decomposition(intent, cmap) if has_decomposition else ([], [])
        errors += [f"{sdd_path}: {ident}@{commit}: {e}" for e in dec_errors]
        if map_errors or dec_errors or not comp or not num:
            continue
        if comp not in {c.get("name") for c in cmap}:
            errors.append(f"{sdd_path}: компонента {comp} нет в карте редакции {commit}")
        elif has_decomposition and (comp, num.group(1)) not in pairs:
            errors.append(f"{sdd_path}: пара {comp}/{num.group(1)} не названа в декомпозиции {ident} "
                          f"в редакции {commit}")
    sources = [SOURCE_LINE.match(ln) for ln in lead if SOURCE_LINE.match(ln)]
    kb_sources = [(m.group(1)[len("product-kb:"):], m.group(2).split("#", 1)[0])
                  for m in sources if m.group(1).startswith("product-kb:")]
    if not kb_sources:
        errors.append(f"{sdd_path}: у SDD со строкой «Намерение:» нет ни одного источника product-kb")
    for ident, commit in kb_sources:
        try:
            kb_commit(kb, commit)
        except KbError as e:
            errors.append(f"{sdd_path}: источник {ident}@{commit}: {e}")
            continue
        if "/" in ident or ident.endswith(".md"):
            if not safe_path(ident) or kb_read(kb, commit, ident) is None:
                errors.append(f"{sdd_path}: источник {ident}@{commit} не читается")
        else:
            errors += [f"{sdd_path}: источник {ident}@{commit}: {e}" for e in check_chain(kb, ident, commit)]
    return errors


CONTRACT_PREFIX = "product-kb:contracts/"
CONTRACT_REF = re.compile(r"product-kb:contracts/([A-Za-z0-9._/-]+)@([0-9a-f]{7,40})(?![A-Za-z0-9_])")


def contract_refs(text):
    """Ссылки на контракты в тексте SDD: ([(путь, коммит)], [ошибки]). Каждое вхождение префикса разбирается
    отдельно, повтор той же ссылки сливается, один путь с двумя коммитами — ошибка (SDD 0003, E2)."""
    refs, errors, pos = [], [], text.find(CONTRACT_PREFIX)
    while pos != -1:
        m = CONTRACT_REF.match(text, pos)
        if m is None:
            errors.append(f"вхождение «{text[pos:pos + 60].splitlines()[0]}» не ссылка "
                          f"{CONTRACT_PREFIX}<путь>@<коммит>: в пути латиница, цифры и . _ - /, коммит — 7–40 "
                          "строчных шестнадцатеричных цифр")
        elif (m.group(1), m.group(2)) not in refs:
            other = [c for p, c in refs if p == m.group(1)]
            if other:
                errors.append(f"контракт {CONTRACT_PREFIX}{m.group(1)} указан с двумя редакциями: {other[0]} и {m.group(2)}")
            else:
                refs.append((m.group(1), m.group(2)))
        pos = text.find(CONTRACT_PREFIX, pos + 1)
    return refs, errors


def critic_input(sdd_path):
    """trace.py input: вход разбора критика (SDD 0003, E1) — (объект, [ошибки])."""
    try:
        from workflow_contract import contract_view, native, load_contract
        text = contract_view(sdd_path)
    except (OSError, UnicodeDecodeError, ValueError) as e:
        return None, [f"{sdd_path}: SDD не читается: {e}"]
    lead = []
    for ln in [ln for ln in (section(text_lines(text), "Problem") or []) if ln.strip()]:
        if not (SOURCE_LINE.match(ln) or INTENT_LINE.match(ln) or COMPONENT_LINE.match(ln)):
            break
        lead.append(ln)
    intents = [INTENT_LINE.match(ln).group(1) for ln in lead if INTENT_LINE.match(ln)]
    refs, errors = contract_refs(text)
    if len(intents) > 1:
        errors.append(f"{sdd_path}: строк «Намерение:» {len(intents)}, вход разбора допускает одну")
    if intents and not INTENT_REF.match(intents[0]):
        errors.append(f"{sdd_path}: строка «Намерение: {intents[0]}» не в формате product-kb:<ID>@<коммит>")
    lines = text_lines(text)
    criteria = section(lines, "Acceptance Criteria") or section(lines, "Semantics") or []
    sources, source_errors = requirement_sources(sdd_path, criteria)
    errors += source_errors
    out = {"sdd": str(sdd_path), "intent": None, "contracts": [], "sources": sources}
    if native(sdd_path):
        contract = load_contract(sdd_path)
        out.update(intent=contract["intent"], contract_hash=contract["contract_hash"], line_map=contract["line_map"], files=contract["files"])
    if errors or not (intents or refs):
        return (None if errors else out), errors
    try:
        kb = kb_dir()
    except KbError as e:
        return None, [f"{sdd_path}: {e}"]

    def document(path, commit):
        sha = kb_commit(kb, commit)
        pinned = git_kb(kb, "rev-parse", f"{sha}:{path}", check=False)
        if pinned is None:
            raise KbError(f"файла {path} нет в редакции {commit}")
        head = git_kb(kb, "rev-parse", f"HEAD:{path}", check=False)
        return {"ref": f"{path}@{commit}" if path.startswith("contracts/") else None, "path": path,
                "pinned": pinned.decode().strip(), "head": head.decode().strip() if head else None}

    try:
        if intents:
            ident, commit = INTENT_REF.match(intents[0]).groups()
            out["intent"] = dict(document(kb_find_intent(kb, kb_commit(kb, commit), ident), commit),
                                 ref=f"{ident}@{commit}")
        for path, commit in refs:
            out["contracts"].append(document(f"contracts/{path}", commit))
    except KbError as e:
        return None, [f"{sdd_path}: product-kb: {e}"]
    return out, []


def check_map(component=None):
    """trace.py map [<компонент>]: карта коммита HEAD клона product-kb. Незакоммиченная правка не проверяется."""
    try:
        kb = kb_dir()
        head = kb_commit(kb, "HEAD")
        text = kb_read(kb, head, "components.yaml")
    except KbError as e:
        return [str(e)]
    if text is None:
        return [f"в коммите HEAD {head[:12]} клона product-kb нет components.yaml"]
    work = kb / "components.yaml"
    if not work.is_file() or work.read_text(encoding="utf-8", errors="replace") != text:
        print(f"NOTE components.yaml в рабочем дереве не закоммичен: проверена карта коммита HEAD {head[:12]}")
    cmap, errors = read_map(text)
    if component is not None and not errors and component not in {c.get("name") for c in cmap}:
        errors.append(f"компонента {component} нет в карте коммита HEAD {head[:12]}")
    if not errors:
        print(f"карта коммита HEAD {head[:12]}: компонентов {len(cmap)}")
    return errors


# --- сверка декомпозиции вниз (M-8, INTENT-DECOMP-1) -----------------------------------------------------
# Профиль кита, а не норма книги: файл локальной привязки COMPONENT_BINDING, коды 0/1/2/3, строки отчёта.
# Клоны только читаются: rev-parse, ls-tree, cat-file, log, merge-base.

def repo_path(value):
    """Путь из файла привязки или слота: относительный — от корня проекта."""
    path = Path(value)
    return (path if path.is_absolute() else ROOT / path).resolve()


def branch_commit(repo, branch):
    """SHA основной ветки repo или None: без checkout и fetch, без подстановки HEAD."""
    out = git_kb(repo, "rev-parse", "--verify", "--quiet", f"refs/heads/{branch}^{{commit}}", check=False)
    return out.decode().strip() if out else None


def is_clone(path):
    top = git_kb(path, "rev-parse", "--show-toplevel", check=False) if path.is_dir() else None
    return top is not None and Path(top.decode("utf-8", "surrogateescape").strip()).resolve() == path


def read_binding(text):
    """Файл привязки: ({имя или @kb/@code: поля}, [ошибки]). Строки через табуляцию, `#` — комментарий."""
    entries, errors = {}, []
    for n, ln in enumerate(text.replace("\r\n", "\n").split("\n"), 1):
        if not ln.strip() or ln.lstrip().startswith("#"):
            continue
        fields = ln.split("\t")
        want = 2 if fields[0] == "@kb" else 3
        if len(fields) != want or not all(f.strip() for f in fields):
            errors.append(f"файл привязки, строка {n} не по формату: {ln!r}")
        elif fields[0] in entries:
            errors.append(f"файл привязки, строка {n}: повтор {fields[0]}")
        else:
            entries[fields[0]] = fields[1:]
    return entries, errors


def sdd_intent_refs(text, ident):
    """Коммиты строк «Намерение: product-kb:<ident>@<коммит>» шапки Problem и сообщения о неверных строках на это
    намерение: не по формату или ниже шапки (ревью r1 M-8, B1)."""
    problem = section(text_lines(text), "Problem") or []
    refs, bad, in_head = [], [], True
    for ln in [ln for ln in problem if ln.strip()]:
        in_head = in_head and bool(SERVICE_LINE.match(ln))
        m = re.match(r"^\s*(?:[-*]\s+)?Намерение:\s*`?(.*?)`?\s*$", ln)
        if not m:
            continue
        ref = INTENT_REF.match(m.group(1))
        if not ((ref and ref.group(1) == ident) or ident in re.split(r"[:@\s`]+", m.group(1))):
            continue
        if not in_head:
            bad.append(f"строка «Намерение: {m.group(1)}» ниже шапки Problem: служебные строки идут первыми строками раздела")
        elif ref and ref.group(1) == ident:
            refs.append(ref.group(2))
        else:
            bad.append(f"строка «Намерение: {m.group(1)}» не в формате product-kb:<ID>@<коммит>")
    return refs, bad


class IntentReport:
    """Строки отчёта trace.py intent: прочитанное, ссылки, события, неполнота и ошибки входа."""

    def __init__(self, ident):
        self.ident, self.lines, self.refs, self.events, self.unread, self.errors = ident, [], [], [], [], []

    def result(self):
        """(код, строки). 0 — совпадает, 1 — расхождения, 2 — ошибка входа, 3 — неполна."""
        ident = self.ident
        if self.errors:
            return 2, self.lines + [f"ERROR {e}" for e in self.errors] + [f"intent {ident}: ошибка входа"]
        body = self.lines + self.refs + sorted(self.events) + [f"UNREAD {u}" for u in self.unread]
        if self.unread:
            return 3, body + [f"intent {ident}: сверка неполна"]
        if self.events:
            return 1, body + [f"intent {ident}: расхождений {len(self.events)}"]
        return 0, body + [f"intent {ident}: совпадает"]


def intent_inputs(rep):
    """Клон знания, привязка, карта и план декомпозиции намерения. None, если читать дальше нечего."""
    ident = rep.ident
    try:
        kb = kb_dir()
        slot = chain_env_value("COMPONENT_BINDING")
    except KbError as e:
        rep.unread.append(f"kb {e}")
        return None
    if slot is None or not slot.strip() or slot.strip() == "—" or "{{" in slot:
        rep.unread.append(f"binding слот COMPONENT_BINDING не задан ({slot!r})")
        return None
    if not repo_path(slot).is_file():
        rep.unread.append(f"binding нет файла {slot}")
        return None
    binding, binding_errors = read_binding(repo_path(slot).read_text(encoding="utf-8"))
    rep.errors += binding_errors
    if rep.errors:
        return None
    if "@kb" not in binding:
        rep.unread.append("@kb в файле привязки нет строки @kb")
        return None
    kb_sha = branch_commit(kb, binding["@kb"][0])
    if kb_sha is None:
        rep.unread.append(f"kb нет ветки {binding['@kb'][0]}")
        return None
    rep.lines.append(f"READ kb {kb_sha}")
    text = kb_read(kb, kb_sha, "components.yaml")
    if text is None:
        rep.unread.append(f"map в коммите {kb_sha} нет components.yaml")
        return None
    cmap, map_errors = read_map(text)
    names = [c.get("name") for c in cmap]
    rep.errors += map_errors + [f"файл привязки: компонента {n} нет в карте" for n in binding
                                if not n.startswith("@") and n not in names]
    rep.errors += [f"файл привязки: неизвестная строка {n}" for n in binding
                   if n.startswith("@") and n not in ("@kb", "@code")]
    try:
        intent_path = kb_find_intent(kb, kb_sha, ident)
        pairs, dec_errors = read_decomposition(kb_read(kb, kb_sha, intent_path), cmap)
        rep.errors += [f"{ident}@{kb_sha}: {e}" for e in dec_errors]
    except KbError as e:
        rep.errors.append(str(e))
    if rep.errors:
        return None
    changed = git_kb(kb, "log", "-1", "--format=%H", kb_sha, "--", intent_path).decode().strip()
    return kb, binding, cmap, pairs, changed


def intent_component(rep, comp, binding, pinned):
    """Ссылки `Намерение:` SDD одного компонента: {(компонент, номер): [коммиты]} или None, если не прочитан."""
    name = comp["name"]
    if "repo" in comp:
        if name not in binding:
            rep.unread.append(f"{name} нет привязки в файле привязки")
            return None
        root, branch, prefix = repo_path(binding[name][0]), binding[name][1], ""
    else:
        if "@code" not in binding:
            if "@code в файле привязки нет строки @code" not in rep.unread:
                rep.unread.append("@code в файле привязки нет строки @code")
            rep.unread.append(f"{name} нет строки @code для компонента с path")
            return None
        root, branch, prefix = repo_path(binding["@code"][0]), binding["@code"][1], comp["path"].strip("/") + "/"
    if not is_clone(root):
        rep.unread.append(f"{name} нет клона {root}")
        return None
    # SHA основной ветки фиксируется один раз на вызов: компоненты монорепозитория читаются из одного коммита (B4).
    if (str(root), branch) not in pinned:
        pinned[(str(root), branch)] = branch_commit(root, branch)
    sha = pinned[(str(root), branch)]
    if sha is None:
        rep.unread.append(f"{name} нет ветки {branch} в {root}")
        return None
    rep.lines.append(f"READ {name} {sha}")
    spec_dir = prefix + comp["spec_dir"].strip("/")
    found, own = {}, {}
    try:
        for path in kb_files(root, sha, spec_dir):
            num = SDD_NUMBER.match(path.rsplit("/", 1)[-1])
            if path.rsplit("/", 1)[0] == spec_dir and num and path.endswith(".md"):
                found.setdefault(num.group(1), []).append(path)
        for num, paths in sorted(found.items()):
            if len(paths) > 1:
                rep.errors.append(f"{name}/{num}: несколько файлов SDD с одним номером: {', '.join(paths)}")
                continue
            text = kb_read(root, sha, paths[0])
            if text is None:
                raise KbError(f"файл {paths[0]} в коммите {sha} не прочитан")
            sdd_refs, bad = sdd_intent_refs(text, rep.ident)
            rep.errors += [f"{name}/{num} ({paths[0]}): {b}" for b in bad]
            own[(name, num)] = sdd_refs
    except KbError as e:
        # Непрочитанный компонент — неполнота, а не расхождение (B2).
        rep.unread.append(f"{name} {e}")
        return None
    return own


def intent_compare(rep, kb, pairs, read, facts, changed):
    """План декомпозиции против фактов компонентов: MISSING, NOREF, STALE, EXTRA."""
    ident = rep.ident
    for comp, num in pairs:
        if comp in read and (comp, num) not in facts:
            rep.events.append(f"MISSING {comp}/{num}")
        elif comp in read and not facts[(comp, num)]:
            rep.events.append(f"NOREF {comp}/{num}")
    for (comp, num), sdd_refs in sorted(facts.items()):
        for ref in sdd_refs:
            rep.refs.append(f"REF {comp}/{num} {ident}@{ref}")
            try:
                ref_sha = kb_commit(kb, ref)
                kb_find_intent(kb, ref_sha, ident)
            except KbError as e:
                rep.unread.append(f"ref:{comp}/{num} {e}")
                continue
            if git_kb(kb, "merge-base", "--is-ancestor", changed, ref_sha, check=False) is None:
                rep.events.append(f"STALE {comp}/{num} {ref} {changed}")
        if sdd_refs and (comp, num) not in pairs:
            rep.events.append(f"EXTRA {comp}/{num}")


def intent_report(ident):
    """trace.py intent <ID>: (код, строки отчёта). 0 — совпадает, 1 — расхождения, 2 — ошибка входа, 3 — неполна."""
    rep = IntentReport(ident)
    inputs = intent_inputs(rep)
    if inputs is None:
        return rep.result()
    kb, binding, cmap, pairs, changed = inputs
    facts, read, pinned = {}, set(), {}
    for comp in cmap:
        own = intent_component(rep, comp, binding, pinned)
        if own is not None:
            read.add(comp["name"])
            facts.update(own)
    if rep.errors:
        return rep.result()
    intent_compare(rep, kb, pairs, read, facts, changed)
    return rep.result()


REQ_TYPO = re.compile(r"^###\s*Requirement\b", re.I)
TEST_LINE = re.compile(r"^\s*(?:[-*]\s+)?Test:\s*`?([^`]+?)`?\s*(?:<!--.*)?$")
SOURCE_LINE = re.compile(r"^\s*(?:[-*]\s+)?Источник:\s*`?([^`@\s]+)@([^`\s]+)`?\s*$")
INTENT_LINE = re.compile(r"^\s*(?:[-*]\s+)?Намерение:\s*`?([^`\s]+)`?\s*$")
COMPONENT_LINE = re.compile(r"^\s*(?:[-*]\s+)?Компонент:\s*`?([^`\s]+)`?\s*$")
SERVICE_LINE = re.compile(r"^\s*(?:[-*]\s+)?(?:Источник|Намерение|Компонент):")
PLACEHOLDER = re.compile(r"[<>{}]")


def lines_of(path):
    """Строки файла. Блок кода и комментарий HTML, который открывает строку (`<!--` в её начале) и длится до строки
    с `-->`, дают пустые строки: внутри них заголовки и ограничители не действуют. `<!--` в середине строки — текст
    (SDD 0002, Constraints)."""
    from workflow_contract import contract_view
    return text_lines(contract_view(path))








def block_scenarios(body):
    """Scenarios of a requirement block: [(name, test|None, has_body)]."""
    out, cur = [], None
    for ln in body:
        m = SCN_HDR.match(ln)
        if m and ln.startswith("####"):
            cur = [norm(m.group(1)), None, False]
            out.append(cur)
            continue
        if cur is None:
            continue
        if ln.strip():
            cur[2] = True
        t = TEST_LINE.match(ln)
        if t:
            cur[1] = t.group(1).strip()
    return [tuple(x) for x in out]


def requirements(lines):
    """[(name, id|None, [(scenario, test|None)])] from requirement blocks anywhere in lines."""
    return [(name, block_id(lines[h + 1:e]), [(n, t) for n, t, _ in block_scenarios(lines[h + 1:e])])
            for name, h, e in requirement_blocks(lines)]


def check_sdd(path):
    from workflow_contract import contract_view
    try:
        text = contract_view(path)
    except (ValueError, OSError) as error:
        return [str(error)], []
    errors, lines = [], text_lines(text)
    from workflow_contract import native
    if not native(path):
        errors += [f"{path}: {e}" for e in metadata_errors(text)]
    problem = section(lines, "Problem")
    if problem is None:
        errors.append(f"{path}: нет '## Problem', а её открывает строка источника (Стандарт 3)")
    else:
        head = [ln for ln in problem if ln.strip()]
        # Шапка — служебные строки в начале раздела. Строка с меткой, но не по формату, — ошибка, а не начало
        # текста: иначе неверная строка «Намерение:» выключила бы проверку вверх (ревью r1, CMAP-R1-2).
        lead, rest = [], []
        for i, ln in enumerate(head):
            if SOURCE_LINE.match(ln) or INTENT_LINE.match(ln) or COMPONENT_LINE.match(ln):
                lead.append(ln)
            elif SERVICE_LINE.match(ln):
                errors.append(f"{path}: служебная строка не по формату: «{ln.strip()}»; ожидается "
                              f"'Источник: <id>@<редакция>', 'Намерение: product-kb:<ID>@<коммит>' или 'Компонент: <компонент>'")
            else:
                rest = head[i:]
                break
        for ln in rest:
            if SERVICE_LINE.match(ln):
                errors.append(f"{path}: служебная строка ниже шапки Problem: «{ln.strip()}»; служебные строки "
                              f"идут первыми строками раздела")
        sources = [SOURCE_LINE.match(ln) for ln in lead if SOURCE_LINE.match(ln)]
        if not sources:
            errors.append(f"{path}: '## Problem' не начинается со строки 'Источник: <id>@<редакция>'")
        for m in sources:
            if PLACEHOLDER.search(m.group(0)):
                errors.append(f"{path}: строка источника — незаполненный шаблон: {m.group(0).strip()}")
        for ln in lead:
            m = INTENT_LINE.match(ln)
            if m and PLACEHOLDER.search(m.group(1)):
                errors.append(f"{path}: строка намерения — незаполненный шаблон: {ln.strip()}")
        comps = [COMPONENT_LINE.match(ln) for ln in lead if COMPONENT_LINE.match(ln)]
        if len(comps) != 1:
            errors.append(f"{path}: в '## Problem' должен быть назван ровно один компонент "
                          f"('Компонент: <компонент>', Стандарт 6), найдено {len(comps)}")
        for m in comps:
            if PLACEHOLDER.search(m.group(1)):
                errors.append(f"{path}: строка компонента — незаполненный шаблон: {m.group(0).strip()}")
        if any(INTENT_LINE.match(ln) for ln in lead):
            errors += check_upward(path, lead)
    # Standard 2 field name. SDDs written before the template was aligned with the book use "Semantics".
    criteria = section(lines, "Acceptance Criteria")
    if criteria is None:
        criteria = section(lines, "Semantics")
        if criteria is not None:
            print(f"NOTE {path}: прежнее имя раздела 'Semantics' — переименуйте в 'Acceptance Criteria' (Стандарт 2)")
    for ln in criteria or []:
        if REQ_TYPO.match(ln) and not REQ_HDR.match(ln):
            errors.append(f"{path}: заголовок '{ln.strip()}' похож на требование, но без двоеточия: "
                          f"'### Requirement: <имя>'")
    reqs = requirements(criteria or [])
    errors += [f"{path}: {e}" for e in requirement_sources(path, criteria or [])[1]]
    if not reqs:
        errors.append(f"{path}: в '## Acceptance Criteria' нет '### Requirement:'")
    seen = {}
    for name, rid, scns in reqs:
        if not rid:
            errors.append(f"{path}: у требования '{name}' нет строки 'ID:'")
        elif PLACEHOLDER.search(rid):
            errors.append(f"{path}: у требования '{name}' незаполненный ID: {rid}")
        elif rid in seen:
            errors.append(f"{path}: ID {rid} занят и '{seen[rid]}', и '{name}'")
        else:
            seen[rid] = name
        if not scns:
            errors.append(f"{path}: у требования '{name}' нет сценария")
        for sname, test in scns:
            if not test:
                errors.append(f"{path}: у сценария '{sname}' требования '{name}' нет строки 'Test:'")
    return errors, reqs


def known_ids(sdd_path):
    """IDs of every SDD next to this one, every main spec and every open delta."""
    ids = set()
    files = list(Path(sdd_path).resolve().parent.glob("*.md"))
    openspec = ROOT / openspec_dir()
    if openspec.is_dir():
        files += [p for p in openspec.glob("specs/*/spec.md")]
        files += [p for p in openspec.glob("changes/*/specs/*/spec.md") if "archive" not in p.parts]
    for f in files:
        for _, rid, _ in requirements(lines_of(f)):
            if rid:
                ids.add(rid)
    return ids


def test_texts(test_dir):
    out = {}
    for p in sorted(Path(test_dir).rglob("*")):
        if p.is_file():
            try:
                out[p] = p.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
    return out


def test_exists(test_id, texts):
    """A `Test:` id exists if it occurs literally, all its parts occur on one line
    (Suite.Case written as TEST(Suite, Case)), or it is path::name of a real file.
    path::Class::method (TRACE-TESTS-1): in a Python file the class defines the method, an unparsable
    Python file gives no fact, in another file both names occur."""
    if "::" in test_id:
        path, _, name = test_id.partition("::")
        f = ROOT / path
        if f.is_file() and name:
            text = f.read_text(encoding="utf-8", errors="ignore")
            if "::" in name:
                # Класс names[0], вложенные классы names[1:-1] и функция names[-1] в теле последнего класса.
                names, found = name.split("::"), None
                if f.suffix == ".py":
                    # Неразбираемый файл Python факта не даёт: поиск имён в тексте разбор не подменяет (ревью r1, M9-R1-1).
                    try:
                        body, found = ast.parse(text).body, True
                    except (SyntaxError, ValueError):
                        body, found = [], False
                    for i, part in enumerate(names if found else []):
                        kinds = (ast.FunctionDef, ast.AsyncFunctionDef) if i == len(names) - 1 else (ast.ClassDef,)
                        node = next((n for n in body if isinstance(n, kinds) and n.name == part), None)
                        if node is None:
                            found = False
                            break
                        body = node.body
                if found is None:
                    found = all(re.search(rf"(?<!\w){re.escape(n)}(?!\w)", text) for n in names)
                return found
            if name in text:
                return True
    parts = [p for p in re.split(r"[^\w]+", test_id) if p]
    for text in texts.values():
        if test_id in text:
            return True
        if len(parts) > 1 and any(all(p in ln for p in parts) for ln in text.split("\n")):
            return True
    return False


def check_tests(sdd_path, test_dir, pattern):
    errors, reqs = check_sdd(sdd_path)
    if not pattern or "{{" in pattern:
        return errors + ["REQ_ID_PATTERN не задан: тест-сироту не отличить от текста -> остановка (запрет по умолчанию)"]
    try:
        rx = re.compile(pattern)
    except re.error as e:
        return errors + [f"REQ_ID_PATTERN — неверное регулярное выражение: {e}"]
    if not Path(test_dir).is_dir():
        return errors + [f"{test_dir}: каталога тестов нет"]
    texts = test_texts(test_dir)
    for name, rid, scns in reqs:
        for sname, test in scns:
            if test and test.split()[0] != "manual" and not test_exists(test, texts):
                errors.append(f"план без факта: для 'Test: {test}' ({name} / {sname}) нет теста в {test_dir}")
        if rid and not any(rid in t for t in texts.values()):
            if not all(t and t.split()[0] == "manual" for _, t in scns):
                errors.append(f"на требование {rid} ('{name}') не ссылается ни один тест — к нему не привязан ни один прогон")
    known = known_ids(sdd_path)
    for p, text in texts.items():
        for m in sorted(set(rx.findall(text))):
            tok = m if isinstance(m, str) else m[0]
            if tok not in known:
                errors.append(f"сирота: {p.relative_to(ROOT) if p.is_relative_to(ROOT) else p} ссылается на {tok}, которого нет ни в одной SDD и спецификации-факте")
    return errors


def text_hash(path):
    text = Path(path).read_text(encoding="utf-8").replace("\r\n", "\n")
    norm = "\n".join(ln.rstrip() for ln in text.strip().split("\n"))
    return "sha256:" + hashlib.sha256(norm.encode("utf-8")).hexdigest()[:16]


def report(errors, what):
    for e in errors:
        print(f"ERROR {e}")
    print(f"trace {what}: ошибок {len(errors)}")
    return 0 if not errors else 1


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a or a[0] not in ("sdd", "tests", "hash", "map", "intent", "input"):
        sys.exit(__doc__)
    if a[0] == "input" and len(a) == 2:
        data, errs = critic_input(a[1])
        for e in errs:
            print(f"ERROR {e}", file=sys.stderr)
        if errs:
            sys.exit(1)
        print(json.dumps(data, ensure_ascii=False))
        sys.exit(0)
    if a[0] == "intent" and len(a) == 2:
        code, out = intent_report(a[1])
        print("\n".join(out))
        sys.exit(code)
    if a[0] == "map" and len(a) in (1, 2):
        sys.exit(report(check_map(a[1] if len(a) == 2 else None), "map"))
    if a[0] == "sdd" and len(a) == 2:
        sys.exit(report(check_sdd(a[1])[0], "sdd"))
    if a[0] == "hash" and len(a) == 2:
        print(text_hash(a[1]))
        sys.exit(0)
    if a[0] == "tests" and len(a) >= 3:
        pat = os.environ.get("REQ_ID_PATTERN", "")
        if "--id-pattern" in a:
            i = a.index("--id-pattern")
            pat = a[i + 1] if i + 1 < len(a) else ""
        sys.exit(report(check_tests(a[1], a[2], pat), "tests"))
    sys.exit(__doc__)
