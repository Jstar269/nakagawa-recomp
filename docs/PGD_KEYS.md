# Supplying local PSP KIRK / amctrl constants

The private PGD (amctrl) development path currently requires seven fixed PSP platform constants. **This project does not ship them.** They are absent from the current Git tree, normal build output and intended release artifacts. Development environments that exercise this path supply them separately and locally.

## Why they are not included

These values are fixed inputs used by the PSP KIRK/amctrl algorithms. The project's position is intentionally narrow and factual: **they are not distributed here.** Their absence is a concrete risk-reducing/publication-scope choice, not a legal ruling that distributing the implementation or the values would necessarily be unlawful.

Anti-circumvention and reverse-engineering rules vary by jurisdiction and facts. U.S. law includes an interoperability provision in 17 U.S.C. §1201(f), while other regimes have their own rules and conditions. The applicability of those provisions to this implementation/distribution is reserved for qualified review in [`PGD_AMCTRL_REVIEW_PACKET.md`](PGD_AMCTRL_REVIEW_PACKET.md).

Nothing here tells a user where to obtain, derive, dump, extract, or download real values. This document describes only the private file schema expected by the current development code.

## File format

A plain text file with one `name = value` pair per line. `#` begins a comment. Every value is exactly **32 hexadecimal characters** (16 bytes). Case-insensitive. Blank lines are ignored; unknown names are ignored.

```text
# keys/pgd_keys.txt
kirk_keyseed_38 = <32 hex characters>
kirk_keyseed_39 = <32 hex characters>
kirk_keyseed_63 = <32 hex characters>
amctrl_loc_1cd4 = <32 hex characters>
amctrl_loc_1ce4 = <32 hex characters>
amctrl_loc_1cf4 = <32 hex characters>
dnas_1a90       = <32 hex characters>
dnas_1aa0       = <32 hex characters>
```

### Entries

| Name | Development-code role |
| --- | --- |
| `kirk_keyseed_38` | KIRK fixed AES-128 key for keyseed `0x38`; used by the implemented BBMac/BBCipher flow. |
| `kirk_keyseed_39` | KIRK fixed AES-128 key for keyseed `0x39`. |
| `kirk_keyseed_63` | KIRK fixed AES-128 key for keyseed `0x63`. |
| `amctrl_loc_1cd4` | amctrl mixing constant used in the CMAC subkey path. |
| `amctrl_loc_1ce4` | amctrl mixing constant used in BBCipher key derivation. |
| `amctrl_loc_1cf4` | amctrl mixing constant combined with the incoming key in BBCipher. |
| `dnas_1a90` | DNAS constant selected by one PGD flag path. |
| `dnas_1aa0` | Alternate DNAS constant used by the Python reference; the current C runtime fixed-key path does not reach it. |

The C runtime requires the first **seven** entries. The Python reference currently requires all **eight**. Supplying all eight satisfies both development surfaces.

## Where the private file goes

Resolved in this order:

1. `$SR_PGD_KEYS` — explicit path, if set and non-empty.
2. `keys/pgd_keys.txt`, relative to the current working directory.

`keys/` is gitignored, but Git ignore rules are not a security boundary. Prefer keeping real values **outside the repository directory entirely** and point `SR_PGD_KEYS` to that file. Do not place the file in cloud-synced project folders, issue attachments, build artifacts or logs.

```bash
export SR_PGD_KEYS=/path/outside/repo/pgd_keys.txt
```

```powershell
$env:SR_PGD_KEYS = "D:\path\outside\repo\pgd_keys.txt"
```

## Verifying a local file

```bash
python tools/pgd_decrypt.py --check-keys
```

- exit `0` — all eight entries parsed;
- exit `3` — missing, partial or malformed; the diagnostic identifies the missing name/line but does not print values.

To verify only the independently implemented AES core without PSP constants:

```bash
python tools/pgd_decrypt.py --selftest
```

That uses the FIPS-197 known-answer vector and works with no local key file.

## Behavior when constants are absent

Absence is a supported development state.

| Surface | Without local constants |
| --- | --- |
| Current private build | Builds; `src/rt/pgd.c` is presently compiled in, but the PGD path fails closed. |
| `sr_pgd_selftest()` | Passes the FIPS-197-only selftest. |
| `sr_pgd_keys_available()` | Returns `0`. |
| `sr_pgd_open()` | Returns `NULL`. |
| `pgd_decrypt.py` decrypt/`--info` | Exits `3` with an explanatory message. |
| Private PGD integration tests | Skip/report unavailable. |
| Game route | Can run without using the optional installed `GAMEDATA.BDL` cache. |

The conservative initial public-source plan may exclude PGD/amctrl entirely pending #104; therefore this private development schema must **not** be interpreted as a promise that the same interface/component will exist in a public release.

## Separate title version key

The 16-byte per-title version key is a separate private value. It is also not shipped. Private integration tests may read `HST_PGD_VKEY_HEX`; the standalone CLI accepts `--vkey`. Neither mechanism should log or persist the value into repository artifacts.

## Related

- [`PGD_AMCTRL_REVIEW_PACKET.md`](PGD_AMCTRL_REVIEW_PACKET.md) — implementation facts and qualified-review questions.
- [`KEY_HISTORY_SCRUB.md`](KEY_HISTORY_SCRUB.md) — old-history sanitation plan.
- [`../NOTICE.md`](../NOTICE.md) — provenance/trademark/legal notices.
- [`SETUP.md`](SETUP.md) — other private development inputs.
