# Python skip inventory

Skip counts are evidence about availability, not passes. The canonical
discovery command must report the reason for every skip, and optional/private
or sanitizer routes remain explicitly unavailable when their inputs/toolchain
are absent.

**The skip count is conditional on private-input availability and must always be
quoted with that condition.** Six of the skips below occur only when the private
`place_game_here/` inputs are absent. A checkout that has them runs those six
cases instead of skipping them, so the same suite legitimately reports either
27 or 33 skips.

| Count | Scope | Reason | Classification | Needs private inputs |
| ---: | --- | --- | --- | --- |
| 5 | `test_analyze_tailcall`, `test_codegen_no_shadow_stubs` | decrypted `EBOOT.elf` not present | private local input | yes |
| 1 | `test_decompme_export` | private `place_game_here/EBOOT.elf` not present | private local input | yes |
| 1 | `test_codegen_gate_b_encoding.TestGateBElfEncodingAudit` | `mipsel-linux-gnu-gcc` not available | toolchain unavailable; class-level skip | no |
| 4 | `test_pgd_c`, `test_pgd_decrypt` | private `GAMEDATA.BDL` and `HST_PGD_VKEY_HEX` are both required | private local input/key | no (key-gated) |
| 22 | `test_pgd_malformed.TestPgdMalformedE2ESanitized` | ASan+UBSan unavailable on Windows/MinGW; Linux CI lane runs this class | sanitizer/platform | no |

Measured totals on the Windows host:

| Condition | Skips | Composition |
| --- | ---: | --- |
| public-safe checkout, no `place_game_here/` (`f374b310c3e46f886b5f6c2ae9deda5b6f154468`) | 33 | all rows above |
| private inputs present (`de269b01c8f38a395cdac075d58053ab4dfbcc34`) | 27 | 22 sanitizer + 4 PGD key + 1 Gate B |

The 4 PGD cases stay skipped in both columns because they additionally require
the `HST_PGD_VKEY_HEX` key, which is not part of the private workspace shape
that `tools/psp_readiness.py` reports as present.

These are intentional and have explicit conditions in the tests. They are not
converted to successful evidence. The five method IDs under the class-level
Gate B skip are also the exact loader-only IDs documented in
[`TEST_DISCOVERY.md`](TEST_DISCOVERY.md).

The pre-PSP source-owned tests do not change this skip inventory. Any future
change to a count or reason requires a focused review rather than treating a
lower number as an optimization target — and any quoted count must name the
private-input condition it was measured under.
