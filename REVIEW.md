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
- **Generic implementation:** no title-specific branches, title-keyed overrides or private guest
  addresses in `src/rt/` or generic `tools/` code; a game's name or disc ID in a comment, test or
  document is fine. Evidence-backed title behavior belongs in a manifest/config with
  a retirement criterion.
- **Evidence honesty:** preserve existing derived-code records. Code from an emulator or other
  upstream project is acceptable only under the `AGENTS.md` section 4 import rules (GPL-compatible
  licence, exact revision pinned, `PROVENANCE.md` and `CREDITS.md` entries, notices kept, labelled
  `derived-translated`); reject code that breaks them, including undisclosed copying or
  line-by-line translation. PSP values need a measured cell, public specification, or an
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

- Regenerated `PUBLIC_EXPORT.json` and `assets/public_provenance_ledger.json` that add or
  remove exactly the entries for paths the same commit adds or removes, or a focused
  provenance-refresh commit after a merge from `main`. The ledger carries claims only
  (#1673), so a commit that only edits existing files leaves it unchanged. Flag a ledger
  hunk in such a commit, entries for paths the commit does not add, remove or
  reclassify, a refresh commit that changes anything else, or a head whose "Trusted
  provenance attestation" check failed or did not run (it is currently
  observation-only, not required).
  Before flagging a ledger entry, attribute the hunk correctly. A unified diff's trailing
  context lines show the *next* entry's `path`, while the changed lines belong to the
  entry above the hunk. Compare that entry's path with the files the commit changes, not
  the path that follows it. A branch made before #1673 may still show the older ledger
  with a `sha256` per entry; one conversion to the claims form after merging `main` is
  expected.
- New `assets/public_source_profile.json` entries for files the same PR adds, provided
  the attestation check passed on the head. Otherwise flag them as unauthorized public
  paths.
- Missing `Signed-off-by:` on any commit. A DCO sign-off is optional for every contributor
  (`docs/DCO_POLICY.md` section 4); an unticked template box is a question for the author, not a finding.
- Merge commits from `main` into a PR branch; shared branches are not rebased or force-pushed.
- Fail-closed public stubs such as `iso_unavailable.c` and `pgd_unavailable.c`. Do not infer behavior from a filename: `audio_sdl3.c` is the active public-safe SDL3
  host-audio backend, and `audio_unavailable.c` is a byte-identical copy the build doesn't compile.
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
- Docs: the docs row of `AGENTS.md` section 9, and resolved links, paths, flags, and Make
  targets.
- Report skipped or blocked gates as such. Never imply an unrun gate passed.

## Findings to pre-empt

Authors, human or agent, check these before requesting review; reviewers flag them.

- **Repository terms.** Use the words the tree already defines: the *native player*; `NOT_RUN`, `BLOCKED` and `SKIP`
  for unproven results; *no title-specific branch* and *title-specific HLE override*. Name exact commands and files
  (`python tools/import_audit_gate.py`, not "the import audit").
- **Tracking.** Every new claim, roadmap item or milestone links its tracking issue.
- **Boundaries.** Widening an allow-list or match pattern adds refused cases that pin its edge.
- **Indexes.** A new module under `tools/` needs an index entry: a module docstring whose first
  paragraph says what it does, or a row in `tools/README.md` (#1673; no row is needed when the docstring
  exists). Only `python -m unittest tools.test_lint_docs` checks it, not `tools/lint_docs.py` or
  `tools/tree_lint.py`.
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

### Professional finish

Every change, from a person or an agent, reads like careful, deliberate work. Completeness still comes before
brevity, but every line has to carry information. Reviewers flag:

- **Code idiom.** New code that doesn't match the surrounding naming, structure and comment density, or comments that
  restate what the code does instead of saying why.
- **Prose.** Filler, hype, hedging or marketing tone in code comments, docs, commit messages or PR text, and emoji
  anywhere in code, docs or UI strings.
- **Leftovers.** Dead code, duplicate helpers, debug output, commented-out blocks, and stray or one-off files (scratch
  scripts, notes, logs).
- **Terms.** A new word for something the docs already name; use the existing term (see Repository terms above).
- **UI text.** Labels and messages that aren't short and plain, or that say more than the user needs to act.
- **Pull request text.** Anything beyond what changed, the evidence for it, and what is not done or not verified.
  No repeated summaries or decorative formatting.

## Review style

Lead with the concrete failure scenario: inputs first, then the wrong result. Cite
`file:line`, use one finding per thread, and do not restate the PR or praise unchanged
code. Label PSP-hardware-dependent claims as measured, specified, or assumed.
