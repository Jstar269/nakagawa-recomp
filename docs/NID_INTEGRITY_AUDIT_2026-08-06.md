# NID→name→signature→handler integrity audit (2026-08-06)

**Scope:** exhaustive exact-main integrity audit of every `sr_hle_register()` in
`src/rt/hle.c` against the project's authoritative NID table
(`src/rt/nid_names.h`, generated from the tracked PPSSPP-derived corpus
`tools/nid_corpus.json`), plus a handler-shape review of the four subsystems
tracked by the sceSasCore, sceReg, sceDisplay-VBLANK, and scePower issues
(75, 78, 83, and 86 respectively).

**Audited head:** this audit began at `main` = `7199d82` and its results were
carried into `main` through the landed changes recorded below (subsequent
commits by other lanes advanced the tree; the routing-fix content is
identical).

**Method:** `tools/hle_manifest.py` extracts all registrations fail-closed
(371 at audit time), every registered NID present in `nid_names.h` was
compared name-for-name, and each claim from issues #75/#78/#83/#86 was
verified against both the generated table and the raw registrations. Every
claim in all four issues reproduced on exact main. `nid_name_proof.py`
independently verifies the corpus↔header chain; NOTICE.md records the corpus
names as PPSSPP-sourced (`Core/HLE` tables, pinned at PPSSPP `f0c28c67`).

## Pre-fix findings (all reproduced on exact main)

| NID | Registered name (pre-fix) | Canonical name (table) | Handler | Issue |
| --- | --- | --- | --- | --- |
| `0x9EC3676A` | `__sceSasSetSimpleADSR` | `__sceSasSetADSRmode` | `h_SasSetSimpleADSR` | #75 |
| `0x33D4AB37` | `__sceSasSetNoise` | `__sceSasRevType` | `h_SasSetNoise` | #75 |
| `0x0CAE832B` | `sceRegInit` | `sceRegCloseCategory` | `h_ok` | #78 |
| `0x1D8A762E` | `sceRegExit` | `sceRegOpenCategory` | `h_ok` | #78 |
| `0x28A8E98A` | `sceRegOpenRegistry` | `sceRegGetKeyValue` | `h_ok` | #78 |
| `0x92E41280` | `sceRegCloseRegistry` | `sceRegOpenRegistry` | `h_ok` | #78 |
| `0xD4475AA8` | `sceRegOpenCategory` | `sceRegGetKeyInfo` | `h_ok` | #78 |
| `0xFA8A5739` | `sceRegCloseCategory` | `sceRegCloseRegistry` | `h_ok` | #78 |
| `0x36CDFADE` | `sceDisplayWaitVblankStartCB` | `sceDisplayWaitVblank` | `h_DisplayWaitVblank` | #83 |
| `0x2033261A` | `scePowerGetBatteryLifePercent` | *(absent — bogus)* | `h_PowerGetBatteryLifePercent` | #86 |
| `0x0EB81464` | `scePowerIsBatteryExist` | *(absent — bogus)* | `h_PowerIsBatteryExist` | #86 |
| `0x87440E5E` | `scePowerIsPowerOnline` | *(absent — bogus)* | `h_PowerIsPowerOnline` | #86 |
| `0xFEE3D382` | `scePowerGetCpuClockFrequencyInt` | *(absent — bogus)* | `h_PowerGetCpuClockFrequencyInt` | #86 |
| `0x478FE6F5` | `scePowerGetBusClockFrequencyInt` | `scePowerGetBusClockFrequency` | `h_PowerGetBusClockFrequencyInt` | #86 |

Plus 18 `sas_ok[]`-loop entries registered under the generic `__sceSas_ok`
label whose NIDs exist in the table under their real names (e.g.
`0xB7660A23` = `__sceSasSetNoise`, `0xCBCD4F79` = `__sceSasSetSimpleADSR`,
`0x07F58C24` = `__sceSasGetAllEnvelopeHeights`, `0x267A6DD2` =
`__sceSasRevParam`, …). One `sas_ok[]` entry, `0xD5EBBCDC`, has **no canonical
name in the tracked corpus** (external PPSSPP sources suggest
`__sceSasSetSteepness`); it stays under the generic label until the name is
added to the corpus with provenance.

## Routing fix (mechanically certain subset)

Each binding above where the canonical name/NID is proven by the generated
table was corrected in `src/rt/hle.c` with **no handler-behavior change**:

- relabeled the 10 static registrations whose NIDs are valid but whose names
  were wrong;
- replaced the 4 bogus power NIDs with their canonical NIDs (`0x2085D15D`,
  `0x0AFD0D8B`, `0x87440F5E`, `0xFDB5BFE9`) so real title imports route to
  the already-existing handlers instead of remaining unregistered;
- expanded the `sas_ok[]` dynamic loop into 19 literal registrations under
  their canonical names (all still `h_ok` fake-success stubs; classification
  unchanged).

Post-fix: **0 registrations disagree with `nid_names.h`** (371 registrations,
0 findings). The 4 removed NIDs are recorded in `RETIRED_NIDS`
(`tools/hle_registry_meta.py`) with issue links; the gate fails if any is
ever re-registered.

## Machine-enforced regression

The one-off review is now a CI-enforced invariant:

1. `tools/hle_manifest.py` cross-checks **every** registration against the
   generated `nid_names.h` table; a disagreeing label is a `mislabeled_nid`
   finding even when absent from the curated map.
2. `tools/hle_registry_meta.py` carries `KNOWN_NID_NAMES` (canonical labels
   for the corrected NIDs), `KNOWN_NID_ISSUES` (finding → issue attribution),
   and `RETIRED_NIDS` (the fabricated NIDs, with re-registration detection).
3. `tools/import_audit_gate.py` fails on any unwaived finding and treats a
   retired NID's removal as a reviewed retirement rather than a coverage
   regression.
4. `tools/test_hle_manifest.py` locks each corrected NID/name/handler pair
   and asserts the exhaustive table cross-check is clean on the live manifest.

A reverted registration label on any of these NIDs now fails the gate and the
unit suite.

## Residual gaps (kept visible; issues stay open)

- **#75** — the two relabeled SAS NIDs still route to the old-shaped handlers
  (`0x9EC3676A`→`h_SasSetSimpleADSR`, `0x33D4AB37`→`h_SasSetNoise`); the real
  `SetNoise`/`SetSimpleADSR` NIDs remain `h_ok` stubs; `h_SasInit` validation,
  voice-index bounds, ADSR flag handling, noise-key-on, loop-policy, span
  validation, and Core/CoreWithMix timing all remain implementation work.
- **#78** — all six sceReg NIDs are still `h_ok` fake-success; `sceRegExit`
  (`0x9B25EDF1`) and the key read/write APIs remain unregistered.
- **#83** — the CB variants (`0x8EB9EC49`, `0x46F186C3`, `0x40F1469C`,
  `0x77ED8B3A`) remain unregistered; the WaitVblank vs WaitVblankStart timing
  distinction is not modeled (both route to `h_DisplayWaitVblank`).
- **#86** — getters still return fixed constants (100/1/1/1/333/166); the
  float return shaping for `scePowerGetBusClockFrequency` /
  `scePowerGetCpuClockFrequency` is unmodeled; aliases `0xBD681969` and
  `0xFEE03A2F` remain unregistered; `h_PowerRegisterCallback` is partially
  implemented (16 slots, `-1` allocation, initial notify) but unregister and
  deletion cleanup remain.

## Related tracker reconciliations recorded in this pass

- **#296 (HQ-1 VFPU addressing)** — closed as completed: hardware measurement
  (PSP-3001/6.61-ARK, 2/2 reproducible runs, 0 mismatches across all 128
  single encodings + 14 wide encodings) confirmed the current decode; the
  verifier `tools/psp_oracle/verify_vfpu_addr.py` is committed. IND-2
  unblocked.
- **#304 (PROV-F6 sal063 CREDITS)** — resolved by PR #306 (`ff4732f`):
  `THIRD_PARTY_LICENSES/SAL063_CREDITS.txt` vendored verbatim, NOTICE.md
  gained the sal063 per-file inventory and the PPSSPP `4e109dd6` sceMpeg pin.
  The #102 history check for a `gpu_vk/` PPSSPP-linking bridge remains open.
- **#72/#89/#74/#139** — evidence recorded in
  `docs/COVERAGE_LEDGER.md` and on the issues; see the ledger's cross-file
  pass section.

## Reconciliation note — 2026-08-10 (recovery into current `main`)

The pre-fix findings, the mechanically-certain routing fix, and the post-fix
"0 registrations disagree with the table" result above were re-verified
against current `main` (`e0aa4e28`) and remain accurate: the canonical labels,
the four replacement power NIDs (`0x2085D15D`, `0x0AFD0D8B`, `0x87440F5E`,
`0xFDB5BFE9`), the expanded `sas_ok[]` registrations, and the residual
handler-shape gaps tracked by #75/#78/#83/#86 are all present on `main`
(e.g. `0x9EC3676A`/`0x33D4AB37` still route `h_SasSetSimpleADSR`/`h_SasSetNoise`).

The **enforcement mechanism** described in the "Machine-enforced regression"
section above differs from what `main` ultimately adopted:

- `RETIRED_NIDS` (with re-registration rejection in `tools/hle_manifest.py`)
  was **not** adopted. The fabricated power NIDs are instead simply absent
  from `hle.c`; `tools/hle_manifest.py` keeps no retired-NID registry.
- The exhaustive per-registration cross-check lives in `tools/test_hle_manifest.py`
  (`test_no_new_registration_contradicts_the_canonical_table_by_name`, with the
  `REVERSE_NID_DEBT` roster and a fail-closed minimum-parse guard on
  `nid_names.h`), not in `tools/hle_manifest.py` itself. The curated
  `KNOWN_NID_NAMES`/`KNOWN_NID_ISSUES` maps in `tools/hle_registry_meta.py`
  are the live `mislabeled_nid` finding source.
- `tools/import_audit_baseline.json` was refreshed on `main` by the NID
  baseline repair (PR #322); this audit's older baseline snapshot is
  superseded.

This note keeps the dated audit record truthful as historical evidence; the
invariant it documents (a reverted label or a resurrected fabricated NID fails
the gate/suite) holds on current `main` through the mechanism above.
