# Claude Code guidance — Nakagawa Recomp

Read [`AGENTS.md`](AGENTS.md), the single policy authority, before acting.
Start with [Where to start](AGENTS.md#where-to-start) to select the checkout and working directory.

- Every session: [preflight](AGENTS.md#1-sources-and-live-preflight),
  [hard stops](AGENTS.md#3-human-only-hard-stops), and
  [private/public boundaries](AGENTS.md#5-private-and-public-boundaries).
- Changes: [autonomy](AGENTS.md#2-operating-modes-and-autonomy),
  [provenance](AGENTS.md#4-provenance-and-publication),
  [correctness](AGENTS.md#6-correctness-and-evidence), and
  [worktree lifecycle](AGENTS.md#8-workspace-branch-and-worktree-lifecycle).
- Completion: [validation](AGENTS.md#9-validation-and-gate-routing),
  [PR authorization](AGENTS.md#10-pr-and-integration-authorization), and
  [reporting](AGENTS.md#11-reporting-and-cleanup).

Use [`docs/README.md`](docs/README.md) for maintained contracts, including
[`docs/SETUP.md`](docs/SETUP.md) for current build and manager commands and
[`docs/CI.md`](docs/CI.md) for local/hosted validation.
