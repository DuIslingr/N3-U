#!/usr/bin/env python3
"""Print the read skeleton of decompiled loader functions.

Keeps only the lines of a Ghidra C listing that matter for serialization order:
CMemoryStream reads, calls to other Load-style functions, and the control flow
(if/else/loops/braces) around them. Used to port the game's level readers.

Usage: read_skeleton.py DECOMP.c FUNCTION_NAME_SUBSTRING [...]
"""

import re
import sys

KEEP = re.compile(
    r"CMemoryStream::(Read\w*|GetString\w*)|::Load\w*\(|::Create\w*\(|CMemoryStream::Read\(|LoadConfig|LoadZonesLink|"
    r"^\s*(if|else|do|while|for|switch|case|default|goto|return|LAB_)|^\s*\}|\{\s*$")
NOISE = re.compile(r"Println|ScopeEvent|MemoryMonitor|guard_|atexit|_M_|__throw|Debugger")


def functions(path):
    name, body = None, []
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("// ===== "):
            if name:
                yield name, body
            name, body = line[9:].strip(), []
        elif name:
            body.append(line.rstrip("\n"))
    if name:
        yield name, body


def skeleton(body):
    out = []
    for line in body:
        if KEEP.search(line) and not NOISE.search(line):
            out.append(line)
    # drop empty brace pairs left behind
    changed = True
    while changed:
        changed = False
        for i in range(len(out) - 1):
            if out[i].strip().endswith("{") and out[i + 1].strip() == "}" and \
                    not re.search(r"Read|Load", out[i]):
                del out[i:i + 2]
                changed = True
                break
    return out


def main():
    path, wanted = sys.argv[1], sys.argv[2:]
    for name, body in functions(path):
        if any(w in name for w in wanted):
            print(f"// ===== {name}")
            print("\n".join(skeleton(body)))


if __name__ == "__main__":
    main()
