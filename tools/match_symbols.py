#!/usr/bin/env python3
"""Carry function names from one binary to another (the symbolized Android libNOVA3 to the
stripped iOS executable) by what the functions do rather than their bytes, since the two
were built by different compilers.

Input: two ExportFeatures.java dumps (JSON lines: entry, name, size, strings, calls).
1. Anchors: a set of string literals referenced by exactly one function on each side.
2. Propagation: around each matched pair, callees and callers that stand at the same
   place (by call order, or as the only unmatched candidate) are matched too; repeat.
Output: "ios_entry android_name score" lines, best first, for ApplyNames.java.

Usage: match_symbols.py android.jsonl ios.jsonl out.txt
"""

import json
import sys
from collections import defaultdict


def load(path):
    funcs = {}
    for line in open(path, encoding="utf-8"):
        f = json.loads(line)
        funcs[f["entry"]] = f
    return funcs


def generic(name):
    # Names Ghidra made up (not worth carrying), or the source has none.
    base = name.split("::")[-1]
    return base.startswith(("FUN_", "thunk_FUN_", "LAB_", "SUB_")) or base.startswith("_GLOBAL__")


def main():
    a, b, out = load(sys.argv[1]), load(sys.argv[2]), sys.argv[3]
    match = {}       # android entry -> ios entry
    score = {}
    taken = set()    # ios entries matched

    def pair(x, y, s):
        if x in match or y in taken:
            return False
        match[x], score[x] = y, s
        taken.add(y)
        return True

    # 1. Anchors: functions whose set of (distinctive) strings is the same on both sides.
    def string_owners(funcs):
        owners = defaultdict(set)
        for e, f in funcs.items():
            for s in f["strings"]:
                owners[s].add(e)
        return owners

    oa, ob = string_owners(a), string_owners(b)
    for s, ea in oa.items():
        eb = ob.get(s)
        if eb and len(ea) == 1 and len(eb) == 1:
            pair(next(iter(ea)), next(iter(eb)), 100)
    # Functions sharing several strings with exactly one function on the other side.
    for e, f in a.items():
        if e in match or len(f["strings"]) < 2:
            continue
        cand = defaultdict(int)
        for s in f["strings"]:
            for y in ob.get(s, ()):
                if len(oa[s]) <= 3:
                    cand[y] += 1
        good = [y for y, n in cand.items() if n >= max(2, len(f["strings"]) * 0.6) and y not in taken]
        if len(good) == 1:
            pair(e, good[0], 80)
    print(f"anchors: {len(match)}", file=sys.stderr)

    # 2. Propagation through the call graph.
    def internal_calls(f):
        return [c for c in f["calls"] if not c.startswith("x:")]

    callers_a, callers_b = defaultdict(list), defaultdict(list)
    for e, f in a.items():
        for c in internal_calls(f):
            callers_a[c].append(e)
    for e, f in b.items():
        for c in internal_calls(f):
            callers_b[c].append(e)

    changed = True
    rounds = 0
    while changed and rounds < 50:
        changed, rounds = False, rounds + 1
        for x, y in list(match.items()):
            ca, cb = internal_calls(a[x]), internal_calls(b[y])
            # The same number of calls: the same order.
            if len(ca) == len(cb):
                for p, q in zip(ca, cb):
                    if p in a and q in b and pair(p, q, 60):
                        changed = True
            # The only unmatched distinct callee left on each side.
            ra = [c for c in dict.fromkeys(ca) if c not in match and c in a]
            rb = [c for c in dict.fromkeys(cb) if c not in taken and c in b]
            if len(ra) == 1 and len(rb) == 1 and pair(ra[0], rb[0], 50):
                changed = True
            # Callers the same way.
            pa = [c for c in dict.fromkeys(callers_a[x]) if c not in match]
            pb = [c for c in dict.fromkeys(callers_b[y]) if c not in taken]
            if len(pa) == 1 and len(pb) == 1 and pair(pa[0], pb[0], 40):
                changed = True
        print(f"round {rounds}: {len(match)}", file=sys.stderr)

    with open(out, "w", encoding="utf-8") as o:
        n = 0
        for x, y in sorted(match.items(), key=lambda kv: -score[kv[0]]):
            name = a[x]["name"]
            if generic(name):
                continue
            o.write(f"{y}\t{name}\t{score[x]}\n")
            n += 1
    print(f"{n} names written to {out}", file=sys.stderr)


if __name__ == "__main__":
    main()
