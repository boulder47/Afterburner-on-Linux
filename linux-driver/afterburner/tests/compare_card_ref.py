#!/usr/bin/env python3
"""compare_card_ref.py — compare Afterburner card output (padded interleaved
yuv422p8, [U,Y0,V,Y1] groups) against an FFmpeg yuv422p reference.

Usage: compare_card_ref.py <card.yuv> <ref.yuv> W H [stride]
  stride defaults to ceil(W*2/64)*64 (firmware padded layout, FINDINGS §35.2).

Reports luma and chroma match stats, testing both direct-row and even-row
chroma hypotheses (§32: card chroma may be packed at half vertical rate).
"""
import sys


def main():
    card_path, ref_path = sys.argv[1], sys.argv[2]
    w, h = int(sys.argv[3]), int(sys.argv[4])
    stride = int(sys.argv[5]) if len(sys.argv) > 5 else ((w * 2 + 63) // 64) * 64

    card = open(card_path, "rb").read()
    ref = open(ref_path, "rb").read()
    need = w * h * 2
    assert len(card) >= stride * h, f"card too small: {len(card)} < {stride*h}"
    assert len(ref) == need, f"ref size {len(ref)} != {need}"

    y_ref = ref[:w * h]
    u_ref = ref[w * h:w * h + (w // 2) * h]
    v_ref = ref[w * h + (w // 2) * h:]
    cw = w // 2  # chroma width

    def stats(name, pairs):
        n = len(pairs)
        exact = sum(1 for a, b in pairs if a == b)
        d1 = sum(1 for a, b in pairs if abs(a - b) <= 1)
        d2 = sum(1 for a, b in pairs if abs(a - b) <= 2)
        maxd = max((abs(a - b) for a, b in pairs), default=0)
        print(f"{name:28s} n={n:7d} exact={exact/n*100:6.2f}% "
              f"±1={d1/n*100:6.2f}% ±2={d2/n*100:6.2f}% maxdiff={maxd}")

    y_pairs, u_pairs, v_pairs = [], [], []
    for y in range(h):
        row = card[y * stride:(y + 1) * stride]
        for g in range(cw):
            u, y0, v, y1 = row[g * 4], row[g * 4 + 1], row[g * 4 + 2], row[g * 4 + 3]
            y_pairs.append((y0, y_ref[y * w + g * 2]))
            y_pairs.append((y1, y_ref[y * w + g * 2 + 1]))
            u_pairs.append((u, u_ref[y * cw + g]))
            v_pairs.append((v, v_ref[y * cw + g]))

    print(f"card: {len(card)} B (stride={stride})   ref: {w}x{h} yuv422p")
    stats("luma  card[y] vs ref[y]", y_pairs)
    stats("chromaU card[y] vs ref[y]", u_pairs)
    stats("chromaV card[y] vs ref[y]", v_pairs)

    # half-vertical hypothesis: card row y carries ref chroma row 2y (for y < h/2)
    u2, v2 = [], []
    for y in range(h // 2):
        row = card[y * stride:(y + 1) * stride]
        for g in range(cw):
            u2.append((row[g * 4], u_ref[(2 * y) * cw + g]))
            v2.append((row[g * 4 + 2], v_ref[(2 * y) * cw + g]))
    print(f"--- half-vertical chroma hypothesis (card row y vs ref row 2y, y<{h//2}) ---")
    stats("chromaU card[y] vs ref[2y]", u2)
    stats("chromaV card[y] vs ref[2y]", v2)


if __name__ == "__main__":
    main()
