#!/usr/bin/env python3
"""Prepare fixed Release pairs and run the local macOS performance gate."""

import argparse
from contextlib import contextmanager
from datetime import datetime, timedelta
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import signal
import subprocess
import sys
import tempfile

from nfr71 import HOT_PATHS, PROFILES as DESTINATION_PROFILES, machine_lock
from nfr72 import DELIVERY_PATHS, PROFILES, registrations

ROOT = Path(__file__).resolve().parents[1]
HISTORICAL_BASE = "230eefe8321f5d9a956a9fb89b09af1dab84cbad"
VCPKG_PIN = "cd61e1e26a038e82d6550a3ebbe0fbbfe7da78e3"
PAIRS = ("sdd72", "m07")
PATHS = set((*HOT_PATHS, *DELIVERY_PATHS))
PROFILE_PATHS = set((*DESTINATION_PROFILES, *PROFILES))
COMMAND_PATH = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"


def query(command, cwd=None):
    return subprocess.check_output(command, cwd=cwd, text=True).strip()


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")


def read_json(path):
    return json.loads(path.read_text())


@contextmanager
def lock(store):
    store.mkdir(mode=0o700, parents=True, exist_ok=True)
    with machine_lock(), (store / "lock").open("a") as stream:
        fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


def historical_overlay(source, output):
    # Same measurement-only normalization used by the former GitHub gate.
    path = source / "tests/BenchmarkTest.cpp"
    content = path.read_text()
    for name in ("AutoAck_Persistent_RoundTrip", "ClientAck_Persistent_RoundTrip",
                 "Transacted_Persistent_RoundTrip", "Transacted_Batch_Persistent"):
        signature = f"BenchmarkFixture, {name}"
        legacy = f"BENCHMARK_F({signature})"
        if legacy in content:
            pattern = rf"^{re.escape(legacy)}(.*?^\}})"
            replacement = (rf"BENCHMARK_DEFINE_F({signature})\1"
                           f"\nBENCHMARK_REGISTER_F({signature})->MeasureProcessCPUTime();")
            content, count = re.subn(pattern, replacement, content, flags=re.M | re.S)
        else:
            pattern = rf"(BENCHMARK_REGISTER_F\({re.escape(signature)}\)[^;]*);"

            def measure(match):
                if "MeasureProcessCPUTime()" in match[1]:
                    return match[0]
                return f"{match[1]}->MeasureProcessCPUTime();"

            content, count = re.subn(pattern, measure, content)
        if count != 1:
            raise ValueError(f"expected one persistent benchmark registration: {name}")
    path.write_text(content)
    with output.open("w") as stream:
        subprocess.run(["git", "diff", "--", "tests/BenchmarkTest.cpp"], cwd=source,
                       check=True, stdout=stream)


def machine():
    return dict(
        os=query(["/usr/bin/sw_vers", "-productVersion"]),
        os_build=query(["/usr/bin/sw_vers", "-buildVersion"]),
        model=query(["/usr/sbin/sysctl", "-n", "hw.model"]),
        arch=platform.machine(),
    )


def build_metadata(store):
    sdk = store / "sdk"
    vcpkg = query(["git", "rev-parse", "HEAD"], cwd=sdk / "vcpkg")
    if vcpkg != VCPKG_PIN:
        raise ValueError(f"unexpected vcpkg commit: {vcpkg}")
    return dict(
        machine=machine(), compiler=query(["/usr/bin/c++", "--version"]),
        compiler_path=query(["/usr/bin/xcrun", "--find", "clang++"]),
        sdk=query(["/usr/bin/xcrun", "--show-sdk-path"]),
        sdk_version=query(["/usr/bin/xcrun", "--show-sdk-version"]),
        cmake=query(["cmake", "--version"]).splitlines()[0],
        vcpkg=vcpkg, packages_sha256=digest(sdk / "installed/vcpkg/status"),
        configuration="RelWithDebInfo",
    )


def build(store, revision, destination, parallel, log):
    sdk = store / "sdk"
    env = dict(os.environ, VCPKG_ROOT=str(sdk / "vcpkg"), CC="/usr/bin/cc", CXX="/usr/bin/c++")
    with tempfile.TemporaryDirectory(prefix="build-", dir=store) as directory:
        source = Path(directory) / "source"
        subprocess.run(["git", "clone", "--shared", "--no-checkout", str(ROOT), str(source)],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run(["git", "checkout", "--detach", revision], cwd=source,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
        if revision == HISTORICAL_BASE:
            historical_overlay(source, destination.with_name("baseline-overlay.diff"))
        for command in (["cmake", "--preset", "release", "-DENABLE_TESTS=ON",
                         "-DVCPKG_MANIFEST_INSTALL=OFF",
                         f"-DVCPKG_INSTALLED_DIR={sdk / 'installed'}"],
                        ["cmake", "--build", "--preset", "release", "--parallel", str(parallel)]):
            subprocess.run(command, cwd=source, env=env, stdout=log,
                           stderr=subprocess.STDOUT, check=True)
        shutil.copy2(source / "cmake-build-releasewithdebuginfo/tiny_mq", destination)
    return digest(destination)


def verify_registrations(baseline, candidate):
    for binary, required in ((baseline, PATHS), (candidate, PATHS | PROFILE_PATHS)):
        missing = required - registrations(binary)
        if missing:
            raise ValueError(f"missing registrations for {binary}: {sorted(missing)}")


def prepare(args):
    revisions = {"baseline": args.baseline, "candidate": args.candidate}
    if args.baseline == args.candidate or not all(re.fullmatch("[0-9a-f]{40}", sha)
                                                  for sha in revisions.values()):
        raise ValueError("two distinct full commit SHAs are required")
    manifests = [query(["git", "rev-parse", f"{sha}:vcpkg.json"], ROOT)
                 for sha in revisions.values()]
    if manifests[0] != manifests[1]:
        raise ValueError("the pair requires different dependency manifests")
    with lock(args.store):
        directory = args.store / "pairs" / args.pair
        if directory.exists():
            raise ValueError(f"pair already prepared: {directory}")
        directory.parent.mkdir(exist_ok=True)
        log_path = args.store / f"build-{args.pair}.log"
        with tempfile.TemporaryDirectory(prefix="prepare-", dir=args.store) as draft:
            prepared = Path(draft) / args.pair
            prepared.mkdir()
            metadata = build_metadata(args.store)
            with log_path.open("w") as log:
                fingerprints = {side: build(args.store, sha, prepared / side, args.parallel, log)
                                for side, sha in revisions.items()}
            verify_registrations(prepared / "baseline", prepared / "candidate")
            overlay = prepared / "baseline-overlay.diff"
            write_json(prepared / "pair.json", dict(
                revisions=revisions, binaries_sha256=fingerprints, dependencies_blob=manifests[0],
                overlay_sha256=digest(overlay) if overlay.exists() else None, **metadata,
            ))
            prepared.rename(directory)
        print(f"prepared {args.pair}: {directory}", flush=True)


def power():
    return query(["/usr/bin/pmset", "-g", "batt"]).splitlines()[0]


def idle_seconds():
    state = query(["/usr/sbin/ioreg", "-r", "-c", "IOHIDSystem", "-d", "1"])
    match = re.search(r'"HIDIdleTime" = (\d+)', state)
    if match is None:
        raise ValueError("HID idle time unavailable")
    return int(match[1]) / 1_000_000_000


def quiet(args, night):
    now = datetime.now().astimezone()
    if not night[0] <= now < night[1]:
        return "outside the approved window"
    if power() != "Now drawing from 'AC Power'":
        return "external power unavailable"
    if args.idle_minutes and idle_seconds() < args.idle_minutes * 60:
        return "recent user activity"
    return None


def active_window(start, end):
    now = datetime.now()
    first_day = now.date()
    spans_midnight = end < start
    if spans_midnight and now.time() < start:
        first_day -= timedelta(days=1)
    last_day = first_day + timedelta(days=spans_midnight)
    return (datetime.combine(first_day, start).astimezone(),
            datetime.combine(last_day, end).astimezone())


def terminate(process):
    if process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()


def measure(args, pair, out, night):
    binaries = args.store / "pairs" / pair
    manifest = read_json(binaries / "pair.json")
    if manifest["machine"] != machine():
        raise ValueError(f"{pair}: host or OS changed, rebuild both revisions")
    for name in ("baseline", "candidate"):
        if digest(binaries / name) != manifest["binaries_sha256"][name]:
            raise ValueError(f"{pair}: prepared {name} binary does not match its fingerprint")
    verify_registrations(binaries / "baseline", binaries / "candidate")
    out.mkdir()
    write_json(out / "pair.json", manifest)
    summary = out / "summary.json"
    command = [sys.executable, "-B", str(ROOT / "tests/nfr72.py"),
               "--baseline", str(binaries / "baseline"), "--candidate", str(binaries / "candidate"),
               "--out", str(out / "raw"), "--summary", str(summary)]
    if args.smoke:
        command.append("--smoke")
    else:
        command = ["/usr/bin/caffeinate", "-is", *command]
    with (out / "gate.log").open("w") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        try:
            while True:
                if not args.smoke and (reason := quiet(args, night)):
                    raise InterruptedError(reason)
                try:
                    status = process.wait(timeout=10)
                    break
                except subprocess.TimeoutExpired:
                    continue
        finally:
            terminate(process)
    if not args.smoke and (reason := quiet(args, night)):
        raise InterruptedError(reason)
    if status not in (0, 1) or not summary.exists():
        raise ValueError(f"{pair}: incomplete gate, exit {status}; see gate.log")
    report = read_json(summary)
    expected = "smoke" if args.smoke else ("passed" if status == 0 else "rejected")
    if (report["verdict"] != expected or (args.smoke and status != 0)
            or report["repetitions"] != (2 if args.smoke else 9)
            or set(report["paths"]) != PATHS or set(report["profiles"]) != PROFILE_PATHS):
        raise ValueError(f"{pair}: inconsistent gate evidence")
    return expected


def run(args):
    night = active_window(args.start, args.end)
    if not args.smoke and (reason := quiet(args, night)):
        print(f"{datetime.now().astimezone().isoformat()} skipped: {reason}", flush=True)
        return 0
    with lock(args.store):
        now = datetime.now().astimezone()
        if args.smoke or args.manual:
            runs = args.store / ("manual" if args.manual else "smoke")
            runs.mkdir(exist_ok=True)
            out = Path(tempfile.mkdtemp(prefix=f"{now.date()}-", dir=runs))
        else:
            out = args.store / "runs" / night[0].date().isoformat()
            if out.exists():
                return 0
            out.mkdir(parents=True)
        mode = "manual" if args.manual else "smoke" if args.smoke else "nightly"
        if args.manual:
            print(f"started manual: {out}", flush=True)
        result = dict(mode=mode, started=now.isoformat(), finished=None, machine=machine(),
                      idle_minimum_seconds=args.idle_minutes * 60,
                      ac=power(), idle_seconds=idle_seconds(), load=os.getloadavg(),
                      harness_sha256={name: digest(ROOT / "tests" / name)
                                      for name in ("nfr-nightly.py", "nfr71.py", "nfr72.py")},
                      pairs={}, verdict="incomplete")
        try:
            for pair in args.pair or PAIRS:
                result["pairs"][pair] = measure(args, pair, out / pair, night)
            passes = all(value == "passed" for value in result["pairs"].values())
            result["verdict"] = "smoke" if args.smoke else ("passed" if passes else "rejected")
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            result["error"] = str(error)
        finally:
            result["finished"] = datetime.now().astimezone().isoformat()
            if args.manual:
                result["final_load"] = os.getloadavg()
            write_json(out / "result.json", result)
            print(f"{result['verdict']}: {out}", flush=True)
        return 0 if result["verdict"] in ("smoke", "passed") else 1


def launch_agent(args):
    start = args.start.hour * 60 + args.start.minute
    end = args.end.hour * 60 + args.end.minute
    if end <= start:
        end += 24 * 60
    command = [sys.executable, "-B", str(Path(__file__).resolve()),
               "--store", str(args.store), "run",
               "--start", args.start.isoformat(timespec="minutes"),
               "--end", args.end.isoformat(timespec="minutes"),
               "--idle-minutes", str(args.idle_minutes)]
    plistlib.dump(dict(
        Label="com.bas524.tiny-mq.perf-nightly",
        ProgramArguments=command,
        StartCalendarInterval=[dict(Hour=(minute // 60) % 24, Minute=minute % 60)
                               for minute in range(start, end, 15)],
        StandardOutPath=str(args.store / "launchd.log"),
        StandardErrorPath=str(args.store / "launchd-error.log"),
    ), sys.stdout.buffer)


def parse_time(value):
    try:
        return datetime.strptime(value, "%H:%M").time()
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected HH:MM") from error


def aborted(_signal, _frame):
    raise InterruptedError("launcher terminated")


def main():
    os.umask(0o077)
    os.environ["PATH"] = COMMAND_PATH
    signal.signal(signal.SIGTERM, aborted)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--store", type=Path, required=True)
    commands = parser.add_subparsers(dest="command", required=True)
    preparation = commands.add_parser("prepare")
    preparation.add_argument("pair", choices=PAIRS)
    preparation.add_argument("--baseline", required=True)
    preparation.add_argument("--candidate", required=True)
    preparation.add_argument("--parallel", type=int, default=4)
    conditions = argparse.ArgumentParser(add_help=False)
    conditions.add_argument("--start", type=parse_time, required=True)
    conditions.add_argument("--end", type=parse_time, required=True)
    conditions.add_argument("--idle-minutes", type=int, required=True)
    commands.add_parser("launch-agent", parents=[conditions])
    nightly = commands.add_parser("run", parents=[conditions])
    nightly.add_argument("--pair", choices=PAIRS, action="append")
    mode = nightly.add_mutually_exclusive_group()
    mode.add_argument("--smoke", action="store_true")
    mode.add_argument("--manual", action="store_true")
    args = parser.parse_args()
    args.store = args.store.resolve()
    try:
        if args.command == "prepare":
            prepare(args)
            return 0
        minimum_idle = 0 if getattr(args, "manual", False) else 1
        if args.start == args.end or args.idle_minutes < minimum_idle:
            parser.error(f"require a nonempty window and at least {minimum_idle} idle minutes")
        if args.command == "launch-agent":
            launch_agent(args)
            return 0
        if args.pair and not (args.smoke or args.manual):
            parser.error("nightly mode measures both pairs")
        if args.pair and len(set(args.pair)) != len(args.pair):
            parser.error("pairs must be unique")
        return run(args)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
