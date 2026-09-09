# Claude Code guidance — Nakagawa Recomp

[`AGENTS.md`](AGENTS.md) is the canonical operating contract. This file is a Claude-oriented
router into it and the maintained documents, and must not become a second policy authority.

- **Read-only review, diagnosis, or a question:** this file is enough. Do not create a branch,
  worktree, commit, PR, or any external mutation; report the boundary and stop.
- **Any mutation:** read `AGENTS.md` first — section 2 autonomy, 3 human-only stops, 8 worktree
  lifecycle, 9 gate routing, 10 PR authorization. Then fetch `origin`, record the exact base SHA,
  inspect status, and check live source, GitHub Issues, and overlapping open PRs.

## What actually builds in this tree

This is the sanitized public source tree. Private backends and the retail title manifest are
excluded by `assets/public_source_profile.json`, so the Makefile auto-selects `PUBLIC_SAFE=1`,
links the `*_unavailable.c` stubs, and makes the retail title route refuse rather than silently
disable its bindings. Confirm the effective mode rather than assuming it:

```bash
mingw32-make --no-print-directory compiler-info
```

The source-owned, game-input-free ladder is the real inner loop here — not a fallback:

```bash
mingw32-make production-smoke production-smoke-gap cosim-selftest platform-ladder
mingw32-make selftest sched-selftest hle-thread-selftest public-safe-verify
python -m unittest discover -s tools -p "test_*.py"
```

## Maintained contracts — current facts live here, not in this file

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — runtime, two-phase build, execution tiers.
- [`docs/SETUP.md`](docs/SETUP.md) — supported host/toolchain contract.
- [`docs/CI.md`](docs/CI.md) — local/hosted gate routing and evidence limits.
- [`docs/PUBLICATION_READINESS.md`](docs/PUBLICATION_READINESS.md) — public boundary and provenance.
- [`docs/DEBUGGING.md`](docs/DEBUGGING.md) — diagnostics, environment switches, route replay.
- [`ISSUES.md`](ISSUES.md) — live work areas and the execution-contract state not to overstate.

## Non-negotiables

On Windows use PowerShell 7.6+ (`pwsh`) for project manager commands. Treat missing external
inputs and unavailable hardware as explicit `BLOCKED` or `NOT_RUN` evidence, never a silent pass;
a local pass is never hosted-CI or hardware evidence. Never invent DCO identity or
`Signed-off-by:` text, expose private material, hand-edit generated translation units, or create
tag or release artifacts. `src/rt/recomp.h` is a load-bearing ABI: MIPS `$ra` is `r[31]`, with no
separate `lr`. Keep PSP semantics host-neutral and put host behavior behind the narrow host
abstractions. Preserve unrelated user state and the documented worktree lifecycle.

Run only the gates proportional to the changed surface, then report exact commands and statuses.
