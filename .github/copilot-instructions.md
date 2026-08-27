# Copilot Instructions for Nakagawa Recomp

Read [`AGENTS.md`](../AGENTS.md) before acting. It is the single repository policy authority;
this file is a concise derived summary and must not define a conflicting rule.

## Preflight and scope

- Use live source, tests, Makefiles, GitHub Issues, and maintained `docs/` contracts as authority.
- Before mutation, fetch `origin`, record the exact base SHA, inspect status, and check overlapping
  open PRs. A read-only review does not create a branch, worktree, commit, or PR.
- Preserve unrelated changes, private inputs, ignored local configuration, and other worktrees.
  Use at most one dedicated temporary worktree for a mutating mission.

## Safety

- Keep private title inputs, decrypted modules, generated retail output, saves, keys, captures,
  traces, local paths, and private evidence out of public history and AI context.
- Do not hand-edit generated translation units or mask unknown operations with fake success, sleeps,
  loop caps, forced returns, or address-only workarounds.
- Never invent provenance, licensing, DCO identity, or `Signed-off-by:` text. Candidate policy and
  ledger bytes do not self-attest. Stop with `PROVENANCE_UNRESOLVED` when a new implementation path
  lacks a genuine path-specific record.
- Tags, releases, release assets, and published versions require explicit current-turn maintainer
  authorization. Hardware and external-oracle claims require their own qualified evidence.

## Verification

Use [`docs/CI.md`](../docs/CI.md) for current gate routing and [`docs/SETUP.md`](../docs/SETUP.md)
for host requirements. Run proportional local gates, including the Python suite, focused native
selftests, documentation lint, policy/publication audits, and pre-commit when applicable. Local
passes are not hosted-CI or hardware passes; report `PASS`, `FAIL`, `SKIP`, `BLOCKED`, or `NOT_RUN`.

Generated files, provenance metadata, and public exports must be changed only through their
documented source/generator path. Preserve the ordinary worktree lifecycle and report exact SHAs,
changed files, tests, uncertainty, and retained cleanup state.
