#!/usr/bin/env python3
"""The heap study of Task 5c, Part A (report-only, never the gate): does the Windows heap's per-process slow mode last, and does it reach
a game? One allocator per call (the trees of ZODIAC_BENCH_ALLOCATOR: the C runtime's NT heap, the Segment Heap, mimalloc).

    python -I tools/bench_heap_study.py --bench <bench_types> --hooks <hooks bench_types> --out-dir <dir> [--processes 5]
        [--soak 180] [--game 60] [--label nt]

For each process index, three fresh processes, in this order so a drift of the machine spreads over all kinds:
  soak-<i>: the hooks bench with ZODIAC_ARRAY_BLOCK_CACHE=0 (every ScriptArray object from the heap again, as before Task 5b fix round 2),
            --soak <soak> --heap-state on array_insert_last_string, string_split and dict_keys_10 (keys() alone: one ScriptArray object
            per call), both sides, per-second medians;
  game-<i>: the plain bench, --game-loop <game> --heap-state (frame-time percentiles of a script game loop);
  bench-<i>: the plain bench as the gate runs it (every gated operation, --calibrate time, --detail --faults) plus --heap-state.
Each process's stdout lines go to <dir>/<name>.json, its stderr to <dir>/<name>-stderr.txt. Prints, per process, the soak's per-op medians
over its first and last 10 seconds and over all of it, the heap scan's slow sizes at start and end, the game loop's p50/p99/p99.9/max, and
the bench's slowest operations against this job's median; writes everything parsed to <dir>/runs.jsonl. Exit codes: 0 done, 1 bad
arguments, 2 a process failed.
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
from pathlib import Path

SOAK_OPS = ("array_insert_last_string", "string_split", "dict_keys_10")
# The operations Task 5b saw bimodal per process (the heap's slow mode), compared across this job's bench processes.
BIMODAL = ("dict_iterate_10", "string_split", "array_insert_last_int", "array_insert_last_string")


def launch(command: list, out: Path, env_extra: dict) -> str:
    env = dict(os.environ)
    env.pop("ZODIAC_ARRAY_BLOCK_CACHE", None)
    env.pop("ZODIAC_PRIVATE_HEAP", None)
    env.update(env_extra)
    done = subprocess.run(command + ["--out", str(out)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
    Path(str(out).replace(".json", "-stderr.txt")).write_text(done.stderr, encoding="utf-8")
    if done.returncode != 0:
        raise RuntimeError(f"{' '.join(command)} exited with {done.returncode}:\n{done.stderr[-2000:]}")
    return done.stderr


def heap_lines(stderr: str) -> list:
    lines = []
    for line in stderr.splitlines():
        if line.startswith("bench_types: heap {"):
            lines.append(json.loads(line[len("bench_types: heap "):]))
    return lines


def header(stderr: str, key: str) -> str:
    prefix = f"bench_types: {key}: "
    for line in stderr.splitlines():
        if line.startswith(prefix):
            return line[len(prefix):]
    return ""


def records(path: Path) -> list:
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Soak, game loop and gate bench in fresh processes, with the heap's state.")
    parser.add_argument("--bench", type=Path, required=True)
    parser.add_argument("--hooks", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--processes", type=int, default=5)
    parser.add_argument("--soak", type=int, default=180)
    parser.add_argument("--game", type=int, default=60)
    parser.add_argument("--label", default="")
    args = parser.parse_args(argv)
    if args.processes < 1 or args.soak < 1 or args.game < 1:
        print("bench_heap_study: --processes, --soak and --game need at least 1", file=sys.stderr)
        return 1
    args.out_dir.mkdir(parents=True, exist_ok=True)
    results = []
    try:
        for index in range(1, args.processes + 1):
            soak_command = [str(args.hooks), "--diagnostic", "--calibrate", "time", "--soak", str(args.soak), "--heap-state"]
            for op in SOAK_OPS:
                soak_command += ["--op", op]
            out = args.out_dir / f"soak-{index}.json"
            stderr = launch(soak_command, out, {"ZODIAC_ARRAY_BLOCK_CACHE": "0"})
            results.append({"kind": "soak", "index": index, "label": args.label, "heap_kind": header(stderr, "heap kind"), "image_base": header(stderr, "image base"),
                "heap": heap_lines(stderr), "steps": [line for line in stderr.splitlines() if line.startswith("bench_types: faults dict_keys_10")], "lines": records(out)})
            print(f"bench_heap_study: soak-{index} done", file=sys.stderr, flush=True)
            out = args.out_dir / f"game-{index}.json"
            stderr = launch([str(args.bench), "--game-loop", str(args.game), "--heap-state"], out, {})
            results.append({"kind": "game", "index": index, "label": args.label, "heap_kind": header(stderr, "heap kind"), "heap": heap_lines(stderr), "lines": records(out)})
            print(f"bench_heap_study: game-{index} done", file=sys.stderr, flush=True)
            out = args.out_dir / f"bench-{index}.json"
            stderr = launch([str(args.bench), "--detail", "--faults", "--calibrate", "time", "--heap-state"], out, {})
            results.append({"kind": "bench", "index": index, "label": args.label, "heap_kind": header(stderr, "heap kind"), "heap": heap_lines(stderr), "lines": records(out)})
            print(f"bench_heap_study: bench-{index} done", file=sys.stderr, flush=True)
    except (RuntimeError, OSError, ValueError) as error:
        print(f"bench_heap_study: {error}", file=sys.stderr)
        return 2
    finally:
        with (args.out_dir / "runs.jsonl").open("w", encoding="utf-8") as out:
            for result in results:
                out.write(json.dumps(result) + "\n")
    text = render(results)
    (args.out_dir / "tables.txt").write_text(text + "\n", encoding="utf-8")
    print(text, flush=True)
    return 0


def render(results: list) -> str:
    machine = next((line["machine"]["cpu"] for r in results for line in r["lines"] if "machine" in line), "?")
    lines = [f"machine: {machine}; heap kinds: {sorted({r['heap_kind'] for r in results})}", ""]
    lines.append("Soak (hooks bench, array block cache off): per op, ratio and Zodiac ns over the first 10 s / the last 10 s / all seconds; heap scan slow sizes at start and end:")
    for r in (r for r in results if r["kind"] == "soak"):
        cells = []
        for op in SOAK_OPS:
            series = [line for line in r["lines"] if line.get("soak") == op]
            if not series:
                continue
            def med(part, field):
                return statistics.median(line[field] for line in part) if part else float("nan")
            first, last = series[:10], series[-10:]
            cells.append(f"{op} ratio {med(first, 'ratio'):.3f}/{med(last, 'ratio'):.3f}/{med(series, 'ratio'):.3f} ns {med(first, 'new_ns'):.1f}/{med(last, 'new_ns'):.1f}/{med(series, 'new_ns'):.1f} ({len(series)} s)")
        slow = " | ".join(f"{h['when']} slow {h.get('slow_sizes')}" for h in r["heap"])
        lines.append(f"soak-{r['index']} base {r.get('image_base', '')}: " + "; ".join(cells) + f" | {slow}")
        for step in r["steps"]:
            lines.append(f"    {step}")
    lines.append("")
    lines.append("Game loop (plain bench): frames, mean, p50, p90, p99, p99.9 and max frame in microseconds, per-second p50 range:")
    for r in (r for r in results if r["kind"] == "game"):
        loop = next((line["game_loop"] for line in r["lines"] if "game_loop" in line), None)
        if not loop:
            continue
        seconds = loop["second_p50_us"]
        lines.append(f"game-{r['index']}: {loop['frames']} frames, mean {loop['mean_us']:.2f}, p50 {loop['p50_us']:.2f}, p90 {loop['p90_us']:.2f}, p99 {loop['p99_us']:.2f}, p99.9 {loop['p999_us']:.2f}, max {loop['max_us']:.1f}; second p50 {min(seconds):.2f}-{max(seconds):.2f}"
            + " | " + " | ".join(f"{h['when']} slow {h.get('slow_sizes')}" for h in r["heap"]))
    lines.append("")
    benches = [r for r in results if r["kind"] == "bench"]
    ops = {}
    for r in benches:
        for line in r["lines"]:
            if "op" in line:
                ops.setdefault(line["op"], []).append(line["new_ns"])
    lines.append("Gate bench (plain): per process, the bimodal operations' Zodiac ns over this job's median, and its ratio of string_byte_ascii:")
    for r in benches:
        by_op = {line["op"]: line for line in r["lines"] if "op" in line}
        cells = [f"{op} {by_op[op]['new_ns'] / statistics.median(ops[op]):.3f}" for op in BIMODAL if op in by_op]
        lines.append(f"bench-{r['index']}: " + ", ".join(cells) + (f", string_byte_ascii ratio {by_op['string_byte_ascii']['ratio']:.3f}" if "string_byte_ascii" in by_op else "")
            + " | " + " | ".join(f"{h['when']} slow {h.get('slow_sizes')}" for h in r["heap"]))
    lines.append("")
    lines.append("Gate bench: per operation, the median over processes of Zodiac's ns and of the ratio:")
    for op, values in ops.items():
        ratios = [line["ratio"] for r in benches for line in r["lines"] if line.get("op") == op]
        lines.append(f"{op}: zodiac {statistics.median(values):.2f} ns, ratio {statistics.median(ratios):.3f}, max/min ns {max(values) / min(values):.3f}")
    return "\n".join(lines)


if __name__ == "__main__":
    sys.exit(main())
