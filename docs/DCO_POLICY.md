# Contributor Rights-Attestation Policy (DCO 1.1)

This document defines the contributor rights-attestation framework for the Nakagawa Recomp project (`https://github.com/Jstar269/nakagawa-recomp`), based on the standard **Developer Certificate of Origin (DCO) 1.1**.

> [!IMPORTANT]
> **DCO is Not Copyright Assignment**: A DCO sign-off certifies that you have the right to submit your contribution under the project's open-source terms. You retain copyright to your original work.
>
> **Residual Publication Blocker**: The exact final open-source presentation for outside contributions remains gated on component-level provenance. The public candidate excludes the unresolved PGF/font and PGD/amctrl surfaces; see [`PUBLIC_SOURCE_PROFILE.md`](PUBLIC_SOURCE_PROFILE.md) and [`NOTICE.md`](../NOTICE.md).

---

## 1. Four-Layer Provenance & Governance Framework

To maintain complete legal and technical transparency, contributions to Nakagawa Recomp distinguish four distinct layers:

1. **DCO 1.1 Rights Attestation (`Signed-off-by:`)**:
   Certification by the contributor that they authored the contribution or have sufficient rights to submit it under the project's applicable terms.
2. **Third-Party Source & Asset Disclosure**:
   Mandatory disclosure in the pull request and code comments of any third-party source code, libraries, or asset data incorporated into the PR, including exact upstream URLs, commit SHAs, and original license terms.
3. **AI-Assisted Work Disclosure**:
   Mandatory disclosure of any AI-assisted code generation, translation, or reimplementation, adhering strictly to [`docs/AI_USAGE.md`](AI_USAGE.md).
4. **Repository-Wide Legal Clearance**:
   Engineering evidence and DCO sign-offs do not constitute legal advice or repository-wide legal clearance. Open legal/license blockers remain managed through formal review packets.

---

## 2. Developer Certificate of Origin 1.1 Full Text

```text
Developer Certificate of Origin
Version 1.1

Copyright (C) 2004, 2006 Open Source Development Labs, Inc.
Everyone is permitted to copy and distribute verbatim copies of this license document, but changing it is not allowed.

1.0 Developer's Certificate of Origin 1.1

By making a contribution to this project, I certify that:

(a) The contribution was created in whole or in part by me and I have the right to submit it under the open source license indicated in the file; or

(b) The contribution is based upon previous work that, to the best of my knowledge, is covered under an appropriate open source license and I have the right under that license to submit that work with modifications, whether created in whole or in part by me, under the same open source license (unless I am permitted to submit under a different license), as indicated in the file; or

(c) The contribution was provided directly to me by some other person who certified (a), (b) or (c) and I have not modified it.

(d) I understand and agree that this project and the contribution are public and that a record of the contribution (including all personal information I submit with it, including my sign-off) is maintained indefinitely and may be redistributed consistent with this project or the open source license(s) involved.
```

---

## 3. How to Sign Off Commits

To certify your contribution under DCO 1.1, append a `Signed-off-by:` line with your real name and email address to every commit message:

```text
Signed-off-by: Real Name <email@example.com>
```

You can automatically add this line when committing using Git's `-s` / `--signoff` flag:

```bash
git commit -s -m "feat(component): add new functionality"
```

---

## 4. Correction Workflow for Missing Sign-Offs

This section applies to contributions that require a sign-off — that is, everything **not** covered by the §5.1 maintainer standing waiver. Waived maintainer commits need no correction, and their history must not be rewritten to add trailers.

If you submitted a pull request with missing or incorrect DCO sign-off lines, follow these steps to correct your branch:

### Single Commit Branch

```bash
git commit --amend -s
git push --force-with-lease
```

### Multiple Commit Branch

```bash
git rebase -i --exec "git commit --amend --no-edit -s" @~N  # where N is the number of commits
git push --force-with-lease
```

---

## 5. Automated Bots & Dependency Updates Policy

- **Automated Bots (e.g., Dependabot, Renovate)**:
  Commits and pull requests generated automatically by trusted repository bots carry the bot's standard commit signature. They are reviewed by maintainers before merge and do not require a manual human DCO sign-off trailer.

### 5.1 Maintainer Standing Waiver

The maintainer operates under a **standing DCO waiver**. Its terms:

- **Scope**: commits authored by the maintainer, and commits prepared under the maintainer's direction by AI assistants or automated agents and merged by the maintainer. The maintainer is the sole rights-holder for these contributions and certifies them by merging.
- **Duration**: the waiver is in force **until the maintainer explicitly revokes it in this document**. It does **not** expire on public launch and is not time-limited, commit-range-limited, or repository-visibility-limited. It continues to apply after the public repository launches.
- **Effect**: a missing `Signed-off-by:` trailer on a waived commit is **not** a merge blocker and is not a defect to be "corrected". The absence of a trailer on such a commit records that the maintainer did not make a separate written attestation, not that rights are unresolved.
- **No retroactive rewrite**: sign-offs are never retroactively fabricated for existing Git history, and history is not rewritten to satisfy a sign-off checker while this waiver is in force.
- **No fabrication by agents**: this waiver grants an agent **no** authority to add a sign-off. AI tools and automated agents must never invent or add a `Signed-off-by:` identity on behalf of any human, company, or the tool itself, whether or not a waiver applies. See [`AGENTS.md`](../AGENTS.md) and [`docs/AI_USAGE.md`](AI_USAGE.md).
- **Not extended to outside contributors**: this waiver is personal to the maintainer. It confers nothing on third-party contributors, who remain fully subject to §1–§4 — DCO 1.1 sign-off on every commit, third-party source and asset disclosure, and AI-assisted work disclosure. Nothing in this section reduces those requirements.

**Revocation** is a deliberate maintainer act: strike this section (or mark it revoked with a date) in a normal commit. Commits made while the waiver was in force remain covered; the waiver is not retroactively undone. Only after explicit revocation may DCO become a required status check (§6).

---

## 6. Maintenance & Enforcement

- The pull request template (`.github/PULL_REQUEST_TEMPLATE.md`) includes mandatory checkboxes for DCO sign-off status, third-party source disclosure, and AI-assisted work disclosure.
- **DCO must not be configured as a required status check on the public repository while the §5.1 maintainer standing waiver is in force.** A required check would block the maintainer's own waived commits on a single-maintainer repository, which the waiver exists to prevent.
- An automated DCO check **may** run in advisory (non-blocking) mode. Its `action_required` or failing result on a waived maintainer commit is expected and carries no merge significance; it is not evidence of a rights problem and must not be "fixed" by rewriting history or adding a trailer on the maintainer's behalf.
- DCO may be promoted to a required status check only after the standing waiver is explicitly revoked under §5.1. Third-party contributions remain subject to DCO 1.1 by policy and review regardless of whether an automated check is enforcing it.
