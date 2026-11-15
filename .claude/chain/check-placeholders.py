#!/usr/bin/env python3
"""Незаполненный слот {{…}} в установленной оснастке — сфабрикованная оснастка (AEF, Стандарт 15):
агент прочитает слот как инструкцию. Запускать в корне проекта после заполнения
PROJECT.md и CALIBRATION.md; выход 1, если слоты остались.

Запуск: .claude/chain/check-placeholders.py [каталог ...]   (по умолчанию .claude и PROJECT.md)
"""
import os
import re
import sys

SLOT = re.compile(rb"\{\{[A-Z0-9_]+\}\}")
SELF = "/.claude/chain/check-placeholders.py"


def files(target):
    """Files of a target as grep -r visits them: depth first in directory order, links not followed."""
    if os.path.islink(target):
        return
    if os.path.isdir(target):
        try:
            entries = list(os.scandir(target))
        except OSError:
            return
        for e in entries:
            yield from files(os.path.join(target, e.name))
    elif os.path.isfile(target):
        yield target


def main(argv):
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    try:
        os.chdir(root)
    except OSError:
        return 2
    hits = []
    for target in argv[1:] or [".claude", "PROJECT.md"]:
        for path in files(target):
            try:
                with open(path, "rb") as f:
                    lines = f.read().split(b"\n")
            except OSError:
                continue
            if any(b"\0" in line for line in lines):  # a binary file: grep reports it in one line
                found = [f"Binary file {path} matches"] if any(SLOT.search(line) for line in lines) else []
            else:
                found = [f"{path}:{n}:{m.group().decode()}" for n, line in enumerate(lines, 1) for m in SLOT.finditer(line)]
            hits += [h for h in found if SELF not in h]
    if not hits:
        print("слотов нет: оснастка заполнена")
        return 0
    print("\n".join(hits))
    print()
    print("разные слоты:")
    counts = {}
    for h in hits:
        slots = SLOT.findall(h.encode())
        slot = slots[-1].decode() if slots else h  # as sed: a line without a slot is counted whole
        counts[slot] = counts.get(slot, 0) + 1
    for line in sorted((f"{c:4d} {s}" for s, c in counts.items()), key=lambda x: (int(x.split()[0]), x), reverse=True):
        print(line)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
