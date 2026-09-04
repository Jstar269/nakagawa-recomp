# PSP threading oracle harness

Source-owned measurement harness for `docs/research/PSP_THREADING_SEMANTICS.md`.
Implements the frozen hardware-oracle design as a deterministic PSP program
that emits scalar records via the shared `tools/psp_oracle/protocol.py` channel.

**Status:** `PSP_THREADING_ORACLE_DESIGN = FROZEN`, `HARDWARE_EXECUTION = NOT RUN`.
No emulator consensus is treated as hardware fact. Unknown outcomes remain
`HARDWARE_UNKNOWN` until an authorized device run replaces placeholders.
`PSPDEV_BUILD = NOT RUN` until `psp-config` compilation and disassembly/
relocation/offset inspection succeeds; `REGISTER_CAPTURE_BUILD_VALIDATION =
REQUIRED_BEFORE_HARDWARE`.

## Campaign (corrected minimum fidelity)

- `CAMPAIGN_VERSION = psp-threading-v1` (matrix `fixtures/psp_threading_oracle/matrix.json`)
- Gating minimum: **28 unique cases** (6 controls + 22 discriminators) across **5 launches** (repeat 56/10):
  - L1 `CT-ATTR` 8 cases: `CT-A01 CT-A03 CT-A04 CT-A05 CT-A07 CT-A11 CT-A12 CT-D00`
  - L2 `CT-PRIO+OPT` 8 cases: `CT-B01 CT-B02 CT-B05 CT-B06 CT-C01 CT-C02 CT-C03 CT-C04`
  - L3 `ST-ARGS` 3 cases: `ST-SA01 ST-SA02 ST-SA03`
  - L4 `ST-LIFE+RET` 6 cases: `ST-SL01 ST-SL02 ST-SL04 ST-SR01 ST-SR02 ST-SR03`
  - L5 `ST-SCHED` 3 cases: `ST-SP01 ST-SP02 ST-SP03`
  - `CT-C05` (`KERNEL 1`) and `CT-C06` (genuine block UID) are **full-campaign only** (`PSP_THREADING_FULL=1`) – valuable option experiments beyond the 28-case gating minimum (partition-ID primary vs block-UID competing hypothesis). Full accounting from research note (59 unique / 71 records / 13 launches) remains unchanged as consolidated.
- Repeat target: 56 records / 10 launches (one full repeat). Each record includes `CAMPAIGN_VERSION`, `RUN_ID`, `CASE_ID`, `result` (raw return code), `attempt=0x…`, bounded `out*` scalars, and `canary=0xA5A5A5A5`. Host parser rejects truncated, unknown version, duplicate, malformed, bad-canary, or unknown-field streams. A synthetic stream with placeholder `model=unknown` etc. is `SYNTHETIC_TEST_ONLY`; only a real PSP capture bound via explicit `HardwareCaptureContext` (raw_capture_sha256, binary_sha256, source_commit, model, firmware, timestamp/run_id, runner) validates as `HARDWARE_MEASURED`. Raw capture text never self-promotes.

For every case the matrix encodes `CASE_ID`, `OPERATION`, `INPUTS`, `CONTROL_GROUP`, `MEASUREMENT_FIELDS`, `REQUIRED_OUT_FIELDS`, `OPTIONAL_OUT_FIELDS`, `LAUNCH`/`PHASE`, and `EXPECTED_CLASSIFICATION`. The parser enforces per-case required fields, rejects missing/misspelled/unknown-hex fields (fail-closed; only `future_` namespace allowed, grammar `future_[A-Za-z0-9][A-Za-z0-9_]*`), checks `canary`, `result`, `attempt`, oversized `probe_len` (>64), duplicate `run/case/attempt`, and incompatible `campaign_version`. Unknown hardware outcomes are never stored as expected constants.

## Status semantics (Stage 3)

- `PASS` = `MEASURED`: experiment completed structurally and all required measurement fields were captured. The PSP API return code remains raw in `result`; an API error (e.g., `0x800201ac`) can still be `PASS` if the experiment measured that outcome.
- `FAIL` = `ERROR`: harness itself could not produce a valid measurement (e.g., thread creation failed before measurement, allocation failed and `NOT_MEASURABLE` could not be recorded). Distinguished from raw API errors preserved as `PASS` data.
- `SKIP` = `NOT_MEASURABLE_WITH_CURRENT_HARNESS`: bounded scalar-only, orientation not established or resource unavailable; no unsafe memory read performed.
- `UNKNOWN_RESULTS_PRESERVED = true`: emulator/phantom expected answers are never encoded; raw values retained and divergent observations across repeats are preserved (no majority vote).

## Evidence model (Stage 4)

`PARSED_CAPTURE` (via `parse_threading_output(text)`) carries no authority by default (`UNVERIFIED_CAPTURE`). `EVIDENCE PROVENANCE` is out-of-band:

- `SYNTHETIC_TEST_ONLY` via `SyntheticCaptureContext` for host fixtures
- `HARDWARE_MEASURED` via `HardwareCaptureContext` binding `raw_capture_sha256`, `binary_sha256`, `source_commit`, `model`, `firmware`, `capture_timestamp`/`run_id`, `runner`

A hand-edited file with plausible `model=PSP-3001 firmware=6.61 binary_sha256=…` remains `UNVERIFIED_CAPTURE` even if every field looks real. Classification is an explicit provenance claim from the capture workflow, not text self-assertion.

## Strict per-case schemas (Stage 5)

`matrix.json` defines for every `CASE_ID` the `required_out_fields` (e.g., `CT-A01: out0..out3`, `ST-SA02: out0..out8`), `optional_out_fields` (empty; only `future_[A-Za-z0-9][A-Za-z0-9_]*` allowed as explicit forward-compatible hex namespace under `future_` prefix), `classification`, `launch` group, and `control` relation. Parser rejects: missing required field, misspelled mandatory field, unexpected arbitrary field (including unexpected hex), near-miss extension prefix (`future`, `future_`, `future__invalid`), oversized field (`probe_len >64`, `stack_bytes` etc.), duplicate field/case, incompatible campaign version, and extension shadowing a required field. Unknown mandatory `unknown_mandatory` and typo `otu0` etc. are fail-closed. Grammar: `future_[A-Za-z0-9][A-Za-z0-9_]*` (identifier-style, not dotted).

## Run/attempt identity (Stage 6)

Every observation carries validated `run_id` (hex, from `NAKAGAWA_PSP_META`), `attempt` (hex, per-record, sane 0..255), `case_id`. Analyzer validates: syntactic `run_id`, duplicate `run/case/attempt` impossible, expected repetitions present, case belongs to expected launch group, divergent observations preserved, `run_id` uniqueness across launches (not derived from list position).

## Stack/memory probe safety (Stage 7)

Orientation/control case `CT-D00` (thid 0 vs explicit current UID) executes before content probing where design requires it. No assumption that `info.stack` is low base; orientation remains `HARDWARE_UNKNOWN` until measured. Stack window uses checked arithmetic: `if offset > size -> reject; if length > size - offset -> reject; then form address` – no overflowing pointer formed then checked. If `stackSize` unavailable, probe is `NOT_MEASURABLE_WITH_CURRENT_HARNESS` (`out6/out7 = 0xffffffff / 0`) and no out-of-allocation read occurs. `STACK_PROBE_MAX_BYTES = 64`.

## StartThread argument copy (Stage 8)

Deterministic synchronization via `sceKernelWaitThreadEnd` (not `sceKernelDelayThread` as semantic proof). Preserved raw observations: `source_ptr` (`out6`), `child_ptr` (`out1`), `child_sp` (`out2`), `source_checksum` (`out3`), `child_checksum` (`out4`), `raw_equality` (`out5`), `inside_flag` (`out7`), `a1_minus_sp` (`out8`), plus `result`/`size` (`a0`). Child checksum is **not mutated** to signal mismatch; raw equality indicator is separate. Zero-size (`ST-SA01`) never dereferences child pointer. Checked pointer+length arithmetic used for `a1_inside_stack` determination.

## Effective attribute (Stage 9)

`effective_attr` is reported only if `SceKernelThreadInfo` exposes observed attr (`info.attr`); otherwise `NOT_MEASURED` (`0xffffffff`). Requested attr is **never** echoed as measurement.

## Assembly (Stage 10)

`thread_shim.S` is minimal earliest-entry trampoline: `sp` captured before frame, `a0-a3 t0-t3 gp sp ra hi/lo` stored before C prologue modifies them, explicit `%hi/%lo` via `lui/ori` for relocation safety (PSPSDK convention), no pseudo-instructions before capture except documented `$at` usage, ` $t8/$t9` unavoidable temporaries (`t0/t1` overwritten only after storage). C side has `_Static_assert` for all `ThreadEntrySnapshot` offsets. No claim of `PIC_SAFE`/`ABI_PROVEN`/`COMPILES` without `PSPDEV_BUILD` inspection of assembler output, relocations, symbol transfer, snapshot offsets.

## Option UID cases (Stage 11)

`CT-C05` (`KERNEL 1`) and `CT-C06` (genuine block UID via `sceKernelAllocPartitionMemory(2,…)` using returned UID, not its address, cleaned up every path) keep partition-ID primary and block-UID competing hypothesis distinct; allocation failure is recorded as structural measurement (`out2 = block_ret`, status `PASS` with raw code), not silent.

## Build

Requires PSPDEV/PSPSDK (`psp-config --pspsdk-path`). No private inputs, no retail bytes, no firmware patches.

```powershell
# All gating cases in one binary (convenient for parser tests):
make -C fixtures/psp_threading_oracle
# Or one phase per launch (canonical 5-launch minimum):
make -C fixtures/psp_threading_oracle clean
make -C fixtures/psp_threading_oracle CASE=ct-attr EBOOT.PBP
make -C fixtures/psp_threading_oracle CASE=ct-prio-opt EBOOT.PBP
make -C fixtures/psp_threading_oracle CASE=st-args EBOOT.PBP
make -C fixtures/psp_threading_oracle CASE=st-life-ret EBOOT.PBP
make -C fixtures/psp_threading_oracle CASE=st-sched EBOOT.PBP
```

Artifacts are under `fixtures/psp_threading_oracle/build/` (`EBOOT.PBP`, `*.prx`), ignored and source-owned. Record the built PRX SHA-256 and `git rev-parse HEAD` for any capture.

If `psp-config` is absent the host parser/tests still run; report `PSP BUILD = NOT RUN` with the toolchain reason.

## What is measured

- **CreateThread attributes** (`CT-A*`): return code, created UID, status/wait type via `sceKernelReferThreadStatus`, `effective_attr` (not fabricated). Values: `VFPU`, `USER`, `USBWLAN`, `VSH`, `SCRATCH_SRAM`, `NO_FILLSTACK` etc. Each discriminator has `CT-A01` control.
- **Priority/stack** (`CT-B*`): bounded priority edges, stack size probes.
- **Option structure** (`CT-C*`): `NULL`, `size=4`, `size=8+USER(2)`, invalid `7`, `KERNEL(1)`, genuine block UID – all gating.
- **Stack initialization**: `CT-D00` orientation via `ReferThreadStatus(0)` vs explicit current UID; bounded 64-byte window with checked arithmetic.
- **StartThread argument copy** (`ST-SA*`): synthetic patterns (16B, 64B), zero-size never dereferenced, raw checksums preserved.
- **Register snapshot**: `thread_shim.S` earliest capture, canary `0xA5A5A5A5`.
- **Lifecycle/scheduling** (`ST-SL*`/`ST-SR*`/`ST-SP*`): dormant/active/exited restart, return sanitization (`0x77` vs `0x800201ac` vs `0x80000000`), phase-state scheduling (`PHASE` before/after, `ENTRY_COUNT` auxiliary, `mfhi/mflo` before GPR stores).

Every malformed/invalid input is isolated in its own record and never terminates the whole campaign.

## Result format and host tooling

PSP side emits `NAKAGAWA_PSP_META` plus one `NAKAGAWA_PSP_TEST` per case with `attempt` and `canary`. Host parser: `tools/psp_threading_oracle/parser.py` (`parse_threading_output`, `evidence_label` with context, `analyze_runs` with `run_id`/`attempt` validation, `human_table`) reuses strict protocol and adds campaign/attempt/canary/per-case field checks plus stability analysis (divergent retained).

Synthetic fixtures are generated in `tools/test_psp_threading_oracle.py`; no retail data.

## Hardware safety

All reads/writes bounded and checked, every created thread/block cleaned up where possible, no unbounded loops, no flash writes, no firmware modification, campaign timeout via host `run_psplink --timeout`. No hardware is touched by the build or by host tests.

## How to run later (explicit hardware authorization required)

1. Ensure PSPDEV is installed and `psp-config` works (`PSPDEV_BUILD = NOT RUN` otherwise).
2. Acquire global hardware lock (per `docs/HARDWARE_ORACLE.md`).
3. Build the desired phase: `make -C fixtures/psp_threading_oracle CASE=ct-attr EBOOT.PBP`.
4. Launch via PSPLink and capture with `tools/psp_oracle/run_psplink.py` plus provenance flags; runner constructs `HardwareCaptureContext` binding `raw_capture_sha256`.
5. Parse with `python -m psp_threading_oracle.parser` or `python tools/psp_threading_oracle/analyzer.py` with explicit evidence context.
6. Repeat for all 5 phases (10-launch repeat target); retain every capture, report stability. Until execution, results remain `NOT RUN` and no `HARDWARE_MEASURED` label is valid.

`REGISTER_CAPTURE_BUILD_VALIDATION = REQUIRED_BEFORE_HARDWARE`.
