# dev/testmedia/ — sample video material

Large/regenerable test media for `abplayer` and the V-matrix tests. The video files are kept on
disk but **not tracked in git** (see `.gitignore`) — only this index file (and the small
historical `frame0.png`) is committed.

| File | Size | Codec / format | Used for |
|---|---|---|---|
| `prorestest.mov` | 57M | ProRes 422, 870 frames | Canonical test clip: full-stream bit-exact verification vs FFmpeg (P-A), startup offset self-check + its negative test (`tests/selfcheck_negative.py`) |
| `prores422.MOV` | 1.6G | ProRes 422 HQ (`apch`), 3840×2160 @ 30 fps, 568 frames | Sustained realtime playback passes (P-D): two consecutive full runs at source framerate with zero late frames / recoveries / AER errors |
| `prores4444.mov` | 442M | ProRes 4444 (`ap4h`) | Profile-matrix testing — firmware acks the frame but produces no output (decode absent from fw 2.62) |
| `FILMPLUS-ProResRaw-NinjaV-FX6-3.MOV` | 1.5G | ProRes RAW (`prrf`, bayer) | Profile-matrix testing; its first sample is the reference for the RAW frame fixture in `../../testdata/prores_raw_frame0.bin` (header 3872×2192 with 16 px crop fields → FFmpeg outputs 3840×2160) |
| `DW0001C027_251020_135413_p1I7H.mxf` | 872M | MXF/KLV capture | Reference material for profile testing (MXF is not supported by the frame extractor — use a MOV/MP4 container for extraction) |
| `frame0.yuv`, `frame0.png` | 8M / 28K | Raw YUV + rendered PNG of an early single-frame decode | Historical artifacts from the first live decode tests (Phase 3 era) |

Notes:

- Single-frame ProRes **fixtures** used by `abctl decode` and the test clients live in
  `../../testdata/` (tracked, small). Extract new ones with FFmpeg or the repo's frame
  extractor; see `dev/rework/` for the analysis scripts.
- To regenerate a reference YUV from any clip: `ffmpeg -i <file> -frames:v 1 -pix_fmt yuv422p ref.yuv`.
