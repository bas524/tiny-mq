#!/usr/bin/env python3
"""Отпечаток рабочего дерева для привязки свидетельства к редакции кода (AEF, Стандарт 15:
«0 отметок „выполнено“ без привязанного свидетельства»).

Стадия, которая прогоняет проверки, снимает отпечаток до проверок и сразу после последнего прогона.
Если они совпали, второй идёт в поле evidence_tree пакета. Маршрутизатор считает отпечаток заново
и сравнивает. Совпадение подтверждает только то, что покрываемое состояние в момент снятия и в момент
маршрутизации одно и то же. Факт прогона и неизменность входов во время проверки оно не доказывает:
их подтверждает собственный прогон проверяющего.

Что покрывается:
  - коммит HEAD: неизменённые отслеживаемые файлы задаёт он;
  - каждый изменённый, удалённый и новый неотслеживаемый файл: тип, режим и содержимое,
    для символической ссылки — её текст, а не цель;
  - отслеживаемые файлы служебных каталогов цепочки (HANDOFF_DIR, REVIEWS_DIR) тоже.
Что не покрывается:
  - неотслеживаемые файлы служебных каталогов: пакеты и логи стадий, иначе запись пакета меняла бы снимок;
  - игнорируемые файлы (.gitignore): сборка и кэши. Использовать их как независимый вход проверки
    без отдельной привязки нельзя;
  - содержимое цели символической ссылки за пределами дерева.
Подмодули не поддерживаются: при их наличии отпечаток не выдаётся, чтобы не выдать неполный.

    python3 .claude/chain/fingerprint.py        # печатает tree:<хеш>, при любой ошибке — код 2
    python3 .claude/chain/fingerprint.py --paths <путь>...   # снимок только этих путей: paths:<хеш>

Режим --paths маршрутизатор использует, чтобы зафиксировать принятое состояние файлов стадии знания:
стадия объяснения не вправе их менять.
"""
import hashlib
import os
import stat
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True  # route.py импортируется без __pycache__ в .claude/chain
sys.path.insert(0, str(Path(__file__).resolve().parent))
from route import load_env  # один загрузчик chain.env с маршрутизатором (M-13)

ROOT = Path(__file__).resolve().parents[2]


def fail(msg):
    print(f"fingerprint: {msg}", file=sys.stderr)
    sys.exit(2)


def chain_env():
    """Переменные chain.env так, как их видит маршрутизатор (CHAIN-CONFIG-2): файл исполняет его загрузчик load_env."""
    p = ROOT / ".claude" / "chain" / "chain.env"
    try:
        return load_env(str(p), dict(os.environ))[0]
    except (OSError, RuntimeError) as e:
        fail(f"{p} не исполняется: {e}")


def service_dir(value, default):
    d = value if value else default
    path = Path(d)
    if "{{" in d or path.is_absolute() or ".." in path.parts or d in ("", "."):
        fail(f"неверный служебный каталог в chain.env: {d!r}")
    return d.rstrip("/") + "/"


def git(*args):
    try:
        return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        err = getattr(e, "stderr", b"") or b""
        fail(f"git {' '.join(args)}: {err.decode('utf-8', 'replace').strip() or e}")


def field(h, value):
    """Length-prefixed field: no two different records encode to the same bytes."""
    if isinstance(value, str):
        value = value.encode("utf-8", "surrogateescape")
    h.update(str(len(value)).encode() + b":" + value + b";")


def entry(h, rel):
    """One path into the hash: its name, then type and mode and content (or link text, or deletion)."""
    path = os.path.join(os.fsencode(ROOT), rel)
    field(h, rel)
    try:
        st = os.lstat(path)
    except FileNotFoundError:
        field(h, b"deleted")
        return
    except OSError as e:
        fail(f"lstat {rel!r}: {e}")
    if stat.S_ISLNK(st.st_mode):
        field(h, b"link")
        field(h, os.readlink(path))
    elif stat.S_ISREG(st.st_mode):
        field(h, b"file")
        field(h, oct(stat.S_IMODE(st.st_mode)))
        try:
            with open(path, "rb") as f:
                field(h, hashlib.sha256(f.read()).hexdigest())
        except OSError as e:
            fail(f"чтение {rel!r}: {e}")
    else:
        fail(f"неподдерживаемый тип пути {rel!r} (каталог, вложенный репозиторий или устройство)")


def paths_snapshot(rels):
    h = hashlib.sha256()
    field(h, b"paths")
    for rel in sorted(set(os.fsencode(r) for r in rels)):
        if rel.startswith(b"/") or b".." in rel.split(b"/"):
            fail(f"путь вне дерева: {rel!r}")
        entry(h, rel)
    print("paths:" + h.hexdigest())


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--paths":
        paths_snapshot(sys.argv[2:])
        return
    env = chain_env()
    hd = service_dir(env.get("HANDOFF_DIR", ""), "handoffs")
    rd = service_dir(env.get("REVIEWS_DIR", ""), "docs/reviews")
    head = git("rev-parse", "--verify", "HEAD").strip()

    for data in (git("ls-files", "--stage", "-z"), git("ls-tree", "-r", "-z", "HEAD")):
        if any(row.startswith(b"160000 ") for row in data.split(b"\0")):
            fail("подмодули пока не поддерживаются: отпечаток был бы неполным")

    tracked_changed = [p for p in git("diff", "--name-only", "--no-renames", "-z", "HEAD").split(b"\0") if p]
    untracked = [p for p in git("ls-files", "--others", "--exclude-standard", "-z").split(b"\0") if p]
    hdb, rdb = hd.encode(), rd.encode()
    untracked = [p for p in untracked if not (p.startswith(hdb) or p.startswith(rdb))]

    h = hashlib.sha256()
    field(h, b"head")
    field(h, head)
    for rel in sorted(set(tracked_changed) | set(untracked)):
        entry(h, rel)
    print("tree:" + h.hexdigest())


if __name__ == "__main__":
    main()
