#!/usr/bin/env python3
"""Carry function names from one binary to another (the symbolized Android libNOVA3 to the
stripped iOS executable) by what the functions do rather than their bytes, since the two
were built by different compilers.

Input: two ExportFeatures.java dumps (JSON lines: entry, name, size, strings, calls).
1. Anchors: a set of string literals referenced by exactly one function on each side.
2. Propagation: around each matched pair, callees and callers that stand at the same
   place (by call order, or as the only unmatched candidate) are matched too; repeat.
Output: "ios_entry android_name score" lines, best first, for ApplyNames.java.

3. Vtables (vtable_match.py): an iOS vtable of the same length whose matched slots agree
   with an Android one (and none disagree) is that class's; all its slots pair up.
   Steps 2 and 3 alternate until nothing changes.

Usage: match_symbols.py android.jsonl ios.jsonl out.txt [vtables.json]
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
        if x in match or y in taken or x not in a:
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

    vt_a, vt_b = [], defaultdict(list)
    if len(sys.argv) > 4:
        v = json.load(open(sys.argv[4]))
        vt_a = [(n, slots) for n, slots in v["android"] if len(slots) >= 2]
        for addr, slots in v["ios"]:
            vt_b[len(slots)].append((addr, slots))
    vt_done = set()

    def vtables():
        found = False
        for n, sa in vt_a:
            if n in vt_done:
                continue
            best, best_score, tie = None, 0, False
            for addr, sb in vt_b.get(len(sa), ()):
                agree = conflict = 0
                for p, q in zip(sa, sb):
                    if p is None or q is None:
                        continue
                    if p in match:
                        if match[p] == q:
                            agree += 1
                        else:
                            conflict += 1
                    elif q in taken:
                        conflict += 1
                if conflict or agree == 0:
                    continue
                if agree > best_score:
                    best, best_score, tie = (addr, sb), agree, False
                elif agree == best_score:
                    tie = True
            if not best or tie or (len(sa) > 3 and best_score < 2):
                continue
            vt_done.add(n)
            for p, q in zip(sa, best[1]):
                if p and q and pair(p, q, 70):
                    found = True
        return found

    def ext(c):
        return c[2:].lstrip("_").lower() if c.startswith("x:") else None

    def same(p, q):
        if p.startswith("x:") or q.startswith("x:"):
            return ext(p) is not None and ext(p) == ext(q)
        return match.get(p) == q

    def aligned_gaps(ca, cb):
        n, m = len(ca), len(cb)
        if not n or not m or n * m > 250000:
            return []
        # Longest common subsequence of corresponding calls.
        dp = [[0] * (m + 1) for _ in range(n + 1)]
        for i in range(n - 1, -1, -1):
            for j in range(m - 1, -1, -1):
                dp[i][j] = dp[i + 1][j + 1] + 1 if same(ca[i], cb[j]) else max(dp[i + 1][j], dp[i][j + 1])
        anchors, i, j = [(-1, -1)], 0, 0
        while i < n and j < m:
            if same(ca[i], cb[j]):
                anchors.append((i, j))
                i, j = i + 1, j + 1
            elif dp[i + 1][j] >= dp[i][j + 1]:
                i += 1
            else:
                j += 1
        anchors.append((n, m))
        out = []
        for (i0, j0), (i1, j1) in zip(anchors, anchors[1:]):
            ga, gb = ca[i0 + 1:i1], cb[j0 + 1:j1]
            if ga and len(ga) == len(gb):
                out.extend((p, q) for p, q in zip(ga, gb) if not p.startswith("x:") and not q.startswith("x:"))
        return out

    def similar(fa, fb):
        # Different compilers, but not wildly different functions.
        sa, sb = max(fa["size"], 1), max(fb["size"], 1)
        return 0.2 <= sb / sa <= 5 and abs(len(fa["calls"]) - len(fb["calls"])) <= max(3, len(fa["calls"]) // 2)

    # Similarity for picking among candidates: shared strings, shared rare constants (rare
    # in their own binary), size, the number of calls and the imports called.
    def rare_consts(funcs):
        df = defaultdict(int)
        for f in funcs.values():
            for c in f.get("consts", ()):
                df[c] += 1
        for f in funcs.values():
            f["rare"] = {c for c in f.get("consts", ()) if df[c] <= 40}
            f["sset"] = set(f["strings"])
            f["ext"] = {c[2:].lstrip("_").lower() for c in f["calls"] if c.startswith("x:")}

    rare_consts(a), rare_consts(b)

    def jac(x, y):
        return len(x & y) / len(x | y) if x or y else 0.0

    def sim(fa, fb):
        sa, sb = max(fa["size"], 1), max(fb["size"], 1)
        na, nb = len(fa["calls"]), len(fb["calls"])
        score = 2.0 * jac(fa["sset"], fb["sset"]) + 1.5 * jac(fa["rare"], fb["rare"])
        score += 0.5 * min(sa, sb) / max(sa, sb) + 0.5 * (min(na, nb) + 1) / (max(na, nb) + 1)
        score += 0.5 * jac(fa["ext"], fb["ext"])
        return score

    def mutual_best(xs, ys, floor, margin):
        # Pairs that are each other's best by a clear margin.
        xs = [x for x in xs if x not in match and x in a]
        ys = [y for y in ys if y not in taken and y in b]
        if not xs or not ys or len(xs) * len(ys) > 4000:
            return []
        scores = {(x, y): sim(a[x], b[y]) for x in xs for y in ys}
        def best(items):
            items = sorted(items, key=lambda t: -t[1])
            return items[0] if items and items[0][1] >= floor and (len(items) == 1 or items[0][1] - items[1][1] >= margin) else None
        out = []
        for x in xs:
            bx = best([(y, scores[(x, y)]) for y in ys])
            if not bx:
                continue
            by = best([(x2, scores[(x2, bx[0])]) for x2 in xs])
            if by and by[0] == x:
                out.append((x, bx[0]))
        return out

    # Functions sharing strings: within each string's owners (when few), mutual bests.
    def string_groups():
        found = False
        for st, ea in oa.items():
            eb = ob.get(st)
            if not eb or len(ea) > 60 or len(eb) > 60:
                continue
            for x, y in mutual_best(list(ea), list(eb), 1.6, 0.3):
                found |= pair(x, y, 55)
        return found

    def neighbours():
        found = False
        for x, y in list(match.items()):
            if y not in b:
                continue
            ra = [c for c in dict.fromkeys(internal_calls(a[x]))]
            rb = [c for c in dict.fromkeys(internal_calls(b[y]))]
            for p, q in mutual_best(ra, rb, 1.2, 0.25):
                found |= pair(p, q, 45)
            pa, pb = list(dict.fromkeys(callers_a[x])), list(dict.fromkeys(callers_b[y]))
            for p, q in mutual_best(pa, pb, 1.4, 0.3):
                found |= pair(p, q, 40)
        return found

    changed = True
    rounds = 0
    while changed and rounds < 50:
        changed, rounds = vtables(), rounds + 1
        changed |= string_groups()
        changed |= neighbours()
        for x, y in list(match.items()):
            if y not in b:   # a vtable slot Ghidra has not made a function of
                continue
            ca, cb = a[x]["calls"], b[y]["calls"]
            # The two call sequences aligned on what already corresponds (matched callees,
            # the same imports); equal runs between those anchors pair up in order.
            for p, q in aligned_gaps(ca, cb):
                if p in a and q in b and similar(a[p], b[q]) and pair(p, q, 60):
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
