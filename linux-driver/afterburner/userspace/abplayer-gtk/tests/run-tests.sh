#!/usr/bin/env bash
# run-tests.sh — `make check` for Afterburner Player. No card needed.
#
#  1. parser + per-frame self-check against ffmpeg-made ProRes clips
#     (all four 422 profiles, odd width, faststart + colr, interlaced/PAR)
#     plus a sparse >4 GiB file (64-bit mdat largesize) and audio-track-first
#  2. rejections: 4444, 4444 XQ, non-ProRes, truncated file, corrupt sample table
#  3. fuzzing the parser under ASan/UBSan
#  4. if a Wayland/X display is available and ./abplayer-gtk is built: the
#     full display pipeline — real ProRes frames decoded by ffmpeg to UYVY are
#     replayed through the simulated card in the card's padded layout, rendered
#     by the GL path, read back with --export-png and compared against an
#     independent model of the conversion (BT.601/709 limited range, co-sited
#     4:2:2 chroma). Expected: max diff <= 1.
set -uo pipefail
cd "$(dirname "$0")/.."
OUT=tests/out
TM=./tests/test_mov
mkdir -p "$OUT"
pass=0
fail=0

if ! command -v ffmpeg >/dev/null; then
	echo "SKIP: ffmpeg is needed to generate ProRes test clips (pacman -S ffmpeg)"
	exit 77
fi

gen() { ffmpeg -loglevel error -y "$@" || { echo "ffmpeg failed: $*"; exit 1; }; }
run() {
	local name=$1
	shift
	if "$@" >"$OUT/$name.log" 2>&1; then
		echo "PASS  $name"
		pass=$((pass + 1))
	else
		echo "FAIL  $name"
		sed 's/^/      /' "$OUT/$name.log"
		fail=$((fail + 1))
	fi
}

echo "== generating clips in $OUT"
for p in 0 1 2 3; do
	gen -f lavfi -i testsrc2=size=640x360:rate=30000/1001 -frames:v 20 \
		-c:v prores_ks -profile:v $p "$OUT/p$p.mov"
done
gen -f lavfi -i testsrc2=size=640x360:rate=30000/1001 -frames:v 10 \
	-c:v prores_ks -profile:v 4 -pix_fmt yuva444p10le "$OUT/p4444.mov"
gen -f lavfi -i testsrc2=size=322x180:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 \
	-t 1 -c:v prores_ks -profile:v 2 -c:a pcm_s16le -shortest "$OUT/odd_audio.mov"
gen -f lavfi -i testsrc2=size=1920x1080:rate=24 -frames:v 6 -c:v prores_ks -profile:v 3 \
	-color_primaries bt709 -color_trc bt709 -colorspace bt709 -movflags +faststart "$OUT/hd_colr.mov"
gen -f lavfi -i testsrc2=size=1440x1080:rate=30000/1001 -frames:v 4 -c:v prores_ks -profile:v 2 \
	-flags +ildct+ilme -top 1 -vf setsar=4/3 "$OUT/interlaced_par.mov"
gen -f lavfi -i testsrc2=size=640x360:rate=30000/1001 -frames:v 3 \
	-c:v prores_ks -profile:v 5 -pix_fmt yuva444p10le "$OUT/p4444xq.mov"
gen -f lavfi -i testsrc2=size=640x360:rate=30 -frames:v 5 -c:v mpeg4 "$OUT/mpeg4.mp4"
gen -i "$OUT/odd_audio.mov" -map 0:a -map 0:v -c copy "$OUT/audio_first.mov"
# >4 GiB recording layout without writing 4 GiB: rewrite mdat as a 64-bit
# largesize box padded with a sparse 4.5 GiB hole, moov after it. ffmpeg's
# 8-byte 'wide' box before mdat makes room for the 16-byte header, so chunk
# offsets stay valid.
python3 - "$OUT/p3.mov" "$OUT/largesize_4g.mov" <<'EOF'
import struct, sys
src = open(sys.argv[1], 'rb').read()
boxes, off = [], 0
while off + 8 <= len(src):
    sz, = struct.unpack('>I', src[off:off + 4])
    boxes.append((src[off + 4:off + 8], off, sz))
    off += sz
assert [b[0] for b in boxes] == [b'ftyp', b'wide', b'mdat', b'moov'], boxes
(_, fo, fs), _, (_, mo, ms), (_, vo, vs) = boxes
hole = 4608 << 20
payload = src[mo + 8:mo + ms]
with open(sys.argv[2], 'wb') as f:
    f.write(src[fo:fo + fs])
    f.write(struct.pack('>I4sQ', 1, b'mdat', 16 + len(payload) + hole))
    f.write(payload)
    f.seek(hole, 1)
    f.write(src[vo:vo + vs])
EOF
head -c 200000 "$OUT/p2.mov" >"$OUT/truncated.mov"
python3 - "$OUT/p2.mov" "$OUT/bad_stsz.mov" <<'EOF'
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
i = d.find(b'stsz')
e = i + 4 + 12 + 4 * 5                      # sample #5's size entry
v = int.from_bytes(d[e:e + 4], 'big')
d[e:e + 4] = (v + 100).to_bytes(4, 'big')   # FINDINGS §42.1-style corruption
open(sys.argv[2], 'wb').write(d)
EOF

echo "== parser"
run apco      $TM --expect fourcc=apco w=640 h=360 n=20 fps=29.97 matrix=6 "$OUT/p0.mov"
run apcs      $TM --expect fourcc=apcs w=640 h=360 n=20 fps=29.97 "$OUT/p1.mov"
run apcn      $TM --expect fourcc=apcn w=640 h=360 n=20 fps=29.97 "$OUT/p2.mov"
run apch      $TM --expect fourcc=apch w=640 h=360 n=20 fps=29.97 "$OUT/p3.mov"
run odd_width $TM --expect w=322 h=180 n=25 fps=25 "$OUT/odd_audio.mov"
run faststart_colr $TM --expect fourcc=apch w=1920 h=1080 n=6 fps=24 matrix=1 "$OUT/hd_colr.mov"
run interlaced_par $TM --expect w=1440 h=1080 n=4 "$OUT/interlaced_par.mov"
run audio_track_first $TM --expect fourcc=apcn w=322 h=180 n=25 "$OUT/audio_first.mov"
run largesize_mdat_4g $TM --expect fourcc=apch w=640 h=360 n=20 "$OUT/largesize_4g.mov"
rm -f "$OUT/largesize_4g.mov" # sparse, but don't leave a 4.5 GiB file behind
grep -q "par=4:3 il=1" "$OUT/interlaced_par.log" && echo "PASS  par_and_interlace_flags" &&
	pass=$((pass + 1)) || { echo "FAIL  par_and_interlace_flags"; fail=$((fail + 1)); }
grep -q "stride=704 rows=192" "$OUT/odd_width.log" && echo "PASS  odd_width_card_layout (FINDINGS §35.3)" &&
	pass=$((pass + 1)) || { echo "FAIL  odd_width_card_layout"; fail=$((fail + 1)); }

echo "== rejections"
run reject_4444      $TM --expect-fail "$OUT/p4444.mov"
run reject_4444xq    $TM --expect-fail "$OUT/p4444xq.mov"
run reject_mpeg4     $TM --expect-fail "$OUT/mpeg4.mp4"
run reject_truncated $TM --expect-fail "$OUT/truncated.mov"
run reject_bad_stsz  $TM --expect-fail "$OUT/bad_stsz.mov"

echo "== fuzz (ASan/UBSan)"
run fuzz_p2  $TM --fuzz 3000 1 "$OUT/p2.mov"
run fuzz_odd $TM --fuzz 2000 2 "$OUT/odd_audio.mov"
run fuzz_hd  $TM --fuzz 1000 3 "$OUT/hd_colr.mov"

echo "== display pipeline (simulated card replaying ffmpeg-decoded frames)"
if [ ! -x ./abplayer-gtk ]; then
	echo "SKIP  ./abplayer-gtk not built"
elif [ -z "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]; then
	echo "SKIP  no Wayland/X display"
else
	gen -i "$OUT/odd_audio.mov" -frames:v 10 -pix_fmt uyvy422 -f rawvideo "$OUT/odd.uyvy"
	gen -i "$OUT/hd_colr.mov" -frames:v 6 -pix_fmt uyvy422 -f rawvideo "$OUT/hd.uyvy"
	pipeline() { # clip uyvy w h frame matrix
		local png="$OUT/pipe_$(basename "$1" .mov)_f$5.png"
		AB_SIM_UYVY="$2" GTK_A11Y=none ./abplayer-gtk --simulate --frame "$5" \
			--export-png "$png" "$1" || return 1
		ffmpeg -loglevel error -y -i "$png" -f rawvideo -pix_fmt rgb24 "$png.rgb" || return 1
		python3 - "$2" "$png.rgb" "$3" "$4" "$5" "$6" <<'EOF'
import sys
uyvy, rgbf, W, H, n, m = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
row = (W + 1) // 2 * 4
raw = open(uyvy, 'rb').read()[n * row * H:(n + 1) * row * H]
rgb = open(rgbf, 'rb').read()
kr, kb = (0.2126, 0.0722) if m == 1 else (0.299, 0.114)
kg = 1 - kr - kb
worst = 0
for y in range(H):
    r = raw[y * row:(y + 1) * row]
    groups = len(r) // 4
    for x in range(W):
        g = x // 2
        Y = r[g * 4 + (1 if x % 2 == 0 else 3)]
        if x % 2 == 0:
            U, V = r[g * 4], r[g * 4 + 2]
        else:
            n2 = min(g + 1, groups - 1)
            U = (r[g * 4] + r[n2 * 4]) / 2
            V = (r[g * 4 + 2] + r[n2 * 4 + 2]) / 2
        yy, cb, cr = (Y - 16) / 219, (U - 128) / 224, (V - 128) / 224
        exp = (yy + (2 - 2 * kr) * cr,
               yy - (2 * kb * (1 - kb) / kg) * cb - (2 * kr * (1 - kr) / kg) * cr,
               yy + (2 - 2 * kb) * cb)
        o = (y * W + x) * 3
        for c in range(3):
            e = min(255, max(0, round(exp[c] * 255)))
            worst = max(worst, abs(rgb[o + c] - e))
print(f"{W}x{H} frame {n}: max |rendered - model| = {worst}")
sys.exit(0 if worst <= 1 else 1)
EOF
	}
	run pipeline_322x180_bt601 pipeline "$OUT/odd_audio.mov" "$OUT/odd.uyvy" 322 180 7 6
	run pipeline_1080p_bt709 pipeline "$OUT/hd_colr.mov" "$OUT/hd.uyvy" 1920 1080 3 1
fi

echo "== $pass passed, $fail failed"
[ "$fail" -eq 0 ]
