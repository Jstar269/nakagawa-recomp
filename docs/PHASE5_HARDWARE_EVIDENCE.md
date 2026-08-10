# Phase-5 Stock-Hardware Evidence (Import Model & Fail-Closed Pipeline)

Date: 2026-08-03. This page records the stock-PSP (retail unit, official firmware)
verification for the Phase-5 import-model audit (GitHub issue #197). No machine
serial, MAC, or host-private paths are recorded here; all artifact hashes are
public digests of source-owned fixture builds.

## Scope

The audit verifies that the corrected import pairing (`tools/psp_import_table.py`,
`tools/imports.py`) and the fail-closed codegen path (unmapped import stubs now abort
via `sr_unimplemented` instead of silently returning) are exercised end-to-end on
real hardware using a source-owned, `-nostdlib` PSPDEV PRX fixture with exactly the
imports under test and no libc/newlib, display, or debug-screen code.

## Fixtures

| Fixture | Purpose | Imports | Result |
| --- | --- | --- | --- |
| `fixtures/nakagawa_minimal_v3` | Breadcrumb diagnostic: prove execution, arithmetic, Open/Write/Close, persistent FS output before the exit boundary | `sceIoOpen 0x109f50bc`, `sceIoWrite 0x42ec03ac`, `sceIoClose 0x810c4bc3` (IoFileMgrForUser) + `sceKernelExitGame 0x05572a5f` (LoadExecForUser) | PASS through pre-exit; still black/hung after S4 |
| `fixtures/nakagawa_minimal_v4` | Lifecycle isolation: same workload but `module_start` returns 0 instead of calling `sceKernelExitGame` | IoFileMgrForUser only (`0x810c4bc3`, `0x109f50bc`, `0x42ec03ac`) | Workload PASS; XMB lifecycle not exercised |

Both fixtures are PRX entry 0x8 (`module_start`), built with the PSPDEV v20260801
toolchain (psp-gcc 15.2.0, ebootsign), rebased at `0x08800000`. Import tables were
audited clean (no missing, no fake-success imports) before signing.

Signed-EBOOT digests (SHA-256):

- v3 `EBOOT_SIGNED.PBP` `c89ec86ab9e8efc6b2497480f1ff4ce4514e2bd40f2fcd3312ee7bb69ca5ad6d`
- v4 `EBOOT_SIGNED.PBP` `28b0616aca8460171c3059e5be3b343eb90a41f2db6ed9f23cc6267eb5e515ac`

## Evidence channels

- Success/error markers are 0-byte files created via Open+Close only (by design);
  `NK_S0_START`, `NK_S1_OPEN_OK`/`NK_E1_OPEN_FAIL`, `NK_S2_WRITE_OK`/`NK_E2_WRITE_BAD`,
  `NK_S3_CLOSE_OK`/`NK_E3_CLOSE_BAD`, `NK_S4_PRE_EXIT`.
- The result payload `NAKAGAWA_MINIMAL SUM=5050\n` is produced only when the
  u32 accumulator loop equals 5050 (otherwise `NAKAGAWA_MINIMAL SUM=BAD\n`), so a
  correct payload also proves guest arithmetic.
- The payload is persisted through `sceIoWrite`/`sceIoClose` before the exit path.

## Results

### v3 (exit via `sceKernelExitGame`)

All five success markers and no `E*` markers; the result file on the Memory Stick
contained byte-exact `NAKAGAWA_MINIMAL SUM=5050\n`. Stock execution, arithmetic,
`sceIoOpen`, `sceIoWrite`, `sceIoClose`, and persistent filesystem output are proven
through the pre-exit boundary. The PSP remained on a black screen after S4 and could
not enter sleep, which motivated the v4 lifecycle isolation fixture.

### v4 (normal return from `module_start`)

All five success markers and no `E*` markers; result file byte-exact. `module_start`
returned; the PSP remained on a black screen rather than returning to XMB, but
unlike v3 it could enter sleep.

Per authoritative PSPDEV semantics (`src/startup/crt0_prx.c`), returning 0 from a
PRX `module_start` means successful module start; it is not an XMB application-exit
primitive, and a PRX may remain resident. The earlier "must return to XMB"
acceptance criterion for v4 was therefore invalid, and no further termination
variant is warranted.

## Acceptance

- **Phase-5 real-hardware workload acceptance: PASS.** Signed OFW loading, module
  execution, computed 5050, `sceIoOpen`, `sceIoWrite`, `sceIoClose`, persistent
  byte-exact result, complete intended `module_start` workload, and return from
  `module_start` are all proven on stock hardware.
- **Normal XMB application lifecycle: NOT TESTED by this PRX-style fixture.** A
  conventional PSPDEV `main`/crt0 application would be required; this is not a
  Phase-5 blocker.

## Related record

The guest file-descriptor namespace defect observed by this evidence (real PSP
persists the payload while the runtime routes the first file write to host stdout)
is tracked as [#243](https://github.com/Jstar269/nakagawa-recomp/issues/243) with a
regression/fix design; it is deliberately kept out of the Phase-5/import branch
(this branch does not implement the fix), and Phase 5 acceptance for #197 does not
depend on it.
