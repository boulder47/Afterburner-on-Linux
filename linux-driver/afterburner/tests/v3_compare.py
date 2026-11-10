#!/usr/bin/env python3
"""v3_compare.py — V3 (plan.md §8.5) card-output vs FFmpeg-reference compare.

Card output layout (FINDINGS §32, live-verified on 320x180):
    interleaved [U, Y0, V, Y1] x (W/2 groups per row) x H rows = W*H*2 bytes
    luma: full height, all rows valid
    chroma: HALF vertical resolution — card row y matches reference chroma
            row 2y for y < H/2; rows >= H/2 carry no valid chroma.

Reference (FFmpeg planar yuv422p 8-bit):
    Y plane W*H | U plane (W/2)*H | V plane (W/2)*H   [total = W*H*2]
        ffmpeg -i p4k_f0.mov -frames:v 1 -pix_fmt yuv422p -f rawvideo ref.yuv

Usage:
    python3 v3_compare.py <card_out.bin> <ref_i422_8.yuv> [--w 3840] [--h 2160]
"""
import argparse
import numpy as np


def stats(name, a, b):
    d = np.abs(a.astype(np.int16) - b.astype(np.int16))
    n = d.size
    exact = int((d == 0).sum())
    within1 = int((d <= 1).sum())
    print(f"{name}: n={n} exact={exact/n:.4%} within±1={within1/n:.4%} "
          f"MAD={d.mean():.4f} maxdiff={int(d.max())}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("card_out")
    ap.add_argument("ref_yuv")
    ap.add_argument("--w", type=int, default=3840)
    ap.add_argument("--h", type=int, default=2160)
    args = ap.parse_args()

    W, H = args.w, args.h
    card = np.fromfile(args.card_out, dtype=np.uint8)
    ref = np.fromfile(args.ref_yuv, dtype=np.uint8)
    print(f"card: {card.size} bytes (expect {W*H*2})  ref: {ref.size} bytes "
          f"(expect {2*W*H}, planar yuv422p = Y + 2x(W/2)H)")
    assert card.size == W * H * 2, "card output size mismatch"
    assert ref.size == 2 * W * H, "reference size mismatch (yuv422p planar)"

    # sentinel coverage: any byte still 0x5A = c2h DMA never touched it
    untouched = int((card == 0x5A).sum())
    print(f"sentinel(0x5A) bytes remaining: {untouched} "
          f"({'FULL COVERAGE' if untouched == 0 else 'PARTIAL — inspect tail'})")

    # longest run of identical bytes (hole-pattern check, §32.4)
    c = card.tobytes()
    best = cur = 1
    for i in range(1, len(c)):
        cur = cur + 1 if c[i] == c[i - 1] else 1
        best = max(best, cur)
    print(f"longest identical-byte run: {best} "
          f"({'OK' if best < 32 else 'HOLE PATTERN PRESENT'})")

    # card layout -> planes
    g = card.reshape(H, W // 2, 4)
    luma = np.stack([g[:, :, 1], g[:, :, 3]], axis=2).reshape(H, W)
    card_u = g[:, :, 0]   # H x (W/2)
    card_v = g[:, :, 2]

    ysz = W * H
    cysz = W * H // 2
    Y = ref[:ysz].reshape(H, W)
    U = ref[ysz:ysz + cysz].reshape(H, W // 2)   # yuv422p: full vertical res
    V = ref[ysz + cysz:].reshape(H, W // 2)

    print("\n--- luma (all rows) ---")
    stats("Y", luma, Y)

    print(f"\n--- chroma valid region (card rows 0..{H//2 - 1} vs ref even rows) ---")
    stats("U", card_u[:H // 2], U[::2][:H // 2])
    stats("V", card_v[:H // 2], V[::2][:H // 2])

    print(f"\n--- chroma invalid region (card rows {H//2}..{H-1}) ---")
    tail = g[H // 2:, :, ::2]  # U and V bytes of the bottom half
    print(f"bottom-half chroma: mean={tail.mean():.2f} "
          f"(garbage expected — deterministic per §32.5, not a failure)")


if __name__ == "__main__":
    main()
