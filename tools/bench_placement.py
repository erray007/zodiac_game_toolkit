#!/usr/bin/env python3
"""The placement study of Task 5c (report-only, never the gate): does an operation's speed follow the file it runs from, and is that ASLR?

    python -I tools/bench_placement.py --variant aslr=<bench_types> --variant fixed=<bench_types> --out-dir <dir>
        [--copies 6] [--runs 2] [--rounds 101] [--game SECONDS] [--bench-arg=...]... --op NAME...

With --game, after the runs every copy also runs one script game loop of that many seconds (bench_types --game-loop), and the tables
start with each variant's median frame-time percentiles.

Each variant is a bench_types build (here the release bench as the gate links it, and the same bench linked without ASLR,
-DZODIAC_BENCH_FIXED_BASE=ON). Each is copied `copies` times (with its link map) into <dir>/copies/<label>-<k>/: Windows draws an image
base per image file, so the copies of one build load at different bases, and every process of one copy at the same base. The runs, each
its own process, are interleaved (for each run index, for each copy, for each variant) so a drift of the machine spreads over all of them.
Every run prints its image base (bench_types' stderr line) and the gate's numbers for the operations.

Prints, per variant and operation, the spread of the ratio, of Zodiac's ns and of the add-on's ns within one copy (its runs) and across
copies (the spread of the copies' medians); then every copy's base and medians; then, from the copies' medians, how often a gate that runs
3 processes of one file, or one process of each of 3 copies, would see an operation's ratio above 1.05. Writes every run to
<dir>/runs.jsonl and the tables to <dir>/tables.txt. Exit codes: 0 done, 1 bad arguments, 2 a run failed.
"""

import argparse
import itertools
import json
import re
import shutil
import statistics
import subprocess
import sys
from pathlib import Path

LIMIT = 1.05
# Functions whose link-map addresses show each copy's layout (the same in every copy of one build; the base moves them all together).
MAPPED = ("zodiac::StringBindingAccess::byte_at(", "StringCharAt", "asCContext::ExecuteNext()", "CallSystemFunctionNative")
BASE = re.compile(r"^bench_types: image base (?:0x)?([0-9A-Fa-f]+)\s*$")


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


def run(exe: Path, out: Path, rounds: int, ops: list, bench_args: list) -> dict:
    command = [str(exe), "--rounds", str(rounds), "--out", str(out)] + bench_args
    for op in ops:
        command += ["--op", op]
    done = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
    out.with_suffix(".stderr.txt").write_text(done.stderr, encoding="utf-8")
    if done.returncode != 0:
        raise RuntimeError(f"{exe} exited with {done.returncode}:\n{done.stderr[-2000:]}")
    result = {"machine": None, "image_base": None, "ops": {}}
    for line in done.stderr.splitlines():
        match = BASE.match(line)
        if match:
            result["image_base"] = int(match.group(1), 16)
    for raw in out.read_text(encoding="utf-8").splitlines():
        if not raw.strip():
            continue
        record = json.loads(raw)
        if "machine" in record:
            result["machine"] = record["machine"]["cpu"]
        else:
            result["ops"][record["op"]] = {"ratio": record["ratio"], "zodiac": record["new_ns"], "reference": record["addon_ns"]}
    return result


def spread(values: list) -> float:
    """(max - min) / median, in percent."""
    middle = statistics.median(values)
    return 100.0 * (max(values) - min(values)) / middle if middle else float("nan")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Speed per copy of one build, with and without ASLR.")
    parser.add_argument("--variant", action="append", required=True, help="label=path of a bench_types build")
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--copies", type=int, default=6)
    parser.add_argument("--runs", type=int, default=2)
    parser.add_argument("--rounds", type=int, default=101)
    parser.add_argument("--op", action="append", required=True)
    parser.add_argument("--bench-arg", action="append", default=[], help="an argument passed to every run (e.g. --calibrate, time); repeatable")
    parser.add_argument("--game", type=int, default=0, help="seconds of one --game-loop process per copy after the runs (0: none)")
    args = parser.parse_args(argv)
    if args.copies < 1 or args.runs < 1:
        print("bench_placement: --copies and --runs need at least 1", file=sys.stderr)
        return 1
    variants = []
    for item in args.variant:
        label, _, path = item.partition("=")
        if not label or not path:
            print("bench_placement: --variant needs label=path", file=sys.stderr)
            return 1
        variants.append((label, Path(path)))
    args.out_dir.mkdir(parents=True, exist_ok=True)
    copies = {}
    maps = {}
    for label, exe in variants:
        maps[label] = mapped(exe)
        for index in range(args.copies):
            folder = args.out_dir / "copies" / f"{label}-{index + 1}"
            folder.mkdir(parents=True, exist_ok=True)
            target = folder / exe.name
            # With the mode, so a copy stays executable on Linux.
            shutil.copy2(exe, target)
            copies[(label, index + 1)] = target
    records = []
    games = []
    try:
        for run_index in range(args.runs):
            for index in range(1, args.copies + 1):
                for label, _ in variants:
                    out = args.out_dir / f"{label}-{index}-run{run_index + 1}.json"
                    result = run(copies[(label, index)], out, args.rounds, args.op, args.bench_arg)
                    records.append({"variant": label, "copy": index, "run": run_index + 1, **result})
                    base = result["image_base"]
                    print(f"bench_placement: {label} copy {index} run {run_index + 1} base {hex(base) if base is not None else '?'} {json.dumps(result['ops'])}", file=sys.stderr, flush=True)
        # The script game loop (bench_types --game-loop), one process per copy, interleaved like the runs.
        for index in range(1, args.copies + 1):
            for label, _ in variants:
                if args.game <= 0:
                    break
                out = args.out_dir / f"{label}-{index}-game.json"
                done = subprocess.run([str(copies[(label, index)]), "--game-loop", str(args.game), "--out", str(out)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
                if done.returncode != 0:
                    raise RuntimeError(f"{label} copy {index} game loop exited with {done.returncode}:\n{done.stderr[-2000:]}")
                loop = next(json.loads(line)["game_loop"] for line in out.read_text(encoding="utf-8").splitlines() if "game_loop" in line)
                games.append({"variant": label, "copy": index, "game": loop})
                print(f"bench_placement: {label} copy {index} game p50 {loop['p50_us']:.2f} p99 {loop['p99_us']:.2f} us", file=sys.stderr, flush=True)
    except (RuntimeError, OSError, ValueError, KeyError) as error:
        print(f"bench_placement: {error}", file=sys.stderr)
        return 2
    finally:
        with (args.out_dir / "runs.jsonl").open("w", encoding="utf-8") as out:
            for label, found in maps.items():
                out.write(json.dumps({"variant": label, "mapped": found}) + "\n")
            for record in records + games:
                out.write(json.dumps(record) + "\n")
    text = render(records, maps, args, [label for label, _ in variants], games)
    (args.out_dir / "tables.txt").write_text(text + "\n", encoding="utf-8")
    print(text, flush=True)
    return 0


def render(records: list, maps: dict, args, labels: list, games: list) -> str:
    lines = [f"machine: {records[0]['machine']}", ""]
    if games:
        lines.append("Game loop (one process per copy): median over copies of p50 / p99 / p99.9 frame time in microseconds:")
        for label in labels:
            mine = [g["game"] for g in games if g["variant"] == label]
            if mine:
                lines.append(f"{label}: " + " / ".join(f"{statistics.median(m[key] for m in mine):.2f}" for key in ("p50_us", "p99_us", "p999_us")))
        lines.append("")
    def pick(label, index, op, field):
        return [r["ops"][op][field] for r in records if r["variant"] == label and r["copy"] == index]
    lines.append("Spread within one copy (median over copies of its runs' spread) and across copies (spread of the copies' medians), in percent:")
    lines.append("variant | op | ratio within | ratio across | zodiac within | zodiac across | add-on within | add-on across | ratio range of copies")
    gate = []
    for label in labels:
        for op in args.op:
            cells = []
            for field in ("ratio", "zodiac", "reference"):
                per_copy = [pick(label, index, op, field) for index in range(1, args.copies + 1)]
                within = statistics.median(spread(values) for values in per_copy) if args.runs > 1 else float("nan")
                across = spread([statistics.median(values) for values in per_copy])
                cells += [f"{within:.1f}", f"{across:.1f}"]
            medians = [statistics.median(pick(label, index, op, "ratio")) for index in range(1, args.copies + 1)]
            lines.append(f"{label} | {op} | " + " | ".join(cells) + f" | {min(medians):.3f}-{max(medians):.3f}")
            # A gate of 3 processes of one file decides on one copy's placement; one of 3 copies, on the median of 3 placements.
            one_file = sum(m > LIMIT for m in medians) / len(medians)
            triples = list(itertools.combinations(medians, 3)) if len(medians) >= 3 else []
            three = sum(statistics.median(t) > LIMIT for t in triples) / len(triples) if triples else float("nan")
            gate.append(f"{label} | {op} | {one_file:.2f} | {three:.2f}")
    lines.append("")
    lines.append("Every copy: image base, then per operation ratio / zodiac ns / add-on ns (medians of its runs):")
    for label in labels:
        for index in range(1, args.copies + 1):
            bases = sorted({r["image_base"] for r in records if r["variant"] == label and r["copy"] == index and r["image_base"] is not None})
            cells = [f"{op} {statistics.median(pick(label, index, op, 'ratio')):.3f}/{statistics.median(pick(label, index, op, 'zodiac')):.2f}/{statistics.median(pick(label, index, op, 'reference')):.2f}" for op in args.op]
            lines.append(f"{label}-{index} base {','.join(hex(b) for b in bases) or '?'}: " + "  ".join(cells))
    lines.append("")
    lines.append(f"Share of copies above {LIMIT} (a gate on 3 processes of one file), and of all 3-copy sets whose median is (a gate on one process each of 3 copies):")
    lines.append("variant | op | one file | three copies")
    lines += gate
    lines.append("")
    lines.append("Link-map addresses of each build (mod 64 / mod 4096):")
    for label, found in maps.items():
        lines.append(f"{label}: " + ", ".join(f"{name} {value} ({int(value, 16) % 64}/{int(value, 16) % 4096})" for name, value in found.items()))
    return "\n".join(lines)


if __name__ == "__main__":
    sys.exit(main())
