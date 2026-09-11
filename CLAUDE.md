# Claude Code guidance — Nakagawa Recomp

[`AGENTS.md`](AGENTS.md) is the canonical operating contract. This file is a Claude-oriented
router into it and the maintained documents, and must not become a second policy authority.

- **Every session, before anything else:** read `AGENTS.md` sections 3 and 5. They carry the
  human-only hard stops and the private/public boundary, and those bind read-only work too —
  reading or disclosing a private input, route, save, capture, trace, key or path is not a
  mutation, so nothing else here prevents it.
- **Read-only review, diagnosis, or a question:** that plus this file is enough. Do not create a
  branch, worktree, commit, PR, or any external mutation; report the boundary and stop.
- **Any mutation:** also read sections 2 autonomy, 8 worktree lifecycle, 9 gate routing, 10 PR
  authorization. Then fetch `origin`, record the exact base SHA, inspect status, and check live
  source, GitHub Issues, and overlapping open PRs.

## What actually builds in this tree

This is the sanitized public source tree. Private backends and the retail title manifest are
excluded by `assets/public_source_profile.json`, so the Makefile auto-selects `PUBLIC_SAFE=1`,
links the `*_unavailable.c` stubs, and makes the retail title route refuse rather than quietly
disabling its bindings. Confirm with `mingw32-make --no-print-directory compiler-info` rather
than assuming.

The source-owned, game-input-free ladder is the real inner loop here, not a fallback:

```bash
mingw32-make production-smoke production-smoke-gap cosim-selftest platform-ladder
mingw32-make selftest sched-selftest hle-thread-selftest public-safe-verify
python -m unittest discover -s tools -p "test_*.py"   # ~2000 tests, slow
```

That ladder is Windows-only, and hosted CI also compiles the POSIX host backend on Linux — so a
change can pass every gate above and still fail there. **If you touch a `*_posix.c` / `*_win32.c`
pair, or anything else behind a `!_WIN32` guard, run the Linux leg before you push.** WSL is
installed on this host and reproduces it exactly; the command and the feature-test-macro trap that
makes this fail silently on Windows are in
[`docs/PLATFORM_PORTABILITY.md`](docs/PLATFORM_PORTABILITY.md).

## Maintained contracts — current facts live here, not in this file

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — runtime, two-phase build, execution tiers.
- [`docs/SETUP.md`](docs/SETUP.md) — supported host/toolchain contract.
- [`docs/CI.md`](docs/CI.md) — local/hosted gate routing and evidence limits.
- [`docs/PUBLICATION_READINESS.md`](docs/PUBLICATION_READINESS.md) — public boundary, provenance.
- [`docs/README.md`](docs/README.md) — index of every other document, with status taxonomy.

## Non-negotiables

On Windows use PowerShell 7.6+ (`pwsh`) for project manager commands. Treat missing external
inputs and unavailable hardware as explicit `BLOCKED` or `NOT_RUN` evidence, never a silent pass;
a local pass is never hosted-CI or hardware evidence. Never invent DCO identity or
`Signed-off-by:` text, expose private material, hand-edit generated translation units, or create
tag or release artifacts. `src/rt/recomp.h` is a load-bearing ABI: MIPS `$ra` is `r[31]`, with no
separate `lr`. Keep PSP semantics host-neutral. Preserve unrelated user state.

Run only the gates proportional to the changed surface, then report exact commands and statuses.
