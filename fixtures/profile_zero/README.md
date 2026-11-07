<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 the Nakagawa Recomp authors -->

# Profile-zero guest fixture

`main.c` and `Makefile` are the project-authored PSPSDK guest behind the two
profile-zero manifests (`assets/titles/synthetic.json` and
`assets/titles/synthetic-title2.json`). The repository commits only the digests
of the PSPDEV build output (`prebuilt/SHA256SUMS`), not the generated executables
themselves (#1100). Hosted CI builds them from this source in the
`public_export` job, checks them against the digests and hands them to the
jobs that run `mingw32-make profile-zero-e2e`, the whole manifest -> ProgramImage
-> AOT package -> production-runtime route; a host with PSPDEV rebuilds them
itself.

## Contents

- `prebuilt/EBOOT.PBP` (generated, gitignored) — the packed PSP boot file named
  by the manifests' build target (`profile_zero.build.target`).
- `prebuilt/profile_zero_guest.prx` (generated, gitignored) — the relocatable guest module the route
  loads as a `ProgramImage`. Each manifest rebuilds these same bytes under its
  own `game_name` (`synthetic.prx`, `synthetic_title2.prx`); the gate fails
  closed if any rebuilt byte differs from the committed digests.
- `prebuilt/SHA256SUMS` — the committed digests, the only file stored here.

The generated files are project-authored output of the project-authored source in this
directory. They contain no retail or private bytes.

## Reproduce (requires PSPDEV/PSPSDK)

From the repository root, with `psp-config`, `psp-gcc`, `psp-prxgen`, and
`pack-pbp` on `PATH` (the committed bytes came from the PSPDEV distribution
pinned by `assets/upstream/pspdev.lock.json`: release `v20261101`, official
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
present and fails on byte drift; on hosts without PSPDEV it runs the same
seven-case route on bytes already staged in `prebuilt/` (in hosted CI, the
`profile-zero-fixture` artifact), checked against the digests, and reports
`NOT_RUN` when no bytes are staged (a failure instead when `NK_PROFILE_ZERO_REQUIRED=1`,
as in hosted CI). The rebuild is a make of its own: it drops the calling make's
`MAKEFLAGS`, so `make CC=gcc profile-zero-e2e` cannot replace PSPSDK's `psp-gcc`.

The rebuild-and-compare is authoritative only for the pinned distribution.
The gate first verifies that the installed PSPDEV tool binaries match
the identities `assets/upstream/pspdev.lock.json` records for release
`v20261101` and the `pspdev-ubuntu-latest-x86_64.tar.gz` release asset; a
PSPDEV from a different asset class (for example the Debian archives) ships
different tool binaries and produces different fixture bytes, so the gate fails
closed naming the pin instead of reporting drift from an unauthoritative
toolchain. That check covers the four build tools only, and they are not unique
to one release: `v20261001` ships the same `psp-config`, `psp-gcc`,
`psp-prxgen`, and `pack-pbp` bytes as `v20261101` but different SDK libraries,
so a `v20261001` installation passes the identity check and then fails the
rebuild as byte drift.

The `v20261001` rebuild changed both generated outputs, so only
`prebuilt/SHA256SUMS` was refreshed; the generated PBP and PRX remain ignored.
Its PSPSDK `build.mak` changes the default `MEMSIZE` from 1 to 2 for firmware
versions above 3.90, changing the SFO included in `EBOOT.PBP`. All 11 PSP SDK
archives linked by this fixture also changed, while the compiled `main.o`
remained byte-identical; this changed `profile_zero_guest.prx`.

The `v20261101` rebuild changed both generated outputs again, and again only
`prebuilt/SHA256SUMS` was refreshed. The compiler, linker, and packaging tools
are byte-identical to `v20261001`'s, and so are `main.o` and the generated
`PARAM.SFO`. The change comes from two C library objects the release rebuilt:
newlib's `makebuf.o` in `libc.a` (newlib `09d1adaf`, which enables
`st_blksize` buffering for the PSP) and `_stat` in PSPSDK's `libcglue.a`
(pspsdk `c3da4314`, which reports a 64 KiB `st_blksize` for regular files
instead of a fixed 16 KiB). In the linked ELF, `__smakebuf_r`
shrinks from 0x154 to 0x100 bytes, `__swhatbuf_r` grows from 0xbc to 0xdc and
`_stat` from 0x1f0 to 0x1f8, so `.text` is 44 bytes shorter and
`profile_zero_guest.prx`, and the `EBOOT.PBP` that contains it, change. The 11
PSPSDK archives named on the link line were also rebuilt, but their members are
byte-identical except one `libpspdebug.a` object.
