# Runtime lookup tables

`assets/vfpu/*.dat` contains PPSSPP-origin VFPU lookup data required by
`src/rt/vfpu_interp.c`. Every file was verified byte-for-byte against the same
path in PPSSPP commit
`f0baf3ade7bcb6c86f0835962b36eb4e51559d8f` on 2026-07-18.

[`vfpu/PROVENANCE.json`](vfpu/PROVENANCE.json) records each file's byte
count, Git blob ID, pinned upstream commit, and upstream path.

## Upstream creation history

The tables are not unexplained firmware/game blobs. Their PPSSPP development history is public:

- PPSSPP issue [`#16946`](https://github.com/hrydgard/ppsspp/issues/16946) documents the VFPU accuracy research and measured lookup-table approach.
- PPSSPP PR [`#16984`](https://github.com/hrydgard/ppsspp/pull/16984), authored by `fp64` and merged on 2023-03-28, introduced the current family of `assets/vfpu/*.dat` files together with the emulator code that consumes them.
- The PR discussion explicitly chose binary files under PPSSPP's `assets/` tree rather than giant generated C headers. Later PPSSPP commits retained fallback approximations for installations where these asset files are missing.

This history establishes an upstream contributor/project provenance chain in addition to the byte-level pin. It does not make the tables game or PSP-firmware extracts. Final redistribution review should still preserve PPSSPP's applicable project notices and should not infer rights solely from the fact that bytes are publicly downloadable.

Verify the checked-in bytes without network access:

```powershell
python tools/verify_vfpu_provenance.py
```

Re-verify against upstream PPSSPP on demand, without cloning it — this queries
the pinned commit's tree through the GitHub API and compares blob ids (needs the
`gh` CLI and network; reports a clear "skipped" rather than passing if either is
missing):

```powershell
python tools/verify_vfpu_provenance.py --upstream-api
```

If instead a local PPSSPP checkout contains the pinned commit, recheck against it:

```powershell
python tools/verify_vfpu_provenance.py --upstream-checkout third_party/ppsspp-src
```

All 15 assets were confirmed byte-for-byte identical to PPSSPP
`f0baf3ade7bcb6c86f0835962b36eb4e51559d8f` via `--upstream-api`; the four
`font/*.pgf` files match the same commit's `assets/flash0/font/` tree. See
[../NOTICE.md](../NOTICE.md) for licensing and attribution.
