# Optional local symbol-reference setup

`hst_manager.ps1 -Action FindSymbol` can search an OpenGrip-style
`functions.csv` when a contributor keeps that reference data locally. This is
an optional reverse-engineering aid; it is not required to build or run
Nakagawa Recomp.

## Supported local paths

The manager checks these locations in order:

1. `docs/opengrip_ref/functions.csv`
2. `OpenGrip_For_Inspiration/functions.csv`

Both parent directories are ignored by the repository. Keep the CSV and any
associated OpenGrip checkout, decompiler export, annotations, or game-derived
material untracked.

Use only reference material that you are authorized to possess. Do not copy a
third-party repository, raw decompiler output, proprietary game bytes, private
symbols, or local-path-bearing exports into Git history merely to enable the
lookup command.

## Setup

Place or link an authorized local `functions.csv` at either supported path. The
first path is preferable when only the CSV is needed; the second supports a
complete local inspiration/reference checkout.

Verify that Git excludes the selected path before using it:

```bash
git check-ignore -v docs/opengrip_ref/functions.csv
# or
git check-ignore -v OpenGrip_For_Inspiration/functions.csv
```

Then run a lookup from the repository root:

```powershell
.\hst_manager.ps1 -Action FindSymbol -FindName Camera_Update
.\hst_manager.ps1 -Action FindSymbol -FindName 47054
```

The command performs a text search and prints at most 20 matching CSV rows. It
does not download, generate, or validate the reference data.

## Public documentation rule

Facts learned from a local symbol reference may be documented when they are
independently supportable and do not reproduce protected implementation or
private game data. Keep the local CSV itself and raw reverse-engineering
exports outside the published repository.
