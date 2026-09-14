#!/usr/bin/env python3
"""Run two prebuilt benchmark binaries sequentially in alternating order."""

import argparse
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import platform
import statistics
import subprocess
import sys


def positive(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def read_results(output):
    rows = list(csv.DictReader(io.StringIO("\n".join(
        line for line in output.splitlines() if not line.startswith("#")
    ))))
    if not rows or len({row["scenario"] for row in rows}) != len(rows):
        raise ValueError("missing or duplicated benchmark scenarios")
    for row in rows:
        for key in ("mean_ns_per_op", "p50_batch_ns_per_op",
                    "p95_batch_ns_per_op", "p99_batch_ns_per_op"):
            value = float(row[key])
            if not math.isfinite(value) or value <= 0:
                raise ValueError(f"invalid timing: {row['scenario']} {key}")
    return {row["scenario"]: row for row in rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--output", required=True, type=Path,
                        help="new directory for raw CSV and manifest")
    parser.add_argument("--runs", type=positive, default=6)
    parser.add_argument("--samples", type=positive, default=200)
    parser.add_argument("--build-notes", required=True,
                        help="source revisions, compiler, flags and build conditions")
    args = parser.parse_args()
    if args.samples > 10000:
        parser.error("samples must not exceed 10000")
    binaries = {"baseline": args.baseline.resolve(strict=True),
                "candidate": args.candidate.resolve(strict=True)}
    args.output.mkdir(parents=True, exist_ok=False)
    manifest = {
        "format": "eme_comparison_v1",
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "runs": args.runs,
        "samples": args.samples,
        "build_notes": args.build_notes,
        "binaries": {name: {"path": str(path), "sha256": hashlib.sha256(
            path.read_bytes()).hexdigest()} for name, path in binaries.items()},
        "order": [],
        "status": "running",
    }
    manifest_path = args.output / "manifest.json"

    def save_manifest():
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")

    save_manifest()
    results = {name: [] for name in binaries}
    reference_headers = None
    try:
        for run in range(args.runs):
            order = ("baseline", "candidate") if run % 2 == 0 else ("candidate", "baseline")
            for name in order:
                command = [str(binaries[name]), "--samples", str(args.samples)]
                process = subprocess.run(command, text=True, capture_output=True,
                                         timeout=300, check=False)
                (args.output / f"{run:02d}-{name}.csv").write_text(process.stdout)
                (args.output / f"{run:02d}-{name}.stderr").write_text(process.stderr)
                process.check_returncode()
                headers = [line for line in process.stdout.splitlines() if line.startswith("#")]
                if len(headers) != 2 or not headers[0].startswith("# eme_benchmarks_v1,") or not headers[1].startswith("# benchmark_source_sha256="):
                    raise ValueError("unsupported benchmark metadata")
                if reference_headers is None:
                    reference_headers = headers
                    manifest["benchmark_metadata"] = headers
                elif headers != reference_headers:
                    raise ValueError("benchmark source or fixture metadata differs")
                rows = read_results(process.stdout)
                results[name].append(rows)
                manifest["order"].append({"run": run, "binary": name})
                save_manifest()
                print(f"run {run + 1}/{args.runs}: {name} complete", file=sys.stderr)

        reference = results["baseline"][0]
        identity = ("scenario", "samples", "batch_size", "bytes_per_op", "measured_ops", "digest")
        for runs in results.values():
            for rows in runs:
                if rows.keys() != reference.keys():
                    raise ValueError("scenario sets differ; binaries are not comparable")
                for name, row in rows.items():
                    if any(row[key] != reference[name][key] for key in identity):
                        raise ValueError(f"workload or output differs: {name}")

        summary = []
        for name in reference:
            before = [float(run[name]["mean_ns_per_op"]) for run in results["baseline"]]
            after = [float(run[name]["mean_ns_per_op"]) for run in results["candidate"]]
            paired = [(candidate / baseline - 1) * 100
                      for baseline, candidate in zip(before, after)]
            summary.append({
                "scenario": name,
                "baseline_median_ns": statistics.median(before),
                "candidate_median_ns": statistics.median(after),
                "baseline_min_ns": min(before), "baseline_max_ns": max(before),
                "candidate_min_ns": min(after), "candidate_max_ns": max(after),
                "paired_median_change_percent": statistics.median(paired),
                "paired_min_change_percent": min(paired),
                "paired_max_change_percent": max(paired),
            })
        with (args.output / "comparison.csv").open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=summary[0].keys())
            writer.writeheader()
            writer.writerows(summary)
        manifest["status"] = "complete"
        save_manifest()
        print(json.dumps(summary, indent=2))
    except Exception as error:
        manifest["status"] = "failed"
        manifest["error"] = str(error)
        save_manifest()
        raise


if __name__ == "__main__":
    main()
