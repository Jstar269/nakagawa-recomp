# Review guidance

Nakagawa Recomp is an experimental PSP static recompiler: generated C plus a native C
runtime, an interpreter floor, and HLE, moving toward low-level emulation.
[`AGENTS.md`](AGENTS.md) is the operating contract; this file adds review priorities.

## Priorities

- **Private/public boundary:** reject game binaries/assets, firmware, decrypted modules,
  keys, saves, captures, traces, private addresses or paths in any file, test, document,
  log, or CI artifact.
- **ABI and transfer safety:** `src/rt/recomp.h` `CpuState` is load-bearing. A layout
  change must bump `SR_CPUSTATE_ABI_VERSION` and update `tools/mem_debug.py`; MIPS `$ra`
  is `r[31]`, with no separate link register.
- **Default-lane stability:** an opt-in change such as `--lle-cpu` or
  `--lle-import-seam` must leave generated output byte-identical without the flag and
  keep unsupported behavior fail-closed.
- **Visible failure:** invalid guest pointers, unknown opcodes, unowned targets, missing
  inputs, and unavailable backends must be reported, not guessed or synthesized.
- **Generic implementation:** no game names or private guest addresses in `src/rt/` or
  generic `tools/` code. Evidence-backed title behavior belongs in a manifest/config with
  a retirement criterion.
- **Evidence honesty:** preserve existing derived-code records and reject copied or
  translated emulator code. PSP values need a measured cell, public specification, or an
  explicit unmeasured/fail-closed label. Do not present engineering controls as legal
  conclusions or call code clean-room/independent without a supporting trusted record.
- **Host neutrality:** shared runtime and `*_posix.c` changes must build on Linux as well
  as Windows, and generic PSP semantics must not depend on Windows APIs.

## Severity

- **Critical:** private material or local paths; secrets; guest-to-host memory safety;
  a silent `CpuState` ABI change; or a default behavior change hidden behind an opt-in
  claim.
- **Warning:** missing span/pointer validation; a flag honored in only one execution tier;
  transfer state not restored on every exit; an unmeasured PSP value presented as fact;
  a test that cannot fail; or a doc claim that contradicts the Makefile,
  `nk_manager.ps1`, workflow, or source.
- **Suggestion:** naming, comment accuracy, and smaller refactors.

## Do not flag

- Regenerated `PUBLIC_EXPORT.json` and `assets/public_provenance_ledger.json` that
  re-hash exactly the paths the same commit changes (the pre-commit publication audit
  requires that), or a focused provenance-refresh commit after a merge from `main`. Flag
  entries for paths the commit does not change, a refresh commit that changes anything
  else, or a head whose "Trusted provenance attestation" check failed or did not run (it
  is currently observation-only, not required).
  Before flagging a ledger entry, attribute the hunk correctly. A unified diff's trailing
  context lines show the *next* entry's `path`, while the changed `sha256` belongs to the
  entry above the hunk. Compare that entry's path with the files the commit changes, not
  the path that follows it.
- New `assets/public_source_profile.json` entries for files the same PR adds, provided
  the attestation check passed on the head. Otherwise flag them as unauthorized public
  paths.
- Missing `Signed-off-by:` on maintainer or maintainer-directed agent commits covered by
  `docs/DCO_POLICY.md` section 5.1. Other contributors still require DCO 1.1 sign-off.
- Merge commits from `main` into a PR branch; shared branches are not rebased or force-pushed.
- Fail-closed public stubs such as `iso_unavailable.c` and `pgd_unavailable.c`. Do not infer behavior from a filename: `audio_unavailable.c` is
  the active public-safe SDL3 host-audio backend.
- Formatting-only differences already enforced by `.clang-format`, Ruff, or markdownlint.
- Edits to generated `build/<game>/*_recomp_*.c`; those files are never committed. Flag
  a PR only if it actually adds one.

## Evidence expectations

- Codegen: a focused `tools/test_*.py` regression and proof that default output is
  unchanged when the feature is gated.
- Runtime: the applicable source-owned ladder from `production-smoke`,
  `production-smoke-gap`, `cosim-selftest`, `cosim-mutants`, `sched-selftest`,
  `dispatch-selftest`, `hle-thread-selftest`, `selftest`, and `public-safe-verify`.
  Wire new selftests into a hosted target such as `native-core-tests` and keep Linux
  linkage valid.
- Interpreter: `cosim-mutants` still kills every mutant. A changed pattern count means
  update the mutant, not weaken the gate.
- Docs: `python -m unittest tools.test_lint_docs`, Markdown lint, and resolved links,
  paths, flags, and Make targets.
- Report skipped or blocked gates as such. Never imply an unrun gate passed.

## Findings to pre-empt

Authors, human or agent, check these before requesting review; reviewers flag them.

- **Repository terms.** Use the words the tree already defines: the *native player*; `NOT_RUN`, `BLOCKED` and `SKIP`
  for unproven results; *no title-specific branch* and *title-specific HLE override*. Name exact commands and files
  (`python tools/import_audit_gate.py`, not "the import audit").
- **Tracking.** Every new claim, roadmap item or milestone links its tracking issue.
- **Boundaries.** Widening an allow-list or match pattern adds refused cases that pin its edge.
- **Indexes.** A new module under `tools/` gets its row in `tools/README.md`; only
  `python -m unittest tools.test_lint_docs` checks it, not `tools/lint_docs.py` or `tools/tree_lint.py`.
- **Linux native builds.** Native code and selftests build and link on hosted Linux with and without SDL3: no
  Windows-only library, and `-lm` wherever `<math.h>` is used (`native-core-tests`, `portable-core-objects`).
- **Native test order.** A binary added to `native-core-tests` joins `EXPECTED_RUNS` in
  `tools/test_native_core_build.py` at the position the target runs it.
- **Markdown tables.** A blank line before and after, and one column style in every row, such as `| a | b |` over
  `| --- | --- |` (markdownlint MD058 and MD060; `npx --yes markdownlint-cli2@0.24.0 <file>`).
- **Local-path words.** `tools/publish_audit.py` reports `LOCAL_PATH` for user-profile and temporary-directory
  paths and for the cloud-folder product name anywhere in tracked text, docstrings and comments included. Describe
  them in words, or split the literal the way `publish_audit.py` does.
- **Citations.** Every cited path, line, flag, section and Make target exists: check with `git grep` before citing,
  and point at the section that actually states a rule.
- **CI claims.** State the platform and the routing condition; for example, hosted `python_tools` runs on Linux and
  only when a change routes Python.
- **New public paths.** The private admission and policy delta are on the private authority repository's `main`
  before the hosted attestation runs
  ([step by step](docs/PUBLICATION_READINESS.md#step-by-step-a-pull-request-that-adds-a-public-path)).

## Review style

Lead with the concrete failure scenario: inputs first, then the wrong result. Cite
`file:line`, use one finding per thread, and do not restate the PR or praise unchanged
code. Label PSP-hardware-dependent claims as measured, specified, or assumed.
