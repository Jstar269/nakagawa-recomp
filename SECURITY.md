# Security policy

## Supported versions

The unreleased `main` branch is currently the only supported development line. There are no supported
release versions yet, and the project does not claim a release-grade security posture: treat arbitrary
PSP and game inputs as untrusted until the parser and guest-span hardening work tracked in GitHub Issues
is complete.

| Version | Supported | Status / Support Policy |
| :--- | :--- | :--- |
| `main` | :white_check_mark: | Active development line. Fixes for all security reports land here. |
| `< v0.0.1` | :x: | No prior release exists. |
| `v0.0.1` | :hourglass: | Planned first experimental platform release ([#278](https://github.com/Jstar269/nakagawa-recomp/issues/278)); not yet published. |

### Release policy transition (v0.0.1)

When the first formal experimental platform release (`v0.0.1`) ships:

- The `main` branch remains the primary supported development line.
- `v0.0.1` is an experimental prerelease milestone, not an LTS or patch-supported line: fixes for security issues identified in `v0.0.1` land on `main` and flow into subsequent releases rather than receiving backported security point releases.
- Release qualification authority [issue #278](https://github.com/Jstar269/nakagawa-recomp/issues/278) owns updating this policy and marking `v0.0.1` active atomically at release publication time. No release exists today.

## Report a vulnerability

Do not open a public issue for a vulnerability, and do not attach proprietary game inputs, private
paths, secrets, or exploit data to a public thread.

Report privately with GitHub's **Report a vulnerability** button on the repository's
[Security tab](https://github.com/Jstar269/nakagawa-recomp/security). If that button is ever
unavailable, contact the repository owner through the private contact method on their GitHub profile
and ask for a secure channel before sending details.

Include the affected revision, impact, reproduction conditions, relevant logs with personal and game
data removed, and any proposed mitigation. Reports are handled on a best-effort basis; no fixed
response or remediation time is promised before the project has a formal release team.

## Scope

In scope:

- memory safety and guest-to-host boundary issues in the runtime (`src/rt/`), including HLE handlers
  that read or write guest pointers;
- unsafe processing of malformed ELF/PRX/ISO/XB/PSMF or save inputs by the runtime, the Python
  translation tools (`tools/`), or the native player and its core library (`src/player/`, `src/core/`);
- path traversal or unsafe filesystem access in the VFS, extraction and staging tools, the native
  player;
- host-side hardware-runner and oracle tooling (`tools/psp_oracle/`) that talks to a connected device;
- publication and provenance controls that could let private inputs, paths, keys, or traces reach the
  public tree, an export, or a CI log; and
- GitHub Actions workflows, dependencies, and release-packaging issues directly affecting this
  repository.

Out of scope:

- vulnerabilities in the original game, PSP firmware, or third-party products when used independently;
- compatibility bugs without a security impact; and
- reports that require redistribution of copyrighted game data.

This research software processes untrusted binary formats and is not sandboxed. Run it only on files
you are authorized to use.
