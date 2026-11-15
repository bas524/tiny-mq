#!/usr/bin/env python3
"""Рабочая копия задачи знания (AEF, Том IV, §10.6; Стандарт 18).

Задача знания пишет в репозиторий знания продукта, но не в общий клон из PRODUCT_KB_PATH:
тот закрыт на запись для сессий задач компонента. Для неё заводится отдельная копия,
и запись в ней ограничена целевым набором из файла задачи (templates/knowledge-task.md).

  .claude/chain/kb-task.py open  <источник> <каталог> <файл задачи>   # клонировать, записать набор и базу
  .claude/chain/kb-task.py check <каталог>                            # 0 — правки только в целевом наборе

<источник> — адрес репозитория, bare-репозиторий или закрытый клон с origin. Копия получает
origin источника, а не сам источник: push из копии не попадает в чужое рабочее дерево.
Целевой набор — точные относительные пути файлов. Каталоги и маски отклоняются при open.

check сравнивает копию с базой — коммитом на момент open, поэтому отправка правок
или потеря upstream нарушения не скрывают. check обнаруживает запись вне набора после
неё, но не предотвращает её: это не замена песочницы и прав на запись до запуска."""
import os
import re
import shutil
import stat
import subprocess
import sys

SPECIAL = set(" \t\n'\"\\|&;()<>!{}*[]?^$`,")


def shell_quote(s):
    """The name as bash 3.2 `printf %q` prints it in the C locale: the message format of the former bash command.
    In a UTF-8 locale bash 3.2 splits multibyte characters, so the C locale is the reference."""
    if s == "":
        return "''"
    raw = s.encode("utf-8", "surrogateescape")
    if any(b < 0x20 or b >= 0x7f for b in raw):
        names = {0x07: "a", 0x08: "b", 0x09: "t", 0x0a: "n", 0x0b: "v", 0x0c: "f", 0x0d: "r", 0x1b: "E"}
        out = []
        for b in raw:
            if b in names:
                out.append("\\" + names[b])
            elif b < 0x20 or b >= 0x7f:
                out.append("\\%03o" % b)
            elif chr(b) in "'\\":
                out.append("\\" + chr(b))
            else:
                out.append(chr(b))
        return "$'" + "".join(out) + "'"
    out = []
    for i, c in enumerate(s):
        if c in SPECIAL or (i == 0 and c == "#"):
            out.append("\\" + c)
        else:
            out.append(c)
    return "".join(out)


def git(*args, quiet=False):
    """(returncode, stdout) of git; stderr goes to the terminal unless quiet."""
    r = subprocess.run(["git", *args], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL if quiet else None)
    return r.returncode, r.stdout.decode("utf-8", "surrogateescape")


def fail(msg, code):
    print(msg, file=sys.stderr)
    sys.exit(code)


def usage():
    print(__doc__)
    sys.exit(2)


def read_scope(task):
    """Target set: list items with a path in backquotes in the «Целевой набор» section of the task file."""
    out, inside = [], False
    with open(task, encoding="utf-8", errors="surrogateescape") as f:
        for line in f.read().split("\n"):
            if line.startswith("## Целевой набор"):  # the start rule comes first, as in the awk program
                inside = True
                continue
            if inside and line.startswith("## "):
                break
            if inside:
                m = re.match(r"- `([^`<]+)`", line)
                if m:
                    out.append(re.match(r"- `([^`]+)`", line).group(1))
    return out


def links(d):
    """Symbolic links in the working tree, .git pruned, in find order."""
    found = []

    def rec(p):
        try:
            entries = list(os.scandir(p))
        except OSError:
            return
        for e in entries:
            q = os.path.join(p, e.name)
            if e.name == ".git":
                continue
            if e.is_symlink():
                found.append(q)
            elif e.is_dir(follow_symlinks=False):
                rec(q)
    rec(d)
    return found


def no_links(d):
    """Symbolic links in the working tree lead writes outside the copy."""
    found = links(d)
    if not found:
        return True
    print("небезопасная копия задачи знания: символические ссылки в рабочем дереве:", file=sys.stderr)
    print("\n".join(found), file=sys.stderr)
    return False


def chmod_user_write(path):
    for dirpath, dirnames, filenames in os.walk(path):
        for p in [dirpath] + [os.path.join(dirpath, n) for n in filenames]:
            st = os.lstat(p)
            if not stat.S_ISLNK(st.st_mode):
                os.chmod(p, stat.S_IMODE(st.st_mode) | stat.S_IWUSR)


def cmd_open(src, d, task):
    if not os.path.isfile(task):
        fail(f"нет файла задачи: {task}", 2)
    if os.path.lexists(d):
        fail(f"каталог уже существует: {d}", 2)
    scope = read_scope(task)
    if not scope:
        fail("в задаче пуст раздел «Целевой набор»: без него запись не ограничена (Стандарт 18)", 1)
    for p in scope:
        if (p.startswith("/") or p.startswith("../") or "/../" in p or p.endswith("/..") or p == ".git"
                or p.startswith(".git/") or p.endswith("/") or any(c in p for c in "*?[")):
            fail(f"целевой набор — точные относительные пути файлов, без каталогов и масок: {p}", 1)
    # Where the copy pushes: origin of a local working clone, else the source itself (address or bare
    # repository). A working clone without origin is refused: a push would land in its working tree.
    up = src
    if git("-C", src, "rev-parse", "--git-dir", quiet=True)[0] == 0:
        if git("-C", src, "rev-parse", "--is-bare-repository")[1].strip() != "true":
            up = git("-C", src, "remote", "get-url", "origin", quiet=True)[1].rstrip("\n")
            if not up:
                fail(f"у локального клона {src} нет origin: копии некуда отправлять правки", 1)
            if not (up.startswith("/") or "://" in up or ":" in up):
                try:
                    up = os.path.realpath(os.path.join(src, up))
                    if not os.path.isdir(up):
                        raise OSError
                except OSError:
                    fail(f"origin клона {src} не разрешается", 1)
    # --no-local, --no-hardlinks, --dissociate: the copy owns its object store. Otherwise it would share
    # object files with the source (chmod of the copy would open the locked clone) or borrow via alternates.
    if subprocess.run(["git", "clone", "-q", "--no-local", "--no-hardlinks", "--dissociate", "--", src, d]).returncode:
        sys.exit(1)
    alt = os.path.join(d, ".git", "objects", "info", "alternates")
    if os.path.isfile(alt) and os.path.getsize(alt) > 0:
        fail("копия задачи знания заимствует объекты другого репозитория", 1)
    if git("-C", d, "remote", "set-url", "origin", up)[0]:
        sys.exit(1)
    if not no_links(d):
        sys.exit(1)
    for p in scope:
        if os.path.isdir(os.path.join(d, p)):
            fail(f"в целевом наборе каталог вместо файла: {p}", 1)
    chmod_user_write(d)
    info = os.path.join(d, ".git", "info")
    with open(os.path.join(info, "kb-task-scope"), "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write("\n".join(scope) + "\n")
    rc, head = git("-C", d, "rev-parse", "--verify", "HEAD")
    with open(os.path.join(info, "kb-task-base"), "w") as f:
        f.write(head)
    if rc:
        sys.exit(2)
    shutil.copyfile(task, os.path.join(info, "kb-task.md"))
    print(f"копия задачи знания: {d} (целевой набор: {len(scope)} путей)")


def cmd_check(d):
    gitdir = os.path.join(d, ".git")
    if not os.path.isdir(gitdir) or os.path.islink(gitdir):
        fail(f"это не самостоятельная копия задачи знания: {d}", 2)
    rc, inside = git("-C", d, "rev-parse", "--is-inside-work-tree", quiet=True)
    if rc:
        fail(f"git не читает {d}", 2)
    if inside.strip() != "true":
        sys.exit(2)
    sf, bf = os.path.join(gitdir, "info", "kb-task-scope"), os.path.join(gitdir, "info", "kb-task-base")
    if not (os.path.isfile(sf) and os.path.getsize(sf) and os.path.isfile(bf) and os.path.getsize(bf)):
        fail("нет целевого набора или базы задачи: открой копию заново (.claude/chain/kb-task.py open)", 2)
    with open(bf) as f:
        base = f.read().rstrip("\n")
    if git("-C", d, "rev-parse", "--verify", "-q", f"{base}^{{commit}}", quiet=True)[0]:
        fail(f"база задачи {base} не найдена в копии", 2)
    if not no_links(d):
        sys.exit(1)
    # --no-renames: a rename is the removal of the old path and a new one, both are checked. -z: names come
    # unquoted. Untracked files include ignored ones: .gitignore is not a write permission.
    rc1, changed = git("-C", d, "diff", "--no-ext-diff", "--no-textconv", "--no-renames", "--name-only", "-z", base, "--")
    if rc1:
        sys.exit(2)
    rc2, others = git("-C", d, "ls-files", "--others", "-z")
    if rc2:
        sys.exit(2)
    with open(sf, encoding="utf-8", errors="surrogateescape") as f:
        allowed = set(f.read().split("\n"))
    names = [n for n in (changed + others).split("\0") if n]
    failed = False
    for name in names:
        if name not in allowed:
            print(f"SCOPE-VIOLATION: правка вне целевого набора задачи знания: {shell_quote(name)} (Стандарт 18)",
                  file=sys.stderr)
            failed = True
    if failed:
        sys.exit(1)
    print(f"правки в пределах целевого набора: {len(names)} файлов")


def main(argv):
    cmd = argv[1] if len(argv) > 1 else ""
    args = argv[2:]
    if cmd == "open":
        src, d, task = (args + ["", "", ""])[:3]
        if not src or not d or not task:
            usage()
        cmd_open(src, d, task)
    elif cmd == "check":
        d = args[0] if args else ""
        if not d:
            usage()
        cmd_check(d)
    else:
        usage()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
