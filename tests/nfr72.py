#!/usr/bin/env python3
"""REQ-72-9: NfrGateTest.DeliveryHotPathBudget."""
import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess

from nfr71 import HOT_PATHS as DESTINATION_PATHS, PROFILES as DESTINATION_PROFILES, compare, run

DELIVERY_PATHS = (
    "ClientAck_Persistent_RoundTrip/process_time",
    *(f"ClientAck_Batch_NonPersistent/{size}" for size in (1, 100, 1000)),
    "Transacted_NonPersistent_RoundTrip",
    "Transacted_Persistent_RoundTrip/process_time",
    *(f"Transacted_Batch_NonPersistent/{size}" for size in (10, 100, 1000)),
    *(f"Transacted_Batch_Persistent/{size}/process_time" for size in (10, 100, 1000)),
)
PROFILES = (
    "Delivery_Listener/process_time",
    "Delivery_AsyncSend/process_time",
    "Delivery_JournalReplay/process_time",
    "Delivery_DeadLetter/process_time",
)


def one_cpu_per_core(inherited, cpu_root):
    topology = {}
    for cpu in inherited:
        siblings = set()
        path = cpu_root / f"cpu{cpu}" / "topology" / "thread_siblings_list"
        for span in path.read_text().strip().split(","):
            first, _, last = span.partition("-")
            lo, hi = int(first), int(last or first)
            if lo < 0 or hi < lo:
                raise ValueError(f"invalid CPU range in {path}")
            siblings.update(other for other in inherited if lo <= other <= hi)
        topology[cpu] = frozenset(siblings)

    if any(cpu not in siblings or any(topology[other] != siblings for other in siblings)
           for cpu, siblings in topology.items()):
        raise ValueError("inconsistent CPU sibling topology")
    selected = {min(siblings) for siblings in topology.values()}
    return selected if len(selected) >= 2 else inherited


def configure_cpu_affinity(cpu_root=Path("/sys/devices/system/cpu")):
    try:
        inherited = os.sched_getaffinity(0)
    except (AttributeError, OSError) as error:
        print(f"CPU affinity: inherited=unavailable effective=unavailable ({error})", flush=True)
        return

    effective = inherited
    try:
        if len(inherited) >= 2:
            selected = one_cpu_per_core(inherited, cpu_root)
            if selected != inherited:
                # Both binaries and their worker threads inherit this mask.
                os.sched_setaffinity(0, selected)
                effective = None
                effective = os.sched_getaffinity(0)
    except (AttributeError, OSError, ValueError) as error:
        print(f"CPU affinity: {error}", flush=True)
    reported = sorted(effective) if effective is not None else "unavailable"
    print(f"CPU affinity: inherited={sorted(inherited)} effective={reported}", flush=True)


def registrations(binary):
    output = subprocess.check_output(
        [str(binary.resolve()), "--gbench", "--benchmark_list_tests"], text=True
    )
    return {line.removeprefix("BenchmarkFixture/") for line in output.splitlines()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--summary", type=Path)
    args = parser.parse_args()
    if args.baseline.resolve() == args.candidate.resolve():
        parser.error("baseline and candidate must be separate binaries")
    args.out.mkdir(parents=True, exist_ok=True)
    configure_cpu_affinity()

    print("[REQ-71-7] NfrGateTest.DestinationHotPathBudget", flush=True)
    print("[REQ-72-9] NfrGateTest.DeliveryHotPathBudget", flush=True)
    print("ABBA: median CPU and real ns/delivery; persistent gates include storage worker CPU", flush=True)
    reps, minimum, warmup = (2, "0.05s", 0) if args.smoke else (9, "1.2s", 2)
    registered = registrations(args.baseline)
    passed = True
    results = {}
    for name in (*DESTINATION_PATHS, *DELIVERY_PATHS):
        baseline_name = name
        if name not in registered and args.smoke:
            baseline_name = name.removesuffix("/process_time")
            print(f"{name}: local smoke uses legacy registration {baseline_name}", flush=True)
        if baseline_name not in registered:
            parser.error(f"missing comparable baseline benchmark: {baseline_name}")
        directory = args.out / name.replace("/", "-")
        first = {}
        results[name] = [first]
        stable, within_budget = compare(args, name, reps, minimum, warmup, directory,
                                        baseline_name, report=first)
        if not stable and not args.smoke:
            retry_minimum = "12s" if name.startswith("Transacted_Batch_Persistent/") else minimum
            print(f"{name}: repeating once because CPU CV exceeded 5% (minimum {retry_minimum})", flush=True)
            retry = {}
            results[name].append(retry)
            stable, within_budget = compare(args, name, reps, retry_minimum, warmup,
                                            directory.with_name(directory.name + "-repeat"),
                                            report=retry)
        passed &= stable and within_budget

    names = (*DESTINATION_PROFILES, *PROFILES)
    profiles = run(args.candidate, names, reps, minimum, warmup, args.out / "profiles.json")
    profile_medians = {}
    for name in names:
        cpu, real = zip(*profiles[name])
        cpu_median, real_median = statistics.median(cpu), statistics.median(real)
        profile_medians[name] = dict(cpu_median=cpu_median, real_median=real_median)
        print(f"{name}: CPU median {cpu_median:.1f} ns/op, "
              f"real median {real_median:.1f} ns/op", flush=True)
    if args.smoke:
        print("Smoke completed. Run without --smoke for the gate.")
    if args.summary:
        verdict = "passed" if passed else "rejected"
        report = dict(verdict="smoke" if args.smoke else verdict, repetitions=reps,
                      measurement_unit="ns/delivery", paths=results, profiles=profile_medians)
        with args.summary.open("x") as output:
            json.dump(report, output, indent=2, allow_nan=False)
            output.write("\n")
    return 0 if args.smoke or passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
