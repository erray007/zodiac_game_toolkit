#!/usr/bin/env python3
"""The sequence experiment of Task 5b fix round 2 (Windows, report-only): which bench_types processes after a fresh binary fall into the
dict_iterate_10 slow mode, and whether a fresh copy, a heap prewarm or a Windows Defender exclusion changes that.

    python -I tools/bench_sequence.py --plain <bench_types.exe> [--hooks <hooks bench_types.exe>] --out-dir <dir> [--k 6] [--rounds 101]

Every run is the gate's invocation (bench_types --detail --faults --layout --rounds R --out ...), each its own process, in this order:
  1. plain 1..K, right after the link;
  2. hooks 1..3 (a ZODIAC_TEST_HOOKS build, whose faults lines carry keys()'s per-step cycles), when given;
  3. prewarm 1..3: the plain binary with --prewarm-heap;
  4. copy 1..2: the plain binary copied to a new folder;
  5. excluded 1..2: copied to a folder that Add-MpPreference -ExclusionPath has excluded from Defender first.
Per run: dict_iterate_10's ratio and Zodiac nanoseconds, string_byte_ascii's ratio, the faults line of dict_iterate_10 (with the steps in a
hooks build) and the process's total page faults. Writes <dir>/sequence.jsonl and prints a table. Exit code 0 unless a run fails (2).
"""

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

SLOW = 1.3


def run(exe: Path, out: Path, rounds: int, extra: list) -> dict:
    command = [str(exe), "--detail", "--faults", "--layout", "--rounds", str(rounds), "--out", str(out)] + extra
    done = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
    out.with_suffix(".stderr.txt").write_text(done.stderr, encoding="utf-8")
    if done.returncode != 0:
        raise RuntimeError(f"{exe} exited with {done.returncode}:\n{done.stderr[-2000:]}")
    ops = {}
    machine = None
    for raw in out.read_text(encoding="utf-8").splitlines():
        if raw.strip():
            record = json.loads(raw)
            if "machine" in record:
                machine = record["machine"]["cpu"]
            else:
                ops[record["op"]] = record
    faults_line = next((line for line in done.stderr.splitlines() if line.startswith("bench_types: faults dict_iterate_10 ")), "")
    total = re.search(r"bench_types: process faults (\d+)", done.stderr)
    return {"machine": machine, "di10": ops["dict_iterate_10"]["ratio"], "di10_ns": ops["dict_iterate_10"]["new_ns"],
            "byte": ops["string_byte_ascii"]["ratio"], "faults": faults_line[len("bench_types: faults dict_iterate_10 "):],
            "process_faults": int(total.group(1)) if total else -1}


def exclude(folder: Path) -> str:
    done = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", f"Add-MpPreference -ExclusionPath '{folder}'; (Get-MpPreference).ExclusionPath -join ';'"],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    return f"exit {done.returncode}: {done.stdout.strip()}"


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Which bench processes after a fresh binary are slow.")
    parser.add_argument("--plain", type=Path, required=True)
    parser.add_argument("--hooks", type=Path)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--k", type=int, default=6)
    parser.add_argument("--rounds", type=int, default=101)
    args = parser.parse_args(argv)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    plan = [(f"plain{i}", args.plain, []) for i in range(1, args.k + 1)]
    if args.hooks:
        plan += [(f"hooks{i}", args.hooks, []) for i in range(1, 4)]
    plan += [(f"prewarm{i}", args.plain, ["--prewarm-heap"]) for i in range(1, 4)]
    records = []
    try:
        for name, exe, extra in plan:
            records.append({"run": name, **run(exe, args.out_dir / f"{name}.json", args.rounds, extra)})
            print(f"bench_sequence: {name} {records[-1]}", file=sys.stderr, flush=True)
        copy_dir = args.out_dir / "copy"
        copy_dir.mkdir(exist_ok=True)
        fresh = copy_dir / args.plain.name
        shutil.copy2(args.plain, fresh)
        for i in (1, 2):
            records.append({"run": f"copy{i}", **run(fresh, args.out_dir / f"copy{i}.json", args.rounds, [])})
            print(f"bench_sequence: copy{i} {records[-1]}", file=sys.stderr, flush=True)
        excluded_dir = (args.out_dir / "excluded").resolve()
        excluded_dir.mkdir(exist_ok=True)
        note = exclude(excluded_dir)
        print(f"bench_sequence: Defender exclusion of {excluded_dir}: {note}", file=sys.stderr, flush=True)
        fresh = excluded_dir / args.plain.name
        shutil.copy2(args.plain, fresh)
        for i in (1, 2):
            records.append({"run": f"excluded{i}", "exclusion": note, **run(fresh, args.out_dir / f"excluded{i}.json", args.rounds, [])})
            print(f"bench_sequence: excluded{i} {records[-1]}", file=sys.stderr, flush=True)
    except (RuntimeError, OSError, ValueError, KeyError) as error:
        print(f"bench_sequence: {error}", file=sys.stderr)
        return 2
    finally:
        with (args.out_dir / "sequence.jsonl").open("w", encoding="utf-8") as out:
            for record in records:
                out.write(json.dumps(record) + "\n")
    print(f"machine: {records[0]['machine']}")
    print("run  di10  di10_ns  slow  byte  process_faults  dict_iterate_10 faults line")
    for r in records:
        print(f"{r['run']}  {r['di10']:.3f}  {r['di10_ns']:.1f}  {'SLOW' if r['di10'] > SLOW else '-'}  {r['byte']:.3f}  {r['process_faults']}  {r['faults']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
