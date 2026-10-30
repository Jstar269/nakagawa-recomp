# Contribution Rights and Sign-Off Policy (DCO 1.1)

This document defines how contributions to Nakagawa Recomp (`https://github.com/Jstar269/nakagawa-recomp`) record the contributor's right to submit them. A sign-off under the standard **Developer Certificate of Origin (DCO) 1.1** is welcome but optional (maintainer decision, 2026-10-30).

> [!IMPORTANT]
> **How a contribution is licensed**: the project is licensed under GPL-3.0-or-later. Under GitHub's Terms of Service ("inbound = outbound", section D.6), a contribution you submit to this repository is provided under that same licence. In the pull request template, you confirm that you wrote the change or have the right to submit it under that licence. You retain copyright to your original work; neither the confirmation nor a sign-off is a copyright assignment.
>
> **Residual Provenance Boundary**: Contributions target the active sanitized
> public repository, but component-level provenance remains unresolved for the
> excluded PGF/font and PGD/amctrl surfaces. Engineering publication controls and
> DCO attestations are not legal clearance; see
> [`PUBLIC_SOURCE_PROFILE.md`](PUBLIC_SOURCE_PROFILE.md) and
> [`NOTICE.md`](../NOTICE.md).

---

## 1. Four-Layer Provenance & Governance Framework

To maintain complete legal and technical transparency, contributions to Nakagawa Recomp distinguish four distinct layers:

1. **Right to submit (required)**:
   Every pull request confirms, in the template, that its author wrote the change or has the right to submit it under GPL-3.0-or-later. A DCO 1.1 sign-off (`Signed-off-by:`) is optional and adds a written, per-commit record of the same certification.
2. **Third-Party Source & Asset Disclosure (required)**:
   Mandatory disclosure in the pull request and code comments of any third-party source code, libraries, or asset data incorporated into the PR, including exact upstream URLs, commit SHAs, and original license terms.
3. **AI-Assisted Work Disclosure (required)**:
   Mandatory disclosure of any AI-assisted code generation, translation, or reimplementation, adhering strictly to [`docs/AI_USAGE.md`](AI_USAGE.md).
4. **Repository-Wide Legal Clearance**:
   Engineering evidence and DCO sign-offs do not constitute legal advice or repository-wide legal clearance. Open legal/license blockers remain managed through formal review packets.

---

## 2. Developer Certificate of Origin 1.1 Full Text

```text
Developer Certificate of Origin
Version 1.1

Copyright (C) 2004, 2006 The Linux Foundation and its contributors.

Everyone is permitted to copy and distribute verbatim copies of this
license document, but changing it is not allowed.


Developer's Certificate of Origin 1.1

By making a contribution to this project, I certify that:

(a) The contribution was created in whole or in part by me and I
    have the right to submit it under the open source license
    indicated in the file; or

(b) The contribution is based upon previous work that, to the best
    of my knowledge, is covered under an appropriate open source
    license and I have the right under that license to submit that
    work with modifications, whether created in whole or in part
    by me, under the same open source license (unless I am
    permitted to submit under a different license), as indicated
    in the file; or

(c) The contribution was provided directly to me by some other
    person who certified (a), (b) or (c) and I have not modified
    it.

(d) I understand and agree that this project and the contribution
    are public and that a record of the contribution (including all
    personal information I submit with it, including my sign-off) is
    maintained indefinitely and may be redistributed consistent with
    this project or the open source license(s) involved.
```

---

## 3. How to Sign Off Commits (Optional)

If you choose to certify your contribution under DCO 1.1, append a `Signed-off-by:` line with your real name and email address to every commit in the pull request:

```text
Signed-off-by: Real Name <email@example.com>
```

You can automatically add this line when committing using Git's `-s` / `--signoff` flag:

```bash
git commit -s -m "feat(component): add new functionality"
```

Only the person who wrote a commit may sign it off.

---

## 4. Pull Requests Without a Sign-Off

A missing `Signed-off-by:` line is **not** a merge blocker and not a defect. The maintainer may merge a pull request without one, on the strength of the right-to-submit confirmation in the template and the repository licence. Third-party and AI-assistance disclosure are still required.

The process is meant to be forgiving. An unticked box, a missing sign-off or an incomplete disclosure is never by itself a reason to close or reject a pull request: the maintainer asks about it in review, or fills in what is known. The only point that cannot be waived is the license itself: code taken from another project must name its source and be under a GPL-compatible license, because GPL-3.0-or-later requires it.

- Nobody, person or agent, adds a sign-off on someone else's behalf.
- History is not rewritten to add sign-offs to commits already merged.
- If you want to add sign-offs to your own open pull request, amend your own commits:

### Single Commit Branch

```bash
git commit --amend -s
git push --force-with-lease
```

### Multiple Commit Branch

```bash
git rebase --exec "git commit --amend --no-edit -s" @~N  # where N is the number of commits
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
- **Outside contributors**: since sign-off is optional for everyone (§4), this waiver no longer separates the maintainer from other contributors. Outside contributors still confirm their right to submit in the pull request template and still make the third-party source and AI-assisted work disclosure (§1).

**Revocation** is a deliberate maintainer act: strike this section (or mark it revoked with a date) in a normal commit. Commits made while the waiver was in force remain covered; the waiver is not retroactively undone.

---

## 6. Maintenance & Enforcement

- The pull request template (`.github/PULL_REQUEST_TEMPLATE.md`) asks every contributor to confirm their right to submit, to disclose third-party material and AI assistance, and offers an optional DCO sign-off box.
- **DCO is not a required status check on the public repository.** A sign-off is optional (§4), so a required check would block contributions this policy accepts.
- An automated DCO check **may** run in advisory (non-blocking) mode. Its `action_required` or failing result on a commit without a sign-off carries no merge significance; it is not evidence of a rights problem and must not be "fixed" by rewriting history or adding a trailer on anyone's behalf.
- Making sign-off required again is a maintainer policy change to this document, not a repository setting alone.
