#!/usr/bin/env python3
"""The pack read gate of Task 13: bench_files' gated read operations (bench_files --list-gated: pack against disk) in several
processes, each from its own copy of the executable, then tools/check_bench.py on their results.

    python -I tools/bench_files_gate.py --bench <bench_files> --out-dir <dir> [--processes 3] [--limit RATIO | --limit OP=RATIO]...
        [--bench-arg ARG]... [--report-only]

The method is the bench_types gate's (spec 8.4): each side equal-time calibrated (bench_files' read operations always are), 3
processes gated on the median of their ratios per operation, and every process from its own copy of the build, since Windows draws
an image base per file and keeps it for every process of that file (Task 5c), so three copies are three placements. --limit goes to
check_bench.py as it is (the measured limits are in ci.yml); --bench-arg is passed to every bench process (e.g. --chunk-log2 14).
Into --out-dir go ops.txt (the gated operations), files-<n>.json and files-<n>-stderr.txt per process, and check.txt (the check's
stdout). Exit codes: check_bench.py's (0 within the limits, 1 an operation above its limit, 2 a malformed or incomplete result), 2
when a bench process fails or the bench cannot be run; with --report-only an exit code 1 of the check becomes 0 with a warning
(an ::warning:: annotation under GitHub Actions), and everything else stays as it is.
"""

import argparse
import contextlib
import io
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_bench


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Run bench_files' read operations in several processes and gate them with check_bench.py.")
    parser.add_argument("--bench", type=Path, required=True, help="the bench_files executable")
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--processes", type=int, default=3)
    parser.add_argument("--limit", action="append", default=[], help="passed to check_bench.py (RATIO or OP=RATIO; repeatable)")
    parser.add_argument("--bench-arg", action="append", default=[], help="an argument for every bench process (repeatable)")
    parser.add_argument("--report-only", action="store_true", help="an operation above its limit warns instead of failing")
    args = parser.parse_args(argv)
    if args.processes < 1:
        print("bench_files_gate: --processes must be at least 1", file=sys.stderr)
        return 2
    args.out_dir.mkdir(parents=True, exist_ok=True)
    try:
        listed = subprocess.run([str(args.bench), "--list-gated"], capture_output=True, text=True, encoding="utf-8", check=False)
    except OSError as error:
        print(f"bench_files_gate: cannot run {args.bench}: {error}", file=sys.stderr)
        return 2
    if listed.returncode != 0 or not listed.stdout.strip():
        print(f"bench_files_gate: {args.bench} --list-gated exited with {listed.returncode}:\n{listed.stderr}", file=sys.stderr)
        return 2
    ops = [name.strip() for name in listed.stdout.splitlines() if name.strip()]
    expected = args.out_dir / "ops.txt"
    expected.write_text("".join(name + "\n" for name in ops), encoding="utf-8")
    copies = args.out_dir / "copies"
    copies.mkdir(exist_ok=True)
    results = []
    for index in range(1, args.processes + 1):
        copy = copies / f"bench_files-{index}{args.bench.suffix}"
        shutil.copy2(args.bench, copy)
        out = args.out_dir / f"files-{index}.json"
        command = [str(copy)] + [part for name in ops for part in ("--op", name)] + list(args.bench_arg) + ["--out", str(out)]
        done = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace", check=False)
        (args.out_dir / f"files-{index}-stderr.txt").write_text(done.stderr, encoding="utf-8")
        sys.stdout.write(done.stderr)
        sys.stdout.flush()
        if done.returncode != 0:
            print(f"bench_files_gate: process {index} exited with {done.returncode}", file=sys.stderr)
            return 2
        results.append(str(out))
    check_args = results + ["--expected", str(expected)] + [part for value in args.limit for part in ("--limit", value)]
    captured = io.StringIO()
    with contextlib.redirect_stdout(captured):
        code = check_bench.main(check_args)
    (args.out_dir / "check.txt").write_text(captured.getvalue(), encoding="utf-8")
    sys.stdout.write(captured.getvalue())
    sys.stdout.flush()
    if code == 1 and args.report_only:
        warning = "pack reads: an operation is above its limit (report-only)"
        print(f"bench_files_gate: {warning}", file=sys.stderr)
        if os.environ.get("GITHUB_ACTIONS") == "true":
            print(f"::warning title=Pack read gate report-only::{warning}", flush=True)
        return 0
    return code


if __name__ == "__main__":
    sys.exit(main())
