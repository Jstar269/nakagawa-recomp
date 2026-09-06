# Document-truth audit 2026-09-05 — reference snapshot

STATUS = REFERENCE_SNAPSHOT
AUDIT_BASE = 312d1d5d844adce1cd252fb8fd1b6ff6a1553a7b (public origin/main, 2026-09-04)
LIVE_GITHUB_CHECKED = 2026-09-05
CURRENT_AUTHORITY = live source and tests, machine policy
(`assets/public_source_profile.json`, `PUBLIC_EXPORT.json`), and live GitHub
issue state, in that order. `ISSUES.md` is not authority.

This file is a frozen read-only finding, not permanent architectural authority.
Do not cite its counts, states, or SHAs as current. Do not extend it in place;
later sweeps create a new dated snapshot. It is intentionally excluded from the
documentation freshness linter's mutable-fact checks.

Live tracker truth at check time: 17 objects total — six OPEN (numbers 23, 63, 67, 69, 70, 98) and eleven CLOSED (numbers 12, 26, 38, 40, 41, 43, 64, 68, 110, 116, 132).
Issue #132 closed 2026-08-27; issue #40 closed 2026-09-01; issue #98 open,
updated 2026-09-01.

## Known examples (independently verified)

- `ISSUES.md` named #132 Open; live GitHub reports CLOSED. Confirmed stale.
- `ISSUES.md` named #40 Open; live GitHub reports CLOSED. Confirmed stale.
- Issue #98 still listed `SR_CALL_GUEST_STACK` and `0x00046d14` as remaining
  work although current `docs/PORTING.md` marks both surfaces retired.
  Confirmed stale: `SR_CALL_GUEST_STACK` has zero hits in `src/rt/hle.c`
  (replaced by per-owner per-depth frames in `src/rt/nested_frames.c`) and
  `00046d14` has zero hits in `tools/codegen.py`.

## TOP 25 highest-impact stale/contradictory claims

1. `ISSUES.md:15` P0 Open #132 trusted refresh vs live CLOSED 2026-08-27.
   UPDATE.
2. `ISSUES.md:17` P1 Open #40 vs live CLOSED 2026-09-01. The overlap/vhdp
   follow-up it calls open (synthetic corpus repair + emitter census) has
   since landed and is enforced by
   `test_every_word_decodes_without_fallback` in
   `tools/test_vfpu_synth_corpus.py`. UPDATE.
3. `ISSUES.md:23` #98 census "46 title guest addresses across 59 sites,
   16 behaviour-altering" vs live `docs/PORTING.md` + `tools/compat_overrides.py`
   (gate `tools/test_compat_manifest.py`, 42 passed): 30 distinct addresses /
   36 sites, all DIAGNOSTIC_ONLY, 0 EXPLICIT_COMPATIBILITY_OVERRIDE, 16
   addresses migrated to the separately audited `HLE_TITLE_CONFIGURED_COMPAT`
   inventory. UPDATE, preferably generated.
4. Live #98 body "Remaining debt" items 1 (`SR_CALL_GUEST_STACK`) and 3
   (`0x00046d14`) vs `docs/PORTING.md` C-4/C-5 RETIRED + live source.
   Only item 2 (disc.id duplicate) and item 4 (general coupling) remain true.
   UPDATE the live issue body.
5. "Checked-in HST manifest" contradiction in `docs/PORTING.md:47,120`,
   `docs/TITLE_CODEGEN_PLAN.md:116-117`,
   `tools/title_manager_plan.ps1:324`, and
   `tools/test_generic_title_planning_proof.py:261-262` vs
   `git ls-files assets/titles/` (only `README.md`, `pspdev-phase5.json`,
   `synthetic.json`, `synthetic-title2.json`; no `hst-ucus98701.json`),
   `assets/public_source_profile.json` exclude_paths, and
   `assets/titles/README.md` ("intentionally not checked in"). UPDATE all sites.
6. `docs/PORTING.md:17-34,40-45` Step 1 worked example requires the absent
   `assets/titles/hst-ucus98701.json` (fails at `hst_manager.ps1:200` in a
   public clone; `tools/test_hst_title_manifest.py:20-24` skips without it).
   Public copy-pasteable input is `assets/titles/synthetic.json`. UPDATE.
7. "Git-ignored HST manifest" in `docs/TITLE_CODEGEN_PLAN.md:377`,
   `Makefile:349`, `assets/titles/README.md:12,72` vs `.gitignore:1-164`
   (no such rule; `git check-ignore` exit 1). Real enforcement is publication
   exclusion via `assets/public_source_profile.json`. UPDATE wording.
8. Mutex `h_ok` stub claims in `docs/PSP_INTR_WAITS_MATRIX.md:583-588` and
   `docs/IMPORT_AUDIT.md:104-109` vs live dedicated
   `h_LockMutex/CB/Try/Unlock` in `src/rt/hle.c:10372-10842` (landed via
   `d562efa` 2026-09-01); 18 matrix cells moved. UPDATE / GENERATE_FROM_AUTHORITY.
9. Intr-matrix "Current-main classification ... 50 CONFORMS, 91 deviations ..."
   (`docs/PSP_INTR_WAITS_MATRIX.md:225-244`) vs header disclaimer
   ("historical evidence") + moved Mutex numerator; no snapshot hash.
   UPDATE (pin hash, rename to Snapshot) + GENERATE_FROM_AUTHORITY.
10. DMA concurrency "hardware-blocked" (`tools/psp_oracle` manifest note
    2026-08-20; `docs/HARDWARE_ORACLE.md:95-99` Loop-C "unanswered") vs live
    #23: 2026-08-27 PSP-3000/6.61/ARK-5.1.0 campaign measured TryMemcpy BUSY
    `0x80000021` 64/64 and blocking wait+0 64/64. Invalid-tail precedence
    remains correctly NOT_MEASURED (SKIP `setup_mask=0x3`). UPDATE.
11. VFPU Loop-A "future fuzz" (`docs/HARDWARE_ORACLE.md:83-88`) vs live #40:
    Group A 13 records 4/4 PASS, Group B 91 cells 3/3 identical PASS.
    Bulk fuzz still unbuilt; PPSSPP-derived warning still valid for unmeasured
    encodings. UPDATE scope.
12. `docs/HARDWARE_ORACLE.md:98` lists "#64 VBLANK sub-interrupts" as
    unanswered vs shipped `PSP-DISPLAY-001 implemented` (landed `5676b65`
    2026-08-22; 12/12 per delay; +0/+1 coalescing; 59.9418 Hz), correctly
    recorded in `docs/ARCHITECTURE.md:448-468`. UPDATE the index only.
13. `docs/ARCHITECTURE.md:189` "mpeg.c ... derived in part from PPSSPP" omits
    the vendored `src/rt/atrac3p/*` real ATRAC3+ decoder + bridge + selftests
    (`Makefile:381-401`). UPDATE.
14. `docs/HARDWARE_ORACLE.md:84-99,218-220` bare pre-republication numbers
    (#1/#2/#13/#14/#16/#36/#64/#88; only #35 labeled historical) in a
    `Status: proposal` CURRENT doc. UPDATE (banner or explicit live links).
15. "Second fixture" in `docs/PORTING.md:56-60` and
    `docs/TITLE_CODEGEN_PLAN.md:124-128` vs three checked-in fixtures
    (`Makefile:33`; proof test `test_generic_title_planning_proof.py:102-122`).
    UPDATE both pages.
16. `assets/titles/README.md:66-68` "both carry the identical rendering"
    (`HST_EXTRA_SPANS`/`TITLE_EXTRA_SPANS`) vs live: generic planner emits
    only `TITLE_EXTRA_SPANS` (`title_codegen_plan.py:174-178`, asserted by
    proof tests); `HST_EXTRA_SPANS` is synthesized only in the HST adapter.
    UPDATE (scope both-keys to the HST path).
17. `assets/titles/synthetic-title2.json:20-123,130-134` references
    `fixtures/synthetic_title2/*` paths that do not exist while claiming
    `program-image: implemented`. Planner does not check existence by design.
    GENERATE_FROM_AUTHORITY (materialize fixture) or UPDATE to planned.
18. `assets/titles/README.md:12` "Private bindings belong in a separate
    Git-ignored workspace manifest" vs live: no such format exists; bindings
    are CLI/env (`title_codegen_plan.py:414-418`, `Makefile:288-292`,
    `hst_manager.ps1:140,194-236`) and never written back. UPDATE.
19. `assets/public_source_profile.json:165` includes `TODO.md`, which was never
    tracked or logged (`git ls-files`/`git log -- TODO.md` empty); export
    still claims tracked 674 / included 674. UPDATE (remove entry or
    materialize file, then regen export through the trusted policy path).
20. `docs/TOOLCHAIN_BASELINE_2026-08.md:1-5` has no HISTORICAL banner;
    ESLint `^10.8.0` / TypeScript `^7.0.2` rows superseded by live
    `interface/package.json` (`^9.22.0` / `^6.0.3`); MSYS2/Vulkan reference
    versions ~5 weeks old. MOVE_TO_ARCHIVE or banner HISTORICAL/REFERENCE.
21. `docs/PSPDEV_LOCAL_VERIFICATION.md:190-191` "DCO intentionally not enforced
    during the current private-development phase" vs live `CONTRIBUTING.md`,
    `DCO_POLICY.md` section 5.1, and the active public repository.
    SUPERSEDE with the public-phase rule.
22. `docs/research/PSP_THREADING_SEMANTICS.md:18-21` review base `d8b0d4f`
    resolves to `fatal: bad object` publicly (link 404s); header
    `HARDWARE_EXECUTION=NOT RUN` misleads because lifecycle/callback/arg5
    facts in the same document are separately MEASURED. The CT/ST 28/5 oracle
    itself is genuinely NOT_RUN and `FIRMWARE_GENERIC_ARG6_8=NOT_MEASURED`
    remains accurate. UPDATE (rebase table, qualify header).
23. `docs/TITLE_CODEGEN_PLAN.md:25` "planner is the only place" vs manifest-less
    direct-Make bypass (`docs/PORTING.md:116`; `Makefile:11-60`) and the
    absent-file sync anchor (`Makefile:47`); `FUNCS_PER_CHUNK=2000` in four
    coinciding defaults (`Makefile:195`, `title_codegen_plan.py:191,321`,
    `hst_manager.ps1:282`, `codegen.py:1886`). UPDATE scope; MERGE default.
24. `docs/SETUP.md:3` "core build is Windows-only" overbroad vs
    `PLATFORM_PORTABILITY.md:27-28` host-neutral object gate (explicitly not
    Linux support) + `ARCHITECTURE.md:359,400`; toolchain repeated in six
    places; test flag sets drift (`README` vs `CONTRIBUTING` vs
    `CI.md:75-101` + `AGENTS.md` section 9). `CI.md:224-228`
    `windows-2022`-vs-Windows-11 wording is the correct pattern — keep as
    template. UPDATE (SETUP stays authority; rest become pointers).
25. Legacy second-truth copies kept intentionally: `hst_manager.ps1:238-258`
    legacy `0x0029a060` (pinned by `test_title_manager_adapter.py:243-250`
    to exactly one literal + warning), `tools/hst_doctor_core.py:16`
    `EXPECTED_DISC_ID` vs manifest `disc.id` (self-admitted in
    `PORTING.md:316-320` with a stated retirement criterion). KEEP with
    explicit LEGACY_ADAPTER labels; do not merge prematurely.

## Full finding table

PATH / LIVE CLAIM / ACTUAL AUTHORITY / STATUS / DISPOSITION. "OK" rows were
checked and need no action. Nothing qualifies for deletion; stale entries are
evidentiary pins to version or supersede, never to delete.

- `ISSUES.md:15` / P0 Open #132 / live #132 CLOSED 2026-08-27 / STALE state / UPDATE.
- `ISSUES.md:17` / P1 Open #40, follow-up open / live #40 CLOSED 2026-09-01;
  corpus repair + emitter census landed and enforced / STALE state / UPDATE.
- `ISSUES.md:23` / 46 addresses / 59 sites, 16 behaviour-altering /
  `PORTING.md:241-248` + `compat_overrides.py` (30/36 DIAGNOSTIC_ONLY, 0
  override, 16 configured) + manifest gate 42 passed / STALE counts /
  UPDATE via generation.
- Live #98 body items 1+3 / remaining debt: shared scratch stack, `0x46d14`
  stub / `PORTING.md` C-4/C-5 RETIRED + zero live hits + `nested_frames.c`
  exists / SUPERSEDED / UPDATE the live issue body.
- Title-manifest "checked-in" sites listed above / checked-in HST manifest /
  `git ls-files assets/titles/` has no `hst-ucus98701.json`; profile excludes
  it; titles README says intentionally not checked in / CONTRADICTION /
  UPDATE (5 sites).
- `docs/PORTING.md:17-45` / HST manifest worked example / file absent in
  public clones; command fails at `hst_manager.ps1:200` / CONTRADICTION /
  UPDATE (synthetic primary).
- "Git-ignored" sites listed above / ignored HST manifest / no `.gitignore`
  rule; `check-ignore` exit 1; real control is publication exclusion /
  STALE mechanism / UPDATE wording (+ deliberate ignore rule).
- `PSP_INTR_WAITS_MATRIX.md:583-588`, `IMPORT_AUDIT.md:104-109` / Mutex on
  `h_ok` / dedicated `h_LockMutex*` at `hle.c:10372-10842`; conformance base
  moved / STALE / UPDATE + GENERATE_FROM_AUTHORITY.
- `PSP_INTR_WAITS_MATRIX.md:20-24,225-244` / current-main 50/91/20/55 /
  header says historical; numerator moved; no snapshot hash / STALE framing /
  UPDATE + GENERATE_FROM_AUTHORITY; MOVE_TO_ARCHIVE on fork.
- `HARDWARE_ORACLE.md` Loop-C + oracle manifest note / #23 hardware-blocked /
  #23 concurrency MEASURED 64/64 on 2026-08-27; invalid-tail still
  NOT_MEASURED / PARTLY SUPERSEDED / UPDATE (keep NOT_MEASURED).
- `HARDWARE_ORACLE.md` Loop-A / future VFPU fuzz / Group A/B MEASURED per
  #40 / PARTLY SUPERSEDED / UPDATE scope.
- `HARDWARE_ORACLE.md:98` / #64 VBLANK unanswered / `PSP-DISPLAY-001`
  implemented; `ARCHITECTURE.md:448-468` correct / STALE index / UPDATE.
- `ARCHITECTURE.md:189` / MPEG/ATRAC PPSSPP-derived / vendored atrac3p
  decoder + bridge + selftests exist / INCOMPLETE / UPDATE.
- `ARCHITECTURE.md:298-347` / CALL/TAIL floor / accurate but missing the
  `nested_frames` boundary; see `dispatch_isolation_selftest` / INCOMPLETE /
  UPDATE citation.
- `HARDWARE_ORACLE.md:84-99` / bare tracker numbers / live map in
  `ISSUES.md:16-29`; only #35 labeled historical / STALE style / UPDATE.
- Fixture-count sites listed above / "second fixture" / three fixtures
  checked in; `Makefile:33`; proof test / STALE count / UPDATE.
- `assets/titles/README.md:63-68` / both span seams generic / generic is
  `TITLE_EXTRA_SPANS` only / CONTRADICTION / UPDATE scope.
- `synthetic-title2.json` / implemented program-image + paths / fixture
  paths absent; no existence test / UNPROVEN / GENERATE or mark planned.
- `assets/titles/README.md:12` / workspace manifest file / no such format;
  live bindings are CLI/env / CONTRADICTION / UPDATE.
- `public_source_profile.json:165` / `TODO.md` included / never tracked /
  STALE entry / UPDATE via trusted policy path.
- Planner-only + `FUNCS_PER_CHUNK` sites listed above / single contract /
  direct-Make bypass exists; four coinciding defaults / CONTRADICTION +
  DUPLICATE / UPDATE scope; MERGE default.
- `hst_doctor_core.py:16` vs manifest / disc.id twice (+ adapter third) /
  self-admitted with criterion / TRACKED DUPLICATE / KEEP.
- `hst_manager.ps1:256` / legacy `0x0029a060` / pinned to one literal +
  warning by adapter tests / INTENTIONAL LEGACY / KEEP.
- `TOOLCHAIN_BASELINE_2026-08.md` / current baseline; ESLint10/TS7 / no
  banner; live `package.json` ESLint9/TS6 / SUPERSEDED rows / banner or
  MOVE_TO_ARCHIVE.
- `PSPDEV_LOCAL_VERIFICATION.md:190-191` / private-phase, no DCO / live
  governance says otherwise / STALE / SUPERSEDE.
- `PSP_THREADING_SEMANTICS.md:9-21` / NOT RUN header + `d8b0d4f` base / base
  404s publicly; lifecycle separately MEASURED / MISLEADING + STALE pointer /
  UPDATE.
- `SETUP.md:3,49` / Windows-only; CMake/Ninja not required / gate is not
  support; staged CMake target in portability plan / IMPRECISE /
  UPDATE qualifiers.
- Toolchain x6 / test-flag drift / `SETUP.md:41-43` declares SETUP authority;
  executable contract in `hst_doctor_checks.py`; routing in `CI.md` + AGENTS /
  DUPLICATE / KEEP authority, demote rest to pointers.
- `DEBUGGING.md:160` / bare `(issue #64)` / content consistent with #64
  Closed; style only / STALE style / UPDATE link.
- `ISSUE196_DIRECT_XB.md`, `IMPORT_AUDIT.md`, matrix, `OSPS_BASELINE.md`
  banners / historical scope / correctly labeled post-`07202dd` / OK / KEEP.
- `README.md:149` Wiki link / `has_wiki=true` live; content unverified /
  NEEDS VERIFICATION / KEEP link, verify content.
- Routes/VBLANK prose (`DEBUGGING.md`, `SETUP.md`, `ARCHITECTURE.md`) /
  renderer/HLE/timing scope / consistent with #64 Closed / #70 Open; no
  oracle or Linux overclaim / OK / KEEP.

## Proposed docs taxonomy

- CURRENT (contract, maintained): `SETUP.md`, `CI.md`, `ARCHITECTURE.md`,
  `PORTING.md`, `TITLE_CODEGEN_PLAN.md`, `PLATFORM_PORTABILITY.md`,
  `DEBUGGING.md`, `WORKSPACE_DOCTOR.md`, `STATIC_VERIFY.md`,
  `PUBLICATION_READINESS.md`, `PUBLIC_SOURCE_PROFILE.md`,
  `PROVENANCE_MERGE_GATE.md`, `PROJECT_MODEL.md`, `HARDWARE_ORACLE.md` (after
  index fix; trace-oracle stays labeled proposal), `SYMBOL_REFERENCE.md`,
  `DECOMPME_INTEGRATION.md`, `AI_USAGE.md`, `PSPDEV_LOCAL_VERIFICATION.md`
  (after DCO fix), `provenance/HST_PUBLIC_CENSUS.md`,
  `provenance/INDEPENDENCE_MODEL.md`, `provenance/INDEPENDENCE_BACKLOG.md`,
  `provenance/GUEST_INTERP_ATTESTATION.md`, root `README.md` / `ISSUES.md` /
  `AGENTS.md` / `CONTRIBUTING.md`.
- REFERENCE (dated evidence; live source authoritative):
  `provenance/GENERICITY_CENSUS_20260826.md` (already scoped),
  `TOOLCHAIN_BASELINE_2026-08.md` (needs banner),
  `research/PSP_THREADING_SEMANTICS.md` (frozen design + measured-scope table).
- HISTORICAL (pre-republication snapshot; do not cite as status):
  `OSPS_BASELINE.md`, `PSP_INTR_WAITS_MATRIX.md` (snapshot once a live table
  is generated), `IMPORT_AUDIT.md`, `ISSUE196_DIRECT_XB.md`,
  `provenance/MODIFIED_FILE_NOTICES.md`.
- SUPERSEDED (rows/sections only, preserved inline with pointer): #98 items
  1+3, Mutex `h_ok` paragraphs, DMA hardware-blocked note, VFPU unmeasured
  framing, ESLint/TS baseline rows, private-phase DCO line,
  `HARDWARE_ORACLE.md` bare tracker list style, `d8b0d4f` links.

## Facts that must be machine-generated, not hand-maintained

Issue open/closed state (live GitHub; enforced offline by the At-a-glance
rule in `audit_public_issue_links.py` and online by its live query);
`hle.c` address/site/bucket census and compatibility-override counts
(`test_compat_manifest.py` from `compat_overrides.py`); NID/hook tables
(`hle_manifest.py --evidence-chain`); intr `base[]` totals; tracked/included/
excluded path totals + orphan-include detection; synthetic corpus support
counts; title-manifest span/module/base sync; toolchain effective versions
(doctor output); `PUBLIC_EXPORT.json` + ledger digests (`policy_sync.py`
`--regen-export`). Human docs cite the generating command and commit, never
restate the number.
