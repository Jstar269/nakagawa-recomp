# Nakagawa Recomp `0.1.0-preview.1` — proposed early preview

This is proposed release-note copy. The maintainer must cut any tag or release
manually; this branch does not do that.

## What genuinely works

The public, game-input-free production smoke is the headline:

- `mingw32-make production-smoke` generates a synthetic PSP PRX, statically
  recompiles it MIPS-to-C-to-native, and verifies and runs the AOT output.
- `mingw32-make production-smoke-gap` proves the corresponding fail-closed
  dispatch path when a synthetic function is intentionally omitted from AOT
  emission.
- `mingw32-make cosim-selftest` compares AOT and interpreter traces, writes,
  memory, and architectural state over synthetic cases.
- `mingw32-make platform-ladder` exercises the source-owned synthetic workload
  ladder, including negative cases.
- `selftest`, `sched-selftest`, `hle-thread-selftest`, and
  `public-safe-verify` pass on the verified Windows host.
- The Next.js dashboard builds, lints, typechecks, and passes its source tests.
  Its browser-local ISO9660/PARAM.SFO inspector is an inspection tool, not a
  preparation or execution route.

## Limitations

- **No preparation pipeline is connected in this build.** This preview cannot
  run a retail PSP game. Do not read the synthetic executables, the dashboard,
  or the ISO inspector as gameplay evidence.
- The exact base used for this preview does not contain the native `player`
  target or `src/player`, so no player binary is shipped.
- The preview is verified on Windows 11 x64 only. It makes no Linux or macOS
  support claim.
- Retail executables, ISOs, assets, decrypted modules, generated retail C,
  saves, keys, private traces/captures, private paths, and hardware evidence
  are not part of the package.
- Local verification is not hosted-CI evidence, a hardware pass, legal
  clearance, DCO attestation, or permission to publish.

## Reproduce the centerpiece

From a clean Windows checkout with the supported toolchain:

```powershell
mingw32-make --no-print-directory production-smoke
mingw32-make --no-print-directory production-smoke-gap
```

The commands generate their synthetic inputs beneath the ignored `build/`
tree. They do not require a retail ISO, PRX, save, key, or decrypted module.

## Preview package contents

The prepared package shape is:

```text
nakagawa-recomp-0.1.0-preview.1-windows-x64/
├── bin/production_smoke.exe       synthetic verification binary only
├── source/                         public-safe source export
├── docs/PREVIEW_RELEASE.md        scope and regeneration contract
├── docs/PREVIEW_RELEASE_NOTES.md  this note
├── LICENSE
├── NOTICE.md
└── SHA256SUMS.txt
```

There is no `player.exe` in this package. The exact package command and its
fail-closed scan are in [`PREVIEW_RELEASE.md`](PREVIEW_RELEASE.md).
