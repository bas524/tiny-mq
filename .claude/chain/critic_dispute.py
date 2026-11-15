#!/usr/bin/env python3
"""Оспаривание вопроса критика (AEF, Том IV, §5.3).

Автор или оркестратор считает, что ответ на вопрос критика уже есть в действующей редакции SDD.
Ссылка сама по себе вопрос не закрывает: она становится попыткой оспаривания, а решает её новый критик
с чистым контекстом в следующем обязательном раунде. Неразрешённое разногласие решает Owner спецификации.
Лимит раундов оспаривание не обнуляет.

  python3 .claude/chain/critic_dispute.py open   <задача> <ID вопроса> <место ответа в SDD>
  python3 .claude/chain/critic_dispute.py decide <задача> <попытка> answered|gap <обоснование>   # решение Owner
  python3 .claude/chain/critic_dispute.py list   <задача>

<задача> — имя каталога задачи внутри HANDOFF_DIR, не путь к SDD. <попытка> — идентификатор вида Q1-a1
из вывода open или list.

open регистрирует попытку в журнале оснастки (chain.log, событие DISPUTE-OPENED) вместе с хешем исходных
полей записи. По журналу маршрутизатор понимает, какие попытки открыты: удалённая или изменённая запись
останавливает цепочку, а поле resolution без события маршрутизатора закрытием не считается. Повторная
попытка по тому же вопросу заводит новую запись и прошлую не стирает.

decide — запись решения человека (HUMAN-DECISION) по попытке, которую критик оставил неразрешённой.
"""
# decide — команда человека, как и возобновление после paused: оснастка не проверяет, кто её запустил.
# Ставится вместе с .claude/ (задача сопровождения M-16). Хеш записи, состояние попытки и журнал — те же функции, что
# у маршрутизатора (задача сопровождения M-15): записи пишет и читает один модуль json, без jq.
import datetime
import glob
import json
import os
import re
import subprocess
import sys
import tempfile
import types
from pathlib import Path

sys.dont_write_bytecode = True  # route.py импортируется без __pycache__ в .claude/chain
sys.path.insert(0, str(Path(__file__).resolve().parent))
from route import _text, dispute_digest, dispute_state, jlog, journal_events, load_env

# The project root: two levels above this file's directory, as the caller passed the path.
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def fail(msg, code=2):
    print(msg, file=sys.stderr)
    sys.exit(code)


def usage():
    print(__doc__.rstrip("\n"))
    sys.exit(2)


def safe_id(value):
    if not re.fullmatch(r"[A-Za-z0-9_-]+", value):
        fail(f"недопустимый идентификатор: '{value}' (только латиница, цифры, _ и -)")


def now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def user():
    r = subprocess.run(["git", "-C", ROOT, "config", "user.name"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    return r.stdout.decode("utf-8", "surrogateescape").rstrip("\n") or os.environ.get("USER") or "?"


def _refuse_constant(name):
    raise ValueError(f"{name} не JSON")


def read_json(path):
    """Parsed JSON or None. NaN, Infinity and a lone surrogate are not JSON here, as in route.read_package."""
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f, parse_constant=_refuse_constant)
        json.dumps(data, ensure_ascii=False).encode("utf-8")
    except (OSError, ValueError):  # UnicodeError is a ValueError
        return None
    return data


def write_json(dst, data):
    """Atomic write of a record; a link at the destination is refused."""
    if os.path.islink(dst):
        print(f"место записи — символическая ссылка: {dst}", file=sys.stderr)
        return False
    tmp = None
    try:
        text = json.dumps(data, ensure_ascii=False, indent=2) + "\n"
        fd, tmp = tempfile.mkstemp(prefix=".tmp.", dir=os.path.dirname(dst))
        with os.fdopen(fd, "w", encoding="utf-8", errors="surrogateescape") as f:
            f.write(text)
        os.replace(tmp, dst)
    except (OSError, ValueError) as e:
        print(f"запись {dst} не создана: {e}", file=sys.stderr)
        if tmp and os.path.exists(tmp):
            os.remove(tmp)
        return False
    return True


def state(journal, spec, attempt):
    """open | answered | gap | unresolved | decided — by the harness journal, as route.py reads it."""
    try:
        with open(journal, encoding="utf-8", errors="surrogateescape", newline="") as f:
            text = f.read()
    except OSError:
        return "open"
    return dispute_state(types.SimpleNamespace(spec=spec), journal_events(text), attempt)


def last_archive(task_dir):
    """The archive of the critic package with the largest N among files named exactly critic-r<N>.json."""
    best, bn = "", ""
    for f in sorted(glob.glob(os.path.join(task_dir, "critic-r*.json"))):
        if not os.path.isfile(f) or os.path.islink(f):
            continue
        n = os.path.basename(f)[len("critic-r"):-len(".json")]
        if not n.isascii() or not n.isdigit():
            continue
        n = n.lstrip("0") or "0"
        if not best or len(n) > len(bn) or (len(n) == len(bn) and n > bn):
            best, bn = f, n
    return best


def cmd_open(spec, task_dir, ddir, journal, qid, loc):
    safe_id(qid)
    # Source package of the round: the fresh critic.json, or the archive of the same round if an earlier
    # dispute of this round already archived it. One archive serves every question of the round.
    fresh = os.path.join(task_dir, "critic.json")
    if os.path.exists(fresh):
        src_path, archive = fresh, True
    else:
        src_path, archive = last_archive(task_dir), False
    if not src_path or not os.path.exists(src_path) or os.path.getsize(src_path) == 0 or os.path.islink(src_path):
        fail(f"нет пакета критика в {task_dir}")
    pkg = read_json(src_path)
    if not isinstance(pkg, dict):
        fail(f"пакет критика не JSON: {src_path}")
    # The fields read here have the router's types (HANDOFF.md): a package the router refuses is not registered.
    questions = pkg.get("questions")
    questions = [] if questions is None else questions
    if not (isinstance(questions, list) and all(isinstance(q, dict) for q in questions)):
        fail(f"questions пакета критика {os.path.basename(src_path)} не массив объектов")
    for key in ("sdd_ref", "spec"):
        if not (pkg.get(key) is None or isinstance(pkg.get(key), str)):
            fail(f"{key} пакета критика {os.path.basename(src_path)} не строка")
    found = [q for q in questions if q.get("id") == qid]
    if len(found) != 1:
        fail(f"в пакете критика {os.path.basename(src_path)} вопрос {qid} встречается {len(found)} раз, "
             "а должен ровно один", 1)
    sdd = _text(pkg, "sdd_ref", "spec")
    sf = sdd if sdd.startswith("/") else f"{ROOT}/{sdd}"
    if not os.path.isfile(sf):
        fail(f"SDD из пакета критика не найдена: {sdd}")
    r = subprocess.run(["git", "-C", ROOT, "hash-object", "--", sf], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    rev = r.stdout.decode().strip()
    if r.returncode != 0 or not rev:
        fail("не вычисляется редакция SDD")
    it = pkg.get("iteration")
    it = 1 if it is None else it  # false is not an integer: refused, as the router refuses it
    if isinstance(it, bool) or not isinstance(it, int):
        fail(f"iteration пакета критика {os.path.basename(src_path)} не целое число: {json.dumps(it, ensure_ascii=False)}")
    by = user()
    if os.path.islink(ddir):
        fail("каталог оспариваний — символическая ссылка")
    try:
        os.makedirs(ddir, exist_ok=True)
    except OSError as e:
        fail(f"каталог оспариваний не создан: {e}")
    n = 1
    while os.path.exists(os.path.join(ddir, f"{qid}-a{n}.json")):
        n += 1
    att = f"{qid}-a{n}"
    # Archive the source only after every check above passed; roll back if the record fails.
    if archive:
        k = 1
        while os.path.exists(os.path.join(task_dir, f"critic-r{k}.json")):
            k += 1
        src = f"critic-r{k}.json"
        try:
            os.rename(fresh, os.path.join(task_dir, src))
        except OSError as e:
            fail(f"пакет критика не перенесён в архив: {e}")
        if os.path.exists(fresh + ".routed"):
            os.rename(fresh + ".routed", os.path.join(task_dir, src + ".routed"))
    else:
        src = os.path.basename(src_path)
    record = {"attempt": att, "question_id": qid, "question": found[0], "critic_package": src,
              "critic_iteration": it, "sdd": sdd, "sdd_rev": rev, "answer_location": loc, "disputed_by": by,
              "opened_at": now()}
    path = os.path.join(ddir, f"{att}.json")
    if not write_json(path, record):
        if archive:
            os.rename(os.path.join(task_dir, src), fresh)
            if os.path.exists(os.path.join(task_dir, src + ".routed")):
                os.rename(os.path.join(task_dir, src + ".routed"), fresh + ".routed")
        fail("запись оспаривания не создана, пакет критика возвращён на место")
    ctx = types.SimpleNamespace(outdir=task_dir)
    jlog(ctx, f"DISPUTE-OPENED spec={spec} attempt={att} question={qid} source={src} "
              f"digest={dispute_digest(ROOT, path)} by={by}")
    print(f"попытка оспаривания {att}: {ddir}/{att}.json. Решает новый критик в следующем раунде; ссылка вопрос не закрывает.")


def cmd_decide(spec, task_dir, ddir, journal, att, dec, why):
    safe_id(att)
    if dec not in ("answered", "gap"):
        fail("решение Owner: answered или gap")
    rec = os.path.join(ddir, f"{att}.json")
    if not os.path.exists(rec) or os.path.getsize(rec) == 0:
        fail(f"нет попытки {att}")
    st = state(journal, spec, att)
    if st != "unresolved":
        fail(f"попытка {att} не ждёт решения Owner (состояние: {st})", 1)
    by = user()
    data = read_json(rec)
    if not isinstance(data, dict):
        fail("решение не записано")
    data["human_decision"] = {"decision": dec, "rationale": why, "by": by, "at": now()}
    if not write_json(rec, data):
        fail("решение не записано")
    jlog(types.SimpleNamespace(outdir=task_dir), f"HUMAN-DECISION dispute={att} decision={dec} by={by}")
    print(f"решение Owner по {att} записано: {dec}")


def _field(rec, key):
    """A record field as the list shows it: a string as is, absent or null as `null`."""
    v = rec.get(key) if isinstance(rec, dict) else None
    if v is None:
        return "null" if isinstance(rec, dict) else ""
    return v.rstrip("\n") if isinstance(v, str) else json.dumps(v, ensure_ascii=False)


def cmd_list(spec, ddir, journal):
    if not os.path.isdir(ddir):
        print("оспариваний нет")
        return
    for f in sorted(glob.glob(os.path.join(ddir, "*-a*.json"))):
        if not os.path.exists(f) or f.endswith(".decided-by.json"):
            continue
        name = os.path.basename(f)
        a = name[:-len(".json")]
        if not re.fullmatch(r"[A-Za-z0-9_-]+", a):
            print(f"{name}: недопустимое имя записи")
            continue
        rec = read_json(f)
        print(f"{a}: вопрос {_field(rec, 'question_id')}, место {_field(rec, 'answer_location')} -> "
              f"{state(journal, spec, a)}")


def main(argv):
    cmd = argv[1] if len(argv) > 1 else ""
    spec = argv[2] if len(argv) > 2 else ""
    if not cmd or not spec:
        usage()
    try:
        cfg, _ = load_env(os.path.join(ROOT, ".claude", "chain", "chain.env"), dict(os.environ))
    except (OSError, RuntimeError) as e:
        fail(f".claude/chain/chain.env не исполняется: {e}")
    safe_id(spec)
    hd = cfg.get("HANDOFF_DIR") or "handoffs"
    if "{{" in hd or hd.startswith("/") or ".." in hd:
        fail(f"HANDOFF_DIR в chain.env не задан или небезопасен: '{hd}'")
    task_dir = f"{ROOT}/{hd}/{spec}"
    if not os.path.isdir(task_dir) or os.path.islink(task_dir):
        fail(f"нет каталога задачи: {task_dir}")
    ddir, journal = f"{task_dir}/disputes", f"{task_dir}/chain.log"
    args = argv[3:]
    if cmd == "open":
        qid, loc = (args + ["", ""])[:2]
        if not qid or not loc:
            usage()
        cmd_open(spec, task_dir, ddir, journal, qid, loc)
    elif cmd == "decide":
        att, dec, why = (args + ["", "", ""])[:3]
        if not att or not dec or not re.sub(r"[ \t\n\r\f\v]", "", why):
            usage()
        cmd_decide(spec, task_dir, ddir, journal, att, dec, why)
    elif cmd == "list":
        cmd_list(spec, ddir, journal)
    else:
        usage()


if __name__ == "__main__":
    main(sys.argv)
