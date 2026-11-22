#!/usr/bin/env python3
"""Diagnoses a bimodal bench operation (Task 5b): runs bench_types many times, each in its own process, and tells whether a slow mode
follows the table hash seed (the dictionary's layout) or the process (where the allocator and the loader place things).

    python -I tools/bench_diagnose.py --bench <bench_types> --out-dir <dir> [--seeds 64] [--passes 2] [--unfixed 64] [--rounds 101]
        [--focus dict_iterate_10] [--slow 1.3] [--op NAME]...

The bench must come from a tree configured with ZODIAC_TEST_HOOKS=ON (only there does ZODIAC_TABLE_HASH_SEED fix the keys; the script
checks that it did). Runs, interleaved so a drift of the machine spreads over every kind: seed s = 1..seeds in each of passes passes (the
same seed in two processes: a layout cause gives both the same mode), and unfixed processes with the variable removed from the
environment. Each run is bench_types --diagnostic --layout --rounds R --op ... (default ops: the size-10 dictionary operations and the
diagnostic ones that split dict_iterate_10 into keys() and the lookups; --all runs every gated operation as the CI gate does, plus
dict_keys_10 and dict_get_ordered_10 or the --op ones: the slow mode of dict_iterate_10 has only been seen in such a full run).

Writes <dir>/runs.jsonl (one object per process: kind, seed, pass, machine, every op's ratio and nanoseconds, and the layout of the focus
op's four Zodiac tables) and each run's raw output, and prints a table per process and a summary: how many processes of each kind were
slow (focus ratio above --slow), whether the two passes of each seed agree, and the layout fields compared between slow and normal
processes. Exit codes: 0 done; 1 bad arguments; 2 a bench run failed or the seed override did not take (one seed gave two different
keys, or two unfixed processes the same ones).
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
from pathlib import Path

SEED_VARIABLE = "ZODIAC_TABLE_HASH_SEED"
DEFAULT_OPS = ("dict_insert_10", "dict_lookup_10", "dict_iterate_10", "dict_keys_10", "dict_get_ordered_10")
LAYOUT_FIELDS = ("probe_total", "probe_max", "lines_touched", "index_size", "index_mod4096", "segment0_mod4096", "dictionary_mod4096", "keys_array_mod4096")
LAYOUT_PREFIX = "bench_types: layout "


class DiagnoseError(Exception):
    """A run failed or its output is not what bench_types writes."""


def parse_run(results_text: str, stderr_text: str, focus: str) -> dict:
    """The machine, the {op: {ratio, new_ns, addon_ns}} results and the focus op's per-engine layouts of one run."""
    machine = None
    ops = {}
    for raw in results_text.splitlines():
        if not raw.strip():
            continue
        record = json.loads(raw)
        if "machine" in record:
            machine = record["machine"]
        else:
            ops[record["op"]] = {"ratio": record["ratio"], "new_ns": record["new_ns"], "addon_ns": record["addon_ns"]}
    layouts = []
    for raw in stderr_text.splitlines():
        if not raw.startswith(LAYOUT_PREFIX):
            continue
        record = json.loads(raw[len(LAYOUT_PREFIX):])
        if record["op"] == focus:
            layouts.append(record["table"])
    if machine is None or focus not in ops or not layouts:
        raise DiagnoseError(f"the run's output lacks the machine line, {focus} or its layout")
    return {"machine": machine, "ops": ops, "layouts": layouts}


def run_once(bench: Path, out_dir: Path, name: str, seed, rounds: int, ops: list, focus: str) -> dict:
    env = dict(os.environ)
    env.pop(SEED_VARIABLE, None)
    if seed is not None:
        env[SEED_VARIABLE] = str(seed)
    results = out_dir / f"{name}.json"
    command = [str(bench), "--diagnostic", "--layout", "--rounds", str(rounds), "--out", str(results)]
    for op in ops:
        command += ["--op", op]
    done = subprocess.run(command, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
    (out_dir / f"{name}-stderr.txt").write_text(done.stderr, encoding="utf-8")
    if done.returncode != 0:
        raise DiagnoseError(f"{name}: bench_types exited with {done.returncode}:\n{done.stderr[-2000:]}")
    return parse_run(results.read_text(encoding="utf-8"), done.stderr, focus)


def schedule(seeds: int, passes: int, unfixed: int) -> list:
    """(kind, seed, pass) of every run, interleaved: round k runs seed k of each pass and unfixed process k."""
    runs = []
    for k in range(1, max(seeds, unfixed) + 1):
        for number in range(1, passes + 1):
            if k <= seeds:
                runs.append(("seed", k, number))
            if number == 1 and k <= unfixed:
                runs.append(("unfixed", None, k))
    return runs


def check_override(records: list) -> list:
    """Problems with the seed override: a seed whose processes drew different keys, or unfixed processes that drew the same."""
    problems = []
    by_seed = {}
    unfixed = {}
    for record in records:
        key = record["layouts"][0]["table_key0"]
        if record["kind"] == "seed":
            by_seed.setdefault(record["seed"], set()).add(key)
        else:
            unfixed.setdefault(key, []).append(record["pass"])
    for seed, keys in sorted(by_seed.items()):
        if len(keys) != 1:
            problems.append(f"seed {seed} drew {len(keys)} different keys: {SEED_VARIABLE} did not take (is this a ZODIAC_TEST_HOOKS tree?)")
    for key, numbers in unfixed.items():
        if len(numbers) > 1:
            problems.append(f"unfixed processes {numbers} drew the same keys {key}: the variable leaked into them")
    return problems


def mean(values: list) -> float:
    return statistics.fmean(values) if values else float("nan")


def summary(records: list, focus: str, slow: float) -> str:
    lines = []
    ops = list(records[0]["ops"])
    if len(ops) > 8:
        # A full run: the table shows the size-10 dictionary operations, the focus and string_byte_ascii (runs.jsonl has them all).
        ops = [op for op in ops if (op.startswith("dict_") and op.endswith("_10")) or op in (focus, "string_byte_ascii")]
    machines = sorted({r["machine"]["cpu"] + " / " + str(r["machine"]["logical_cores"]) + " cores / " + r["machine"]["os"] for r in records})
    lines.append("machines: " + "; ".join(machines))
    header = ["kind", "seed", "pass"] + ops + [f"{focus}_z_ns", "probe_total", "lines", "index_mod", "seg0_mod", "keys_mod"]
    lines.append("  ".join(header))
    for r in records:
        first = r["layouts"][0]
        cells = [r["kind"], str(r["seed"] if r["seed"] is not None else "-"), str(r["pass"])]
        cells += [f"{r['ops'][op]['ratio']:.3f}" for op in ops]
        cells += [f"{r['ops'][focus]['new_ns']:.1f}", str(sum(t["probe_total"] for t in r["layouts"])), str(sum(t["lines_touched"] for t in r["layouts"])),
                  str(first["index_mod4096"]), str(first["segment0_mod4096"]), str(first.get("keys_array_mod4096", "-"))]
        lines.append("  ".join(cells))
    lines.append("")
    for kind in ("seed", "unfixed"):
        group = [r for r in records if r["kind"] == kind]
        if not group:
            continue
        slow_group = [r for r in group if r["ops"][focus]["ratio"] > slow]
        normal = [r for r in group if r["ops"][focus]["ratio"] <= slow]
        lines.append(f"{kind}: {len(slow_group)} of {len(group)} processes slow ({focus} ratio above {slow})")
        for name, part in (("slow", slow_group), ("normal", normal)):
            if not part:
                continue
            cells = [f"{op} {statistics.median(r['ops'][op]['ratio'] for r in part):.3f}" for op in ops]
            lines.append(f"  {name}: median ratios " + ", ".join(cells) + f"; {focus} Zodiac ns {statistics.median(r['ops'][focus]['new_ns'] for r in part):.1f}, add-on ns {statistics.median(r['ops'][focus]['addon_ns'] for r in part):.1f}")
    seeds = {}
    for r in records:
        if r["kind"] == "seed":
            seeds.setdefault(r["seed"], []).append(r["ops"][focus]["ratio"] > slow)
    both = sum(1 for modes in seeds.values() if len(modes) > 1 and all(modes))
    one = sum(1 for modes in seeds.values() if len(modes) > 1 and any(modes) and not all(modes))
    none = sum(1 for modes in seeds.values() if len(modes) > 1 and not any(modes))
    if both + one + none:
        lines.append(f"seeds run more than once: {both} slow in every pass, {one} slow in some passes only, {none} never slow")
        lines.append("  (a layout cause makes every pass of a seed agree; a process cause makes the slow passes scatter over the seeds)")
    slow_all = [r for r in records if r["ops"][focus]["ratio"] > slow]
    normal_all = [r for r in records if r["ops"][focus]["ratio"] <= slow]
    if slow_all and normal_all:
        lines.append("layout of the focus tables (mean over the four Zodiac engines' tables), slow against normal processes:")
        for field in LAYOUT_FIELDS:
            def per_run(r):
                values = [t[field] for t in r["layouts"] if field in t]
                return mean(values)
            lines.append(f"  {field}: slow {mean([per_run(r) for r in slow_all]):.1f}, normal {mean([per_run(r) for r in normal_all]):.1f}")
    return "\n".join(lines)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Tell whether a slow bench mode follows the table hash seed or the process.")
    parser.add_argument("--bench", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--seeds", type=int, default=64)
    parser.add_argument("--passes", type=int, default=2)
    parser.add_argument("--unfixed", type=int, default=64)
    parser.add_argument("--rounds", type=int, default=101)
    parser.add_argument("--focus", default="dict_iterate_10")
    parser.add_argument("--slow", type=float, default=1.3)
    parser.add_argument("--op", action="append", default=None)
    parser.add_argument("--all", action="store_true", help="every gated operation (bench_types --list) plus the --op ones, as the CI gate runs them")
    args = parser.parse_args(argv)
    if args.all:
        listed = subprocess.run([str(args.bench), "--list"], stdout=subprocess.PIPE, text=True, check=True).stdout.split()
        ops = listed + [op for op in (args.op or ["dict_keys_10", "dict_get_ordered_10"]) if op not in listed]
    else:
        ops = args.op or list(DEFAULT_OPS)
    if args.focus not in ops:
        ops.append(args.focus)
    if args.seeds < 0 or args.unfixed < 0 or args.passes < 1 or args.seeds + args.unfixed == 0:
        print("bench_diagnose: --seeds and --unfixed must be at least 0 (not both 0), --passes at least 1", file=sys.stderr)
        return 1
    args.out_dir.mkdir(parents=True, exist_ok=True)
    records = []
    runs = schedule(args.seeds, args.passes, args.unfixed)
    try:
        for index, (kind, seed, number) in enumerate(runs, start=1):
            name = f"seed{seed}-pass{number}" if kind == "seed" else f"unfixed{number}"
            record = run_once(args.bench, args.out_dir, name, seed, args.rounds, ops, args.focus)
            record.update({"kind": kind, "seed": seed, "pass": number})
            records.append(record)
            print(f"bench_diagnose: {index} of {len(runs)}: {name} {args.focus} {record['ops'][args.focus]['ratio']:.3f}", file=sys.stderr, flush=True)
    except (DiagnoseError, OSError, ValueError, KeyError) as error:
        print(f"bench_diagnose: {error}", file=sys.stderr)
        return 2
    with (args.out_dir / "runs.jsonl").open("w", encoding="utf-8") as out:
        for record in records:
            out.write(json.dumps(record) + "\n")
    text = summary(records, args.focus, args.slow)
    (args.out_dir / "summary.txt").write_text(text + "\n", encoding="utf-8")
    print(text, flush=True)
    problems = check_override(records)
    for problem in problems:
        print(f"bench_diagnose: {problem}", file=sys.stderr)
    return 2 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
