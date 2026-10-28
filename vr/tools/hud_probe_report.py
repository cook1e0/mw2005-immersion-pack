r"""Summarize bounded UI captures. Does not launch or control the game.

Usage: python vr/tools/hud_probe_report.py C:\Games\NFSMW --require-duplicates
Only complete bursts should be used to judge coverage. Timing columns measure CPU submission
and include diagnostic work; they do not establish GPU cost or an FPS regression.
"""
import argparse
import csv
from collections import defaultdict
from pathlib import Path
import math


def rows(path):
    with path.open(newline="", encoding="utf-8") as file:
        return list(csv.DictReader(file))


def summarize(path):
    frames = rows(path)
    base = str(path)[:-len("_frames.csv")]
    draws = rows(Path(base + "_draws.csv"))
    clips = rows(Path(base + "_clips.csv"))
    if not frames:
        print(f"{path.name}: capture has no completed HUD frames")
        return False
    sums = {key: sum(int(r[key]) for r in frames) for key in
            ("draws", "inside", "recognized", "duplicates", "skipped", "outside", "clips", "changed_clips", "cached")}
    dimensions = sorted({(r["width"], r["height"], r["fixhud"], r["samples"], r["quality"]) for r in frames})
    print(f"\n{path.name}: {len(frames)} of 45 frames, size/FixHUD/MSAA/quality={dimensions}")
    print("  " + ", ".join(f"{key}={value}" for key, value in sums.items()))
    assert all(int(r["duplicates"]) <= int(r["recognized"]) <= int(r["inside"]) <= int(r["draws"]) for r in frames)
    print(f"  draws outside an identified FE call: {sums['draws'] - sums['inside']}")
    groups = defaultdict(lambda: [math.inf, math.inf, -math.inf, -math.inf, 0])
    formats = defaultdict(int)
    for row in draws:
        if row["speedo"] != "1":
            continue
        formats[(row["type"], row["fvf"], row["vs"], row["ps"], row["z"], row["stencil"])] += 1
        values = [float(row[k]) for k in ("canvas_l", "canvas_t", "canvas_r", "canvas_b")]
        if not all(math.isfinite(v) for v in values):
            continue
        group = groups[row["group"]]
        group[:4] = [min(group[0], values[0]), min(group[1], values[1]),
                     max(group[2], values[2]), max(group[3], values[3])]
        group[4] += 1
    for key, group in sorted(groups.items()):
        print(f"  speedometer top group {key}: measured canvas union={group[:4]}, {group[4]} polygons")
    print(f"  speedometer formats (type,FVF,VS,PS,Z,stencil): {dict(formats)}")
    print(f"  changed CPU-clip records: {sum(r['changed'] == '1' for r in clips)}")
    errors = sorted({r["duplicate_hr"] for r in draws if r["speedo"] == "1" and
                     r["duplicate_hr"] not in ("00000000", "00000001")})
    if errors:
        print(f"  duplicate failures: {errors}")
    if any(r["tree_ok"] != "1" for r in frames):
        print("  WARNING: incomplete live tree; duplication disabled for those frames")
    if sums["outside"]:
        print("  WARNING: fixed source rectangle needs calibration; do not claim complete output")
    if not sums["duplicates"]:
        print("  No duplicated speedometer draws. Inspect discovery, format, depth and canvas scales.")
    return sums["duplicates"] > 0 and not errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path)
    parser.add_argument("--require-duplicates", action="store_true")
    args = parser.parse_args()
    paths = sorted(args.path.glob("nfsmw_hudprobe_*_frames.csv")) if args.path.is_dir() else [args.path]
    if not paths:
        parser.error("No HUD probe captures found")
    results = [summarize(path) for path in paths]
    if args.require_duplicates and not any(results):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
