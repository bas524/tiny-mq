#!/usr/bin/env python3
"""REQ-72-9: NfrGateTest.DeliveryHotPathBudget."""
import argparse
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
    args = parser.parse_args()
    if args.baseline.resolve() == args.candidate.resolve():
        parser.error("baseline and candidate must be separate binaries")
    args.out.mkdir(parents=True, exist_ok=True)

    print("[REQ-71-7] NfrGateTest.DestinationHotPathBudget", flush=True)
    print("[REQ-72-9] NfrGateTest.DeliveryHotPathBudget", flush=True)
    print("ABBA: median CPU and real ns/delivery; persistent gates include storage worker CPU", flush=True)
    reps, minimum, warmup = (2, "0.05s", 0) if args.smoke else (9, "1.2s", 2)
    registered = registrations(args.baseline)
    passed = True
    for name in (*DESTINATION_PATHS, *DELIVERY_PATHS):
        baseline_name = name
        if name not in registered and args.smoke:
            baseline_name = name.removesuffix("/process_time")
            print(f"{name}: local smoke uses legacy registration {baseline_name}", flush=True)
        if baseline_name not in registered:
            parser.error(f"missing comparable baseline benchmark: {baseline_name}")
        directory = args.out / name.replace("/", "-")
        stable, within_budget = compare(args, name, reps, minimum, warmup, directory, baseline_name)
        if not stable and not args.smoke:
            print(f"{name}: repeating once because CPU CV exceeded 5%", flush=True)
            stable, within_budget = compare(args, name, reps, minimum, warmup,
                                            directory.with_name(directory.name + "-repeat"))
        passed &= stable and within_budget

    names = (*DESTINATION_PROFILES, *PROFILES)
    profiles = run(args.candidate, names, reps, minimum, warmup, args.out / "profiles.json")
    for name in names:
        cpu, real = zip(*profiles[name])
        print(f"{name}: CPU median {statistics.median(cpu):.1f} ns/op, "
              f"real median {statistics.median(real):.1f} ns/op", flush=True)
    if args.smoke:
        print("Smoke completed. The performance verdict requires the comparable GitHub run.")
    return 0 if args.smoke or passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
