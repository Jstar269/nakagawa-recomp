# Claude Code guidance — Nakagawa Recomp

Read [`AGENTS.md`](AGENTS.md) first. It is the canonical operating contract; this file is only a
short Claude-oriented pointer and must not become a second policy authority.

Before a mutation, fetch `origin`, record the exact base SHA, inspect status, and check live source,
GitHub Issues, and overlapping open PRs. Read-only reviews do not create branches or worktrees.

Use these maintained contracts for current facts:

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — runtime and build structure.
- [`docs/SETUP.md`](docs/SETUP.md) — supported host/toolchain contract.
- [`docs/CI.md`](docs/CI.md) — local/hosted gate routing and evidence limits.
- [`docs/PUBLICATION_READINESS.md`](docs/PUBLICATION_READINESS.md) — public boundary, the two
  provenance authority tiers, and the local readiness sequence to run before opening a PR.

On Windows, use PowerShell 7.6+ (`pwsh`) for project manager commands. Treat missing external
inputs and unavailable hardware as explicit blocked/not-run evidence. Never invent DCO identity or
`Signed-off-by:` text, expose private material, hand-edit generated output, or create tag/release
artifacts. Follow the documented ordinary worktree lifecycle and preserve unrelated user state.

Run only the gates proportional to the changed surface, then report exact commands and statuses.
