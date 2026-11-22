#!/usr/bin/env python3
"""Checks bench_types' results against the speed gate of spec 8.4: no Zodiac type may be more than 5 % slower than its sample add-on.

    python -I tools/check_bench.py <results.json> --expected <ops.txt>

The results file holds one JSON object per line, {"op": name, "new_ns": n, "addon_ns": n, "ratio": r}, as bench_types writes it: new_ns
and addon_ns are the median nanoseconds per iteration of Zodiac's types and of the add-ons, ratio the median of the per-round ratios
new/addon. Exactly one line describes the machine instead: {"machine": {"cpu": brand, "logical_cores": n, "os": build}}, printed above
the table so every log names the CPU it measured. The expected file holds one operation name per line, as `bench_types --list` prints
them (the bench is the one source of the operation list), and the results must cover exactly those operations. Blank lines are ignored
in both. Prints the machine and a table of every operation to stdout.

Exit codes: 0 every ratio is at most LIMIT; 1 some ratio is above LIMIT (each such operation is named on stderr); 2 either file is missing
or unreadable, a line is not such an object (malformed JSON, a missing or non-numeric field, a negative or non-finite number, an empty op
name), an operation appears twice, the file holds no results, the machine line is missing, repeated or malformed, the expected list is
empty, or an expected operation is missing or an unknown one is present (each named on stderr), the arguments are wrong, or the check
itself fails unexpectedly. Only 1 means "too slow": the report-only Linux job tolerates exactly that code.
"""

import argparse
import json
import math
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


def _check(argv) -> int:
    parser = argparse.ArgumentParser(description="Fail when a Zodiac type is more than 5 % slower than its sample add-on.")
    parser.add_argument("results", type=Path, help="bench_types' JSON lines")
    parser.add_argument("--expected", type=Path, required=True, help="the operation names, one per line (bench_types --list)")
    args = parser.parse_args(argv)
    texts = []
    for path in (args.results, args.expected):
        try:
            texts.append(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError) as error:
            print(f"check_bench: cannot read {path}: {error}", file=sys.stderr)
            return 2
    try:
        machine, results = read(texts[0])
        check_complete(results, texts[1])
    except BenchFormatError as error:
        print(f"check_bench: {args.results}: {error}", file=sys.stderr)
        return 2
    print(describe(machine), flush=True)
    print(table(results), flush=True)
    slower = [(op, ratio) for op, _, _, ratio in results if ratio > LIMIT]
    if slower:
        print(f"check_bench: {len(slower)} operation(s) more than {round((LIMIT - 1) * 100)} % slower than the sample add-ons:", file=sys.stderr)
        for op, ratio in slower:
            print(f"  {op}: ratio {ratio:.3f}", file=sys.stderr)
        return 1
    print(f"check_bench: all {len(results)} operations within the {LIMIT} limit.", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
