#!/usr/bin/env python3
"""compare_fullstream.py — per-frame pixel verification of card output dumps
against an FFmpeg yuv422p reference, for a FULL decode run (all frames).

Usage: compare_fullstream.py <ref.yuv> <dump_dir> W H [--start N] [--count N]
                             [--stride S] [--tol-pct 99.5] [--maxdiff 2]

Inputs:
  ref.yuv        planar yuv422p rawvideo from FFmpeg, one frame per W*H*2 B:
                 [Y: W*H][U: (W/2)*H][V: (W/2)*H]
  dump_dir       abplayer --dump-dir output: frame_<absidx>.yuv, card layout =
                 interleaved [U,Y0,V,Y1] groups, row stride S bytes
                 (default ceil(W*2/64)*64), first H rows real (§35.2).

A frame PASSES when luma AND both chroma planes have within-±1 >= tol-pct
AND maxdiff <= maxdiff on every plane.  §38 baseline: warm decoder with real
motion is pixel-perfect (±1 = 100%, maxdiff = 1) — so the default tolerance
flags any frame that deviates from the proven-good behavior.

Exit code: number of failed frames (capped at 250); 251 if dumps are missing.
"""
import argparse
import os
import sys

import numpy as np


def plane_stats(card, ref):
    d = np.abs(card.astype(np.int16) - ref.astype(np.int16))
    n = d.size
    return {
        "exact": float((d == 0).sum()) / n * 100.0,
        "p1": float((d <= 1).sum()) / n * 100.0,
        "p2": float((d <= 2).sum()) / n * 100.0,
        "maxd": int(d.max()),
    }


def fmt(name, s):
    return (f"{name} exact={s['exact']:6.2f}% ±1={s['p1']:6.2f}% "
            f"±2={s['p2']:6.2f}% maxd={s['maxd']}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ref")
    ap.add_argument("dump_dir")
    ap.add_argument("w", type=int)
    ap.add_argument("h", type=int)
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--count", type=int, default=None)
    ap.add_argument("--stride", type=int, default=None)
    ap.add_argument("--tol-pct", type=float, default=99.5)
    ap.add_argument("--maxdiff", type=int, default=2)
    a = ap.parse_args()

    w, h = a.w, a.h
    cw = w // 2
    stride = a.stride if a.stride else ((w * 2 + 63) // 64) * 64
    frame_bytes = w * h * 2          # planar yuv422p
    card_frame = stride * h

    ref_size = os.path.getsize(a.ref)
    n_ref = ref_size // frame_bytes
    if a.count is None:
        count = n_ref - a.start
    else:
        count = min(a.count, n_ref - a.start)
    print(f"ref: {a.ref} ({n_ref} frames of {w}x{h})  "
          f"dumps: {a.dump_dir}  range [{a.start}, {a.start + count})")

    ref = np.memmap(a.ref, dtype=np.uint8, mode="r", shape=(n_ref, frame_bytes))

    fails = []
    missing = []
    worst = {"luma": (float("inf"), -1), "chromaU": (float("inf"), -1),
             "chromaV": (float("inf"), -1)}  # min ±1 % across frames
    for i in range(a.start, a.start + count):
        path = os.path.join(a.dump_dir, f"frame_{i}.yuv")
        if not os.path.exists(path) or os.path.getsize(path) < card_frame:
            missing.append(i)
            continue
        card = np.fromfile(path, dtype=np.uint8, count=card_frame).reshape(h, cw, 4)
        y_c = np.stack([card[:, :, 1], card[:, :, 3]], axis=2).reshape(h, w)
        u_c = card[:, :, 0]
        v_c = card[:, :, 2]

        r = ref[i]
        y_r = r[:w * h].reshape(h, w)
        u_r = r[w * h:w * h + cw * h].reshape(h, cw)
        v_r = r[w * h + cw * h:].reshape(h, cw)

        sl = plane_stats(y_c, y_r)
        su = plane_stats(u_c, u_r)
        sv = plane_stats(v_c, v_r)

        ok = all(s["p1"] >= a.tol_pct and s["maxd"] <= a.maxdiff for s in (sl, su, sv))
        if not ok:
            fails.append((i, sl, su, sv))
        for key, s in (("luma", sl), ("chromaU", su), ("chromaV", sv)):
            if s["p1"] < worst[key][0]:
                worst[key] = (s["p1"], i)

        print(f"frame {i:4d}: {'PASS' if ok else 'FAIL'}  "
              f"{fmt('luma   ', sl)} | {fmt('chromaU', su)} | {fmt('chromaV', sv)}")

    print("\n=== summary ===")
    compared = count - len(missing)
    print(f"frames: {count} total, {compared} compared, "
          f"{len(fails)} failed, {len(missing)} missing dumps")
    for key in ("luma", "chromaU", "chromaV"):
        p1, i = worst[key]
        print(f"worst {key:7s}: ±1={p1:6.2f}% at frame {i}")
    if fails:
        print("failed frames:")
        for i, sl, su, sv in fails:
            print(f"  {i}: {fmt('luma   ', sl)} | {fmt('chromaU', su)} | "
                  f"{fmt('chromaV', sv)}")
    if missing:
        lo, hi = min(missing), max(missing)
        shown = ", ".join(map(str, missing[:20])) + (" ..." if len(missing) > 20 else "")
        print(f"missing dumps ({len(missing)}): {shown} (range {lo}-{hi})")

    code = min(len(fails), 250)
    sys.exit(251 if missing else code)


if __name__ == "__main__":
    main()
