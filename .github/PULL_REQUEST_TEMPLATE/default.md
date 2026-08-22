# Pull request

## Description

Brief description of what this PR does.

## Related Issue

Fixes #(issue number)

## Type of Change

- [ ] Bug fix (non-breaking change that fixes an issue)
- [ ] New feature (non-breaking change that adds functionality)
- [ ] Breaking change (fix or feature that would cause existing functionality to not work as expected)
- [ ] HLE handler (`s_hle[]` registration + matching guest NID)
- [ ] Pipeline change (`tools/codegen.py` or related)
- [ ] Documentation update

## Testing Performed

- [ ] `python -m unittest discover -s tools -p "test_*.py" -v` passes (generator/tooling changes)
- [ ] `hst_manager.ps1 -Action BuildFull -TitleManifest assets/titles/hst-ucus98701.json` succeeds (pipeline/codegen changes)
- [ ] `hst_manager.ps1 -Action BuildFast -TitleManifest assets/titles/hst-ucus98701.json` succeeds (runtime-only changes)
- [ ] `hst_manager.ps1 -Action Test` (selftest) passes
- [ ] `npx --yes markdownlint-cli2@0.23.1` passes (documentation changes)
- [ ] `npm ci && npm test && npm run lint && npm run typecheck && npm run build` passes in `interface/` (dashboard changes)
- [ ] `hst_manager.ps1 -Action Run` — exact observed frontier described below
- [ ] Manual verification (describe below)

Mark non-applicable checks as such in the description; a documentation-only change does not
need a full native rebuild.

## Checklist

- [ ] Code follows project style conventions (see `.clang-format`, `pyproject.toml`)
- [ ] No hand-edits to `build/hst/` generated files
- [ ] Any new HLE handlers are registered and have matching NIDs in `tools/imports.py`
- [ ] `ISSUES.md` updated if resolving or discovering blockers
- [ ] No game inputs, decrypted modules, extracted assets, oracle traces, or framebuffer dumps included
- [ ] New third-party code/data has source, license, and provenance recorded
