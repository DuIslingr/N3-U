#!/usr/bin/env python3
"""Convert decompiled CMemoryStream loader functions into lvc.py read specs.

The game's generated loaders read fields in order, and lists as
`n = ReadInt(); ... if (0 < (int)n) { do { <element reads> } while (...); }`.
This turns each function into a spec string (i f h c s v, [ ... ] for a
count-prefixed list, <Name::Load> for a nested loader call) and flags reads
that sit under any other condition, which need a manual look.

Usage: skel2spec.py DECOMP.c [DECOMP.c ...] > specs.json
"""

import json
import re
import sys

READS = [
    (re.compile(r"CMemoryStream::ReadInt\("), "i"),
    (re.compile(r"CMemoryStream::ReadFloat\("), "f"),
    (re.compile(r"CMemoryStream::ReadShort\("), "h"),
    (re.compile(r"CMemoryStream::Read(Char|Bool|Byte)\("), "c"),
    (re.compile(r"CMemoryStream::ReadBlockStart\("), "h"),
    (re.compile(r"CMemoryStream::ReadBlockEnd\("), "c"),
    (re.compile(r"CMemoryStream::Read(String|StringC|StringW)\("), "s"),
    (re.compile(r"CMemoryStream::Read\([^,]*,\s*\((vector3d|btVector3) \*\)"), "v"),
    (re.compile(r"CMemoryStream::Read\([^,]*,\s*\((vector2d) \*\)"), "f f"),
    (re.compile(r"CMemoryStream::Read\([^,]*,\s*\((quaternion|vector4d|btQuaternion) \*\)"), "f f f f"),
    (re.compile(r"CMemoryStream::Read\([^,]*,\s*\((int|uint) \*\)"), "i"),
    (re.compile(r"CMemoryStream::Read\([^,]*,\s*\(float \*\)"), "f"),
    (re.compile(r"CMemoryStream::Read\([^,]*,\s*\((short|ushort) \*\)"), "h"),
    (re.compile(r"CMemoryStream::Read\([^,]*,\s*\((char|uchar|bool) \*\)"), "c"),
]
NESTED = re.compile(r"\b((?:\w+::)*\w+)::(Load|Create|LoadConfig)\s*\(")
COUNT_ASSIGN = re.compile(r"(\w+)\s*=\s*(?:\([^)]*\))?\s*CMemoryStream::ReadInt\(")
COUNT_TEST = re.compile(r"if \((?:0 < \(int\)(\w+)|(\w+) != 0|0 < (\w+)|\(int\)(\w+) > 0)\)")


def functions(path):
    name, body = None, []
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("// ===== "):
            if name:
                yield name, body
            name, body = line[9:].split(" @ ")[0].strip(), []
        elif name:
            body.append(line.rstrip("\n"))
    if name:
        yield name, body


def convert(body, self_name):
    tokens, warnings = [], []
    counts = {}             # var -> index of its 'i' token
    depth = 0
    open_groups = []        # brace depth at which a [ group's do-block closes
    pending_group = None    # count var waiting for its do {
    cond_depths = []        # brace depths of non-count if blocks
    null_check = {}         # depth -> whether the last if opened there tested the stream for NULL
    for line in body:
        s = line.strip()
        m = COUNT_TEST.search(s)
        if m and s.endswith("{"):
            var = next(g for g in m.groups() if g)
            if var in counts:
                pending_group = var
            else:
                cond_depths.append(depth + 1)
        elif re.match(r"(if|else if)\b", s) and s.endswith("{"):
            # A NULL-stream test picks template vs stream data; it is not a data condition.
            is_null = bool(re.search(r"param_\d+ [!=]= \(CMemoryStream \*\)0x0", s))
            null_check[depth + 1] = is_null
            if not is_null:
                cond_depths.append(depth + 1)
        elif s.startswith("else") and s.endswith("{"):
            if not null_check.get(depth + 1):
                cond_depths.append(depth + 1)
        fm = re.match(r"for \(.*<\s*(?:\(int\))?(\w+);.*\)\s*\{$", s)
        if fm and fm.group(1) in counts:
            idx = counts.pop(fm.group(1))
            tokens[idx] = "["
            open_groups.append(depth + 1)
        if s.startswith("do {") and pending_group:
            idx = counts.pop(pending_group)
            tokens[idx] = "["
            open_groups.append(depth + 1)
            pending_group = None
        # reads
        read = None
        for rx, tok in READS:
            if rx.search(s):
                read = tok
                break
        if read and not re.search(r"CMemoryStream::\w+\(\s*(\([^)]*\)\s*)?param_\d", s):
            warnings.append(f"other stream (skipped): {s[:90]}")
            reassigned = COUNT_ASSIGN.search(s)
            if reassigned:
                counts.pop(reassigned.group(1), None)   # the variable no longer holds our count
            read = None
        if read is None:
            nm = NESTED.search(s)
            if nm and "param_" in s and not s.startswith("//") and nm.group(0).split("::")[0] != "CMemoryStream":
                call = f"{nm.group(1)}::{nm.group(2)}"
                if call != self_name:
                    read = f"<{call}>"
        # Any other assignment to a count variable means it no longer holds the count
        # (Ghidra reuses variables, e.g. an int count later overwritten by float reads).
        assigned = re.match(r"(\w+) = ", s)
        if assigned and not (read == "i" and COUNT_ASSIGN.search(s)):
            counts.pop(assigned.group(1), None)
        if read:
            if any(d <= depth for d in cond_depths) and not open_groups:
                warnings.append(f"conditional: {s[:90]}")
            ca = COUNT_ASSIGN.search(s)
            if ca and read == "i":
                counts[ca.group(1)] = len(tokens)
            tokens.append(read)
        # braces
        depth += s.count("{") - s.count("}")
        if s.startswith("}"):
            while open_groups and depth < open_groups[-1]:
                open_groups.pop()
                tokens.append("]")
            cond_depths = [d for d in cond_depths if d <= depth]
    if open_groups:
        warnings.append("unclosed group")
    return " ".join(tokens), warnings


def main():
    out = {}
    for path in sys.argv[1:]:
        for name, body in functions(path):
            spec, warnings = convert(body, name)
            prev = out.get(name)
            if prev and len(prev["spec"]) >= len(spec):
                continue    # keep the overload that reads the most (the stream one)
            out[name] = {"spec": spec, "warnings": warnings}
    json.dump(out, sys.stdout, indent=1, sort_keys=True)


if __name__ == "__main__":
    main()
