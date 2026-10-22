#!/usr/bin/env python3
"""Split GenerateComponentTemplate's switch into per-component read specs."""
import json, re, sys
sys.path.insert(0, 'tools/ghidra_scripts')
import skel2spec
text = open('work/decomp/gentemplate.c').read().splitlines()
start = next(i for i, l in enumerate(text) if 'switch(param_1)' in l)
cases, cur, ids = {}, None, []
body = []
def flush():
    if ids:
        spec, warns = skel2spec.convert(body, 'GenerateComponentTemplate')
        for c in ids: cases[c] = (spec, warns)
for line in text[start + 1:]:
    m = re.match(r'\s*case (0x[0-9a-f]+|\d+):', line)
    d = re.match(r'\s*default:', line)
    if m or d:
        if body and any(x.strip() for x in body):   # previous case had a body: close it
            flush(); ids, body = [], []
        if m: ids.append(int(m.group(1), 0))
        else: ids = []
        continue
    body.append(line)
flush()
json.dump({hex(k): {'spec': v[0], 'warnings': v[1]} for k, v in sorted(cases.items())}, open('work/decomp/template_specs.json', 'w'), indent=1)
warned = {k: v[1] for k, v in cases.items() if v[1]}
print(len(cases), 'template cases;', len(warned), 'with warnings')
for k in sorted(warned)[:12]: print(hex(k), cases[k][0][:80], warned[k][:2])
