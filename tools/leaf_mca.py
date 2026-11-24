#!/usr/bin/env python3
"""llvm-mca's model of two leaves of the release bench (Task 5b fix round 4, report-only): Zodiac's script byte_at and the add-on's
StringCharAt, each from its first instruction to its first ret (the in-range path), for Zen 4 (znver4, the EPYC 9V74) and Zen 3 (znver3,
the EPYC 7763).

    python -I tools/leaf_mca.py --exe <bench_types> --objdump <llvm-objdump> --mca <llvm-mca> [--image-base]

The functions are found in the bench's link map (<exe>.map); --image-base adds the PE image base to the map's RVAs (Windows). Branch
targets are replaced by one local label, so llvm-mca can read the listing. Prints each listing and llvm-mca's summary. Exit code 0, or 2
when a tool fails.
"""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

LEAVES = {"zodiac byte_at": "zodiac::StringBindingAccess::byte_at(", "add-on StringCharAt": "StringCharAt("}


def address_of(map_text: str, name: str) -> int:
    for line in map_text.splitlines():
        if name in line:
            return int(line.split()[0], 16)
    raise RuntimeError(f"{name} is not in the map")


def listing(objdump: str, exe: Path, start: int) -> list:
    done = subprocess.run([objdump, "-d", "--no-show-raw-insn", "--no-leading-addr", f"--start-address={start}", f"--stop-address={start + 128}", str(exe)],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=True)
    instructions = []
    for raw in done.stdout.splitlines():
        line = raw.strip()
        if not line or line.endswith(":") or line.startswith("Disassembly") or "file format" in line:
            continue
        line = re.sub(r"\s*<[^>]*>", "", line).split("#")[0].strip()
        if re.match(r"^(j\w+|callq?)\s+0x[0-9a-f]+$", line):
            line = re.sub(r"0x[0-9a-f]+$", ".Lout", line)
        instructions.append(line)
        if line.startswith("ret"):
            break
    return instructions


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="llvm-mca of the byte_at leaves.")
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--mca", required=True)
    parser.add_argument("--image-base", action="store_true")
    args = parser.parse_args(argv)
    # Windows' CreateProcess does not resolve a relative path with forward slashes: tools named by path are made absolute.
    for name in ("objdump", "mca"):
        value = getattr(args, name)
        if Path(value).exists():
            setattr(args, name, str(Path(value).resolve()))
        elif shutil.which(value):
            setattr(args, name, shutil.which(value))
    try:
        map_text = Path(str(args.exe) + ".map").read_text(encoding="utf-8", errors="replace")
        base = 0
        if args.image_base:
            headers = subprocess.run([args.objdump, "-p", str(args.exe)], stdout=subprocess.PIPE, text=True, check=True).stdout
            base = int(re.search(r"^ImageBase\s+([0-9a-fA-F]+)", headers, re.M).group(1), 16)
        for title, name in LEAVES.items():
            start = base + address_of(map_text, name)
            body = listing(args.objdump, args.exe, start)
            source = "\n".join(body) + "\n.Lout:\n"
            print(f"== {title} at {start:#x} (mod 64 = {start % 64}), {len(body)} instructions:")
            print(source)
            for cpu in ("znver4", "znver3"):
                done = subprocess.run([args.mca, f"-mcpu={cpu}", "-iterations=100", "-summary-view", "-resource-pressure=false", "-instruction-info=false"],
                                      input=source, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                print(f"-- llvm-mca -mcpu={cpu}:")
                print(done.stdout)
    except (RuntimeError, OSError, subprocess.CalledProcessError, AttributeError) as error:
        print(f"leaf_mca: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
