# Claude Code guidance — Nakagawa Recomp

[`AGENTS.md`](AGENTS.md) is the canonical operating contract. This file is a Claude-oriented
router into it and the maintained documents, and must not become a second policy authority.

- **Every session, first:** read `AGENTS.md` sections 3 and 5 — the human-only hard stops and the
  private/public boundary. They bind read-only work too: disclosing a private input, route, save,
  capture, trace, key or path is not a mutation, so nothing else here prevents it.
- **Read-only review, diagnosis, or a question:** that plus this file is enough. Do not create a
  branch, worktree, commit, PR, or any external mutation; report the boundary and stop.
- **Any mutation:** also read sections 2 autonomy, 8 worktree lifecycle, 9 gate routing, 10 PR
  authorization. Then fetch `origin`, record the exact base SHA, inspect status, and check live
  source, GitHub Issues, and overlapping open PRs.

## What actually builds in this tree

This is the sanitized public source tree. Private backends and the retail title manifest are
excluded by `assets/public_source_profile.json`, so the Makefile auto-selects `PUBLIC_SAFE=1`,
links the `*_unavailable.c` stubs, and makes the retail title route refuse rather than quietly
disabling its bindings. Confirm with `mingw32-make --no-print-directory compiler-info`, don't assume.

The source-owned, game-input-free ladder is the real inner loop here, not a fallback:

```bash
mingw32-make production-smoke production-smoke-gap cosim-selftest platform-ladder
mingw32-make selftest sched-selftest hle-thread-selftest public-safe-verify
python -m unittest discover -s tools -p "test_*.py"   # ~2000 tests, slow
```

## Maintained contracts — current facts live here, not in this file

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — runtime, two-phase build, execution tiers.
- [`docs/SETUP.md`](docs/SETUP.md) — supported host/toolchain contract.
- [`docs/PLATFORM_PORTABILITY.md`](docs/PLATFORM_PORTABILITY.md) — host backends. That ladder is
  Windows-only; CI also compiles POSIX on Linux, so run that leg before pushing a `*_posix.c`.
- [`docs/CI.md`](docs/CI.md) — local/hosted gate routing and evidence limits.
- [`docs/PUBLICATION_READINESS.md`](docs/PUBLICATION_READINESS.md) — public boundary, provenance.
- [`docs/README.md`](docs/README.md) — index of every other document, with status taxonomy.

## Non-negotiables

On Windows use PowerShell 7.6+ (`pwsh`) for project manager commands. Never invent DCO identity or `Signed-off-by:` text,
expose private material, hand-edit generated translation units, or create tag or release artifacts. `src/rt/recomp.h` is a
load-bearing ABI: MIPS `$ra` is `r[31]`, with no separate `lr`. Keep PSP semantics host-neutral. Preserve unrelated user state.

## Done means

The gates proportional to the changed surface have actually run **on every host the change touches** — a Windows-green edit
to a POSIX backend is not done — and you reported the exact commands and statuses, including what you did not run and why.
Missing input or hardware is `BLOCKED`/`NOT_RUN`, never a silent pass; a local pass is never hosted-CI evidence.
