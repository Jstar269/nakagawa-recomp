# Public-safe source profile

The `public-safe-v1` profile is the conservative initial-source configuration for issues
[#98](https://github.com/Jstar269/nakagawa-recomp/issues/98),
[#99](https://github.com/Jstar269/nakagawa-recomp/issues/99), and
[#104](https://github.com/Jstar269/nakagawa-recomp/issues/104). It excludes:

- the lineage-sensitive PGF parser/rasterizer and all bundled PGF fonts;
- the PGD/amctrl runtime, standalone implementation, harness, and implementation tests.

The retained `pgf_unavailable.c` and `pgd_unavailable.c` backends fail closed. They do not
fabricate glyphs, decryption, keys, or successful PSP operations. Consequently this profile is a
buildable generic-source boundary, not a feature-equivalent HST release.

## Produce and verify a candidate

Commit the intended source state, then materialize an exact Git ref outside the repository:

```powershell
python tools/public_candidate.py <staging>\nakagawa-public --ref HEAD
python tools/publish_audit.py --candidate-root <staging>\nakagawa-public --public-scope
mingw32-make -C <staging>\nakagawa-public public-safe-verify
```

A full private checkout defaults to `PUBLIC_SAFE=0`. Because the disputed backends are absent from
the filtered candidate, its Makefile defaults to `PUBLIC_SAFE=1`; asking that tree for
`PUBLIC_SAFE=0` is an error. Windows release asset copying also receives
`-ExcludeOptionalFonts` in public-safe mode.

The exclusion list is machine-readable in [`assets/public_source_profile.json`](../assets/public_source_profile.json).
`PUBLIC_CANDIDATE.json` records the exact source commit and profile digest in each materialized tree.

## Claim boundary

This profile removes the two components from the candidate tree/build. It does not resolve their
underlying provenance or legal questions, approve a visibility change, sanitize historical Git
objects, clear a binary distribution, or replace the required qualified human review of the actual
candidate.
