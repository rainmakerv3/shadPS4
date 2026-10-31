#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

"""Names the functions at offsets into shadPS4.exe, as the sampling profiler logs them, from the
linker map of the build that wrote the log.

usage: symbolize.py shadPS4.map offsets.txt
"""

import bisect
import re
import shutil
import subprocess
import sys

SYMBOL = re.compile(
    r"^\s*([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+(\S+)\s+([0-9a-fA-F]{8,16})\s+(f\s+)?(i\s+)?(\S.*)?$"
)
LOAD_ADDRESS = re.compile(r"Preferred load address is ([0-9a-fA-F]+)")
OFFSET = re.compile(r"0x[0-9a-fA-F]+")


def read_map(path):
    image_base = 0
    functions = {}
    others = {}
    with open(path, encoding="utf-8", errors="replace") as map_file:
        for line in map_file:
            if not image_base:
                match = LOAD_ADDRESS.search(line)
                if match:
                    image_base = int(match.group(1), 16)
                    continue
            match = SYMBOL.match(line)
            if not match:
                continue
            address = int(match.group(4), 16)
            if image_base and address >= image_base:
                address -= image_base
            name = match.group(3)
            obj = (match.group(7) or "").strip()
            table = functions if match.group(5) else others
            # Keep the first name at an address: later ones are mostly folded duplicates.
            table.setdefault(address, (name, obj))
    return image_base, functions, others


def demangle(names):
    tool = next(
        (shutil.which(tool) for tool in ("llvm-undname", "llvm-undname-18", "llvm-undname-19")
         if shutil.which(tool)),
        None,
    )
    if not tool or not names:
        return {}
    result = subprocess.run([tool], input="\n".join(names) + "\n", capture_output=True,
                            text=True, check=False)
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    demangled = {}
    # The tool echoes each name and prints the demangled one under it.
    for i in range(0, len(lines) - 1, 2):
        demangled[lines[i]] = lines[i + 1]
    return demangled


def main():
    image_base, functions, others = read_map(sys.argv[1])
    with open(sys.argv[2], encoding="utf-8") as offsets_file:
        text = "\n".join(line for line in offsets_file if not line.lstrip().startswith("#"))
    offsets = list(dict.fromkeys(int(offset, 16) for offset in OFFSET.findall(text)))
    print(f"image base {image_base:#x}, {len(functions)} functions, {len(others)} other symbols")

    starts = sorted(functions)
    other_starts = sorted(others)
    found = []
    for offset in offsets:
        index = bisect.bisect_right(starts, offset) - 1
        other_index = bisect.bisect_right(other_starts, offset) - 1
        start = starts[index] if index >= 0 else None
        other_start = other_starts[other_index] if other_index >= 0 else None
        # A closer symbol without the function flag still names code, e.g. a static function.
        if other_start is not None and (start is None or other_start > start):
            name, obj = others[other_start]
            start = other_start
        elif start is not None:
            name, obj = functions[start]
        else:
            name, obj = "?", ""
        found.append((offset, start or 0, name, obj))

    demangled = demangle(sorted({name for _, _, name, _ in found}))
    for offset, start, name, obj in found:
        print(f"{offset:#x} = {name}+{offset - start:#x} [{obj}]")
        if name in demangled:
            print(f"    {demangled[name]}")


if __name__ == "__main__":
    main()
