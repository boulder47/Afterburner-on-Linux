# Apple Afterburner — Linux Driver & Realtime ProRes Player

A complete, working **Linux kernel driver** for the Apple Afterburner ProRes decoder card
(PCI ID `[106b:0205]`), built from a byte-level reverse engineering of Apple's closed-source
macOS kext (`AppleAfterburner.kext`, arm64e slice). Includes a **realtime SDL2 video player**
that decodes ProRes MOV/MP4 files on the card at full framerate, plus control and test tooling.

The card is decode-only (no encoder datapath exists in the firmware). Firmware 2.62 — the
latest public Afterburner bitfile (Sonoma and Sequoia kexts ship byte-identical streams) —
decodes **ProRes 422** and **ProRes 422 HQ** pixel-perfectly (±1 vs FFmpeg, pure 10→8-bit
rounding). ProRes 4444 and ProRes RAW are acknowledged by the engine but produce no output.

I have a MacPro 7,1 that I put Limux on and its from this machine I tested the Apple Afterburner Card under Limux.


## Verified status

All validation-matrix rows V1–V8 pass:

- single-frame round-trips from 320×180 up to full 4K (3840×2160)
- multi-frame streams bit-exact vs FFmpeg reference (`prorestest.mov`: 870/870 frames, maxdiff=0)
- sustained **realtime 4K HQ @ 30 fps** playback: two consecutive full passes (568 frames),
  zero late frames / recoveries / AER errors; average decode ~2.75 ms (~10× headroom)
- long-idle survival with the decoder left enabled (~27 min, no kill state)

## Repository layout

```
linux-driver/afterburner/   production driver + userspace tools (the deliverable)
  ab_pci.c ab_regs.c ab_engine.c ab_dma.c ab_chardev.c ab_sysfs.c
  include/                  ab.h, ab_regs.h — register map & ioctl ABI
  Makefile                  Kbuild module build
  userspace/abctl           control client (status / decode / start / stop / unbind)
  userspace/abplayer        realtime SDL2 ProRes player + verification modes
  tests/                    V-matrix test clients, comparison scripts, soak/negative tests
docs/                       deliverable documentation (kext read, VideoToolbox path analysis)
testdata/                   single-frame ProRes fixtures (320×180, 4K, RAW) for abctl decode
dev/                        all development work — RE findings & plans, kext bundles,
                            static-RE artifacts, early probe modules, captures, sample media
```

## Requirements

- Linux with a kernel build environment (`/lib/modules/$(uname -r)/build`)
- `sudo` for `insmod` / device access
- SDL2 development package (only for building `abplayer`; prebuilt binaries are checked in)

## Build & load the driver

```sh
cd linux-driver/afterburner
make                      # builds afterburner.ko against the running kernel
sudo insmod afterburner.ko
```

On probe the module brings the card up (kext-parity init sequence), creates
`/dev/afterburner0`, and exposes live register attributes under
`/sys/class/afterburner/` (`board_id`, `fw_version`, `decoder_ready`, `completion`,
thermal sensors, …). Check with:

```sh
./userspace/abctl status          # card state + thermal snapshot
dmesg | tail                      # bring-up log
```

## Realtime playback (abplayer)

```sh
# rebuild if you changed the source (prebuilt binaries are checked in):
gcc -O2 -o userspace/abplayer userspace/abplayer.c $(pkg-config --cflags --libs sdl2)

./userspace/abplayer dev/testmedia/prorestest.mov
```

- no file argument → interactive picker over `.mov`/`.mp4` files in the cwd
- keys: `q`/ESC quit, space pause/resume, `f` fullscreen
- `-s` sync submit path (30 s poll) instead of the default async pipeline
- verification modes: `--headless` (no window), `--no-pace` (run at card speed),
  `--dump-dir DIR` (write each decoded frame's raw card output as `frame_<idx>.yuv`)

Sample clips live in `dev/testmedia/`: `prorestest.mov` (the canonical test clip,
870 frames), `prores422.MOV` (sustained 4K HQ @30 fps), plus RAW/MXF material used for
profile-matrix testing.

## Single-frame decode & control (abctl)

```sh
gcc -O2 -o userspace/abctl userspace/abctl.c     # no dependencies beyond libc

./userspace/abctl status                          # card state + thermal snapshot
./userspace/abctl decode testdata/prores_3840x2160_frame0.bin out.yuv
                                                  # full round-trip: bind -> mmap -> submit -> dump;
                                                  # auto-parses W/H from the frame header and sizes
                                                  # output per the firmware's padded layout
./userspace/abctl stop                            # disable decoder (kext stop() parity)
./userspace/abctl start                           # re-bring-up after stop
./userspace/abctl unbind                          # release bound DMA regions
```

The ioctl ABI on `/dev/afterburnerN` mirrors the kext's user-client `sMethods` one-to-one:
`submitFrame`, `queryCompletion`, `bindFrame`, `cardStatus`, `checkForSpace`, `unbindAll`,
`getPercent`, `stop`, `start`, plus async pair (`SUBMIT_ASYNC`/`WAIT_COMPL`) and a FLR
escape hatch (`AB_IOC_RESET`). See `ab_chardev.c` for the struct definitions.

### Decoded output format

The card emits **interleaved 8-bit YUV422** — `[U, Y0, V, Y1]` groups (SDL `YUY2`, so
`abplayer` blits with zero conversion). Buffer layout is fixed regardless of buffer size:
row stride = `ceil(W*2/64)*64` bytes, rows = `ceil(H/16)*16`; padding rows repeat the last
real row. Only the first H rows are real content.

## Documentation & development work

- `docs/videotoolbox-afterburner-decode-path.md` — how macOS drives this card (VideoToolbox →
  kext user client), with every claim mapped to byte-verified kext code
- `docs/afterburner-kext-read.md`, `docs/wwdc20-10090-transcript.md` — supporting material
- `dev/FINDINGS.md` — the authoritative reverse-engineering log (register map, descriptor
  protocol, completion semantics, AER root cause, output-format pinning)
- `dev/plan2.md` / `dev/plan.md` — plan of record and historical phase plans
- `dev/rework/` — static RE workspace: extracted kext slices, SDK headers, analysis scripts
- `dev/kexts/` — original + Sonoma + Sequoia kext bundles (firmware provenance)
- `dev/linux-probes/` — the early single-file test modules that decoded the protocol
  (Phases 0–3), with their register dumps
