#!/usr/bin/env python3
"""Дрейф-флаги по отчёту сверки декомпозиции намерения (M-8, INTENT-DECOMP-1).

Запуск:
  python3 .claude/chain/intent-flags.py <файл отчёта trace.py intent> <каталог задач>

Садовник передаёт сюда отчёт `trace.py intent <ID>`. На каждую строку MISSING, NOREF, EXTRA, STALE и UNREAD
скрипт заводит в каталоге задач файл флага. Ключ флага — ID намерения, вид строки и пара «компонент/ID SDD»,
для UNREAD — ID намерения, UNREAD и ID входа. Имя файла — хеш ключа, поэтому текст отчёта на путь не влияет.
Флаг с тем же ключом повторно не заводится: новый отчёт дописывается в его файл, прежний отчёт сохраняется.
Флаг — запись наблюдения и вопрос, а не вердикт о дефекте незавершённой работы.

Скрипт пишет только в каталог задач. Сам каталог и конечный файл флага не могут быть символической ссылкой,
а файл флага, у которого больше одной жёсткой ссылки или который не обычный файл, не дописывается: иначе запись
ушла бы в чужой файл, например в SDD клона. Клонов скрипт не читает.
Код 0 — флаги записаны, 1 — какой-то флаг не записан, 2 — неверный вызов или отчёт.
"""
import hashlib
import os
import re
import stat
import sys

KINDS = ("MISSING", "NOREF", "EXTRA", "STALE", "UNREAD")


def main(argv):
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    report, tasks = argv[1], argv[2]
    if not os.path.isdir(tasks) or os.path.islink(tasks):
        print(f"{tasks}: нет каталога задач", file=sys.stderr)
        return 2
    with open(report, encoding="utf-8") as f:
        text = f.read()
    lines = [ln for ln in text.split("\n") if ln.strip()]
    final = re.match(r"^intent (\S+): ", lines[-1]) if lines else None
    if not final:
        print(f"{report}: последняя строка не «intent <ID>: …» — это не отчёт trace.py intent", file=sys.stderr)
        return 2
    ident = final.group(1)
    digest = hashlib.sha256(text.encode("utf-8")).hexdigest()[:16]
    block = f"\n## Отчёт {digest}\n\n```\n{text.rstrip()}\n```\n"
    rc = 0
    for ln in lines:
        parts = ln.split(" ")
        if parts[0] not in KINDS or len(parts) < 2:
            continue
        key = f"{ident} {parts[0]} {parts[1]}"
        path = os.path.join(tasks, "drift-" + hashlib.sha256(key.encode("utf-8")).hexdigest()[:16] + ".md")
        try:
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                f.write(f"# Дрейф-флаг: {key}\n\nЗапись наблюдения сверки декомпозиции намерения (INTENT-DECOMP-1): "
                        f"вопрос, а не вердикт о дефекте незавершённой работы.\n\nСтрока отчёта: `{ln}`\n" + block)
            continue
        except FileExistsError:
            pass
        except OSError as e:
            print(f"{path}: флаг не записан: {e}", file=sys.stderr)
            rc = 1
            continue
        try:
            fd = os.open(path, os.O_RDWR | os.O_APPEND | os.O_NOFOLLOW)
        except OSError as e:
            print(f"{path}: флаг не дописан, файл не открыть без перехода по ссылке: {e}", file=sys.stderr)
            rc = 1
            continue
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode) or st.st_nlink != 1:
            os.close(fd)
            print(f"{path}: флаг не дописан: не обычный файл или у файла несколько жёстких ссылок", file=sys.stderr)
            rc = 1
            continue
        with os.fdopen(fd, "r+", encoding="utf-8") as f:
            if f"## Отчёт {digest}\n" not in f.read():
                f.write(block)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
