#!/usr/bin/env python3
"""Запуск стадии и запись её завершения в журнал (задача сопровождения M-26, STAGE-EXIT-1).

Маршрутизатор при CHAIN_EXEC=1 запускает эту команду и не ждёт её: хук обязан завершиться быстро. Команда —
наблюдатель. Она запускает стадию отдельным процессом (обёртка модели, флаги, `-p`, текст промпта), ждёт её и пишет
в chain.log задачи (Стандарт 20, Приложение H книги):

  EXIT stage=<стадия> rc=<код> pkg=present|absent
      present — пакет <стадия>.json появился или изменился: его состояние (inode, время изменения, размер, хеш)
      до запуска и после завершения различается;
  INFRA-FAIL stage=<стадия> rc=<код> last=<последняя непустая строка лога стадии>
      только при ненулевом коде. Код 0 без пакета — молчание агента, без INFRA-FAIL.

Стадия — отдельный процесс, поэтому её `exit`, `exec` и сигнал ей запись не отменяют. Завершение по сигналу N
записывается кодом 128+N. Отказ запуска — как в оболочке: 127, если файла обёртки нет, 126 при любом другом отказе
(нет права, неверный формат, промпт не читается). Пакетом считается только обычный файл: каталог на его месте
или файл, который не читается, — pkg=absent. Ошибка чтения состояния пакета запись не срывает, её причина
печатается строкой в лог стадии.
Вне гарантии — стадия, которая намеренно убивает процесс наблюдателя, и сигнал всей группе процессов.

Вывод стадии и сообщения самой команды идут в её стандартный вывод: маршрутизатор направляет его в лог стадии.
Запись делается функцией журнала маршрутизатора под его замком. Если замок не получен за 30 с, запись делается
без него: потерять завершение хуже, чем записать его без очереди.

Запуск: stage_exit.py <каталог задачи> <стадия> <лог стадии> <промпт> -- <обёртка> [<флаги> ...]
"""
import errno
import hashlib
import os
import stat
import subprocess
import sys
import types
from pathlib import Path

sys.dont_write_bytecode = True  # route.py импортируется без __pycache__ в .claude/chain
sys.path.insert(0, str(Path(__file__).resolve().parent))
from route import jlog, with_lock  # тот же журнал и тот же замок, что у маршрутизатора


def state(path):
    """Состояние пакета для сравнения до и после стадии: None — файла нет; ("file", …) — обычный файл;
    ("other", …) — не обычный файл или ошибка чтения (ревью r2, B3). Исключений не бросает."""
    try:
        st = os.stat(path)
        if not stat.S_ISREG(st.st_mode):
            print(f"stage_exit: {path} — не обычный файл, пакетом не считается", flush=True)
            return "other", stat.S_IFMT(st.st_mode)
        with open(path, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
    except FileNotFoundError:
        return None
    except OSError as e:
        print(f"stage_exit: состояние {path} не прочитано: {e}", flush=True)
        return "other", e.errno
    return "file", st.st_ino, st.st_mtime_ns, st.st_size, digest


def last_line(path):
    """Последняя непустая строка файла, без пробелов по краям, или пустая строка."""
    try:
        with open(path, "rb") as f:
            text = f.read().decode("utf-8", "replace")
    except OSError:
        return ""
    return next((ln.strip() for ln in reversed(text.splitlines()) if ln.strip()), "")


def run_stage(command, prompt):
    """Код завершения стадии: её собственный, 128+N по сигналу N. Отказ запуска — 127, если файла обёртки нет,
    иначе 126 (ревью r2, B4); диагностика — строкой в лог стадии."""
    try:
        with open(prompt, encoding="utf-8", errors="surrogateescape") as f:
            text = f.read()
    except OSError as e:
        print(f"stage_exit: промпт не прочитан: {e}", flush=True)
        return 126
    sys.stdout.flush()
    try:
        rc = subprocess.run(command + ["-p", text], stdin=subprocess.DEVNULL).returncode
    except OSError as e:
        print(f"stage_exit: обёртку нельзя запустить: {e}", flush=True)
        return 127 if e.errno == errno.ENOENT else 126
    return 128 - rc if rc < 0 else rc


def main(argv):
    if len(argv) < 7 or argv[5] != "--":
        print(__doc__, file=sys.stderr)
        return 2
    outdir, stage, log, prompt, command = argv[1], argv[2], argv[3], argv[4], argv[6:]
    pkg = os.path.join(outdir, f"{stage}.json")
    before = state(pkg)
    rc = run_stage(command, prompt)
    last = last_line(log)  # до диагностики состояния пакета: последняя строка — стадии или отказа запуска
    after = state(pkg)
    present = after is not None and after[0] == "file" and after != before
    ctx = types.SimpleNamespace(outdir=outdir)
    lock = with_lock(os.path.dirname(os.path.abspath(outdir)), 30)
    try:
        jlog(ctx, f"EXIT stage={stage} rc={rc} pkg={'present' if present else 'absent'}")
        if rc != 0:
            jlog(ctx, f"INFRA-FAIL stage={stage} rc={rc} last={last}")
    finally:
        if lock:
            lock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
