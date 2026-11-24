#!/usr/bin/env python3
"""The placement and interference experiment of Task 5b fix round 3 (report-only): does the reference (add-on) side's time drift on its own
across no-op rebuilds, and does one side's presence in the process shift the other's time?

    python -I tools/bench_reference.py --variant v0=<bench_types> --variant v16=<bench_types> ... [--hooks <hooks bench_types>]
        --out-dir <dir> [--rounds 101] [--within 8] [--each 3] --op NAME...

The variants are builds of the same sources that differ only by a no-op relink (ZODIAC_BENCH_PAD, unused nop bytes); the first one is the
base. Runs, each its own process, interleaved so a drift of the machine spreads over every kind:
  a) the base `within` times in each of three modes: --reference-only, --zodiac-only and mixed (both sides, as the gate runs them);
  b) every other variant `each` times in each mode (the base's first `each` runs of a) count for it);
  c) with --hooks (a Windows ZODIAC_TEST_HOOKS build): 4 times mixed with ZODIAC_PRIVATE_HEAP=1 (Zodiac's own allocations on a heap of
     their own, core/types/heap.hpp), mixed without it, and --reference-only.
Prints, per operation, each side's spread within one binary and across the rebuilds, how much the other side's presence moves each side,
and the private heap arm; writes every run to <dir>/runs.jsonl and the tables to <dir>/tables.txt. Exit codes: 0 done, 1 bad arguments,
2 a run failed.
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
from pathlib import Path

# Mixed first in each group, so with --pin the alone runs after it time exactly its reps.
MODES = {"mixed": [], "reference": ["--reference-only"], "zodiac": ["--zodiac-only"]}
# Functions whose addresses show what a no-op relink moved (from the bench's link map, bench_types.map).
MAPPED = ("StringCharAt", "zodiac::String::script_byte_at(int) const", "asCContext::ExecuteNext()", "CScriptArray::Find", "zodiac::ScriptArray::native_find")


BENCH_ARGS = []


def run(exe: Path, out: Path, rounds: int, ops: list, mode: str, env_extra: dict, pin: Path = None) -> dict:
    command = [str(exe), "--rounds", str(rounds), "--out", str(out)] + MODES[mode] + BENCH_ARGS + (["--pin-reps", str(pin)] if pin else [])
    for op in ops:
        command += ["--op", op]
    env = dict(os.environ)
    env.pop("ZODIAC_PRIVATE_HEAP", None)
    env.update(env_extra)
    done = subprocess.run(command, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
    out.with_suffix(".stderr.txt").write_text(done.stderr, encoding="utf-8")
    if done.returncode != 0:
        raise RuntimeError(f"{exe} {mode} exited with {done.returncode}:\n{done.stderr[-2000:]}")
    result = {"machine": None, "heap": "", "ops": {}}
    for line in done.stderr.splitlines():
        if line.startswith("bench_types: zodiac heap: "):
            result["heap"] = line[len("bench_types: zodiac heap: "):]
    for raw in out.read_text(encoding="utf-8").splitlines():
        if not raw.strip():
            continue
        record = json.loads(raw)
        if "machine" in record:
            result["machine"] = record["machine"]["cpu"]
        elif mode == "mixed":
            result["ops"][record["op"]] = {"zodiac": record["new_ns"], "reference": record["addon_ns"], "ratio": record["ratio"]}
        else:
            result["ops"][record["op"]] = {mode: record["ns"], "p10": record["p10"], "p90": record["p90"]}
    return result


def mapped(exe: Path) -> dict:
    found = {}
    path = Path(str(exe) + ".map")
    if not path.exists():
        return found
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        for name in MAPPED:
            if name not in found and name in line:
                found[name] = line.split()[0]
    return found


def spread(values: list) -> float:
    """(max - min) / median, in percent."""
    middle = statistics.median(values)
    return 100.0 * (max(values) - min(values)) / middle if middle else float("nan")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Reference-only and Zodiac-only drift across no-op rebuilds, and cross-interference.")
    parser.add_argument("--variant", action="append", required=True, help="label=path; the first one is the base")
    parser.add_argument("--hooks", type=Path)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=101)
    parser.add_argument("--within", type=int, default=8)
    parser.add_argument("--each", type=int, default=3)
    parser.add_argument("--op", action="append", required=True)
    parser.add_argument("--pin", action="store_true", help="run every alone mode with the reps of the mixed run just before it (--pin-reps)")
    parser.add_argument("--bench-arg", action="append", default=[], help="an argument passed to every bench run (e.g. --calibrate, count); repeatable")
    args = parser.parse_args(argv)
    BENCH_ARGS[:] = args.bench_arg
    variants = []
    for item in args.variant:
        label, _, path = item.partition("=")
        if not path:
            print("bench_reference: --variant needs label=path", file=sys.stderr)
            return 1
        variants.append((label, Path(path)))
    if args.each > args.within:
        print("bench_reference: --each cannot exceed --within", file=sys.stderr)
        return 1
    args.out_dir.mkdir(parents=True, exist_ok=True)
    base_label, base_exe = variants[0]
    records = []
    def one(phase, label, exe, mode, index, env_extra=None):
        name = f"{phase}-{label}-{mode}-{index}"
        # With --pin, an alone run times exactly the reps of the mixed run of its group (written just before it).
        pin = args.out_dir / f"{phase}-{label}-mixed-{index}.json" if args.pin and mode in ("reference", "zodiac") else None
        result = run(exe, args.out_dir / f"{name}.json", args.rounds, args.op, "mixed" if mode == "private" else mode, env_extra or {}, pin)
        record = {"phase": phase, "variant": label, "mode": mode, "index": index, **result}
        records.append(record)
        print(f"bench_reference: {name} {json.dumps(result['ops'])}", file=sys.stderr, flush=True)
    try:
        for label, exe in variants:
            records.append({"phase": "map", "variant": label, "mode": "", "index": 0, "machine": None, "heap": "", "ops": {}, "mapped": mapped(exe)})
        for index in range(args.within):
            for mode in MODES:
                one("a", base_label, base_exe, mode, index)
        for index in range(args.each):
            for label, exe in variants[1:]:
                for mode in MODES:
                    one("b", label, exe, mode, index)
        if args.hooks:
            for index in range(4):
                one("c", "hooks", args.hooks, "mixed", index)
                one("c", "hooks", args.hooks, "private", index, {"ZODIAC_PRIVATE_HEAP": "1"})
                one("c", "hooks", args.hooks, "reference", index)
    except (RuntimeError, OSError, ValueError, KeyError) as error:
        print(f"bench_reference: {error}", file=sys.stderr)
        return 2
    finally:
        with (args.out_dir / "runs.jsonl").open("w", encoding="utf-8") as out:
            for record in records:
                out.write(json.dumps(record) + "\n")
    measured = [r for r in records if r["phase"] != "map"]
    text = render(measured, records, args, variants)
    (args.out_dir / "tables.txt").write_text(text + "\n", encoding="utf-8")
    print(text, flush=True)
    return 0


def render(measured: list, records: list, args, variants: list) -> str:
    """The tables: within uses all of the base's phase-a runs; across uses `each` runs per variant (the base's first ones)."""
    base_label = variants[0][0]
    within = [r for r in measured if r["phase"] == "a"]
    across = [r for r in measured if (r["phase"] == "a" and r["index"] < args.each) or r["phase"] == "b"]
    hooks = [r for r in measured if r["phase"] == "c"]
    labels = [label for label, _ in variants]
    def pick(pool, variant, mode, op, side):
        return [r["ops"][op][side] for r in pool if r["variant"] == variant and r["mode"] == mode]
    lines = [f"machine: {measured[0]['machine']}", ""]
    lines.append("Each side alone: within one binary (the base, all its runs) and across the no-op rebuilds (spread of the variants' medians):")
    lines.append("op | reference-only ns | within % | across % | zodiac-only ns | within % | across % | mixed ratio (median of variants) | ratio across %")
    for op in args.op:
        rw = pick(within, base_label, "reference", op, "reference")
        zw = pick(within, base_label, "zodiac", op, "zodiac")
        rm = [statistics.median(pick(across, v, "reference", op, "reference")) for v in labels]
        zm = [statistics.median(pick(across, v, "zodiac", op, "zodiac")) for v in labels]
        qm = [statistics.median(pick(across, v, "mixed", op, "ratio")) for v in labels]
        lines.append(f"{op} | {statistics.median(rw):.2f} | {spread(rw):.1f} | {spread(rm):.1f} | {statistics.median(zw):.2f} | {spread(zw):.1f} | {spread(zm):.1f} | {statistics.median(qm):.3f} | {spread(qm):.1f}")
    lines.append("")
    lines.append("Per variant (reference-only ns / zodiac-only ns / mixed ratio):")
    for op in args.op:
        cells = [f"{v} {statistics.median(pick(across, v, 'reference', op, 'reference')):.2f}/{statistics.median(pick(across, v, 'zodiac', op, 'zodiac')):.2f}/{statistics.median(pick(across, v, 'mixed', op, 'ratio')):.3f}" for v in labels]
        lines.append(f"{op}: " + "  ".join(cells))
    lines.append("")
    lines.append("Interference (the base, all its runs): each side's ns in the mixed run over its ns alone, in percent, next to that side's own spread:")
    lines.append("op | reference mixed/alone | reference within % | zodiac mixed/alone | zodiac within %")
    for op in args.op:
        ra = statistics.median(pick(within, base_label, "reference", op, "reference"))
        rx = statistics.median(pick(within, base_label, "mixed", op, "reference"))
        za = statistics.median(pick(within, base_label, "zodiac", op, "zodiac"))
        zx = statistics.median(pick(within, base_label, "mixed", op, "zodiac"))
        lines.append(f"{op} | {100 * (rx / ra - 1):+.1f} | {spread(pick(within, base_label, 'reference', op, 'reference')):.1f} | {100 * (zx / za - 1):+.1f} | {spread(pick(within, base_label, 'zodiac', op, 'zodiac')):.1f}")
    if hooks:
        lines.append("")
        lines.append(f"Private heap arm (hooks build, 4 runs each; heap lines: {sorted({r['heap'] for r in hooks})}):")
        lines.append("op | reference ns alone / mixed / mixed with Zodiac on a private heap | zodiac ns mixed / mixed with a private heap")
        for op in args.op:
            def med(mode, side):
                return statistics.median(pick(hooks, "hooks", mode, op, side))
            lines.append(f"{op} | {med('reference', 'reference'):.2f} / {med('mixed', 'reference'):.2f} / {med('private', 'reference'):.2f} | {med('mixed', 'zodiac'):.2f} / {med('private', 'zodiac'):.2f}")
    lines.append("")
    lines.append("Addresses in each variant's link map (what the relink moved):")
    for r in records:
        if r["phase"] == "map":
            lines.append(f"{r['variant']}: " + ", ".join(f"{k} {v}" for k, v in r["mapped"].items()))
    return "\n".join(lines)


if __name__ == "__main__":
    sys.exit(main())
