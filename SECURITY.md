# Security policy

The unreleased `main` branch is the only supported line. There are no supported release versions yet.

## Report a vulnerability

Do not open a public issue for a vulnerability or attach proprietary game inputs, private paths, secrets, or exploit data to a public thread.

Use GitHub's **Report a vulnerability** button on the repository's Security tab when private vulnerability reporting is enabled. If it is unavailable, contact the repository owner through the private contact method on their GitHub profile and ask for a secure channel before sending details.

Include the affected revision, impact, reproduction conditions, relevant logs with personal/game data removed, and any proposed mitigation. Reports are handled on a best-effort basis; no fixed response or remediation time is promised before the project has a formal release team.

## Scope

In scope:

- memory safety and guest-to-host boundary issues in `src/rt/`;
- path traversal or unsafe filesystem access in the VFS, extraction tools, and dashboard APIs;
- unsafe processing of malformed ELF/PRX/ISO/XB inputs;
- command execution, secret exposure, or access-control issues in `interface/`; and
- dependency or release-packaging issues directly affecting this repository.

Out of scope:

- vulnerabilities in the original game or third-party products when used independently;
- compatibility bugs without a security impact; and
- reports that require redistribution of copyrighted game data.

This research software processes untrusted binary formats and is not sandboxed. Run it only on files you are authorized to use and avoid exposing the dashboard to untrusted networks.
