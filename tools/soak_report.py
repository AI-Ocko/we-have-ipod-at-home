#!/usr/bin/env python3
"""Summarise a log written by soak.py.

Usage: soak_report.py <log file> [--table]

Prints: each track start (path, and how it ended: ENDED frames/expected,
ERROR, or cut off by a skip or a new play), whether any seek happened while
it played, the MEM figures logged at its STARTED; then per-format minimum and
maximum of those figures, the trend from the first quarter of the run to the
last, the `stat` figures, underruns at the end, and every warning the script
logged. With --table every track is listed; without, only the FLACs that
played from start to end with a mismatched frame count, and the summary.
"""
import re
import sys
from statistics import mean


def parse(path):
    plays = []  # dicts
    stats = []
    notes = []
    cur = None
    seeking = False
    for raw in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\s*([\d.]+) (.*)", raw.rstrip("\n"))
        if not m:
            continue
        t, line = float(m.group(1)), m.group(2)
        if line.startswith("# "):
            notes.append((t, line[2:]))
            continue
        if line.startswith("> "):
            if re.match(r"> (seek|hold [46])", line):
                seeking = True
            continue
        if "player: STARTED path=" in line:
            if cur is not None and cur["end"] is None:
                cur["end"] = "cut off"
            mm = re.search(r"STARTED path=(\S.*?) rate=(\d+)", line)
            cur = {"t": t, "path": mm.group(1), "rate": int(mm.group(2)),
                   "end": None, "seeked": False, "mem": None, "mem_end": None}
            plays.append(cur)
            seeking = False
        elif "player: MEM started" in line and cur is not None:
            cur["mem"] = dict((k, int(v)) for k, v in
                              re.findall(r"(\w+)=(\d+)", line.split("MEM started")[1]))
        elif "player: MEM ended" in line and cur is not None:
            cur["mem_end"] = dict((k, int(v)) for k, v in
                                  re.findall(r"(\w+)=(\d+)", line.split("MEM ended")[1]))
        elif "player: ENDED frames=" in line and cur is not None:
            mm = re.search(r"frames=(\d+) expected=(\d+)", line)
            cur["end"] = (int(mm.group(1)), int(mm.group(2)))
            cur["seeked"] = cur["seeked"] or seeking
        elif "player: ERROR reason=" in line:
            reason = line.split("reason=")[1]
            if cur is not None and cur["end"] is None and "id=" not in reason:
                cur["end"] = "ERROR " + reason
            else:
                plays.append({"t": t, "path": "(failed before STARTED)",
                              "end": "ERROR " + reason, "seeked": False,
                              "mem": None, "mem_end": None, "rate": 0})
        elif "SCRUB to" in line and cur is not None:
            cur["seeked"] = True
        elif "dbg: stat " in line:
            stats.append((t, dict(re.findall(r"(\w+)=(\S+)", line.split("dbg: stat ")[1]))))
        if seeking and cur is not None:
            cur["seeked"] = True
            seeking = False
    return plays, stats, notes


def fmt(p):
    path = p["path"]
    return "FLAC" if path.lower().endswith(".flac") else "MP3" if path.lower().endswith(".mp3") else "?"


def main():
    args = sys.argv[1:]
    table = "--table" in args
    args = [a for a in args if a != "--table"]
    plays, stats, notes = parse(args[0])
    started = [p for p in plays if p["mem"]]
    ended = [p for p in plays if isinstance(p["end"], tuple)]
    bad = []
    for p in plays:
        e = p["end"]
        whole = isinstance(e, tuple) and not p["seeked"]
        if whole and fmt(p) == "FLAC" and e[0] != e[1] and "t09_truncated" not in p["path"]:
            bad.append(p)
        if table:
            m = p["mem"] or {}
            print(f"{p['t']:8.1f} {p['path'][-38:]:38} "
                  f"{str(e) if e else '(running)':24} {'seek' if p['seeked'] else '':4} "
                  f"free={m.get('int_free', '-')} largest={m.get('int_largest', '-')} "
                  f"min={m.get('int_min', '-')} psram={m.get('psram_free', '-')} "
                  f"psram_largest={m.get('psram_largest', '-')}")
    print(f"\ntrack starts: {len(started)}, ENDED: {len(ended)}, "
          f"errors: {sum(1 for p in plays if isinstance(p['end'], str) and p['end'].startswith('ERROR'))}, "
          f"cut off (skip): {sum(1 for p in plays if p['end'] == 'cut off')}")
    flac_whole = [p for p in ended if fmt(p) == "FLAC" and not p["seeked"]
                  and "t09_truncated" not in p["path"]]
    print(f"FLACs played start to end: {len(flac_whole)}, frames exact: "
          f"{sum(1 for p in flac_whole if p['end'][0] == p['end'][1])}")
    for p in bad:
        print(f"  MISMATCH {p['path']} {p['end']}")
    t09 = [p["end"] for p in plays if "t09_truncated" in p["path"]]
    if t09:
        print(f"t09_truncated (cut short on purpose): {sorted(set(map(str, t09)))}")
    if started:
        n = len(started)
        q = max(1, n // 4)
        for key in ("int_free", "int_largest", "int_min", "psram_free", "psram_largest"):
            for f in ("FLAC", "MP3"):
                vals = [p["mem"][key] for p in started if fmt(p) == f]
                if not vals:
                    continue
                first = [p["mem"][key] for p in started[:q] if fmt(p) == f]
                last = [p["mem"][key] for p in started[-q:] if fmt(p) == f]
                print(f"MEM started {key:13} {f:4}: min {min(vals):8} max {max(vals):8}"
                      f"  first quarter mean {mean(first) if first else float('nan'):10.0f}"
                      f"  last quarter mean {mean(last) if last else float('nan'):10.0f}")
        ends = [p["mem_end"] for p in plays if p["mem_end"]]
        if ends:
            for key in ("int_free", "int_largest", "psram_free", "psram_largest"):
                vals = [e[key] for e in ends]
                print(f"MEM ended   {key:13}     : min {min(vals):8} max {max(vals):8}"
                      f"  first {vals[0]}  last {vals[-1]}")
    if stats:
        for key in ("heap_data", "heap_data_largest", "heap_data_min", "psram", "psram_largest"):
            vals = [int(s[key]) for _, s in stats if key in s]
            if vals:
                print(f"stat {key:17}: min {min(vals):8} max {max(vals):8} first {vals[0]} last {vals[-1]}")
        print(f"underruns at the last stat: {stats[-1][1].get('underruns')}, "
              f"dec_stack_free min {min(int(s['dec_stack_free']) for _, s in stats)}, "
              f"out_stack_free min {min(int(s['out_stack_free']) for _, s in stats)}, "
              f"ui_stack_free min {min(int(s['ui_stack_free']) for _, s in stats)}")
    for t, n in notes:
        if "WARNING" in n or "serial error" in n or "run " in n:
            print(f"note {t:8.1f}: {n}")


if __name__ == "__main__":
    main()
