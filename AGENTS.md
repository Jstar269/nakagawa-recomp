# AGENTS.md

## Scope

Repository-level guidance for human and AI-assisted work on Nakagawa Recomp. This file governs the
public source tree unless a more specific instruction file applies. Never publish or commit private
game inputs, generated retail-derived output, private oracle traces, keys, saves, local paths, or
private Git objects.

## Sources of truth

Use these domain-specific authorities when project surfaces disagree:

- **Implementation behavior:** Source code, tests, and Makefile.
- **Actionable defects & acceptance criteria:** Public GitHub Issues (`Jstar269/nakagawa-recomp`) where a curated public issue exists.
- **Project manual & research sitemap:** GitHub Wiki (`https://github.com/Jstar269/nakagawa-recomp/wiki`).
- **Concise status dashboard:** [`ISSUES.md`](ISSUES.md), including reference-owned areas that have not yet been curated into public issues.
- **Project identity, scope, & navigation:** [`README.md`](README.md).
- **Toolchain setup & build contract:** [`docs/SETUP.md`](docs/SETUP.md).
- **Documentation role hierarchy:** [`docs/README.md`](docs/README.md).
- **Dated historical evidence & resolved milestones:** [`docs/STATUS_HISTORY.md`](docs/STATUS_HISTORY.md).
- **Machine/operator handoff context:** [`docs/NEXT_SESSION.md`](docs/NEXT_SESSION.md) (local context only).

Do not rely on `opencode.json` or an auto-loaded Markdown tracker; `opencode.json` is local/ignored
state and the former comment-triggered OpenCode workflow is intentionally disabled pending any
explicit re-enable decision.

## Public/private repository boundary

The public repository and the private historical repository have intentionally unrelated histories.
Never merge them, never use `--allow-unrelated-histories`, never mirror private refs into public,
never force-push public history to imitate the private graph, and never expose private Git objects.

Synchronize only content that is independently reviewed for the target side. Generic public fixes may
be reconstructed/backported into the private archive by content. Private-to-public movement requires a
public-safe export/review and must not copy private commits, refs, issue comments, traces, local paths,
or restricted evidence.

See [`docs/DUAL_REPO_SYNC.md`](docs/DUAL_REPO_SYNC.md) and use the drift checker when a change affects
shared generic files.

## Working style

- Start from the exact current public `main` for public work; refresh before relying on status.
- Prefer focused branches/worktrees and reviewable PRs.
- Preserve unrelated dirty work. Do not destructively reset another campaign's checkout.
- Inspect before editing. Do not bulk-rewrite source or documentation because a pattern looks stale.
- Correctness beats apparent progress. Unknown/unsupported behavior should fail visibly or remain
  explicitly unknown rather than fabricate success.
- Historical evidence remains historical. A newer implementation does not justify rewriting a dated
  measurement, audit baseline, or superseded hypothesis.

## Build and verification discipline

Use the manager/Makefile contracts documented in [`docs/SETUP.md`](docs/SETUP.md). Do not invent build
commands or treat a successful compile as semantic acceptance.

For a broad or integration candidate, prefer `.\hst_manager.ps1 -Action Verify` as the canonical
non-interactive local aggregate gate (Python unit suite, sched/profiler/heap/asset-index/HLE-thread
selftests, the VFPU table-loader selftest (`vfpu-tables-selftest`, [`assets/vfpu/`](assets/vfpu/)), the watchpoints-file
parser selftest (`watchpoints-file-selftest`, [`docs/DEBUGGING.md`](docs/DEBUGGING.md)), `vfpu-interp-selftest`, `src/ref` selftest,
`import_audit_gate.py`, `publish_audit.py` over both content sources
(`--tracked-only` and `--tracked-only --worktree`), `gpu-coherence-selftest` and
`gpu-capture-selftest`; exit 77 = Vulkan/validation layer unavailable → SKIP), then add the
focused subsystem tests required by the change.

Do not describe local checks as CI-green. For hosted status, inspect the exact PR/head check results.
The stable required aggregate is `CI required` when hosted CI applies.

## Evidence discipline

Keep evidence classes distinct:

- source/tests establish implementation behavior;
- public Issues establish curated actionable tracking and acceptance criteria;
- dated logs/status history establish what was measured on a particular revision/environment;
- PPSSPP or another emulator can be a differential/reference implementation but is not automatically a
  PSP-hardware oracle;
- hardware claims require the model/firmware/probe/evidence scope needed by the relevant research method;
- green tests, clean provenance records, and publication audits are engineering evidence, not legal
  clearance.

For generalized PSP research and hardware findings, use the evidence model at
[`https://recomp.jaycast.net/`](https://recomp.jaycast.net/) rather than duplicating scholarly claims in
repository docs.

## Publication safety

Never commit or publish:

- commercial ISO/CSO/PBP/EBOOT/retail ELF;
- decrypted retail PRXs;
- generated retail-title C or flat image blobs;
- proprietary textures/audio/video/fonts/assets;
- private disassembly/decompilation, Ghidra/decomp.me material, or private saves/routes;
- raw private traces, framebuffer/memory captures, or oracle material;
- keys/secrets;
- private/local filesystem paths or unique device identifiers;
- private Git history/objects/refs.

Unknown publication status means do not publish. Use fail-closed public profiles/stubs where the
repository policy requires them.

The PGF PPSSPP/JPCSP licensing chain is documented in [docs/PGF_LICENSE_REVIEW_PACKET.md](docs/PGF_LICENSE_REVIEW_PACKET.md), replacement-font licensing in [THIRD_PARTY_LICENSES/PPSSPP_FONTS.txt](THIRD_PARTY_LICENSES/PPSSPP_FONTS.txt), the full-history secret/privacy audit in [docs/KEY_HISTORY_SCRUB.md](docs/KEY_HISTORY_SCRUB.md), and qualified PGD/amctrl distribution review in [docs/PGD_AMCTRL_REVIEW_PACKET.md](docs/PGD_AMCTRL_REVIEW_PACKET.md). Do not
"fix" any of these by changing SPDX text or generic NOTICE wording without resolving the underlying
provenance evidence. Engineering review packets are evidence for qualified review, not legal
clearance. The repository-level declaration is GPL-3.0-or-later ([LICENSE](LICENSE), `NOTICE.md`)
while many source files retain GPL-2.0-or-later — both are deliberate; preserve per-file SPDX.

**AI tools and automated agents must never invent or add a `Signed-off-by:` identity on behalf of a
human, company, or the tool itself.** DCO sign-off is a contributor rights certification. An agent may
preserve a real sign-off supplied by the contributor but may not fabricate one.

## Hardware safety

Hardware research must stay source-owned/synthetic and non-destructive. Do not perform flash/NAND/IPL,
IDStorage, battery EEPROM/service, arbitrary kernel/MMIO writes, or intentional crash/bricking work.
Use bounded RAM-only probes and the established hardware-research/transport qualification method.

## Generated files and source ownership

- Never hand-edit generated files under `build/<game>/`.
- Preserve generator-owned files and manifests; change the generator/source and regenerate.
- Do not treat generated retail output as a public artifact.
- For new or materially derived code/data, disclose material translation/reimplementation lineage,
  including AI-assisted translation, and keep third-party/generated data distinguishable from
  independently authored code.

## Documentation freshness guardrails

- **Evergreen docs stay current:** `README.md`, `docs/ARCHITECTURE.md`, `docs/SETUP.md`, and top-level guides must remain evergreen. Do not embed ephemeral dates ("as of July 25"), exact CI run IDs, or temporary blocker lists in evergreen pages.
- **Volatile status belongs in the status/tracker layer:** Actionable curated defects belong in public GitHub Issues; [`ISSUES.md`](ISSUES.md) provides the concise current map, including areas still owned by reference documents during issue curation.
- **Historical evidence retains dates:** Dated experiment logs, audit snapshots, hardware oracle runs, and [`docs/STATUS_HISTORY.md`](docs/STATUS_HISTORY.md) must retain exact dates and scope. Never retroactively alter past evidence dates.
- **Run documentation linting:** Run `python tools/lint_docs.py` before submitting documentation PRs. With network access, also run `python tools/audit_public_issue_links.py --strict` before review/merge.

## Security and repository settings

- Security tooling in the tree does not prove live repository settings. Verify rulesets, branch
  protection, MFA, visibility, secret scanning, private vulnerability reporting, and Actions status
  against the live public repository when those claims matter.
- Secret/publication checks do not replace a full Git-history scan of any history/ref set intended for
  release.
- Remote GitHub rulesets, security settings, MFA, and visibility controls require explicit owner/
  repository-settings verification; never infer them from source-tree configuration.
