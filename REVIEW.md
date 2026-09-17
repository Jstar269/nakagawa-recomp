# REVIEW.md

Review guidance for Nakagawa Recomp, an experimental PSP static recompiler
(generated C plus a native C runtime, an interpreter floor, and HLE, moving
toward low-level emulation). `AGENTS.md` is the operating contract; this file
only says what a reviewer should look for.

## What matters in this repository

- **Private/public boundary (highest priority).** Nothing may add game
  binaries or assets, firmware, decrypted modules, keys, saves, captures,
  traces, retail addresses from private evidence, or absolute local machine
  paths (user home or workspace directories). This includes docs, test
  fixtures, logs, and CI output.
- **`src/rt/recomp.h` `CpuState` is a load-bearing ABI.** Any layout change
  must bump `SR_CPUSTATE_ABI_VERSION` and update `tools/mem_debug.py`. MIPS
  `$ra` is `r[31]`; there is no separate link register.
- **Default builds must not change behaviour** when a change is gated behind an
  opt-in flag (for example `--lle-cpu`, `--lle-import-seam`): generated C
  without the flag must stay byte-identical, and the default lane must stay
  fail-closed.
- **Fail closed, never guess.** Invalid guest pointers, unknown opcodes,
  unowned jump targets, missing inputs, and unavailable backends must be
  reported, not silently defaulted or synthesized.
- **No title-specific logic in generic code.** Title behaviour belongs in
  manifests or title config with evidence and a retirement criterion, not in
  hard-coded addresses or game names in `src/rt/` or `tools/`.
- **No invented hardware facts.** PSP-specific values must cite a measured
  hardware cell, a public spec, or be labelled unmeasured and kept behind a
  disabled-by-default path. MIPS32 architectural behaviour should cite the
  MIPS32 specification.
- **Provenance honesty.** New code must never be copied or line-by-line
  translated from PPSSPP, sal063, or other emulators; flag any new path that
  looks copied, whatever its record says. Existing derived code must keep its
  derived record. Flag docs that call project code "clean-room", or that turn
  engineering controls into legal conclusions.
- **Host neutrality.** A change to a `*_posix.c` backend or a shared runtime
  file must build on Linux as well as Windows (MSYS2 UCRT64 GCC).

## Severity calibration

- **Critical:** private material or local paths entering the tree; a key or
  secret; a guest-to-host memory-safety bug (unchecked guest pointer or length,
  out-of-bounds host write); a silent `CpuState` ABI change; a default-lane
  behaviour change hidden behind an "opt-in" claim.
- **Warning:** missing span or pointer validation in an HLE handler; a flag
  that is set in one execution tier (AOT or interpreter) but not honoured by the
  other; state such as `next_pc` / `in_delay_slot` not restored on every exit
  path; an unmeasured PSP value presented as fact; a new test that cannot fail;
  a doc claim that contradicts the Makefile, `nk_manager.ps1`, or source.
- **Suggestion:** naming, comment accuracy, smaller refactors.

## Do not flag

- The mere presence of regenerated `PUBLIC_EXPORT.json` and
  `assets/public_provenance_ledger.json` changes in a
  "provenance: refresh public controls ..." commit. Do flag them if the
  "Trusted provenance attestation" check failed or did not run on the head
  (that check is currently observation-only, not required), or if the
  commit changes anything beyond those regenerated outputs.
- New entries in `assets/public_source_profile.json` for files the same PR
  adds, provided the attestation check passed on the head. Otherwise flag
  them as unauthorized public paths.
- Missing `Signed-off-by:` trailers on maintainer or maintainer-directed agent
  commits, which the standing DCO waiver covers (`docs/DCO_POLICY.md`
  section 5.1). Commits from anyone else still need a DCO 1.1 sign-off; flag
  those when it is missing.
- Merge commits from `main` into a PR branch. The project never rebases or
  force-pushes shared branches.
- `*_unavailable.c` stubs that always fail. The public tree deliberately
  excludes some backends (PGF fonts, PGD, ISO, audio) and links fail-closed
  stubs instead.
- Formatting-only differences that `.clang-format`, `ruff`, or markdownlint
  already enforce.
- Edits to generated `build/<game>/*_recomp_*.c`. These files are never
  committed; flag it only if a PR actually adds one.

## Verification expectations

- Codegen changes: a focused `tools/test_*.py` test, plus evidence that the
  default output is unchanged when the feature is gated.
- Runtime changes: the source-owned ladder that needs no game input
  (`production-smoke`, `production-smoke-gap`, `cosim-selftest`,
  `cosim-mutants`, `sched-selftest`, `dispatch-selftest`,
  `hle-thread-selftest`, `selftest`, `public-safe-verify`). A new selftest must
  be wired into a target that hosted CI runs (for example
  `native-core-tests`) and must link on Linux.
- Interpreter changes: `cosim-mutants` must still kill every mutant. A mutant
  pattern count that no longer matches is a signal to update the mutant, not
  to weaken it.
- Docs changes: `python -m unittest tools.test_lint_docs` and markdownlint.
  Links and Makefile target names must resolve.
- A PR that says "not run" or "blocked" for a gate is acceptable. A PR that
  implies a gate passed without running it is not.

## Review style

Lead with the concrete failure scenario (inputs, then the wrong result).
Cite `file:line`. One finding per thread. Do not restate the PR description or
praise unchanged code. If a finding depends on PSP hardware behaviour, say
whether it is measured, specified, or assumed.
