#!/usr/bin/env python3
"""NfrGateTest.HotPathBudget: REQ-70-6, matched RelWithDebInfo benchmark builds."""
import argparse
import hashlib
import json
import os
import platform
import statistics
import subprocess
import tempfile
from pathlib import Path


BENCHMARKS = {
    "BenchmarkFixture/AutoAck_NonPersistent_RoundTrip",
    "BenchmarkFixture/AutoAck_Persistent_RoundTrip",
    "MessageSelector",
}
FILTER = "BenchmarkFixture/AutoAck_(NonPersistent|Persistent)_RoundTrip$|^MessageSelector$"
# Two four-sample ABBA blocks and one pair yield nine values per build.
SERIES = (("A", 2), ("B", 2), ("B", 2), ("A", 2),
          ("A", 2), ("B", 2), ("B", 2), ("A", 2), ("A", 1), ("B", 1))
NANOSECONDS = {"ns": 1, "us": 1_000, "ms": 1_000_000, "s": 1_000_000_000}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("raw_log", type=Path)
    parser.add_argument("--baseline-ref", required=True)
    parser.add_argument("--candidate-ref", required=True)
    args = parser.parse_args()
    binaries = {"A": args.baseline.resolve(), "B": args.candidate.resolve()}
    results = {name: {"A": [], "B": []} for name in sorted(BENCHMARKS)}
    with args.raw_log.open("w") as raw, tempfile.TemporaryDirectory(prefix="tiny-mq-nfr70-") as work:
        def record(data):
            json.dump(data, raw)
            raw.write("\n")
            raw.flush()

        record({"platform": platform.platform(), "cpu_count": os.cpu_count(),
                "binaries": {label: {"path": str(binary), "sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}
                             for label, binary in binaries.items()},
                "baseline_ref": args.baseline_ref, "candidate_ref": args.candidate_ref})
        for label, repeat in SERIES:
            command = [str(binaries[label]), "--gbench", f"--benchmark_filter={FILTER}",
                       f"--benchmark_repetitions={repeat}", "--benchmark_min_time=1.2s",
                       "--benchmark_format=json"]
            proc = subprocess.run(command, cwd=work, capture_output=True, text=True)
            record({"build": label, "command": command, "stdout": proc.stdout, "stderr": proc.stderr,
                    "returncode": proc.returncode})
            proc.check_returncode()
            for run in json.loads(proc.stdout)["benchmarks"]:
                if run.get("run_type") != "iteration":
                    continue
                if run.get("error_occurred"):
                    raise RuntimeError(run.get("error_message", "Benchmark failed"))
                results[run["name"]][label].append(run["cpu_time"] * NANOSECONDS[run["time_unit"]])

    passed = True
    print("NfrGateTest.HotPathBudget REQ-70-6 (ns CPU time/op, median of 9 per build)")
    for name, samples in results.items():
        base, candidate = samples["A"], samples["B"]
        if len(base) != 9 or len(candidate) != 9:
            raise RuntimeError(f"{name}: expected nine observations from each build")
        old, new = statistics.median(base), statistics.median(candidate)
        base_cv = statistics.stdev(base) / statistics.mean(base)
        candidate_cv = statistics.stdev(candidate) / statistics.mean(candidate)
        delta = new / old - 1
        print(f"{name}: {old:.2f} -> {new:.2f} ({delta:+.2%}); CV {base_cv:.2%} / {candidate_cv:.2%}")
        # A production performance regression cannot be inferred from a noisy
        # series. A noisy control may still corroborate a very large speedup.
        stable = max(base_cv, candidate_cv) <= 0.05 or (
            delta < 0 and max(candidate) <= min(base) * 0.95 and candidate_cv <= 0.05)
        passed = passed and stable and delta <= 0.05
    print("PASS" if passed else "FAIL: over budget or noisy; see the raw series")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
