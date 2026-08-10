# Ghidra-assisted analysis (optional, developer-only)

An optional second opinion on the recomp pipeline's own binary analysis. Nothing
in the build or runtime depends on it; consumers and CI never need it. It exists
because Ghidra's mature Allegrex support catches function-discovery disagreements
that become silent `NONPLT_MISS` dispatch faults at runtime (see the
[2026-07-18 history](STATUS_HISTORY.md)), and its decompiler makes targeted
reverse engineering (hook triage, heap-bug hunts) far faster than raw MIPS.

Everything lives under `third_party/` (gitignored) and is driven headlessly by
`tools/ghidra_headless.py` — no GUI clicks are part of any workflow here.

## One-time setup

1. **Ghidra 12.1** — download `ghidra_12.1_PUBLIC` from the
   [official Ghidra releases](https://github.com/NationalSecurityAgency/ghidra/releases) and extract to
   `third_party/ghidra/ghidra_12.1_PUBLIC/` (or set `GHIDRA_HOME` to wherever it
   lives). Needs a JDK 21+ on `PATH`.
2. **ghidra-allegrex** — use a build that targets the exact Ghidra version and
   install it exactly once. Upstream's v21.3 release targets Ghidra 12.0, not
   12.1; do not install that archive into 12.1 merely because it is the newest
   release. For Ghidra 12.1, use a 12.1-targeted upstream build or build the
   current upstream source with `GHIDRA_INSTALL_DIR` pointing at the 12.1
   installation and `GHIDRA_USER_DIR` pointing at its user directory, then run
   `gradlew ghidraInstall`, as documented by
   [ghidra-allegrex](https://github.com/kotcrab/ghidra-allegrex). Install either:
   - GUI: `File → Install Extensions → +` (lands in
     `%APPDATA%\ghidra\ghidra_12.1_PUBLIC\Extensions\`), or
   - headless-only: unzip into `<GHIDRA_HOME>/Ghidra/Extensions/`.

   Install to **one** location only. If both exist, every Ghidra launch dies with
   `Multiple modules collided with same name: ghidra-allegrex`.
3. **Decrypted EBOOT** — the same `place_game_here/EBOOT.elf` the build already
   uses (see [SETUP.md](SETUP.md)). Never redistribute the binary, raw analysis
   database, decompiler output, or mechanically translated implementation.

## Workflow

```bash
python tools/ghidra_headless.py validate          # clean loader/language compatibility check
python tools/ghidra_headless.py analyze           # one-time import + auto-analysis (~2 min)
python tools/ghidra_headless.py info              # setup/project status
python tools/ghidra_headless.py export-functions  # -> third_party/ghidra/exports/functions.csv
python tools/ghidra_crosscheck.py                 # diff Ghidra's function list vs tools/analyze.py
python tools/ghidra_headless.py decompile 0x1c008 0x46c4c   # -> exports/decomp/<addr>.c
python tools/ghidra_headless.py refs 0x1c008      # who references an address, and how
```

Addresses are the pipeline's base-0 view; the tool translates to Ghidra's image
base automatically (`--raw` disables). All Ghidra output is logged to
`logs/ghidra_<command>.log`. Post-scripts are plain-Java GhidraScripts in
`tools/ghidra_scripts/` (compiled on the fly; no Jython/PyGhidra dependency).

Upstream's normal GUI workflow imports a decrypted EBOOT as `PSP Executable
(ELF)` / `Allegrex`, usually at image base `0x08804000`; PPSSPP `.sym` imports
then use offset zero. This project's headless analysis deliberately uses image
base `0x00000000` so addresses match the recomp pipeline directly. Also, do not
rely on automatic language selection for this dump: Ghidra 12.1 selected generic
`MIPS:LE:64:64-32R6addr` in a clean auto-detect test. The driver therefore forces
`PspElfLoader` and `Allegrex:LE:32:default`; `validate` performs a throwaway
import and fails unless Ghidra reports those exact choices.

`ghidra_crosscheck.py` is the standing tripwire: `ghidra-only` entries are
candidate analyze.py misses (NONPLT_MISS risk — triage every one; the known
benign remainder is documented in [STATUS_HISTORY.md](STATUS_HISTORY.md)),
`analyze-only` entries are usually
its deliberate aggressiveness (jump-table landings) and fine. `--strict` makes
a non-empty ghidra-only list exit 1 once the list is triaged to zero.

## Rules

- **Never publish raw game-analysis artifacts:** no imported game binaries,
  `exports/`, decompiled `.c`, Ghidra project databases, extracted assets,
  mechanically translated proprietary implementation, or substantial oracle
  traces. `third_party/` stays gitignored.
- Small interoperability facts may be documented when necessary: addresses,
  NIDs, ABI behavior, structure offsets, and observed state transitions.
  Independently written compatibility code must express the required behavior,
  not reproduce decompiler output.
- For game-specific fixes, retain a concise provenance note: what was observed,
  which private analysis aid was consulted, whether expressive implementation
  was copied or mechanically translated (the acceptable answer is no), and how
  the independent result was verified.
- Ghidra output is a *lead*, not ground truth — verify against the actual bytes
  (the 2026-07-18 session found Ghidra artifacts too: phantom `thunk_FUN_*`
  splits of bottom-tested loops).
- The GUI project `third_party/ghidra/GhidraProject/OpenGrip` is a legacy
  manual-analysis project; the headless flow deliberately uses its own project
  (`third_party/ghidra/projects/HST`) and never touches it.

## Related: PPSSPP sources for the NID name table

`src/rt/nid_names.h` is generated (and committed) from PPSSPP's HLE tables.
Regenerating it is equally optional-dev-only:

```bash
git clone --depth 1 --filter=blob:none --sparse https://github.com/hrydgard/ppsspp third_party/ppsspp-src
git -C third_party/ppsspp-src sparse-checkout set Core/HLE
python tools/gen_nidnames.py
```

The generator records the source commit in the header and refuses to regress a
populated table to empty when the sources are absent.

## PGD install-cache compatibility (developer-only)

The game's `ms0:/PSP/SAVEDATA/UCUS98701GAMEDATA/GAMEDATA.BDL` (~413 MB) is a PGD
(amctrl) encrypted install cache. `pspdecrypt` cannot decrypt it — that tool only
handles PRX/IPL/PSAR (the EBOOT path). `tools/pgd_decrypt.py` is a dependency-free
decryptor for it: independently expressed AES-128 built from the field definition
(no copied tables, NIST-self-checked) plus a derived-translated KIRK cmd4/7 + amctrl
BBMac/BBCipher/PGD flow using locally supplied PSP platform data. See the
[source archaeology](provenance/PGD_AMCTRL_SOURCE_ARCHAEOLOGY_2026-08-09.md). The runtime path does not
use a per-device fuse key. Any game-specific value remains a private local input
and must not be committed, logged, pasted into issues, or placed in CI configuration.

```bash
python tools/pgd_decrypt.py --selftest                          # AES known-answer test
python tools/pgd_decrypt.py <file> <out> --vkey <32hex> --info  # verify header MACs only
```

Verification is intrinsic (the first header MAC uses the locally supplied fixed
PSP platform data, so a correct implementation reproduces it without the title
version key). Pure-Python AES is slow, so this is the verified reference/oracle;
the runtime decrypt path is a C port of the same algorithm (see the
[2026-07-18 history](STATUS_HISTORY.md)). Guarded by
`tools/test_pgd_decrypt.py`. Real-file tests read the private value from
`HST_PGD_VKEY_HEX` and skip when it or `GAMEDATA.BDL` is unavailable; synthetic
AES tests always run.
