# OpenSSF OSPS Baseline Review (historical pre-republication snapshot)

This document preserves an implementation audit captured before the sanitized
public restoration. It is not current repository status, a certification, a
security guarantee, or a substitute for reviewing live GitHub/account settings.
`Jstar269/nakagawa-recomp` is now the active public source repository. The
private visibility, prospective-publication language, and tracker numbers below
are historical observations from the captured audit and must not be interpreted
as current public issue mappings.

## Historical repository metadata (captured 2026-08-06)

Empirically verified via `gh repo view` and `gh api`:

- **Repository URL**: `https://github.com/Jstar269/nakagawa-recomp`
- **Visibility**: `PRIVATE` (`isPrivate: true`)
- **Default Branch**: `main`
- **Features**: `hasIssuesEnabled: true`, `hasProjectsEnabled: true`, `hasWikiEnabled: false`
- **Automated Security Fixes**: `{"enabled": true, "paused": false}`
- **Active Workflows**: `CI`, `Mypy baseline diagnostic`, `Pre-commit baseline diagnostic`, `Dependabot Updates`, `Dependency Graph`
- **Branch Protection API Posture**: Returns `HTTP 403` ("Upgrade to GitHub Pro or make this repository public to enable this feature") on private free-plan repositories. Branch protection rulesets will automatically become REST-configurable once the public export repository is established.

## Classification Schema

Every control is classified into exactly one of five standard states:

- `VERIFIED_MET`: Confirmed met via live GitHub API, active workflow, or repository source code.
- `VERIFIED_GAP`: Confirmed gap or open blocker requiring technical/licensing resolution.
- `OWNER_VERIFICATION_REQUIRED`: Requires manual verification by account owner/maintainer in GitHub settings UI.
- `RELEASE_PUBLICATION_GATED`: Gated by the creation of the public repository export or official software release.
- `NOT_APPLICABLE`: Trigger is not applicable to current repository architecture.

---

## Level 1 Matrix

| Control | Classification | Live Evidence & Verification Details |
| --- | --- | --- |
| OSPS-AC-01.01 | `OWNER_VERIFICATION_REQUIRED` | Confirm MFA for all accounts with write/admin access in GitHub account/org settings before adding collaborators or publishing. |
| OSPS-AC-02.01 | `OWNER_VERIFICATION_REQUIRED` | Confirm collaborator roles and team permissions are strictly least-privilege in repository Settings -> Collaborators. |
| OSPS-AC-03.01 | `RELEASE_PUBLICATION_GATED` | Configure `main` branch protection ruleset (require status checks, PR review) on the **public export repository** before accepting public traffic. Private free-plan API returns HTTP 403. |
| OSPS-AC-03.02 | `RELEASE_PUBLICATION_GATED` | Enforce branch deletion and force-push protection on the public `main` branch. Private free-plan API returns HTTP 403. |
| OSPS-BR-01.01 | `VERIFIED_MET` | `.github/workflows/ci.yml` does not interpolate untrusted PR title/body/branch metadata directly into shell script execution. Pinned full SHA actions used. |
| OSPS-BR-01.03 | `VERIFIED_MET` | Workflow-level `permissions: contents: read`; checkout steps declare `persist-credentials: false`; no repository secrets exposed to unprivileged PR jobs. |
| OSPS-BR-03.01 | `VERIFIED_MET` | Official project links in documentation, `assets/release_manifest.json`, and `SECURITY.md` use authenticated HTTPS endpoints. |
| OSPS-BR-03.02 | `RELEASE_PUBLICATION_GATED` | No official software release binary distribution channel exists yet. Release assets will be checksummed and signed when established. |
| OSPS-BR-07.01 | `VERIFIED_MET` | `.gitignore`, `tools/publish_audit.py`, `tools/history_audit.py` (0 findings across 680 reachable commits / 4599 objects), and live API `automated-security-fixes` (`{"enabled": true}`) provide verified secret prevention layers. |
| OSPS-DO-01.01 | `RELEASE_PUBLICATION_GATED` | End-user documentation for public releases will be validated against the exact release candidate package. |
| OSPS-DO-02.01 | `VERIFIED_MET` | GitHub Issues enabled (`hasIssuesEnabled: true`), `.github/ISSUE_TEMPLATE/` forms present, and `SECURITY.md` defines reporting channels. |
| OSPS-GV-02.01 | `RELEASE_PUBLICATION_GATED` | Current repository visibility is `PRIVATE`. Public reporting/discussion channels will be established with the public export repository. |
| OSPS-GV-03.01 | `VERIFIED_MET` | `CONTRIBUTING.md`, `AGENTS.md`, `.github/PULL_REQUEST_TEMPLATE.md`, and verification guides explain contribution workflows. |
| OSPS-LE-02.01 | `VERIFIED_GAP` | Root `LICENSE` declares GPL-3.0-or-later, but component-level PGF font lineage (#98/#99) remains open. Do not treat root license as proof that all tracked code is cleared under GPLv2. |
| OSPS-LE-02.02 | `RELEASE_PUBLICATION_GATED` | Component dependency licenses locked via `assets/release_manifest.json` and generated SBOM artifacts (`tools/generate_sbom.py`). |
| OSPS-LE-03.01 | `VERIFIED_MET` | Root `LICENSE` present and SPDX identifiers embedded in repository source files. |
| OSPS-LE-03.02 | `VERIFIED_MET` | SPDX 2.3, SPDX 3.0.1 JSON-LD, and CycloneDX 1.5 SBOM generator (`tools/generate_sbom.py`) and verifier (`tools/verify_sbom.py`) produce machine-readable license notices. |
| OSPS-QA-01.01 | `RELEASE_PUBLICATION_GATED` | The public repository export will be created as a sanitized snapshot. Do not flip this historical development repository public directly. |
| OSPS-QA-01.02 | `RELEASE_PUBLICATION_GATED` | Full history audit #102 complete (0 findings under measured scope); public repository will be constructed with a clean, single-commit history. |
| OSPS-QA-02.01 | `VERIFIED_MET` | `interface/package-lock.json`, `tools/requirements-lock.txt`, and `assets/release_manifest.json` specify exact dependency versions and hashes. |
| OSPS-QA-04.01 | `NOT_APPLICABLE` | Single repository currently. If dual-repository topology (private archive + public export) is adopted, public docs will identify both and state authoritative sources. |
| OSPS-QA-05.01 | `VERIFIED_MET` | `tools/publish_audit.py` rejects unclassified binary formats and `tools/history_audit.py` verified 0 sensitive findings across all reachable commits. |
| OSPS-QA-05.02 | `VERIFIED_GAP` | PGF font license chains (#99) remain open. VFPU table upstream authorship is documented in `assets/README.md`. |
| OSPS-VM-02.01 | `VERIFIED_MET` | `SECURITY.md` defines private security vulnerability reporting procedures and maintainer fallback contact. |

---

## Current Evidence Highlights

### Live CI Posture & Gate Integrity

- Hosted GitHub Actions execution is active (`CI`, `Mypy`, `Pre-commit`, `Dependabot Updates`, `Dependency Graph`).
- **Gate Repair Status**: While individual feature PRs (#293, #294, #300, #301, #303) pass focused test suites, PR #298 / #299 (owned by Freebuff A) is currently open to synchronize `tools/test_generate_sbom.py` following PR #294's SBOM generator refactoring. Do not claim overall CI is fully green until PR #299 lands on `main`.

### Secret & History Prevention Layers

1. `.gitignore` exclusions for local secrets, databases, and build outputs.
2. `tools/publish_audit.py` for current and prospective tree auditing (`--tracked-only` and `--candidate-tree`).
3. `tools/history_audit.py` for full-history non-destructive auditing across all 680 reachable commits and 4,599 objects (0 sensitive findings under measured scope).
4. GitHub Dependabot automated security fixes (`automated-security-fixes` API enabled).

---

## Recurring Re-Review Triggers

This OSPS Baseline audit MUST be re-evaluated and updated upon any of the following events:

1. **Repository Visibility Change**: Changing repository visibility or creating the public export repository.
2. **First Official Public Release**: Tagging an official software version or releasing binary/source distributions.
3. **Maintainer / Access Change**: Adding or removing repository collaborators, admins, or team permissions.
4. **Release Automation Changes**: Introducing automated release workflows, code signing keys, or deployment credentials into GitHub Actions.
5. **Distribution-Channel Expansion**: Publishing to external package managers (npm, PyPI, Cargo, MSYS2).
6. **Security Configuration Modification**: Modifying branch rulesets, secret scanning, or workflow permission models.

---

## Manual Maintainer Verification Checklist

The following settings cannot be queried programmatically via REST without owner/admin access tokens and MUST be manually confirmed by the repository maintainer in the GitHub web interface:

- [ ] **MFA Enforcement**: Confirm Multi-Factor Authentication is required for all accounts with write or admin permissions.
- [ ] **Collaborator Access Review**: Confirm all collaborators in **Settings -> Collaborators** have minimum required privileges.
- [ ] **Secret Scanning & Push Protection**: In **Settings -> Code security and analysis**, verify Secret Scanning and Push Protection are enabled once the repository is public.
- [ ] **Private Vulnerability Reporting**: In **Settings -> Code security and analysis**, verify Private Vulnerability Reporting is checked.
