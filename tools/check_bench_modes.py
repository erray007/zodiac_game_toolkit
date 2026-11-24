#!/usr/bin/env python3
"""Checks that bench_types' two calibration modes give sane results (Task 5b fix round 4), on a fast and a slow operation pair whose two
sides differ about a hundredfold in speed (array_sort_int: Zodiac's sort about 8 us, the add-on's about 1 ms).

    python -I tools/check_bench_modes.py --bench <bench_types> --out-dir <dir>

Runs, at 15 rounds: --calibrate time, --calibrate count (reps taken from the slower side) and --calibrate count --reps 3 (fixed, which also
compares the two sides' checksums every round). For every result line: finite positive nanoseconds and ratio; reps from 1 to 2^24; equal
reps on both sides in count mode, exactly 3 with --reps 3; in time mode the faster side (Zodiac's sort) runs more reps than the slower one;
the mode recorded in the line matches. Exit codes: 0 sane, 1 a check failed (named), 2 a run failed.
"""

import argparse
import json
import math
import subprocess
import sys
from pathlib import Path

OPS = ("string_byte_ascii", "array_sort_int")
MAX_REPS = 1 << 24


def run(bench: Path, out: Path, extra: list) -> dict:
    command = [str(bench), "--rounds", "15", "--out", str(out)] + extra
    for op in OPS:
        command += ["--op", op]
    done = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
    if done.returncode != 0:
        raise RuntimeError(f"{' '.join(command)} exited with {done.returncode}:\n{done.stderr[-2000:]}")
    lines = {}
    for raw in out.read_text(encoding="utf-8").splitlines():
        if raw.strip():
            record = json.loads(raw)
            if "op" in record:
                lines[record["op"]] = record
    return lines


def problems_of(name: str, lines: dict, mode: str, fixed: int = 0) -> list:
    problems = []
    for op in OPS:
        record = lines.get(op)
        if record is None:
            problems.append(f"{name}: no line for {op}")
            continue
        for field in ("new_ns", "addon_ns", "ratio"):
            value = record.get(field)
            if not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
                problems.append(f"{name}: {op} {field} is {value}")
        reps = (record.get("zodiac_reps"), record.get("addon_reps"))
        if any(not isinstance(r, int) or r < 1 or r > MAX_REPS for r in reps):
            problems.append(f"{name}: {op} reps {reps} outside 1 to 2^24")
        if record.get("calibrate") != mode:
            problems.append(f"{name}: {op} records calibrate {record.get('calibrate')!r}, not {mode!r}")
        if mode == "count" and reps[0] != reps[1]:
            problems.append(f"{name}: {op} runs unequal reps {reps} in count mode")
        if fixed and reps != (fixed, fixed):
            problems.append(f"{name}: {op} runs reps {reps}, not the {fixed} asked for")
    if mode == "time" and "array_sort_int" in lines:
        record = lines["array_sort_int"]
        if not record["zodiac_reps"] > record["addon_reps"]:
            problems.append(f"time: array_sort_int gives Zodiac's faster sort {record['zodiac_reps']} reps, not more than the add-on's {record['addon_reps']}")
    return problems


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Sanity of bench_types' time and count calibration.")
    parser.add_argument("--bench", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args(argv)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    try:
        timed = run(args.bench, args.out_dir / "time.json", ["--calibrate", "time"])
        counted = run(args.bench, args.out_dir / "count.json", ["--calibrate", "count"])
        fixed = run(args.bench, args.out_dir / "count3.json", ["--calibrate", "count", "--reps", "3"])
    except (RuntimeError, OSError, ValueError) as error:
        print(f"check_bench_modes: {error}", file=sys.stderr)
        return 2
    problems = problems_of("time", timed, "time") + problems_of("count", counted, "count") + problems_of("count --reps 3", fixed, "count", 3)
    for name, lines in (("time", timed), ("count", counted), ("count --reps 3", fixed)):
        for op in OPS:
            r = lines.get(op, {})
            print(f"{name}: {op} zodiac {r.get('new_ns')} ns x {r.get('zodiac_reps')} reps, add-on {r.get('addon_ns')} ns x {r.get('addon_reps')} reps, ratio {r.get('ratio')}")
    for problem in problems:
        print(f"check_bench_modes: {problem}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
