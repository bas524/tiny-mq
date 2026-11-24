#!/usr/bin/env python3
"""REQ-71-7: NfrGateTest.DestinationHotPathBudget."""
import argparse
import collections
import json
from pathlib import Path
import re
import shlex
import statistics
import subprocess
import tempfile

HOT_PATHS = (
    "AutoAck_NonPersistent_RoundTrip",
    "Topic_AutoAck_NonPersistent_RoundTrip",
    "AutoAck_Persistent_RoundTrip/process_time",
)
PROFILES = (
    "Destination_ConcurrentClaim/process_time",
    "Destination_Fanout/1",
    "Destination_Fanout/2",
    "Destination_Fanout/4",
)
NANOSECONDS = {"ns": 1, "us": 1_000, "ms": 1_000_000, "s": 1_000_000_000}


def run(binary, names, repetitions, minimum, warmup, output):
    # Benchmarks use relative storage paths. Isolate these from the baseline
    # worktree and from the functional test storage.
    output = output.resolve()
    command = [
        str(binary.resolve()), "--gbench",
        "--benchmark_filter=^BenchmarkFixture/(" + "|".join(names) + ")$",
        f"--benchmark_repetitions={repetitions}",
        f"--benchmark_min_time={minimum}", f"--benchmark_min_warmup_time={warmup}",
        "--benchmark_out_format=json", f"--benchmark_out={output}",
    ]
    with tempfile.TemporaryDirectory(prefix="nfr71-", dir=output.parent) as cwd:
        print(f"(cd {shlex.quote(cwd)} && {shlex.join(command)})", flush=True)
        subprocess.run(command, cwd=cwd, check=True)

    results = collections.defaultdict(list)
    for entry in json.loads(output.read_text())["benchmarks"]:
        if entry["run_type"] != "iteration":
            continue
        name = entry["run_name"].removeprefix("BenchmarkFixture/")
        batch = re.search(r"/(\d+)(?:/process_time)?$", name) if "_Batch_" in name else None
        deliveries = entry.get("deliveries_per_iteration", int(batch[1]) if batch else 1)
        scale = NANOSECONDS[entry["time_unit"]] / deliveries
        results[name].append((entry["cpu_time"] * scale, entry["real_time"] * scale))
    for name in names:
        if len(results[name]) != repetitions:
            raise RuntimeError(f"{name}: expected {repetitions} measurements, got {len(results[name])}")
    return results


def compare(args, name, repetitions, minimum, warmup, directory, baseline_name=None, *, report=None):
    directory.mkdir(exist_ok=True)
    samples = {"baseline": [], "candidate": []}
    first = repetitions // 2
    order = (
        ("baseline", first), ("candidate", first),
        ("candidate", repetitions - first), ("baseline", repetitions - first),
    )
    for step, (label, count) in enumerate(order, start=1):
        registration = baseline_name if label == "baseline" and baseline_name else name
        result = run(getattr(args, label), (registration,), count, minimum, warmup,
                     directory / f"{step}-{label}.json")
        samples[label].extend(result[registration])

    (baseline, baseline_real), (candidate, candidate_real) = (
        zip(*samples[label]) for label in ("baseline", "candidate")
    )
    before, after = statistics.median(baseline), statistics.median(candidate)
    cv_baseline, cv_candidate = (
        100 * statistics.stdev(values) / statistics.mean(values)
        for values in (baseline, candidate)
    )
    real_medians = [statistics.median(values) for values in (baseline_real, candidate_real)]
    delta = 100 * (after / before - 1)
    stable = max(cv_baseline, cv_candidate) <= 5.0
    within_budget = after * 20 <= before * 21
    label = "noisy"
    if stable:
        label = "within budget" if within_budget else "regression"
    if report is not None:
        report.update(cpu_median=[before, after], real_median=real_medians,
                      cpu_cv=[cv_baseline, cv_candidate], delta_percent=delta,
                      stable=stable, within_budget=within_budget, samples=directory.name)
    print(f"{name}: CPU median {before:.1f} -> {after:.1f} ns/op ({delta:+.2f}%), "
          f"real median {real_medians[0]:.1f} -> {real_medians[1]:.1f} ns/op, "
          f"CPU CV {cv_baseline:.2f}% / {cv_candidate:.2f}% ({label})", flush=True)
    return stable, within_budget


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=9)
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args()
    if not args.smoke and args.repetitions < 7:
        parser.error("at least seven repetitions are required for the gate")
    if args.baseline.resolve() == args.candidate.resolve():
        parser.error("baseline and candidate must use separate builds")
    args.out.mkdir(parents=True, exist_ok=True)
    print("[REQ-71-7] NfrGateTest.DestinationHotPathBudget", flush=True)
    print("ABBA: median CPU and real ns per operation, CPU coefficient of variation", flush=True)
    print("Persistent queue: process CPU includes the storage worker.", flush=True)
    reps, minimum, warmup = (2, "0.10s", 0) if args.smoke else (args.repetitions, "1.2s", 2)
    passed = True
    for name in HOT_PATHS:
        directory = args.out / name.replace("/", "-")
        stable, within_budget = compare(args, name, reps, minimum, warmup, directory)
        if not stable and not args.smoke:
            print(f"{name}: repeating once because CPU CV exceeded 5%", flush=True)
            stable, within_budget = compare(args, name, reps, minimum, warmup,
                                            directory.with_name(directory.name + "-repeat"))
        passed &= stable and within_budget

    profiles = run(args.candidate, PROFILES, reps, minimum, warmup, args.out / "new-profiles.json")
    for name, values in profiles.items():
        cpu, real = zip(*values)
        print(f"{name}: CPU median {statistics.median(cpu):.1f} ns/delivery, "
              f"real median {statistics.median(real):.1f} ns/delivery")
    if args.smoke:
        print("Smoke completed. Run without --smoke for the gate.")
        return 0
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
