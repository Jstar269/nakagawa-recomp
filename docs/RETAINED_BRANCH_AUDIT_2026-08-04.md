# Retained branch audit — 2026-08-04

This is a read-only audit against `origin/main` at
`f374b310c3e46f886b5f6c2ae9deda5b6f154468`. No branch was rebased, merged,
deleted, or published during the pre-PSP campaign.

| Remote branch | Unique head / scope | Disposition |
| --- | --- | --- |
| `origin/antigravity/issue197-pspdev-phase5` | `022afc2` marked **[SUPERSEDED]**; Phase-5 fixture/provenance and import/codegen exploration | Preserve for provenance review. Its source-owned fixture is not the current probe suite, and the commit is explicitly superseded; do not merge without a fresh exact-base review. |
| `origin/codex/issue196-face-trace` | `1550705`; bounded direct-XB lookup provenance trace in `src/rt/hle.c` | Preserve. It is a focused candidate for #139/#196, but needs private route evidence and exact source review before any PR. |
| `origin/docs/gpl3-license-consistency` | `b9d78ed`; broad GPL/publication documentation lineage | Preserve. It diverges across publication files and includes historical scratch-worktree hygiene; no automatic merge or legal conclusion is justified. |
| `origin/docs/pspdev-integration-plan` | `e80cde7`; 936-line PSPDEV planning document plus prior lock/doctor work | Preserve. The current source-owned probe/readiness slice is narrower and current-main based; this branch remains planning history, not a merge target. |

The branch list is intentionally separate from the current working-tree patch.
Before opening a PR, fetch the exact head again, compare the changed-file set,
run the focused tests, and keep private inputs and DCO attestations out of the
review packet.
