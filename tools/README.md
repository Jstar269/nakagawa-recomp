# `tools/` — Host-side recompiler scripts

These scripts require Python 3.14.x. PowerShell entrypoints require PowerShell 7.4+ (`pwsh`); they
run on the development host and are never executed by `hst.exe` at runtime. For HST, use the canonical `nk_manager.ps1` with `-GameName hst` and `-TitleManifest`; it supplies the required zero base/entry values and drives the Makefile's two-phase build.

## Pipeline (in order)

1. **`prxload.py <elf> <base> --out=build/<g>/<g>_image.bin`**
   Rebase the PRX/ELF at `<base>`, apply type-A relocs (`SHT_PRX_RELOC = 0x700000A0`),
   dump a flat image suitable for `--image`.

2. **`imports.py <elf> <base> --toml=build/<g>/<g>_imports.toml`**
   Walk the import tables, resolve MIPS NIDs to HLE handler names.
   `codegen.py` reads this TOML at translation time.

3. **`codegen.py <elf> build/<g>/<g>_recomp.c --base=<base>`**
   MIPS → C. Consumes `analyze.py`'s function boundaries. The generator splits the
   output into `<g>_recomp_0.c` through `<g>_recomp_N.c`; the number of translation
   units is determined by the discovered function count and `FUNCS_PER_CHUNK`, not by
   a fixed HST-specific chunk count. The split keeps individual files practical for
   gcc `-O0` compilation.

4. **`mingw32-make GAME_NAME=<g> GAME_ELF=<elf> GAME_BASE=<b> GAME_ENTRY=<e> all`**
   Drives the two-phase pipeline and compile. Set `VULKAN_SDK` for direct Make invocations; the
   manager discovers and validates it automatically. Do not replace `all` with a single dependency
   line: generated chunk discovery occurs in the second Make process. For HST, use
   `.\nk_manager.ps1 -Action BuildFull -TitleManifest assets/titles/hst-ucus98701.json -GameName hst` from the repository root.

## Gates

- **`codegen_gate.py <elf> <oracle.trace> <workdir>`** — external-oracle gate.
  Generates C, compiles with `$CC` (or `gcc`), runs to the first HLE boundary, and compares the
  pre-HLE trace with a user-supplied oracle trace (captured from PPSSPP or the reference interpreter).
- **`verify_gates.py`** — orchestrates the optional codegen and microtest gates used by
  `make verify`; it reports blocked when their external inputs are absent.
- **`funcdiff_cmp.py <oracle-trace> <my-trace> <entry-step>`** — compares per-function
  traces supplied by the developer. Fail-closed evidence gate (issue #354): exit 0 requires
  at least one compared step, oracle coverage of every recomp step from `<entry-step>`
  onward (the oracle may continue beyond that slice), and all steps matched. Empty traces,
  oracle truncation, entry beyond the oracle, or malformed records exit nonzero.
- **`microtest_gate.py`, `gen_microtest.py`, `vfpu_fuzz_gen.py`** —
  per-instruction / per-function tests for translator regressions.
- **`tracediff.py`** — trace-format diff during bring-up (`TRACE_FORMAT.md`).
- **`ppmdiff.py`, `ppm2png.py`** — A/B framebuffer diffs and PPM-to-PNG conversion.
  `SR_FBSNAP=<N>` writes rotating PPM snapshots every N vblanks.
- **`nidseq.py`, `gen_nidnames.py`** — NID-table tooling. `nidseq.py <imports.toml>
  <trace>` is informational extraction (no equivalence claim, exit 0). With a second trace,
  `nidseq.py <imports.toml> <trace> <oracle-trace>` is verification: exit 0 means exact
  import-sequence equality with at least one import compared (issue #354); divergence,
  zero compared imports, either-side sequence mismatch, or malformed data exit nonzero.
  `--allow-prefix` explicitly accepts strict-prefix agreement for documented prefix
  analysis and is never the default.
- **`import_audit_gate.py`** — public import-coverage/fake-success gate:
  fail-closed HLE manifest from `src/rt/hle.c` (`hle_manifest.py` +
  `hle_registry_meta.py`), classification baseline drift, and synthetic malformed-ELF
  fixtures (`import_fixtures.py`, `psp_import_table.py`). `import_audit.py` classifies a
  developer-supplied private ELF locally — see [`docs/archive/IMPORT_AUDIT.md`](../docs/archive/IMPORT_AUDIT.md).
- **`lint_docs.py`** — deterministic offline documentation-freshness gate. It scans tracked Markdown
  and rejects current-facing stale-status patterns while preserving explicitly historical evidence.
  It also rejects a hand-written HLE registration / NID / fake-success count on a CURRENT page:
  those counts are a generated artifact (`hle_manifest.py --census-markdown`), so prose may only
  state them labelled historical/capture-time.
  The shared pre-commit/pre-push hooks run it automatically.
- **`audit_public_issue_links.py`** — networked public Issue/PR reference audit for tracked Markdown.
  It verifies URL type, current-facing shorthand references, and explicit current-tracker state labels.
  Normal mode reports `SKIPPED` if GitHub is unavailable; use `--strict` as a live review/merge audit.
  It is deliberately separate from the offline pre-commit hook so lack of network access cannot make
  ordinary local commits nondeterministically fail.
- **`tree_lint.py`** — dead-file detection for the public tree. It reports tracked files that no other
  tracked file names; `python tools/tree_lint.py --check` enforces the reviewed reference and allowlist ratchet
  in the always-on hygiene job; unreferenced files must be removed or have a reviewed allowlist reason in
  `tools/tree_lint_allow.json`. `mingw32-make tree-lint` runs the reporting form, which exits 0 whatever the
  tree contains.
- **`xb_probe.py <archive.xb> [--lookup <inner-key>]`** — bounded, read-only direct-XB
  metadata/lookup prototype (see [`docs/archive/ISSUE196_DIRECT_XB.md`](../docs/archive/ISSUE196_DIRECT_XB.md)). It uses synthetic tests in `test_xb_probe.py`,
  never dumps archive contents by default, and does not participate in production HLE lookup.
- **`extract_xb.py <xbdata-dir>`** — batch XB extractor with no third-party dependency
  (see [`docs/SETUP.md`](../docs/SETUP.md)). The whole pipeline is repository-owned:
  `xb_probe.py` parses and decodes each member — including the nested `DEFLATE → LZS`
  layer — under the budget contract at the top of the module, and `extract_xb.py`
  normalizes each member name once and writes it at that same identity, so the validated
  path and the written path are the same on every host. Archives are staged and promoted
  only on full success, the produced tree is re-verified for reparse points, whole-file
  reads are size-gated, destinations and generated files are not replaced without
  `--overwrite`, and both worker count and in-flight task count are bounded. Synthetic
  tests live in `test_extract_xb_security.py` and `test_extract_xb_gim.py`.

Run the generator regression suite without game inputs:

```powershell
python -m unittest discover -s tools -p "test_*.py" -v
```

For a documentation change with network access, also run:

```powershell
python tools/audit_public_issue_links.py --strict
```

## Module index

Every tracked top-level module in `tools/` appears exactly once below, grouped by what it
does, so a new tool cannot ship undiscoverable. `tools/test_lint_docs.py` compares this
section against the tracked module set and fails closed when a module is added, renamed or
removed without a matching row:

```powershell
python -m unittest tools.test_lint_docs -v
```

Every tracked subdirectory of `tools/` is listed separately, by directory rather than by
module, and `tools/test_lint_docs.py` compares that list against the tracked subdirectory
set, so a new subpackage cannot ship undiscoverable either:

<!-- tools-subpackages:begin -->

| Subpackage | Purpose |
| --- | --- |
| `ghidra_scripts/` | Ghidra headless scripts: decompile to C, export the function CSV, list references. |
| `nk_core/` | Portable preparation and runtime library shared by the tools. |
| `psp_oracle/` | Host side of the PSP hardware oracle, runners and `verify_vfpu_addr.py`. |
| `psp_threading_oracle/` | Threading analyzer, parser and evidence model. |

<!-- tools-subpackages:end -->

<!-- tools-index:begin -->

### Guest pipeline and code generation

| Module | Purpose |
| --- | --- |
| `prxload.py` | Rebase a PRX/ELF at a base address and write a flat image. |
| `analyze.py` | Discover function boundaries, executable spans and tail calls in a guest ELF. |
| `imports.py` | Walk guest import tables and resolve MIPS NIDs to HLE handler names. |
| `codegen.py` | Translate guest MIPS to C and split the output into `<g>_recomp_<n>.c` chunks. |
| `prx_reloc_model.py` | Model every PSP module relocation as a function of the load base, for position-independent module translation. |
| `host_stubs.py` | Semantic name overrides for known guest functions, imported by the generator. |
| `compat_overrides.py` | Semantic-debt manifest of every game-address-specific override in the runtime. |
| `entry_frame_balance.py` | Stack-symbolic entry classification: callable boundary versus resume PC. |
| `elf_bounds.py` | Shared, fail-closed ELF32 envelope checks for the offline PSP tools. |
| `hle_manifest.py` | Generate the authoritative HLE registration manifest from the runtime HLE table, plus the semantic-status census artifact (`--census-json` / `--census-markdown`). |
| `hle_registry_meta.py` | Curated classification metadata for the HLE registration manifest. |
| `gen_nidnames.py` | Generate the NID name header from the tracked NID corpus. |
| `nid_name_proof.py` | Independently verify and classify the entries of the generated NID name header. |
| `nid_auditor.py` | NID compliance audit: report which guest imports remain unclassified. |
| `nidseq.py` | Extract a trace's import sequence and verify it against an oracle trace. |
| `gen_microtest.py` | Generate per-instruction and per-function translator micro-test cases. |
| `ghidra_crosscheck.py` | Cross-check a Ghidra function inventory against `analyze.py` discovery. |
| `ghidra_headless.py` | Headless Ghidra driver for the recomp pipeline. |
| `decompme_export.py` | Export one guest function as a decomp.me-ready bundle (read-only exporter). |
| `shader_embed.py` | Regenerate and verify the checked-in Vulkan shader embeddings. |

### Correctness gates and differential evidence

| Module | Purpose |
| --- | --- |
| `codegen_gate.py` | External-oracle gate: generate, compile, run to the first HLE boundary, compare traces. |
| `microtest_gate.py` | Run the generated micro-tests for translator regressions. |
| `funcdiff_cmp.py` | Fail-closed per-function trace comparison against a developer-supplied oracle trace. |
| `verify_gates.py` | Run the optional differential verification gates without shell-specific syntax. |
| `tracediff.py` | Diff two runtime traces in the format documented in `TRACE_FORMAT.md`. |
| `import_audit.py` | Classify a PSP ELF's imports against the HLE registration manifest. |
| `import_audit_gate.py` | Public CI gate for import coverage and fake-success regressions. |
| `import_fixtures.py` | Synthetic PSP ELF fixtures for the import-coverage audit gate. |
| `import_stub_census.py` | Import-stub census: every import-table stub of an image is owned, bodied and reaches the HLE (or a named refusal). |
| `psp_import_table.py` | Defensive PSP ELF import-table parser for the import-coverage gate. |
| `flight_diff.py` | Compare two source-safe flight-recorder bundles. |
| `ge_transition_diff.py` | Offline diff for the narrow GE transition trace. |
| `ge_replay_metrics.py` | Strict parsers for aggregate GE replay CPU-profile summaries. |
| `ge_stat_windows.py` | Read the runtime's `SR_GESTAT` windows by the vblank that closed them, for gates such as the showcase smoke. |
| `perf_summary_diff.py` | Compare two runtime performance summaries within a tolerance. |
| `perf_attribution.py` | Present cadence (flips, re-presents, vblanks per new frame, implied fps) and cost shares for one `SR_PERF` run, or a diff of two runs. |
| `pgf_writer.py` | Deterministic PGF writer for project-generated glyph bitmaps (a fixture generator, never an authenticity claim). |
| `ttf2pgf.py` | Deterministic OpenType/TTF to PGF converter for pinned public fixtures; behaviour contract and independence record in `docs/cleanroom/PGF_CONVERTER_SPEC.md` (issue #313). |
| `evidence_model.py` | Fail-closed evidence grading and revision identity primitives. |
| `waits_census.py` | Regenerate the interrupt/dispatch waits-matrix registration census. |
| `vblank_ledger.py` | Judge one run's VBLANK delivery against the display source that owed it. |
| `boot_gate.py` | Summarize machine-readable native boot milestones from a runtime log. |
| `soak_audit.py` | Judge one soak run from the telemetry the runtime already writes. |
| `verify_vfpu_provenance.py` | Verify the checked-in VFPU data against its pinned provenance manifest. |
| `vfpu_coverage_report.py` | Generate the deterministic VFPU compatibility census; classify an encounter word list (`--encodings`) or the VFPU words a generated AOT route emits (`--route`), failing nonzero on Unsupported/OTHER words. |
| `vfpu_fuzz_gen.py` | VFPU differential-fuzz case generator. |
| `vfpu_overlap_diff_gen.py` | Generate the overlap-differential cases header for the VFPU selftest. |
| `vfpu_synth_gen.py` | Deterministic synthetic VFPU instruction corpus generator. |

### PSP input formats, parsers and oracles

| Module | Purpose |
| --- | --- |
| `xb_probe.py` | Bounded, read-only metadata and lookup access to one explicitly supplied XB archive. |
| `extract_xb.py` | Batch XB extraction with staged writes, size gates and no third-party dependency. |
| `pspsdk_source.py` | Bounded, fail-closed extraction of imports and prototypes from the pinned PSPSDK. |
| `pspsdk_compare.py` | Deterministic comparison of PSPSDK declarations and Nakagawa HLE metadata. |
| `pspsdk_sync.py` | Extract the pinned PSPSDK declarations and compare them with the HLE metadata. |
| `psp_readiness.py` | Report local verification and PSP-oracle readiness without touching inputs. |
| `psp_issue_matrix.py` | Generate a machine-readable classification matrix for every open GitHub issue. |
| `pspdev_lock.py` | Validate and report the pinned PSPDEV source/toolchain lock. |
| `pspdev_probe.py` | Produce bounded local PSPDEV tool identity evidence without changing the lock. |
| `validate_assets.py` | Cross-examine extracted assets against their golden references. |

### Titles, packaging and workspace bring-up

| Module | Purpose |
| --- | --- |
| `nk_cli.py` | Headless CLI for title inspection, preparation, build packaging and launch. |
| `nk_doctor.py` | Fail-closed workspace diagnostics. |
| `nk_doctor_checks.py` | Workspace, toolchain, input, runtime and repository checks behind the doctor. |
| `nk_doctor_core.py` | Shared data structures and bounded file-format validators for the doctor. |
| `hst_test_fixtures.py` | Small source-owned ELF, PSP-header and ISO envelope fixtures for the doctor tests. |
| `nk_clean.py` | Preview-first workspace cleaner for allowlisted output roots. |
| `title_manifest.py` | Validate and deterministically normalize public title manifests. |
| `title_qualification.py` | Qualify public titles: validate manifests, input profiles and bring-up reports, and smoke-launch a public title through the native player in a sandbox. |
| `title_codegen_plan.py` | Build a deterministic code-generation plan or AOT package from a title manifest. |
| `title_catalog_codegen.py` | Deterministic C generator for the native public title catalog. |
| `title_runtime_config.py` | Emit the build-local runtime title configuration consumed by the runtime. |
| `enhancement_package.py` | Validate and verify enhancement package declarations. |
| `stage_runtime_dlls.py` | Stage the host runtime library closure beside a built player or title package. |
| `build_profile.py` | Hash and record the build identities the native build depends on. |
| `build_graph_snapshot.py` | Read-only snapshot of the current Make build graph. |
| `record_toolchain.py` | Record observed live toolchain identities using the doctor's detection functions. |
| `vulkan_sdk.py` | Vulkan SDK validation and discovery shared by the workspace doctor. |
| `library_sweep.py` | Run the production ISO bring-up route across a library and aggregate blockers. |
| `progress_tracker.py` | Run concrete gating checks against real outputs and emit a structured progress record. |

### Publication, provenance and supply chain

| Module | Purpose |
| --- | --- |
| `publication_policy.py` | Canonical publication-eligibility policy. |
| `policy_sync.py` | Report, and optionally apply, drift between tracked paths and the canonical policy. |
| `publish_audit.py` | Audit the prospective tracked tree or a candidate release directory before publication. |
| `public_candidate.py` | Materialize and audit an exact-ref public-safe source candidate. |
| `public_export.py` | Single authoritative public export generator. |
| `build_public_export.py` | Dry-runnable fresh public export generator and public-export gate verifier. |
| `sync_drift_check.py` | Fail-closed reconciliation between a public-safe export and the public repository. |
| `provenance_ledger.py` | Build and validate the explicit public provenance ledger. |
| `provenance_refresh.py` | Generate the public provenance controls with the hosted attestation logic. |
| `provenance_attest_verify.py` | Verify a candidate tree's public provenance against external authority. |
| `provenance_record_gap.py` | Inventory the tracked paths that have no exact trusted provenance record (`--check-records` gates on it); `--check` is the separate upstream-derived disposition ratchet; the two flags are mutually exclusive (exit 2), so run them as two invocations. |
| `modified_file_notice_audit.py` | Check the explicit notices recorded in the inherited-file manifest. |
| `history_audit.py` | Non-destructive full-history secret, proprietary-material and privacy audit. |
| `betterleaks_canary.py` | Exercise the pinned Betterleaks policy with synthetic, non-secret canaries. |
| `gen_key_scrub_spec.py` | Generate a history-rewrite replacement spec that purges key constants. |
| `verify_key_scrub.py` | Check whether any key-handling constant is still reachable in Git history. |
| `generate_sbom.py` | Generate SPDX and CycloneDX software-bill-of-materials artifacts for a release. |
| `verify_sbom.py` | Verify dependency locks, the release manifest and the generated SBOM artifacts. |
| `package_notices.py` | Deterministic third-party notice generation and licensing gate for native packages. |

### Documentation, policy and CI wiring

| Module | Purpose |
| --- | --- |
| `lint_docs.py` | Deterministic offline documentation freshness and staleness linter. |
| `audit_public_issue_links.py` | Networked audit of public Issue and PR references in tracked Markdown. |
| `contrib_check.py` | Run only the local gates that apply to the files you changed. |
| `discovery_contract.py` | Inventory and verify the repository's unittest discovery contract. |
| `ci_paths.py` | Classify a change for the path-gated public CI workflow. |
| `ci_required.py` | Evaluate the stable aggregate status for the path-gated CI workflow. |
| `ci_test_shards.py` | Deterministic cost-weighted partition of the Python test modules into CI shards; weights in `tools/ci_test_weights.json`. |
| `portability_inventory.py` | Public-safe portability inventory of the native runtime and build. |
| `tree_lint.py` | Report tracked files that no other tracked file names; `--check` enforces the reference ratchet. |

### Performance, frames and runtime diagnostics

| Module | Purpose |
| --- | --- |
| `run_perf_benchmarks.py` | Run the public source-owned performance benchmark matrix. |
| `compiler_compare.py` | Compare GCC against Clang on the public corpus, with the semantic veto ahead of every timing. |
| `generate_benchmarks.py` | Generate the public benchmark matrix. |
| `capture_native_screenshots.py` | Capture showcase screenshots from a built native player. |
| `ppmdiff.py` | Diff two PPM framebuffer directories, optionally watching for changes. |
| `ppm2png.py` | Convert framebuffer snapshots written by the runtime into PNG. |
| `pngcmp.py` | Compare two framebuffer PNGs written by `ppm2png.py`. |
| `frame_capture_check.py` | Temporal acceptance check for present-truthful frame captures: frame accounting, black/stale-frame and frame-gap detection, and watchdog/present-gap classification. |
| `padscript_from_log.py` | Convert recorded controller transitions into a replay pad script. |
| `mem_debug.py` | Interactive memory and CPU-state query and mutation tool for bring-up. |

#### Comparing host compilers (issue #317)

`compiler_compare.py` answers "is Clang faster here?" only after it has shown that Clang is
still correct here. Each requested compiler that resolves on `PATH` builds the same public
source-owned corpus into its own build root, then runs a fixed veto chain — the cosimulation,
the cosimulation negative corpus, the stale-code detector and the LLE COP0/exception
selftest. The first failing gate stops the chain, and the compiler is reported `VETOED`
naming that gate, with no build time, no binary size and no workload timing recorded for it.
A compiler missing from `PATH` is reported `NOT_AVAILABLE`, never a pass. The per-compiler
`run_perf_benchmarks.py` workload matrix runs only after the whole chain passes, into a build
prefix of its own so no candidate reuses another candidate's objects.

```powershell
python tools/compiler_compare.py                        # gcc then clang
python tools/compiler_compare.py --compilers gcc        # one candidate
python tools/compiler_compare.py --output build/compiler-compare
```

The report is `build/compiler-compare/compiler_compare.json`, and its shape is fixed by
`validate_report()`, which refuses to accept a report that carries a timing under a vetoed or
unavailable compiler. Exit status is `0` only when at least one compiler is `MEASURED` and
none is `VETOED` or `WORKLOAD_FAILED`; a missing `make` is exit `2`. Each compiler's corpus build
and report stay inside the `--output` root, but five workloads (`production-smoke-gap`, `cosim`
and the three `platform-ladder-*` targets) build into their fixed repository trees under `build/`,
which each compiler rewrites in turn. No title/ISO/ELF/PRX input is read, and running the
harness changes no compiler default. Findings belong in issue #317.

### Regression tests: code generation and the guest interpreter

| Module | Purpose |
| --- | --- |
| `test_analyze_tailcall.py` | Regression tests for the analyzer's tail-call promotion. |
| `test_analyze_import_stub_entries.py` | Import-table stubs outside the named code sections are owned function entries, with codegen bodies. |
| `test_analyze_toml_emit.py` | Locks the analyzer TOML emitter's fail-closed string contract. |
| `test_analyzer_span_scope.py` | Regression tests for analyzer executable-span ownership. |
| `test_codegen_continuations.py` | Linked transfers carry a resume boundary; unlinked ones stay tail dispatches. |
| `test_codegen_entry_semantics.py` | End-to-end regressions for callable versus resume-entry codegen semantics. |
| `test_codegen_fp_convert.py` | Execute generated COP1 conversions against independent fixed vectors. |
| `test_codegen_gate_b_encoding.py` | Regression tests for gate ISA purity: every instruction under test stays pure. |
| `test_codegen_madd_msub.py` | Regression tests for Allegrex madd/maddu/msub/msubu decoding and emission. |
| `test_codegen_nan_trap.py` | Tests for the NaN/Inf origin diagnostic. |
| `test_codegen_no_shadow_stubs.py` | Guard the removal of two code-shadowing custom stubs. |
| `test_codegen_profile_isolation.py` | A profile-free run must inherit no title-specific behavior. |
| `test_codegen_retail_allocator.py` | Retail allocator APIs must bridge to the host allocator. |
| `test_codegen_runtime_modules.py` | Position-independent translation contract of runtime-placed guest modules. |
| `test_codegen_static_verify.py` | Static verification of the generator's constant-propagation state machine. |
| `test_codegen_transfer_target_timing.py` | A computed transfer must read its target register at the transfer. |
| `test_codegen_vfpu_fallback.py` | VFPU fallback emission, including unsupported forms failing closed. |
| `test_cosim_fixture.py` | Structural gates for the AOT/interpreter cosimulation fixture. |
| `test_compat_manifest.py` | Every custom codegen stub and dispatch hook must be in the manifest. |
| `test_cosim_mutation_gate_outcome.py` | Cosim mutation driver verdicts bound to process status, the timeout verdict and child-tree reaping. |
| `test_cpu_lle.py` | Low-level emulation CPU gates: COP0, exceptions, eret, interpreter support. |
| `test_domain_mode.py` | Domain mode table and the import-call seam gates. |
| `test_dispatch_call_boundary.py` | Mutation proof for the production interpreter CALL/RETURN boundary. |
| `test_dispatch_fatal_policy.py` | Tests for fail-closed dispatch and unknown NID handling. |
| `test_direct_j_omission_mutation.py` | Compiled mutation proof for omitted direct-j execution boundaries. |
| `test_entry_frame_balance.py` | Structural entry-role classification regressions. |
| `test_fp_scalar_mutations.py` | Committed mutation regressions for the scalar FPU control-register slice. |
| `test_guest_span_routing.py` | Source-shape gates for the native bulk and parser slice. |
| `test_guest_printf.py` | Behavioral regression checks for the guest formatted-output bridge. |
| `test_hle_umd_wakeup.py` | The ready signal must wake only UMD waiters; a plain wait creates no drive event. |
| `test_import_audit.py` | Tests for the import-table parser, classifier and CI gate. |
| `test_import_name_safety.py` | Synthetic regressions for untrusted PSP import-library metadata. |
| `test_import_stub_census.py` | The import-stub census over synthetic images, including the late-pass regression guard. |
| `test_imports.py` | Tests for the trusted code-generation import-map compatibility path. |
| `test_nid_name_proof.py` | Tests for the independent NID-name verifier. |
| `test_nidseq.py` | Fail-closed import-sequence extraction and oracle comparison. |
| `test_gen_nidnames.py` | Tests for the corpus-driven NID name-table generator. |
| `test_hle_manifest.py` | Tests for the fail-closed HLE registration manifest. |
| `test_stale_code_detect.py` | Opt-in stale translated-code tracking: detection and invalidation. |
| `test_trace_window_probe.py` | Contract test for the address-windowed instruction trace. |
| `test_callback_correctness.py` | Run the callback dispatcher for real instead of grepping its source. |
| `test_debug.py` | Behavioral checks for runtime debug watchpoints. |

### Regression tests: runtime C, drivers and host platforms

| Module | Purpose |
| --- | --- |
| `test_asset_index_c.py` | Host-neutral regression for the dynamic extracted-asset index. |
| `test_build_truth.py` | Regression tests for compiler-profile and transitive-header build truth. |
| `test_dispatch_c.py` | Behavioral and wiring coverage for the guest code-address dispatch table. |
| `test_dmac_semantics.py` | Source-shape gates for the measured DMAC copy path. |
| `test_evf_c.py` | Regression coverage for PSP event-flag semantics. |
| `test_guestmem_c.py` | Behavioral and wiring coverage for the guest-memory bounds contract. |
| `test_mpeg_h264_bounds.py` | Static contracts and arithmetic fixtures for the MPEG bounds work. |
| `test_native_driver_hardening.py` | Malformed-input regression tests for the native runtime driver. |
| `test_native_gate_stub_link.py` | Keep the headless gate link surface synchronized with the runtime. |
| `test_native_host_backends.py` | Run the host-backend native tests from the Python gate. |
| `test_flash0_font.py` | The read-only flash0: font device selftest, through the built runtime. |
| `test_platform_ladder.py` | Regressions for the source-owned second-platform workload ladder. |
| `test_prx_loader_cleanroom.py` | Clean-room black-box tests for the PRX loader. |
| `test_prx_reloc_model.py` | Pins the relocation model to the runtime PRX loader byte for byte at several bases. |
| `test_ref_run_elf_hardening.py` | Malformed-input regression tests for the standalone reference ELF runner. |
| `test_savedata_security.py` | Production-path regressions for the bounded savedata security successor. |
| `test_savedata_spans.py` | Guest span validation, preflight ordering and portable contained-delete seams. |
| `test_sched_invariants.py` | Source-level guards for scheduler and thread-lifecycle invariants. |
| `test_scripted_input.py` | Scripted controller input reaches the guest in guest time, including on a starved host. |
| `test_sdkver_c.py` | Retained-state regression for the compiled SDK-version contract. |
| `test_tracediff_local_coverage.py` | Strict local trace coverage contract for `tracediff.py --strict-local` and the codegen/microtest gates. |
| `test_vfs_c.py` | Host-neutral VFS path joining regression test. |
| `test_vfs_contained.py` | Executable evidence for the host-neutral contained-delete seam. |

### Regression tests: VFPU and floating point

| Module | Purpose |
| --- | --- |
| `test_vfpu_addressing.py` | VFPU vector-register addressing: hardware agreement and cross-implementation identity. |
| `test_vfpu_coverage_census.py` | VFPU coverage census schema, decoder coverage, deterministic output and the `--route` emitted-word census. |
| `test_vfpu_domain_boundary.py` | Contract tests for the out-of-domain transcendental boundary. |
| `test_vfpu_interp_guards.py` | VFPU interpreter guards: register width rejection and overlap-scan limits. |
| `test_vfpu_nan_payload.py` | Contract tests for the VFPU NaN/Inf probe fixture. |
| `test_vfpu_oracle.py` | Contract tests for the VFPU transcendental hardware oracle. |
| `test_vfpu_provenance.py` | Checked-in VFPU assets must match their manifest hashes. |
| `test_vfpu_synth_corpus.py` | Invariant tests for the public VFPU synthetic corpus and coverage report. |
| `test_vfpu_table_manifest.py` | Sync and invariant tests for the fail-closed VFPU table loader. |
| `test_vfpu_trace_bits.py` | Guard the VFPU trace diff against float/int comparison regressions. |

### Regression tests: titles, manifests and packaging

| Module | Purpose |
| --- | --- |
| `test_enhancement_package.py` | Unit tests for the enhancement package schema and validator. |
| `test_font_provisioning.py` | Firmware font provisioning: structural validation and the local cache contract. |
| `test_generic_title_planning_proof.py` | Planning proof: the tracked title is unchanged while a synthetic second title flows generically. |
| `test_hle_title_config_behavior.py` | Production-path regressions for the title-qualified HLE bindings. |
| `test_hle_title_isolation.py` | Title isolation for overrides retired from the generic HLE table. |
| `test_hst_manager_manifest.py` | Hermetic opt-in manager and build planning plus precedence tests. |
| `test_hst_title_manifest.py` | Public manifest identity, zero-based executable policy and owned spans. |
| `test_iso_parity.py` | Differential parity between the native disc, library and launch code and Python. |
| `test_manager_safety.py` | Manager filesystem, process and build safety guarantees. |
| `test_nk_clean.py` | Preview and allowlist safety contracts for the workspace cleaner. |
| `test_nk_cli_progress.py` | CLI progress reporting and log-file options. |
| `test_nk_core.py` | Unit tests for the portable preparation and runtime library. |
| `test_package_cache.py` | Cache tiering, cache-key separation, in-checkout write refusal, bounded external JSON. |
| `test_player_package_determinism.py` | Determinism and public-export exclusion of the player package route. |
| `test_player_package_route.py` | The player package route on a source-owned fixture. |
| `test_prep_fail_closed.py` | Fail-closed preparation regressions. |
| `test_prereq_fetcher.py` | No-network tests for the pinned prerequisite download and install path. |
| `test_profile_zero_e2e.py` | Manifest-driven production-route checks for the two profile-zero fixtures. |
| `test_public_title_isolation.py` | Regression suite for public and private title isolation. |
| `test_second_title_ingest.py` | Multi-title scalability: two distinct profiles ingest through one generic path. |
| `test_showcase.py` | Source-owned showcase art and disc-image regression checks. |
| `test_title_catalog.py` | Title catalog generation, drift detection and native C lookup parity. |
| `test_title_codegen_plan.py` | Title codegen plan contract: manifest-driven spans and fail-closed policy. |
| `test_title_codegen_synthetic.py` | A source-owned manifest uses the generic planning path, deterministically. |
| `test_title_manager_adapter.py` | Manager adapter contract tests that need no private title material. |
| `test_title_manager_plan.py` | Bounded manager plan fields, generic profile and fail-closed conflicts. |
| `test_title_manifest.py` | Manifest schema, canonicalization, native identity and guest module bases. |
| `test_title_manifest_parity.py` | Differential parity between the Python manifest tool and the native C validator. |
| `test_title_qualification.py` | Title qualification: validation refusals, the sandboxed smoke launch, exit codes and report shape. |
| `test_title_protected_digest.py` | Contract tests for the protected title-manifest digest. |
| `test_title_pspdev_phase5.py` | A second wholly source-owned title must prove multi-title planning. |
| `test_title_runtime_config.py` | Tests for the runtime title configuration path. |

### Regression tests: oracles, evidence and parity

| Module | Purpose |
| --- | --- |
| `test_boot_gate.py` | Native boot milestone summary: ordering, content validation and fail-closed phases. |
| `test_audit_public_issue_links.py` | Deterministic offline unit tests for the public Issue/PR reference auditor. |
| `test_decompme_export.py` | Tests for the read-only decompilation export bundle builder. |
| `test_generate_benchmarks.py` | Tests for the benchmark generator: read-only, bounded and atomic. |
| `test_lint_docs.py` | Offline deterministic tests for the documentation freshness linter, including this index. |
| `test_mem_debug.py` | Unit tests for the interactive memory and CPU-state tool. |
| `test_portability_inventory.py` | Tests for the public portability inventory. |
| `test_production_smoke.py` | Regressions for the source-owned full-production smoke guest. |
| `test_psp_decrypt.py` | Source-owned end-to-end and known-answer coverage for the decryption boundary. |
| `test_shader_embed.py` | Tests for deterministic checked-in Vulkan shader provenance. |
| `test_soak_audit.py` | Contract tests for the soak run auditor. |
| `test_vblank_ledger.py` | Contract tests for the whole-run VBLANK ledger audit. |
| `test_evidence_model.py` | Evidence grading, revision identity and the fail-closed grade ladder. |
| `test_extract_xb_gim.py` | Synthetic, retail-free malformed-image coverage for XB extraction. |
| `test_extract_xb_probe_fallback.py` | Extraction must work through the bundled probe without a native library. |
| `test_extract_xb_security.py` | Synthetic, retail-free coverage for extraction containment and limits. |
| `test_flight_build_identity.py` | Reproducible build identity of the flight recorder's build block. |
| `test_flight_diff.py` | Flight-recorder bundle schema, comparability refusal and cross-run diffing. |
| `test_funcdiff_cmp.py` | Fail-closed oracle trace comparison: divergence, empty and truncated traces fail. |
| `test_gate_exit_resolution.py` | Shared exit-stub discovery and trace helpers used by the correctness gates. |
| `test_ge_capture.py` | GE capture route regressions. |
| `test_ge_nonfinite_vertex.py` | Tests for the GE's fail-closed handling of a non-finite vertex. |
| `test_ge_stat_windows.py` | `SR_GESTAT` windows close once per VCOUNT boundary crossing, and the window parser that reads them. |
| `test_ge_transition_trace.py` | Tests for the narrow GE transition trace and its offline diff. |
| `test_hardware_runner_protocol.py` | Executable conformance suite for the hardware runner autonomy design. |
| `test_incident_regression_private.py` | Exact-incident regression against preserved private evidence. |
| `test_intr_waits_matrix.py` | Guards for the interrupt and dispatch-context conformance matrix. |
| `test_library_sweep.py` | Regressions for the private, resumable library compatibility sweep. |
| `test_matrix_disposition.py` | Guard the evidence-matrix disposition field against becoming uninformative. |
| `test_matrix_report.py` | Generate a conservative, machine-readable unittest evidence matrix. |
| `test_parse_fuzz.py` | Seeded, deterministic mutation fuzzing over the public offline parsers. |
| `test_padscript_from_log.py` | Controller log to pad script conversion: press widths and mask-change spans. |
| `test_perf_summary_diff.py` | Performance summary comparison within a tolerance. |
| `test_perf_attribution.py` | Synthetic regressions for the present-cadence and cost attribution report, including source pins on the emitter formats. |
| `test_pgf_writer.py` | PGF writer determinism and public-reader round trip over generated glyph sets. |
| `test_pgf_validate_parity.py` | Field-by-field parity of the native PGF validator and its Python mirror on seeded images and mutations. |
| `test_ttf2pgf.py` | Converter determinism, manifest/pin binding, named refusals, and public-reader round trip over the pinned font fixture. |
| `test_ppmdiff_coverage.py` | Fail-closed coverage tests for the framebuffer diff. |
| `test_frame_capture_check.py` | Frame-capture accounting plus the emitted-format contract that pins the FBSNAP/WATCHDOG patterns to the current runtime strings. |
| `test_progress_evidence.py` | Evidence-integrity regressions for the progress tracker. |
| `test_progress_tracker.py` | Tests for the progress tracker evidence checks. |
| `test_psp_issue_matrix.py` | Issue classification matrix: manifest-backed classes and fixture coverage. |
| `test_psp_leaf_parsers.py` | Fail-closed tests for the PSP-IO, PSP-AUDIO and PSP-CACHE parsers. |
| `test_psp_oracle.py` | Oracle protocol, DMAC records, acceptance gate and runner tests. |
| `test_psp_parsers.py` | Golden parsers, partial streams never passing, dual-channel reconciliation. |
| `test_psp_readiness.py` | Readiness reporting: tool presence is never a reachable device. |
| `test_psp_threading_oracle.py` | Host-side threading oracle parser, matrix and evidence model tests. |
| `test_telemetry_export_security.py` | Telemetry export must not leak private diagnostic data. |
| `test_visual_oracle.py` | Visual-oracle runner guarantees. |
| `test_waits_census.py` | Tests for the waits-matrix census generator. |
| `test_xb_probe.py` | Synthetic, retail-free coverage for the direct archive probe. |

### Regression tests: publication, provenance and supply chain

| Module | Purpose |
| --- | --- |
| `test_dco_policy.py` | Fail-closed checks for the DCO, agent and configuration contract surfaces. |
| `test_elf_bounds.py` | Synthetic malformed-ELF, image and control-flow-graph coverage. |
| `test_history_audit.py` | Repository baseline, finding classification and full-history audit reporting. |
| `test_key_scrub_tools.py` | Hermetic tests for the key-history-scrub helpers. |
| `test_modified_file_notice_audit.py` | Inherited-file notice audit against the recorded notice manifest. |
| `test_package_notices.py` | Third-party notice generation, licensing gates and relink materials. |
| `test_provenance_attest_verify.py` | A coherent candidate must not be able to self-authorize provenance. |
| `test_provenance_attestation_gate.py` | The publication gates must not be bypassed by editing the controls. |
| `test_provenance_control_merge.py` | The committed provenance controls must not conflict across independent changes. |
| `test_provenance_ledger.py` | Structural gate for the public implementation provenance ledger. |
| `test_provenance_record_gap.py` | The record-gap inventory must agree with the gate that produces the finding. |
| `test_provenance_wildcard_regression.py` | Test paths must classify through the canonical policy, not a wildcard. |
| `test_public_candidate.py` | Candidate materialization excludes disputed implementations and all fonts. |
| `test_public_export.py` | Public export generation and its gate. |
| `test_publication_policy_gate.py` | Permanent, self-contained regression suite for the fail-closed publication gate. |
| `test_publish_audit.py` | Publication audit findings, isolated git environment, text hygiene and canonical writers. |
| `test_release_manifest.py` | Release manifest basics and generated SPDX ingestion. |
| `test_sbom.py` | SBOM tooling, Python artifact hashes and toolchain policy verification. |
| `test_sbom_binding.py` | Regressions for Python lock snapshot binding in generated SBOMs. |
| `test_sbom_fail_closed.py` | Fail-closed coverage for the retained Python release dependency lock. |
| `test_sync_drift_check.py` | Unit tests for the private-to-public drift classifier. |
| `test_third_party_notices.py` | Release gate: no bundled binary or declared component ships without a license. |

### Regression tests: workspace, toolchain and repository hygiene

| Module | Purpose |
| --- | --- |
| `test_native_core_build.py` | Pins the native-core-tests dry run: the same test binaries and arguments, one compile per flag set. |
| `test_build_graph_snapshot.py` | Tests for the read-only build graph snapshot. |
| `test_build_system_parity.py` | Machine-checkable parity between the Makefile and the CMake build. |
| `test_ci_paths.py` | Path-gated CI classification tests. |
| `test_ci_required.py` | Required-CI status evaluation tests. |
| `test_ci_test_shards.py` | CI test-shard planner: exactly-once coverage, separation and workflow wiring. |
| `test_compiler_compare.py` | The compiler comparison must not publish a timing its semantic veto did not earn. |
| `test_contrib_check.py` | The contribution gate must route, and must never turn a skip into a pass. |
| `test_discovery_contract.py` | Parallel test execution and discovery contract verification. |
| `test_hst_doctor.py` | Doctor checks: data directories, ELF validation, private inputs, repository contract. |
| `test_hst_doctor_hardening.py` | Doctor hardening: commit identity and the fresh-clone setup matrix. |
| `test_manager_symbol_docs.py` | Keep the manager symbol guidance aligned with the public documentation. |
| `test_prxload_psp_header.py` | PSP header image extents: BSS restore, invalid header, size floor. |
| `test_prxload_relocations.py` | Regression tests for the HI16/LO16 relocation pairing. |
| `test_pspdev_lock.py` | Pinned PSPDEV lock validation, including abbreviated commits failing. |
| `test_pspdev_probe.py` | Bounded PSPDEV tool identity probe with redacted paths. |
| `test_pspsdk_sync.py` | PSPSDK import and prototype extraction, source identity and comparison. |
| `test_public_ci_wiring.py` | Regression checks for public and clean-checkout CI wiring. |
| `test_readme_images.py` | Deterministic checks on the published showcase screenshots. |
| `test_relocated_clone.py` | Contributor quick check in a checkout whose own path contains spaces. |
| `test_text_write_newlines.py` | A tool that emits a tracked text file must pin its line endings. |
| `test_tree_lint.py` | Offline deterministic tests for `tools/tree_lint.py`. |
| `test_validate_assets.py` | Synthetic, retail-free hostile coverage for the asset validator. |
| `test_vulkan_sdk.py` | Vulkan SDK discovery precedence and Makefile wiring. |
| `test_workspace_paths.py` | Workspace path handling with spaces, Unicode and long paths. |

<!-- tools-index:end -->

## Hard rules

- **Never** hand-edit generated files under `build/<game>/`.
- Translation fixes belong here or in the runtime; tests are the gate.
- After any generator change, run `BuildFull` and confirm generated object files are newer than
  their chunk sources before trusting runtime results.
