# Claude Code guidance — Nakagawa Recomp

@AGENTS.md

Claude Code reads `CLAUDE.md` in place of `AGENTS.md` when both exist, so the import line
above loads [`AGENTS.md`](AGENTS.md), the single policy authority, into its context. This
file adds no rules. Start at [Where to start](AGENTS.md#where-to-start): it covers building,
testing, the hard limits and what a good pull request looks like. The numbered sections are
the full contract, and the "Maintainer process" section applies only to the maintainer's
own agents.

Use [`docs/README.md`](docs/README.md) to choose maintained technical guidance and
[`REVIEW.md`](REVIEW.md) for review-specific checks.
