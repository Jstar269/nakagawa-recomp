nakagawa-recomp
=================

This repository is a Scoop bucket for distributing Windows command-line packages.

Quick start
- Add this bucket: scoop bucket add nakagawa-recomp https://github.com/Jstar269/nakagawa-recomp
- Install a package from this bucket: scoop install <package-name>

Manifest guidelines
- Place package manifest JSON files at the repo root or in subfolders (e.g., "apps/").
- Required top-level fields: "version", "architecture" (with 64bit/32bit entries), and a "homepage"/"description".
- Provide a stable download URL and a sha256 hash for each architecture.
- Use GitHub Releases whenever possible; set "checkver" and "autoupdate" to enable automatic updates.

Computing SHA256
- PowerShell: Get-FileHash -Algorithm SHA256 .\file.zip | Select-Object -ExpandProperty Hash
- Unix/macOS: shasum -a 256 file.zip

Testing locally
- To test a manifest file locally, you can run: scoop install <path-to-manifest.json>
- To test a package from this bucket before merging, follow these steps:
  1. fork and clone this repo, add/modify the manifest
  2. run a local validation script (see .github/workflows/validate-manifests.yml for the same checks)
  3. open a PR to propose the change

Contributing
- Follow the template in nakagawa-recomp.json (replace placeholders with real values).
- PR checklist:
  - JSON is valid
  - version is updated
  - URLs point to release artifacts
  - SHA256 hashes are correct
  - autoupdate/checkver configured when possible

Local validation
- Run the included validator: python scripts/validate_manifest.py [manifest-or-dir]
- Compute a SHA256 for a local file or URL (PowerShell):
  - .\tools\compute-sha256.ps1 -Path .\artifact.zip
  - .\tools\compute-sha256.ps1 -Url https://example.com/artifact.zip

CI
- The repository includes a GitHub Actions job (.github/workflows/validate-manifests.yml) that runs the same basic checks on push and PR.

License
- This repository is licensed under the MIT License (see LICENSE).
