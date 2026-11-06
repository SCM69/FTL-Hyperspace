#!/usr/bin/env python3
"""Compare the co-op logs of N FTL copies: hash lines per frame, summaries, desyncs.
Usage: compare_logs.py <out dir> <copies> <frames>
Prints RESULT: PASS only when every copy reached the target frame, no copy reported a desync,
and at least one hash frame was compared with all copies agreeing on every compared frame."""
import os
import re
import sys

HASH = re.compile(r"\[[\d.]+ f(\d+)\] HASH (.*)")
SUMMARY = re.compile(r"SUMMARY frames=(\d+) desyncs=(\d+) stalls=(\d+) players=(\d+)")


def parse(path):
    hashes, summary, desync, errors, applies = {}, None, 0, [], 0
    if not os.path.exists(path):
        return None
    with open(path, errors="replace") as f:
        for line in f:
            m = HASH.search(line)
            if m:
                parts = dict(kv.split("=") for kv in m.group(2).split())
                hashes[int(m.group(1))] = parts
                continue
            m = SUMMARY.search(line)
            if m:
                summary = tuple(int(x) for x in m.groups())
            if "DESYNC" in line or "host reports desync" in line:
                desync += 1
            if "ERROR" in line:
                errors.append(line.strip())
            if " APPLY " in line:
                applies += 1
    return {"hashes": hashes, "summary": summary, "desync": desync, "errors": errors, "applies": applies}


def main():
    out, n, frames = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    logs = [parse(os.path.join(out, f"copy{i}", "coop.log")) for i in range(n)]
    ok = True
    for i, lg in enumerate(logs):
        if lg is None:
            print(f"copy{i}: no coop.log")
            ok = False
            continue
        s = lg["summary"]
        print(f"copy{i}: summary={s} hash_frames={len(lg['hashes'])} applied_cmds={lg['applies']} desync_lines={lg['desync']}")
        for e in lg["errors"][:5]:
            print(f"  {e}")
        if not s or s[0] < frames:
            ok = False
            print(f"  copy{i} did not reach frame {frames}")
        if lg["desync"]:
            ok = False
    if not all(logs):
        print("RESULT: FAIL")
        return 1
    common = sorted(set.intersection(*(set(lg["hashes"]) for lg in logs)))
    first_bad = None
    for f in common:
        vals = [lg["hashes"][f]["all"] for lg in logs]
        if len(set(vals)) > 1:
            first_bad = f
            break
    print(f"compared hash frames: {len(common)}")
    if first_bad is not None:
        ok = False
        print(f"first divergence at frame {first_bad}:")
        keys = logs[0]["hashes"][first_bad].keys()
        for k in keys:
            vals = [lg["hashes"][first_bad].get(k) for lg in logs]
            mark = "  DIFF" if len(set(vals)) > 1 else ""
            print(f"  {k:7s} " + " ".join(vals) + mark)
    if not common:
        ok = False
    print("RESULT: PASS" if ok else "RESULT: FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
