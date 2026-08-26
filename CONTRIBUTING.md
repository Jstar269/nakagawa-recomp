Contributing to nakagawa-recomp

Thanks for helping maintain this Scoop bucket. Please follow this checklist when adding or updating manifests.

Manifest best practices
- Create one JSON manifest per package.
- Use semantic versioning for the "version" field.
- Provide stable download URLs (prefer GitHub Releases) and include sha256 hashes.
- Populate "homepage" and "description" for discoverability.
- Add "bin" entries when the package installs executable(s).

PR checklist
- JSON is valid (run `python -m json.tool <manifest.json>`).
- The version field is correct and bumped.
- Hashes are correct and match the downloaded artifact.
- If possible, include "checkver" and "autoupdate" to enable automatic updates.

How to compute a SHA256 hash
- PowerShell: Get-FileHash -Algorithm SHA256 .\file.zip | Select-Object -ExpandProperty Hash
- Unix/macOS: shasum -a 256 file.zip

Submit a PR
1. Fork the repository, create a topic branch, and add your manifest.
2. Open a PR against main with a clear description and the PR checklist completed.
3. The maintainer will review and merge or request changes.
