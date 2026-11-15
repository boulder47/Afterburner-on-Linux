# dev/ — development work

Everything that is not the production driver, its docs, or the player. Moved here in the
2026-11-15 reorganization (nothing deleted; paths inside older documents predate this move).

| Path | Contents |
|---|---|
| `FINDINGS.md` | **Authoritative RE log** — register map, descriptor protocol, completion semantics, AER root cause, output-format pinning. Read the tail sections first in any dev session. |
| `UC_METHODS.md` | User-client (userland-facing) ABI decode: sMethods selectors, trampolines, slot dispatch. |
| `plan2.md`, `plan.md` | Plan of record (v2, 2026-11-12) and the historical phase plan (Phases 0–3t). |
| `kexts/` | Kext bundles: original capture + Sonoma + Sequoia extractions. All six bitfiles md5-identical → fw 2.62 is the latest public firmware. |
| `rework/` | Static RE workspace: extracted Mach-O slices (`arm64e.bin`, bin offset == vmaddr; `x86_64.bin`), VideoToolbox SDK headers, vtable/import/stub maps, analysis scripts, top-level dump files (`*_arm64e.txt`, `disasm_x86.txt`). |
| `linux-probes/` | Early single-file kernel test modules (Phases 0–3) that decoded the protocol live: BAR map/dump, DMA, submit, IRQ, flash probes + their build artifacts and register dumps. Superseded by `../linux-driver/afterburner/`. |
| `captures/` | Initial macOS-side capture work: ioreg/kernel-log/system-info captures, analysis output, capture tarballs, the original Objective-C bar-dump tool. |
| `testmedia/` | Sample clips for player/tests (mostly gitignored): `prorestest.mov` (canonical 870-frame clip), `prores422.MOV` (4K HQ @30 fps), RAW/MXF profile-matrix material, early frame dumps. |
| `tmp/` | Scratch: disassembly dumps, analysis scripts, live-test output bins. Prunable at will; a few artifacts are tracked for provenance. |
| `.venv/` | Python venv (uv) with capstone — used by the RE scripts in `rework/`. Relocated 2026-11-15; activate-script paths were rewritten to match. |
| `notes/`, `userspace/` | Empty legacy directories, kept for completeness. |
