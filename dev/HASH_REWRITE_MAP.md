# Commit SHA rewrite map (2026-11-15)

History was rewritten with `git filter-branch --index-filter` to remove the 1.6G
`prores422.MOV` blob from every commit that carried it (tracked since old `b634b05`).
All commits from there on have new SHAs; the four oldest are unchanged.
Old references in docs/notes: match by subject line below.

| old | new | subject |
|---|---|---|
| `5407c89` | `5407c89` | Handoff snapshot: Phases 0-3p complete (live driver + static RE) |
| `0e98671` | `0e98671` | Phase 3p complete: vtable/import model fully resolved (FINDINGS.md §23) |
| `5e9abcf` | `5e9abcf` | Phase 3r: consolidate VideoToolbox <-> Afterburner decode path analysis |
| `5c00777` | `5c00777` | Phase 3r+: capture MacOSX10.15.sdk VideoToolbox headers; pin VT plugin dir + error correlation |
| `b634b05` | `e504a08` | 4K ProRes test prep + AER fatal incident (FINDINGS §24) |
| `cc2659e` | `29956b7` | Phase 3q DONE: kext frame-commit path fully decoded (FINDINGS §25) |
| `6974b3c` | `48cebe0` | FINDINGS §26: run #2 post-mortem, stop() decode (BAR0[hdr+4]=0 is the stop command), UC API structure |
| `840853e` | `c6ad4b3` | plan §7: run #2 status + stop-command live test as next action |
| `2364f0a` | `663541b` | FINDINGS §28: AER MalfTLP full root cause — definitive TLP decode (MWr 512B @0x3CCC8FC0, desc #115-130, reserved bit in DW0) + 80s recovery delay = dev->mutex held across ab_probe; corrects §27.4 |
| `689f073` | `e4635cc` | UC_METHODS.md: master map of all decoded UC methods (7 frame methods + vtable + ABI + offsets) |
| `0e02e81` | `eef2169` | UC_METHODS: queryCompletion deep-dive (engine wait + queue drain), cardStatus/getPercent semantics, space-fn table complete, idle/active time accounting, handleInterrupt partial |
| `994780d` | `1ca7e29` | UC_METHODS: AB::reset() per-frame teardown fully decoded (doorbell clears, c2h/h2c drain wait, desc-queue free, fw handshake, tracking reset) |
| `f317db6` | `5838be2` | UC_METHODS: §15 cleanUp+submitDataGated deep dives, §16 engine callback table (62 entries), §13 layout corrections |
| `b64bafa` | `b71d4dc` | UC_METHODS: §12 handleInterrupt FULL decode (bitfile walk, thermal distress mask, PR DMA reset, completion dispatch + drain loop, deferred re-submission nodes 72B, hard-error path), §13 field corrections (+0x90 engine vt+0x208, +0x128 dir table, +0x158 decoder section ptr, +0x160 thermal mask, +0x11B4/0x11C0/0x11C8/0x11D0 new), §14 items 12-16, §17 sleep_timer hang-watchdog decode (clock_interval_to_deadline + engine vt+0x210 poll, reset on hang, decoder disable on exit) |
| `1d73bec` | `dac8056` | UC_METHODS: §18 reconfig_handler FULL decode (firmware image-type state machine golden/HWTE/live/debug; SPI flash update 2x20MB channels, attempt counter max 2, lazy bitfile load live_2.6X_0x01_{primary,secondary}[_ecXX].bin via OSKextRequestResource; commit vcall vt+0x1F0(1000) + BAR0[+0x2004] re-enable 0x00019000/0x0001A000; PCI ext-config 4KB blob flush at entry), helpers change_pointer@8580/spierase@809c/loadBitfile@8628/reconfig_timer@74c4, §13 reconfig field group (+0x138 fw status block, +0x150 SPI ctrl, +0x16C attempts, +0x170/0x174 blob, commit objects D/E, SPI buffer objs), §14 item 16 done + items 17-19 |
| `8ff6631` | `6f9dc43` | UC_METHODS §20: pointer_restore_handler @0x7508 + setPowerState @0x7C60 full decode (batch 2) |
| `29e03b0` | `bc94ed3` | Phase 3t (batch 3): vtable architecture + static-init table — docs closed out |
| `0378b62` | `68805ed` | submit2: modes 14/15 stop-command survival test + no-stop control (FINDINGS §27.5) — planned, unrun |
| `1bd80c1` | `15a2c3e` | Phase 3u: driver rewrite from decoded protocol — plan of record (§8); §7 next actions repointed |
| `209ab64` | `40e04f0` | tmp: refresh handleInterrupt disassembly dump (regenerated during batch 2) |
| `6a0cdbf` | `d458907` | Phase 3u.1: production module skeleton (ab_pci/ab_regs/ab_engine) — builds clean; card dead since run 14b, live acceptance blocked on power cycle (FINDINGS §31) |
| `67c7749` | `bb4a108` | Phase 3u.1 CLOSED: card revived by cold boot; acceptance PASSED (6 probes/5 rmmod cycles clean, zero AER) — FINDINGS §31.1 |
| `2127bfa` | `8b509da` | Phase 3u.2 milestone: multi-frame ProRes decode WORKS — root cause + fix (FINDINGS §32) |
| `135f994` | `e29dad8` | V-matrix: V1 ✅ + V3 4K end-to-end ✅; output format/dim configurability RESOLVED (FINDINGS §33-§35) |
| `218ab07` | `d342dab` | Output size padding rules pinned live (FINDINGS §35.3): stride=ceil(W*2/64)*64, rows=ceil(H/16)*16 |
| `b497a52` | `add84ad` | tmp/rework: ADRP table + final verification script (332/332 Keystone cross-check, FINDINGS §30) |
| `c44d4aa` | `1bdf599` | V-matrix: V3 idle-watch + V4 multi-frame both-channel + V5 stall/recovery all PASSED; 4444 & ProRes RAW (aprn) confirmed not decoded by fw |
| `d58a538` | `ff89a3c` | FINDINGS §37: profile matrix COMPLETE — 422+422HQ decode pixel-perfect, 4444/RAW ack-only (profile-based rejection); RAW frame provenance byte-verified; Sonoma 14.8.5 + Sequoia bitfiles identical to Dec-2019 (no fw update in ~7yrs) |
| `8093403` | `1a1be83` | FINDINGS §37.4 FINAL: Sequoia 15.x extraction complete — all six bitfiles byte-identical to Sonoma + Dec-2019 (fw 2.62 is latest public firmware; RAW/4444 decode absent from every shipped bitfile); kext bundle 3.2→6.0→6.1 with unchanged FPGA streams |
| `41dbc80` | `313e72f` | Phase A complete (plan2): sysfs surface + abctl client validated on hardware |
| `e8ccc20` | `189de5b` | Phase B complete (plan2): kext-parity ioctls, error paths, RECOVER soak |
| `253b08f` | `aa8529a` | Salvage curated RE evidence + fixtures from tmp scratch (pre-V6) |
| `fa1929c` | `55b09dd` | Phase C complete (plan2): V6 destructive — card SURVIVED, expectation refuted |
| `ce48cf4` | `755acc7` | abplayer: fix stsc chunk-range off-by-one corrupting sample offsets 728-780 |
| `06b91a5` | `9338f20` | Async path hardening + wedge diagnostics from abplayer bring-up |
| `3f9d7f1` | `ca40a60` | P-A: full-stream pixel verification — 870/870 bit-exact vs FFmpeg ref (§41) |
| `f6be8a2` | `8fafaf8` | P-B: abplayer hardening — self-check, stop-on-exit lifecycle, run stats (§42) |
| `fe4d4f4` | `5a73db4` | P-D: sustained 4K realtime stream validated — V8 (§43) |
| `4d05dfe` | `8dc2a09` | Reorganize repo layout: production driver + docs + player at top level |
| `539e7ee` | `9cd89e5` | Fix reorg nesting: flatten dev/rework/rework -> dev/rework |
| `5e2d48f` | `15d0713` | Untrack large video media; add placeholder index for dev/testmedia |
