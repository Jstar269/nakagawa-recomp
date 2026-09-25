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

- Regenerated `PUBLIC_EXPORT.json` and `assets/public_provenance_ledger.json` in a
  focused provenance-refresh commit. Flag them if the commit changes anything else, or
  if the "Trusted provenance attestation" check failed or did not run on the head (it
  is currently observation-only, not required).
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

## Review style

Lead with the concrete failure scenario: inputs first, then the wrong result. Cite
`file:line`, use one finding per thread, and do not restate the PR or praise unchanged
code. Label PSP-hardware-dependent claims as measured, specified, or assumed.
