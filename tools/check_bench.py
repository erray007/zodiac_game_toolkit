#!/usr/bin/env python3
"""Checks bench_types' results against the speed gate of spec 8.4: no Zodiac type may be more than 5 % slower than its sample add-on, nor
slower than on main beyond the regression limits.

    python -I tools/check_bench.py <results.json>... --expected <ops.txt> [--baseline <main results.json>...]

Each results file is one bench_types process: one JSON object per line, {"op": name, "new_ns": n, "addon_ns": n, "ratio": r, ...}: new_ns
and addon_ns are the median nanoseconds per iteration of Zodiac's types and of the add-ons, ratio the median of the per-round ratios
new/addon. Further fields (the calibration bench_types records: "calibrate" time or count, "window_ms", "zodiac_reps", "addon_reps") are
accepted in either mode and the mode is printed. Exactly one line describes the machine instead: {"machine": {"cpu": brand,
"logical_cores": n, "os": build}}, printed above the table so every log names the CPU it measured; every file must come from the same
machine. The expected file holds one operation name per line, as `bench_types --list` prints them (the bench is the one source of the
operation list), and every results file must cover exactly those operations, in the same order. Blank lines are ignored.

The add-on gate: per operation, the median over the files of their ratios is at most LIMIT (1.05). Several processes (the Windows gate
runs 3) are needed because one process's code and heap placement moves a ratio by several percent (Task 5b fix rounds 3 and 4).

The regression gate (--baseline, one file per process of main's bench on the same machine, paired with the results files in order, as the
gate runs them in alternation): per operation both have, the median over the pairs of head's new_ns over main's is at most
REGRESSION_LIMIT, and the geometric mean of those medians is at most REGRESSION_GEOMEAN. Operations main does not have are listed, not
compared. The limits come from same-source rebuilds on one machine (Task 5b fix rounds 2 to 4): A/A' runs move single operations by up to
9 % (45339050531: 0.986 to 1.060; 45353776618: string_split 0.909), and no-op relinks move Zodiac's own string_byte_ascii by 7 to 15 % across
builds on an EPYC 7763 (bench-reference 45353765232), while the geometric mean of all operations stays within 1 % (0.9974, 0.9975, 1.0065).
So a per-operation limit tighter than 1.10 would fail rebuilds that change nothing (1.07 failed string_byte_ascii at 1.088 and 1.097 against
main in CI 45353693645 and 45353764588), and the 1.01 geometric mean catches a slowdown spread over many operations.

Exit codes: 0 both gates pass; 1 some gated ratio is above LIMIT or the regression gate fails (each such operation is named on stderr); 2 a
file is missing or unreadable, a line is not such an object (malformed JSON, a missing or non-numeric field, a negative or non-finite
number, an empty op name), an operation appears twice, a file holds no results, the machine line is missing, repeated, malformed or differs
between files, the expected list is empty, an expected operation is missing or an unknown one is present (each named on stderr), the
baseline files do not pair with the results files, the arguments are wrong, or the check itself fails unexpectedly. Only 1 means "too
slow": the report-only Linux job tolerates exactly that code.
"""

import argparse
import json
import math
import sys
import traceback
from pathlib import Path

LIMIT = 1.05
# Zodiac against main on the same machine (paired processes): see the module docstring for how these were chosen.
REGRESSION_LIMIT = 1.10
REGRESSION_GEOMEAN = 1.01
FIELDS = ("new_ns", "addon_ns", "ratio")


class BenchFormatError(Exception):
    """Raised when the results are not a complete set of well-formed lines."""


def _reject_constant(name: str):
    raise ValueError(f"{name} is not a number")


def _number(record: dict, field: str, number: int) -> float:
    value = record.get(field)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise BenchFormatError(f"line {number}: \"{field}\" is missing or not a number")
    value = float(value)
    if not math.isfinite(value) or value < 0:
        raise BenchFormatError(f"line {number}: \"{field}\" is {value}, not a finite number of at least 0")
    return value


def _machine(record: dict, number: int) -> dict:
    machine = record.get("machine")
    if len(record) != 1 or not isinstance(machine, dict):
        raise BenchFormatError(f"line {number}: the machine line holds anything but one \"machine\" object")
    cpu = machine.get("cpu")
    cores = machine.get("logical_cores")
    system = machine.get("os")
    if not isinstance(cpu, str) or not cpu or not isinstance(system, str) or not system:
        raise BenchFormatError(f"line {number}: \"cpu\" and \"os\" must be non-empty texts")
    if isinstance(cores, bool) or not isinstance(cores, int) or cores < 0:
        raise BenchFormatError(f"line {number}: \"logical_cores\" must be a whole number of at least 0")
    return {"cpu": cpu, "logical_cores": cores, "os": system}


def read(text: str) -> tuple:
    """(machine, results) of text: the machine line's fields and the (op, new_ns, addon_ns, ratio) tuples in order. Raises
    BenchFormatError."""
    results = []
    machine = None
    seen = set()
    for number, raw in enumerate(text.splitlines(), start=1):
        if not raw.strip():
            continue
        try:
            record = json.loads(raw, parse_constant=_reject_constant)
        except ValueError as error:
            raise BenchFormatError(f"line {number}: malformed JSON ({error})") from None
        if not isinstance(record, dict):
            raise BenchFormatError(f"line {number}: not a JSON object")
        if "machine" in record:
            if machine is not None:
                raise BenchFormatError(f"line {number}: a second machine line")
            machine = _machine(record, number)
            continue
        op = record.get("op")
        if not isinstance(op, str) or not op:
            raise BenchFormatError(f"line {number}: \"op\" is missing or not a name")
        if op in seen:
            raise BenchFormatError(f"line {number}: {op} appears twice")
        seen.add(op)
        results.append((op,) + tuple(_number(record, field, number) for field in FIELDS))
    if not results:
        raise BenchFormatError("no results")
    if machine is None:
        raise BenchFormatError("no machine line (the CPU, cores and OS bench_types measured on)")
    return machine, results


def parse(text: str) -> list:
    """The (op, new_ns, addon_ns, ratio) tuples of text, in order. Raises BenchFormatError."""
    return read(text)[1]


def describe(machine: dict) -> str:
    return f"machine: {machine['cpu']}, {machine['logical_cores']} logical cores, {machine['os']}"


def check_complete(results: list, expected_text: str) -> None:
    """Raises BenchFormatError unless the results cover exactly the operations listed in expected_text."""
    expected = [name.strip() for name in expected_text.splitlines() if name.strip()]
    if not expected:
        raise BenchFormatError("the expected list holds no operations")
    found = [op for op, *_ in results]
    missing = [name for name in expected if name not in set(found)]
    unknown = [op for op in found if op not in set(expected)]
    problems = []
    if missing:
        problems.append("missing operation(s): " + ", ".join(missing))
    if unknown:
        problems.append("unknown operation(s): " + ", ".join(unknown))
    if problems:
        raise BenchFormatError("; ".join(problems))


def table(results: list) -> str:
    width = max(len("op"), max(len(op) for op, *_ in results))
    lines = [f"{'op':<{width}}  {'new_ns':>12}  {'addon_ns':>12}  {'ratio':>6}"]
    for op, new_ns, addon_ns, ratio in results:
        mark = "  FAIL" if ratio > LIMIT else ""
        lines.append(f"{op:<{width}}  {new_ns:>12.2f}  {addon_ns:>12.2f}  {ratio:>6.3f}{mark}")
    return "\n".join(lines)


def main(argv=None) -> int:
    # An unexpected error would leave Python's exit code 1, which means "too slow"; it is a failure of the check (2) instead,
    # so a caller that tolerates 1 (the report-only Linux job) never mistakes a crash for a slow operation.
    try:
        return _check(argv)
    except Exception:
        traceback.print_exc()
        print("check_bench: internal error", file=sys.stderr)
        return 2


def calibration(text: str) -> str:
    """The calibration the results were measured with, as bench_types records it per line ("time, window 1 ms" or "count"), or
    "not recorded" for results of a bench that predates the field. Either mode is accepted."""
    for raw in text.splitlines():
        if not raw.strip():
            continue
        try:
            record = json.loads(raw, parse_constant=_reject_constant)
        except ValueError:
            continue
        if isinstance(record, dict) and "op" in record and "calibrate" in record:
            mode = record["calibrate"]
            if mode == "time":
                return f"time, window {record.get('window_ms', '?')} ms"
            return str(mode)
    return "not recorded"


def baseline_ns(text: str) -> dict:
    """{op: new_ns} of a baseline run (main's bench, which may predate the machine line); raises BenchFormatError."""
    ns = {}
    for number, raw in enumerate(text.splitlines(), start=1):
        if not raw.strip():
            continue
        try:
            record = json.loads(raw, parse_constant=_reject_constant)
        except ValueError as error:
            raise BenchFormatError(f"line {number}: malformed JSON ({error})") from None
        if isinstance(record, dict) and isinstance(record.get("op"), str):
            ns[record["op"]] = _number(record, "new_ns", number)
    if not ns:
        raise BenchFormatError("no results")
    return ns


def median(values: list) -> float:
    ordered = sorted(values)
    middle = len(ordered) // 2
    return ordered[middle] if len(ordered) % 2 else (ordered[middle - 1] + ordered[middle]) / 2


def gate_table(runs: list) -> tuple:
    """The table of several processes' results and the [(op, gated ratio)] it gates: per op the median over the processes of
    their ratios (each a median over rounds), new_ns and addon_ns as medians too."""
    ops = [op for op, *_ in runs[0]]
    by_run = [{op: (new_ns, addon_ns, ratio) for op, new_ns, addon_ns, ratio in results} for results in runs]
    width = max(len("op"), max(len(op) for op in ops))
    columns = "".join(f"  {'run ' + str(index + 1):>7}" for index in range(len(runs)))
    lines = [f"{'op':<{width}}  {'new_ns':>12}  {'addon_ns':>12}{columns}  {'ratio':>6}"]
    gated = []
    for op in ops:
        values = [run[op] for run in by_run]
        ratio = median([value[2] for value in values])
        gated.append((op, ratio))
        mark = "  FAIL" if ratio > LIMIT else ""
        cells = "".join(f"  {value[2]:>7.3f}" for value in values)
        lines.append(f"{op:<{width}}  {median([v[0] for v in values]):>12.2f}  {median([v[1] for v in values]):>12.2f}{cells}  {ratio:>6.3f}{mark}")
    return "\n".join(lines), gated


def regression_table(runs: list, baselines: list) -> tuple:
    """Zodiac against the baseline (main): per op the median over the run pairs of head's new_ns over main's, and their geometric
    mean. Returns (text, [(op, change)], geomean, ops that main does not have)."""
    heads = [{op: new_ns for op, new_ns, _, _ in results} for results in runs]
    ops = [op for op, *_ in runs[0]]
    compared = [op for op in ops if all(op in base for base in baselines)]
    new = [op for op in ops if op not in compared]
    width = max(len("op"), max(len(op) for op in ops))
    lines = [f"{'op':<{width}}  {'main ns':>12}  {'head ns':>12}  {'head/main':>9}"]
    changes = []
    for op in compared:
        change = median([head[op] / base[op] for head, base in zip(heads, baselines) if base[op] > 0])
        changes.append((op, change))
        mark = "  FAIL" if change > REGRESSION_LIMIT else ""
        lines.append(f"{op:<{width}}  {median([b[op] for b in baselines]):>12.2f}  {median([h[op] for h in heads]):>12.2f}  {change:>9.3f}{mark}")
    geomean = math.exp(sum(math.log(c) for _, c in changes) / len(changes)) if changes else 1.0
    lines.append(f"geometric mean head/main: {geomean:.4f} over {len(changes)} operations ({len(runs)} pairs)")
    if new:
        lines.append("not in main, not compared: " + ", ".join(new))
    return "\n".join(lines), changes, geomean, new


def _check(argv) -> int:
    parser = argparse.ArgumentParser(description="Fail when a Zodiac type is more than 5 % slower than its sample add-on, or than main.")
    parser.add_argument("results", type=Path, nargs="+", help="bench_types' JSON lines, one file per process (the gate runs 3)")
    parser.add_argument("--expected", type=Path, required=True, help="the operation names, one per line (bench_types --list)")
    parser.add_argument("--baseline", type=Path, nargs="+", help="main's bench results, one file per process, paired with the results in order")
    args = parser.parse_args(argv)
    paths = list(args.results) + [args.expected] + list(args.baseline or [])
    texts = {}
    for path in paths:
        try:
            texts[path] = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as error:
            print(f"check_bench: cannot read {path}: {error}", file=sys.stderr)
            return 2
    runs = []
    machine = None
    for path in args.results:
        try:
            this_machine, results = read(texts[path])
            check_complete(results, texts[args.expected])
        except BenchFormatError as error:
            print(f"check_bench: {path}: {error}", file=sys.stderr)
            return 2
        if machine is not None and this_machine != machine:
            print(f"check_bench: {path} was measured on {describe(this_machine)}, not on the first file's machine", file=sys.stderr)
            return 2
        if runs and [op for op, *_ in results] != [op for op, *_ in runs[0]]:
            print(f"check_bench: {path} lists its operations in another order than {args.results[0]}", file=sys.stderr)
            return 2
        machine = this_machine
        runs.append(results)
    baselines = []
    if args.baseline:
        if len(args.baseline) != len(args.results):
            print(f"check_bench: {len(args.baseline)} baseline file(s) for {len(args.results)} result file(s); they are paired in order", file=sys.stderr)
            return 2
        for path in args.baseline:
            try:
                baselines.append(baseline_ns(texts[path]))
            except BenchFormatError as error:
                print(f"check_bench: {path}: {error}", file=sys.stderr)
                return 2
    print(describe(machine), flush=True)
    print(f"calibration: {calibration(texts[args.results[0]])}; gated on the median of {len(runs)} process(es)", flush=True)
    text, gated = gate_table(runs)
    print(text, flush=True)
    failed = False
    slower = [(op, ratio) for op, ratio in gated if ratio > LIMIT]
    if slower:
        failed = True
        print(f"check_bench: {len(slower)} operation(s) more than {round((LIMIT - 1) * 100)} % slower than the sample add-ons:", file=sys.stderr)
        for op, ratio in slower:
            print(f"  {op}: ratio {ratio:.3f}", file=sys.stderr)
    else:
        print(f"check_bench: all {len(gated)} operations within the {LIMIT} limit.", flush=True)
    if baselines:
        text, changes, geomean, _ = regression_table(runs, baselines)
        print("", flush=True)
        print(f"Against main (Zodiac's ns, head over main; limits {REGRESSION_LIMIT} per op, {REGRESSION_GEOMEAN} geometric mean):", flush=True)
        print(text, flush=True)
        regressed = [(op, change) for op, change in changes if change > REGRESSION_LIMIT]
        if regressed or geomean > REGRESSION_GEOMEAN:
            failed = True
            print(f"check_bench: slower than main: {len(regressed)} operation(s) above {REGRESSION_LIMIT}, geometric mean {geomean:.4f} (limit {REGRESSION_GEOMEAN}):", file=sys.stderr)
            for op, change in regressed:
                print(f"  {op}: head/main {change:.3f}", file=sys.stderr)
        else:
            print(f"check_bench: no operation slower than main beyond {REGRESSION_LIMIT}, geometric mean {geomean:.4f}.", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
