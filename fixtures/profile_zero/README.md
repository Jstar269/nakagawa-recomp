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
`pack-pbp` on `PATH` (here: the WSL Ubuntu install at `/usr/local/pspdev/bin`,
`psp-gcc (GCC) 15.2.0`, PSPSDK pinned by `assets/upstream/pspdev.lock.json`):

```bash
mkdir -p /tmp/profile-zero-build
make --no-print-directory \
  -f "$PWD/fixtures/profile_zero/Makefile" \
  -C /tmp/profile-zero-build \
  VPATH="$PWD/fixtures/profile_zero" \
  TARGET=profile_zero_guest \
  EBOOT.PBP
sha256sum /tmp/profile-zero-build/EBOOT.PBP \
          /tmp/profile-zero-build/profile_zero_guest.prx
```

`make` resolves `-f` after `-C`, so the `-f` and `VPATH` paths must be
absolute (as shown). Both digests must equal the entries in `prebuilt/SHA256SUMS`
(`cd fixtures/profile_zero/prebuilt && sha256sum -c SHA256SUMS`).

`profile-zero-e2e` runs exactly this rebuild-and-compare whenever PSPDEV is
present and fails on byte drift; on hosts without PSPDEV (such as hosted CI)
it consumes the committed bytes and runs the same seven-case route without a
toolchain SKIP.
