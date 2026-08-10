# PSPDEV source and license notice

This file records the external sources pinned by `pspdev.lock.json`. The lock is
a reproducibility and audit input; it does not vendor PSPDEV, make it a
Nakagawa runtime dependency, or assert that a source snapshot exactly matches a
binary distribution.

## Pinned sources

| Component | Repository | Pin | License disposition |
| --- | --- | --- | --- |
| PSPDEV distribution metadata | <https://github.com/pspdev/pspdev> | `v20260501` / `cc874700eaef9e00c8ec63e0d116926e1048b656` | MIT |
| PSPSDK | <https://github.com/pspdev/pspsdk> | `314b2083f2e1eaf145fc5de342736336fe1f0148` | Root 3-clause BSD grant; `tools/PrxEncrypter` separately GPL-3.0-only |
| PSP toolchain driver | <https://github.com/pspdev/psptoolchain> | `57a4fc650324dea4637ea5ef9dfc2fc292c004f8` | `NOASSERTION` pending component-level SBOM review |
| Allegrex toolchain scripts | <https://github.com/pspdev/psptoolchain-allegrex> | `a95b7da838d9f506656092c3e0232dcf50389d89` | `NOASSERTION`; fetched compiler components retain their own licenses |
| Extra toolchain scripts | <https://github.com/pspdev/psptoolchain-extra> | `b5f01b00e428a604832e0dfb4bfbb991d1397e3f` | `NOASSERTION` pending tool-level review |
| PSP package recipes | <https://github.com/pspdev/psp-packages> | `0b6dbdef034badf483d94fd8b788315daffaf4bb` | Per-package upstream review required |
| PSPLINK USB | <https://github.com/pspdev/psplinkusb> | `v3.2.1` / `32f2fa9bca0259d68770b9678994e7ad2fd637c3` | BSD-3-Clause |

## Rules

- Prefer executing pinned upstream tools over copying their implementations.
- Preserve upstream notices when declarations, source, or generated artifacts
  are redistributed.
- Do not commit Sony SDK files, PSP firmware modules, internal firmware fonts,
  keys, retail executables, game assets, or private hardware dumps.
- A PSPSDK declaration is community ABI evidence, not hardware-semantic proof.
- Every tracked synthetic binary must have source, generator, toolchain lock,
  output hash, expected result, and redistribution disposition.
- The unresolved `NOASSERTION` entries are deliberate. This notice does not
  claim that the broader publication/SBOM gates are complete.
