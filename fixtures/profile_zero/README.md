<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 the Nakagawa Recomp authors -->

# Profile-zero guest fixture

`main.c` and `Makefile` are the project-authored PSPSDK guest behind the two
profile-zero manifests (`assets/titles/synthetic.json` and
`assets/titles/synthetic-title2.json`). The `prebuilt/` directory commits the
PSPDEV build output so `mingw32-make profile-zero-e2e` can run the whole
manifest -> ProgramImage -> AOT package -> production-runtime route on hosts
without PSPDEV/PSPSDK, including hosted Windows CI.

## Contents

- `prebuilt/EBOOT.PBP` — the packed PSP boot file named by the manifests'
  build target (`profile_zero.build.target`).
- `prebuilt/profile_zero_guest.prx` — the relocatable guest module the route
  loads as a `ProgramImage`. Each manifest rebuilds these same bytes under its
  own `game_name` (`synthetic.prx`, `synthetic_title2.prx`); the gate fails
  closed if any rebuilt byte differs from the committed digests.
- `prebuilt/SHA256SUMS` — the committed digests.

These files are project-authored output of the project-authored source in this
directory. They contain no retail or private bytes.

## Reproduce (requires PSPDEV/PSPSDK)

From the repository root, with `psp-config`, `psp-gcc`, `psp-prxgen`, and
`pack-pbp` on `PATH` (the committed bytes came from the PSPDEV distribution
pinned by `assets/upstream/pspdev.lock.json`: release `v20260601`, official
`pspdev-ubuntu-latest-x86_64.tar.gz` release asset, installed into a WSL Ubuntu
`/usr/local/pspdev`, `psp-gcc (GCC) 15.2.0`):

```bash
out="$(mktemp -d)"
make --no-print-directory \
  -f "$PWD/fixtures/profile_zero/Makefile" \
  -C "$out" \
  VPATH="$PWD/fixtures/profile_zero" \
  TARGET=profile_zero_guest \
  EBOOT.PBP
sha256sum "$out/EBOOT.PBP" "$out/profile_zero_guest.prx"
```

`make` resolves `-f` after `-C`, so the `-f` and `VPATH` paths must be
absolute (as shown). Both digests must equal the entries in `prebuilt/SHA256SUMS`
(`cd fixtures/profile_zero/prebuilt && sha256sum -c SHA256SUMS`).

`profile-zero-e2e` runs exactly this rebuild-and-compare whenever PSPDEV is
present and fails on byte drift; on hosts without PSPDEV (such as hosted CI)
it consumes the committed bytes and runs the same seven-case route without a
toolchain SKIP.

The rebuild-and-compare is authoritative only for the pinned distribution
(#708). The gate first verifies that the installed PSPDEV tool binaries match
the identities `assets/upstream/pspdev.lock.json` records for release
`v20260601` and the `pspdev-ubuntu-latest-x86_64.tar.gz` release asset; a
PSPDEV from a different asset class (for example the Debian archives) or a
different release ships different tool binaries and produces different fixture
bytes, so the gate fails closed naming the pin instead of reporting drift from
an unauthoritative toolchain.
