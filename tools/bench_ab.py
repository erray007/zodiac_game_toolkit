#!/usr/bin/env python3
"""A/B of two bench_types builds on one machine (Task 5b): the baseline A and the candidate B run alternately (A B B A ...), each in its own
process, and their results are paired run by run.

    python -I tools/bench_ab.py --a <bench_types A> --b <bench_types B> --out-dir <dir> [--pairs 8] [--rounds 101] [--op NAME]...

Per operation it prints the median gate ratio (Zodiac/add-on) of each build, the paired change (the median over pairs of B's Zodiac
nanoseconds over A's in the neighbouring run) and how many of A's and B's runs were above the gate's 1.05; then the geometric mean of
the paired changes. A change below 1 means B is faster. Each run's JSON and the table (ab.txt) go to --out-dir. Exit codes: 0 done,
1 bad arguments, 2 a run failed.
"""

import argparse
import json
import math
import statistics
import subprocess
import sys
from pathlib import Path

LIMIT = 1.05


def run(bench: Path, out: Path, rounds: int, ops: list, diagnostic: bool = False, extra: tuple = ()) -> dict:
    command = [str(bench), "--detail", "--rounds", str(rounds), "--out", str(out)] + (["--diagnostic"] if diagnostic else []) + list(extra)
    for op in ops:
        command += ["--op", op]
    done = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
    out.with_suffix(".stderr.txt").write_text(done.stderr, encoding="utf-8")
    if done.returncode != 0:
        raise RuntimeError(f"{bench} exited with {done.returncode}:\n{done.stderr[-2000:]}")
    machine = None
    results = {}
    for raw in out.read_text(encoding="utf-8").splitlines():
        if not raw.strip():
            continue
        record = json.loads(raw)
        if "machine" in record:
            machine = record["machine"]
        else:
            results[record["op"]] = record
    return {"machine": machine, "ops": results}


def table(a_runs: list, b_runs: list) -> str:
    # Operations both builds have (a baseline may predate some), in B's order.
    ops = [op for op in b_runs[0]["ops"] if all(op in r["ops"] for r in a_runs + b_runs)]
    width = max(len(op) for op in ops)
    machine = b_runs[0]["machine"] or a_runs[0]["machine"]
    lines = [f"machine: {machine['cpu']}, {machine['logical_cores']} logical cores, {machine['os']}" if machine else "machine: not recorded"]
    lines.append(f"{'op':<{width}}  {'ratio A':>8}  {'ratio B':>8}  {'B/A ns':>7}  {'A>1.05':>6}  {'B>1.05':>6}")
    changes = []
    for op in ops:
        ra = [r["ops"][op]["ratio"] for r in a_runs]
        rb = [r["ops"][op]["ratio"] for r in b_runs]
        paired = [b["ops"][op]["new_ns"] / a["ops"][op]["new_ns"] for a, b in zip(a_runs, b_runs) if a["ops"][op]["new_ns"] > 0]
        change = statistics.median(paired)
        changes.append(change)
        lines.append(f"{op:<{width}}  {statistics.median(ra):>8.3f}  {statistics.median(rb):>8.3f}  {change:>7.3f}  {sum(x > LIMIT for x in ra):>3}/{len(ra):<2}  {sum(x > LIMIT for x in rb):>3}/{len(rb):<2}")
    lines.append(f"paired geomean B/A: {math.exp(statistics.fmean(math.log(c) for c in changes)):.4f} over {len(changes)} operations, {len(a_runs)} pairs")
    return "\n".join(lines)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Alternate two bench_types builds and pair their results.")
    parser.add_argument("--a", type=Path, required=True)
    parser.add_argument("--b", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--pairs", type=int, default=8)
    parser.add_argument("--rounds", type=int, default=101)
    parser.add_argument("--op", action="append", default=[])
    parser.add_argument("--diagnostic", action="store_true", help="pass --diagnostic, so --op may name the diagnostic operations")
    parser.add_argument("--bench-arg", action="append", default=[], help="an argument passed to every bench run (e.g. --calibrate, count); repeatable")
    args = parser.parse_args(argv)
    if args.pairs < 1:
        print("bench_ab: --pairs must be at least 1", file=sys.stderr)
        return 1
    args.out_dir.mkdir(parents=True, exist_ok=True)
    a_runs, b_runs = [], []
    try:
        for pair in range(args.pairs):
            order = ("a", "b") if pair % 2 == 0 else ("b", "a")
            for side in order:
                bench = args.a if side == "a" else args.b
                result = run(bench, args.out_dir / f"{side}{pair}.json", args.rounds, args.op, args.diagnostic, args.bench_arg)
                (a_runs if side == "a" else b_runs).append(result)
            print(f"bench_ab: pair {pair + 1} of {args.pairs} done", file=sys.stderr, flush=True)
    except (RuntimeError, OSError, ValueError, KeyError) as error:
        print(f"bench_ab: {error}", file=sys.stderr)
        return 2
    text = table(a_runs, b_runs)
    (args.out_dir / "ab.txt").write_text(text + "\n", encoding="utf-8")
    print(text, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
