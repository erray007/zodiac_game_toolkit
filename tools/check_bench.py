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

The regression gate (--baseline, one file per process of main's bench on the same machine, as many as results files; the gate runs them
in alternation) is one-sided: it fails when head is slower. Each process's time is its new_ns divided by its machine-speed factor. Per
operation both have, head's best (minimum over its processes) over main's median is at most that operation's limit; over all of them,
the geometric mean of head's best over main's best is at most the geometric-mean limit; both from --limits
(tools/bench_regression_limits.json; the file says how they were derived). Why these (Task 5c, 96 gate-replica A/A' jobs):
- Head's best: the slow modes seen (the Windows NT heap's per-process slow request sizes) only slow a process down, and in up to 2 of 3
  processes at once, so a median of head's processes reads them as a regression; the fastest one measured the code.
- Main's median: one process of a side is sometimes several percent fast on one operation (its copy's placement; once 13 % on
  array_find_string), which main's best would read as a regression of head. Main's median ignores one such process, and what makes main
  slow only makes head look better, which a one-sided gate may allow. Held out one job at a time, best over best failed 3 of 96 jobs at
  the limits the other 95 give, best over median 0.
- The geometric mean takes best over best: over every operation one operation's outlier barely moves it, and it stays the sensitive
  check for a slowdown spread over everything.
- The speed factor is the median over the compared operations of that process's addon_ns over the operation's median addon_ns across the
  processes of its own side (head's or main's, never both: Task 5c fix round 1). Within a side the add-on code is one build, so the factor
  follows the machine (a runner that gets faster or slower for a whole process, as in one gate replica where one process ran every operation
  10 % faster on both sides) and, as a median over operations, not one operation's slow mode or placement; without it a minimum would read
  such a process as a change of everything on its side. Taken within each side, it never divides away a difference between the two
  builds: a head whose whole build is slower (a dropped optimization, a changed AngelScript patch, a harness change), on both its sides,
  keeps factors near 1 and fails.
The factors are printed, and so is each operation's limit and the median over the pairs of head's raw new_ns over main's, for information.
Operations main does not have are listed, not compared; a time of 0 on either side fails with the operation named (exit 2). The comparison
is report-only, with a REGRESSION GATE REPORT-ONLY warning on stdout and stderr (and, under GitHub Actions, a ::warning:: annotation on
stdout), when main's results were calibrated otherwise than
head's (no "calibrate" field: a bench older than per-side calibration), since head/main then measures the calibration, not the code; it
gates by itself once main's bench records the same mode. Every results file (head's processes) must record the same calibration mode.

Exit codes: 0 both gates pass; 1 some gated ratio is above LIMIT or the regression gate fails (each such operation is named on stderr); 2 a
file is missing or unreadable, a line is not such an object (malformed JSON, a missing or non-numeric field, a negative or non-finite
number, an empty op name), an operation appears twice, a file holds no results, the machine line is missing, repeated, malformed or differs
between files, the results files record different calibration modes, the expected list is empty, an expected operation is missing or an
unknown one is present (each named on stderr), the baseline files are not as many as the results files, an operation has a time of 0 on
either side of the regression gate, the arguments are wrong, or the check itself fails unexpectedly. Only 1 means "too
slow": the report-only Linux job tolerates exactly that code.
"""

import argparse
import json
import math
import os
import sys
import traceback
from pathlib import Path

LIMIT = 1.05
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
    """{op: (new_ns, addon_ns)} of a baseline run (main's bench, which may predate the machine line); raises BenchFormatError."""
    ns = {}
    for number, raw in enumerate(text.splitlines(), start=1):
        if not raw.strip():
            continue
        try:
            record = json.loads(raw, parse_constant=_reject_constant)
        except ValueError as error:
            raise BenchFormatError(f"line {number}: malformed JSON ({error})") from None
        if isinstance(record, dict) and isinstance(record.get("op"), str):
            ns[record["op"]] = (_number(record, "new_ns", number), _number(record, "addon_ns", number))
    if not ns:
        raise BenchFormatError("no results")
    return ns


def calibration_mode(text: str):
    """The "calibrate" field of a results text's operation lines ("time", "count"), None when they have none (a bench older than
    Task 5b fix round 4, which calibrated both sides together), or "mixed" when its lines disagree."""
    modes = set()
    for raw in text.splitlines():
        if not raw.strip():
            continue
        try:
            record = json.loads(raw, parse_constant=_reject_constant)
        except ValueError:
            continue
        if isinstance(record, dict) and "op" in record:
            modes.add(record.get("calibrate"))
    if modes == {None} or not modes:
        return None
    return modes.pop() if len(modes) == 1 else "mixed"


DEFAULT_LIMITS = Path(__file__).resolve().parent / "bench_regression_limits.json"


def read_limits(path: Path) -> dict:
    """{"default": d, "geomean": g, "ops": {op: limit}} from the limits file; raises BenchFormatError."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"), parse_constant=_reject_constant)
    except (OSError, UnicodeDecodeError, ValueError) as error:
        raise BenchFormatError(f"cannot read the regression limits {path}: {error}") from None
    def limit(value, where):
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 1:
            raise BenchFormatError(f"{path}: {where} must be a number of at least 1")
        return float(value)
    if not isinstance(data, dict) or not isinstance(data.get("ops", {}), dict):
        raise BenchFormatError(f"{path}: not an object with \"default\", \"geomean\" and \"ops\"")
    ops = {}
    for op, entry in data.get("ops", {}).items():
        ops[op] = limit(entry.get("limit") if isinstance(entry, dict) else None, f"ops.{op}.limit")
    return {"default": limit(data.get("default"), "default"), "geomean": limit(data.get("geomean"), "geomean"), "ops": ops}


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


def speed_factors(addon: list, ops: list) -> list:
    """Each process's machine-speed factor: the median over ops of its addon_ns over that op's median addon_ns across all the processes
    (addon is one {op: addon_ns} per process). Ops whose add-on time is 0 in every process are left out; with none left, every factor is
    1."""
    usable = [op for op in ops if median([process[op] for process in addon]) > 0]
    if not usable:
        return [1.0] * len(addon)
    typical = {op: median([process[op] for process in addon]) for op in usable}
    return [median([process[op] / typical[op] for op in usable]) for process in addon]


def regression_table(runs: list, baselines: list, limits: dict) -> tuple:
    """Zodiac against the baseline (main): per op head's best over main's median (of new_ns over the process's speed factor, see the
    module text) against that op's limit, and the geometric mean over ops of head's best over main's best. Returns (text,
    [(op, change, limit)], geomean, ops that main does not have). Raises BenchFormatError naming an op that has a time of 0 ns on either
    side (no ratio can be taken)."""
    heads = [{op: (new_ns, addon_ns) for op, new_ns, addon_ns, _ in results} for results in runs]
    ops = [op for op, *_ in runs[0]]
    compared = [op for op in ops if all(op in base for base in baselines)]
    new = [op for op in ops if op not in compared]
    for op in compared:
        if any(base[op][0] <= 0 for base in baselines):
            raise BenchFormatError(f"the baseline gives {op} a time of 0 ns, so head cannot be compared with it")
        if any(head[op][0] <= 0 for head in heads):
            raise BenchFormatError(f"head gives {op} a time of 0 ns, so it cannot be compared with main")
    # Within each side, so a difference between the two builds (both of head's sides slower, say) is never divided away.
    head_factors = speed_factors([{op: side[op][1] for op in compared} for side in heads], compared)
    main_factors = speed_factors([{op: side[op][1] for op in compared} for side in baselines], compared)
    width = max(len("op"), max(len(op) for op in ops))
    lines = [f"process speed factors (the add-on side's median over operations, within each side): head {' '.join(f'{f:.3f}' for f in head_factors)}, main {' '.join(f'{f:.3f}' for f in main_factors)}"]
    lines.append(f"{'op':<{width}}  {'main median':>12}  {'head best':>12}  {'head/main':>9}  {'best/best':>9}  {'pair med':>8}  {'limit':>6}")
    changes = []
    best_changes = []
    for op in compared:
        head_ns = [head[op][0] / factor for head, factor in zip(heads, head_factors)]
        main_ns = [base[op][0] / factor for base, factor in zip(baselines, main_factors)]
        head_best = min(head_ns)
        main_typical = median(main_ns)
        change = head_best / main_typical
        best_changes.append(head_best / min(main_ns))
        paired = median([head[op][0] / base[op][0] for head, base in zip(heads, baselines)])
        limit = limits["ops"].get(op, limits["default"])
        changes.append((op, change, limit))
        mark = "  FAIL" if change > limit else ""
        lines.append(f"{op:<{width}}  {main_typical:>12.2f}  {head_best:>12.2f}  {change:>9.3f}  {best_changes[-1]:>9.3f}  {paired:>8.3f}  {limit:>6.2f}{mark}")
    geomean = math.exp(sum(math.log(c) for c in best_changes) / len(best_changes)) if best_changes else 1.0
    lines.append(f"geometric mean of best/best: {geomean:.4f} over {len(changes)} operations (best of {len(runs)} and {len(baselines)} processes), limit {limits['geomean']}")
    if new:
        lines.append("not in main, not compared: " + ", ".join(new))
    return "\n".join(lines), changes, geomean, new


def _check(argv) -> int:
    parser = argparse.ArgumentParser(description="Fail when a Zodiac type is more than 5 % slower than its sample add-on, or than main.")
    parser.add_argument("results", type=Path, nargs="+", help="bench_types' JSON lines, one file per process (the gate runs 3)")
    parser.add_argument("--expected", type=Path, required=True, help="the operation names, one per line (bench_types --list)")
    parser.add_argument("--baseline", type=Path, nargs="+", help="main's bench results, one file per process, paired with the results in order")
    parser.add_argument("--limits", type=Path, default=DEFAULT_LIMITS, help="the regression gate's per-operation limits (tools/bench_regression_limits.json)")
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
    # Every head process must be calibrated alike, or the median and best over them mix two measurements.
    head_modes = {path: calibration_mode(texts[path]) for path in args.results}
    if len(set(head_modes.values())) > 1 or "mixed" in head_modes.values():
        print("check_bench: the results files disagree on the calibration mode: " + ", ".join(f"{path} {mode}" for path, mode in head_modes.items()), file=sys.stderr)
        return 2
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
        try:
            limits = read_limits(args.limits)
            text, changes, geomean, _ = regression_table(runs, baselines, limits)
        except BenchFormatError as error:
            print(f"check_bench: {error}", file=sys.stderr)
            return 2
        # Both sides must be calibrated alike, or head/main measures the calibration: a bench before Task 5b fix round 4 timed both sides
        # over one shared window, which reads a fast operation paired with a slow add-on several times slower than per-side windows.
        head_mode = calibration_mode(texts[args.results[0]])
        base_modes = {calibration_mode(texts[path]) for path in args.baseline}
        report_only = base_modes != {head_mode} or head_mode is None
        print("", flush=True)
        print(f"Against main (Zodiac's ns, head's best over main's median per op, best over best in the geometric mean; per-op limits from {args.limits.name}, default {limits['default']}, geometric mean {limits['geomean']}):", flush=True)
        if report_only:
            reason = "main's bench records no calibration (it predates per-side calibration)" if base_modes == {None} else f"main's bench calibrated {sorted(str(m) for m in base_modes)}, head {head_mode}"
            warning = f"REGRESSION GATE REPORT-ONLY: {reason}; the two sides are not measured alike, so head/main is not gated"
            print(f"check_bench: {warning}", flush=True)
            print(f"check_bench: {warning}", file=sys.stderr)
            # On GitHub Actions also an annotation, so the run's summary page shows the warning without opening the log.
            if os.environ.get("GITHUB_ACTIONS") == "true":
                print(f"::warning title=Regression gate report-only::{warning}", flush=True)
        print(text, flush=True)
        regressed = [(op, change, limit) for op, change, limit in changes if change > limit]
        if regressed or geomean > limits["geomean"]:
            if not report_only:
                failed = True
            print(f"check_bench: {'(report-only) ' if report_only else ''}slower than main: {len(regressed)} operation(s) above their limits, geometric mean {geomean:.4f} (limit {limits['geomean']}):", file=sys.stderr)
            for op, change, limit in regressed:
                print(f"  {op}: head/main {change:.3f} (limit {limit:.2f})", file=sys.stderr)
        else:
            print(f"check_bench: no operation slower than main beyond its limit, geometric mean {geomean:.4f}.", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
