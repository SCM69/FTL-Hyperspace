#!/usr/bin/env python3
"""Diff RNG caller counts (FTL_COOP_RNGTRACE=1) between two co-op logs, per hash frame.
Usage: trace_diff.py <log A> <log B> [asm file to name FTL addresses]"""
import bisect, re, sys
def load(p):
    out, frame = {}, None
    for l in open(p, errors="replace"):
        m = re.search(r"f(\d+)(?: e\d+)?\] HASH all=(\w+) rng=(\w+)", l)
        if m:
            frame = int(m.group(1)); out[frame] = {"rng": m.group(3), "calls": {}}
        elif "RNGTRACE" in l and frame is not None:
            for t in l.split("RNGTRACE", 1)[1].split():
                w, a, c = t.rsplit(":", 2)
                out[frame]["calls"][w + ":" + a] = int(c)
    return out
names = []
if len(sys.argv) > 3:
    for l in open(sys.argv[3], errors="replace"):
        m = re.match(r"^([0-9a-f]{16}) <(.*)>:$", l)
        if m: names.append((int(m.group(1), 16), m.group(2)))
    names.sort()
def name(k):
    w, a = k.split(":", 1)
    if "+" in a or not names: return k
    i = bisect.bisect_right(names, (int(a, 16), "￿")) - 1
    return f"{'rand' if w == '0' else 'random32'} <- {names[i][1][:70]}+{int(a,16)-names[i][0]:x}" if i >= 0 else k
A, B = load(sys.argv[1]), load(sys.argv[2])
shown = 0
for f in sorted(set(A) & set(B)):
    ca, cb = A[f]["calls"], B[f]["calls"]
    diff = {k: (ca.get(k, 0), cb.get(k, 0)) for k in set(ca) | set(cb) if ca.get(k, 0) != cb.get(k, 0)}
    if diff:
        print(f"frame {f}: rng {'same' if A[f]['rng'] == B[f]['rng'] else 'DIFF'}")
        for k, v in sorted(diff.items()):
            print(f"   {v[0]:5d} {v[1]:5d}  {name(k)}")
        shown += 1
        if shown >= 4: break
if not shown: print("no caller differences")
