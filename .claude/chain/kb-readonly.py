#!/usr/bin/env python3
"""Закрывает клон репозитория знания продукта на запись (AEF, Стандарты 6 и 18).

Сессия задачи компонента читает знание продукта, но не пишет в него: изменения вносит
задача знания в отдельной копии (.claude/chain/kb-task.py) и отдельным MR. Это предотвращение. Обнаружение — в маршрутизаторе
(событие KB-WRITE): если запись всё же случилась, цепочка останавливается.

  .claude/chain/kb-readonly.py lock   <путь>   # снять право записи со всего клона, включая .git
  .claude/chain/kb-readonly.py update <путь>   # открыть, git pull --ff-only, снова закрыть
  .claude/chain/kb-readonly.py status <путь>   # 0 — закрыт, 1 — в клоне есть файлы с правом записи"""
import os
import stat
import subprocess
import sys


def walk(path):
    """The path and everything below it, depth first in directory order; symbolic links are not followed."""
    yield path
    if os.path.isdir(path) and not os.path.islink(path):
        try:
            entries = list(os.scandir(path))
        except OSError:
            return
        for e in entries:
            yield from walk(os.path.join(path, e.name))


def chmod_tree(path, drop):
    """chmod -R a-w (drop) or u+w; symbolic links are left as they are, as chmod -R does."""
    for p in walk(path):
        try:
            st = os.lstat(p)
            if stat.S_ISLNK(st.st_mode):
                continue
            mode = stat.S_IMODE(st.st_mode)
            os.chmod(p, mode & ~0o222 if drop else mode | stat.S_IWUSR)
        except OSError as e:
            print(f"chmod: {p}: {e.strerror}", file=sys.stderr)


def main(argv):
    cmd = argv[1] if len(argv) > 1 else ""
    kb = argv[2] if len(argv) > 2 else ""
    if not cmd or not kb:
        print(__doc__)
        return 2
    r = subprocess.run(["git", "-C", kb, "rev-parse", "--git-dir"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode != 0:
        print(f"это не клон git: {kb}", file=sys.stderr)
        return 2
    if cmd == "lock":
        chmod_tree(kb, True)
        print(f"закрыт на запись: {kb}")
    elif cmd == "update":
        chmod_tree(kb, False)
        rc = subprocess.run(["git", "-C", kb, "pull", "--ff-only", "-q"]).returncode
        chmod_tree(kb, True)
        if rc != 0:
            print(f"git pull не прошёл, клон снова закрыт на запись: {kb}", file=sys.stderr)
            return rc
        print(f"обновлён и закрыт на запись: {kb}")
    elif cmd == "status":
        writable = next((p for p in walk(kb) if os.lstat(p).st_mode & stat.S_IWUSR), None)
        if writable is None:
            print(f"закрыт на запись: {kb}")
        else:
            print(f"открыт на запись: {writable}", file=sys.stderr)
            return 1
    else:
        print(f"неизвестная команда: {cmd} (lock | update | status)", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
