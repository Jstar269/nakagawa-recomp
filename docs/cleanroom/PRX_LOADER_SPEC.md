# Clean-room behaviour specification — runtime PSP module (PRX) loader

Status: spec — no code in this document. Written under the campaign protocol
(`../INDEPENDENCE_CAMPAIGN.md` §5) as a **spec-session** output for rewrite unit
**G3** (verdict (a), campaign §4). The spec session read the derived loader slice
(`tools/prxload.py`, sal063 lineage, ledger record `recompiler-pipeline-inherited`),
a derived C port of it that was discarded unadmitted, and the public sources in §9;
nothing from the derived files is reproduced here beyond the `SEAM:` identifiers §2
defines and the ABI facts §9 cites.

## 0. Scope, workspace, and reading rule

**In scope.** Loading an already-decrypted PSP module image (a relocatable ELF32
module, ELF type `0xFFA0`, or an ordinary ELF32 relocatable/executable image) at a
caller-chosen base address in guest memory: laying out its loadable segments,
zero-filling uninitialised tails, applying the two PSP relocation formats, and
reporting the module's identity, entry point, exported functions, and import stubs so
the runtime can link and start it.

**Out of scope.** Decryption and decompression of `~PSP` containers (a separate
future item beside KIRK, campaign §4 G3), choosing the base address (the title
manifest's `modules[].load_address`, validated by `tools/title_manifest.py`),
reserving guest memory (the allocator's exact-address reservation), running
`module_start`, and resolving imports to handlers (the runtime's HLE registry and
late-import table). The loader *reports*; callers act.

**Reading rule.** Every numbered behaviour cites `[Cn]` from §9 or is marked
`project decision` with its rationale. A fact with neither is a defect in this spec.

**Workspace allow-list** (campaign §5.2; export exactly these, no `.git`):
this spec only. The implementation creates `src/rt/prx_loader.h`, `src/rt/prx_loader.c`,
`tools/test_prx_loader_cleanroom.py` and its fixture builder from it. Explicitly **not** exported: `src/rt/recomp.h`, `tools/prxload.py`,
`tools/imports.py`, `tools/analyze.py`, `tools/codegen.py`, any `src/rt/prx_loader.c`
from repository history, and `src/rt/hle.c`.

## 1. Component role

The recompiler translates a late module's code ahead of time at the manifest base.
At run time the guest's data for that module must exist at the same addresses:
initialised data, relocated pointers, zeroed BSS, the module-info block, and the
export/import tables the guest itself reads. This component produces that memory
image and the metadata the runtime needs; it is the runtime twin of the build-time
relocation step and must produce byte-identical images for the same input and base.

## 2. Normative interface (`SEAM:` identifiers)

All identifiers in this section are defined by this spec.

- `SEAM: sr_prx_load(host_path, base, out, err, errlen)` — load from a host file.
- `SEAM: sr_prx_load_from_memory(data, size, base, out, err, errlen)` — same from a
  caller buffer (tests, embedded images).
- Both return 0 on success and a nonzero value on any failure; on failure `err`
  receives a one-line human-readable reason (truncated to `errlen`) and **no guest
  memory has been written** (§3.9).
- `SEAM: SrPrxImage` — result record with: module name (from module info, up to 27
  characters plus terminator), module attribute and version, `gp` value, entry
  address, image start/end, per-segment records (file offset, guest address, file
  size, memory size), module-info guest address, export list, import-stub list.
- `SEAM: SrPrxExport` — library name (or empty for the system library), NID, guest
  address, and whether it is a function or a variable.
- `SEAM: SrPrxImportStub` — library name, NID, stub guest address.
- `SEAM: sr_prx_image_free(out)` — releases any host memory the record owns.
- `SEAM: sr_prx_guest_write(guest_addr, src, n)` — the only way the loader touches
  guest memory: it copies `n` host bytes to guest memory and returns 0, or nonzero
  when the range is not writable guest memory. The runtime provides it on top of its
  guest-memory translation [C8]; tests provide their own. The loader includes no
  runtime header (project decision: keeps the component self-contained and testable).
- A failed `sr_prx_guest_write` fails the load; earlier writes of the same load are
  not undone, so callers reserve and validate the range first (§3.9 keeps all
  validation before the first write).

Field order, helper structure, and internal names are the implementer's own.

## 3. Behaviour

### 3.1 Input recognition

1. The input must start with the ELF magic `0x7F 'E' 'L' 'F'`, class ELF32,
   little-endian, machine MIPS (8) [C1], [C2]. Anything else fails.
2. ELF type `0xFFA0` is a relocatable PSP module; types 1 (`ET_REL`) and 2
   (`ET_EXEC`) are accepted as ordinary ELF images [C1], [C3].
3. An input that begins with the `~PSP` container magic is refused with a reason
   naming decryption as out of scope (§0) — never parsed as ELF [C3].

### 3.2 Segment layout

1. Loadable segments are the program headers of type `PT_LOAD` (1), taken in
   program-header order [C1]. At most four are accepted — the firmware's relocation
   segment-index field is at most two bits wide (§3.5.2) [C3].
2. Segment *i* is placed at `base + p_vaddr(i)` for relocatable inputs (types
   `0xFFA0`, `ET_REL`) and at `p_vaddr(i)` unchanged for `ET_EXEC` when `base` is 0;
   `ET_EXEC` with a nonzero base is refused (it has no relocations to rebase with)
   [C1], [C3].
3. `p_filesz` bytes are copied from the file; the remaining `p_memsz − p_filesz`
   bytes are zero-filled [C1].
4. A segment whose file range or memory range overflows 32 bits, exceeds the input
   size, or overlaps another loadable segment's memory range fails [C1]
   (project decision for the overlap rule: overlapping segments would make the
   relocation result order-dependent).
5. Image start is the lowest segment address; image end is the highest
   `address + p_memsz`; the caller reserves `[start, end)` (§0).

### 3.3 Entry point

The entry address is `e_entry + base` for relocatable inputs and `e_entry` for
`ET_EXEC` [C1]. When the export list (§3.7) contains the system-library NID
`0xD632ACDB` (`module_start`), that export's address is reported separately; when it
does not, callers use the entry address as the start routine [C4].

### 3.4 Module information block

1. The module-information block (`SceModuleInfo`) is located by the first loadable
   segment's `p_paddr`: for relocatable modules, `p_paddr` holds the block's file
   offset relative to that segment, so its guest address is `segment 0 address +
   (p_paddr − p_offset(0))` [C3], [C4]. When a section named `.rodata.sceModuleInfo`
   exists, its address must agree; disagreement fails (project decision: two
   sources that disagree mean a malformed module).
2. Block layout [C4]: attribute (16 bits), version (2 bytes), name (28 bytes,
   NUL-padded), `gp` value, export table start and end, import (stub) table start and
   end — pointers are guest addresses **after** relocation.
3. The name is reported up to its first NUL; a name with no NUL in 28 bytes fails.

### 3.5 Relocation — common rules

1. Relocation data comes from program headers of type `0x700000A0` (format A) and
   `0x700000A1` (format B) [C3], [C5]. Section headers of type `0x700000A0` are an
   equivalent source for format A when no such program header exists [C3].
2. Every relocation names an **offset segment** (whose base plus an offset gives the
   site address) and an **address segment** (whose load address is the value
   added). Both indices must be less than the number of loadable segments; the
   segment-index field of format B is one bit wide when there are fewer than three
   loadable segments and two bits wide otherwise [C3].
3. A site whose offset is not below its offset segment's `p_memsz`, or whose 4-byte
   word would extend past the segment, fails [C3].
4. The value added is the address segment's **load address** (`base + p_vaddr` for
   relocatable inputs) [C3].
5. All reads and writes of relocation sites are 32-bit little-endian words in the
   laid-out image [C2].
6. Relocations are applied in stream order; later relocations observe earlier ones'
   results [C3].

### 3.6 Relocation formats

**Format A** — a table of 8-byte records (offset, info), little-endian [C3], [C5]:
info bits 0–7 are the relocation kind, bits 8–15 the offset segment, bits 16–23 the
address segment [C3]. Kinds, with *S* the address segment's load address, *W* the
site word, and sign-extension written `sx16` [C2], [C3]:

| Kind | Name | Result |
| --- | --- | --- |
| 0 | NONE | no change |
| 1 | 16 | low 16 bits := low 16 of (`sx16(W)` + *S*); high 16 unchanged |
| 2 | 32 | *W* + *S* (mod 2³²) |
| 4 | 26 | jump target field := ((region of the **site** address \| (field × 4)) + *S*) ÷ 4, masked to 26 bits; opcode bits unchanged. "Region" is the site address's top 4 bits |
| 5 | HI16 | a run of consecutive HI16 records is followed by one **partner** record, the first record after the run whatever its kind [C3]; for each HI16 of the run, with *W* its own site word, *V* = (high16(*W*) × 65536) + `sx16`(low 16 of the partner's site word) + *S*, and the site's low 16 bits := (*V* + 0x8000) ÷ 65536 masked to 16 bits (the carry-adjusted upper half [C2]); high 16 bits unchanged |
| 6 | LO16 | same as kind 1 |
| 7 | GPREL16 | refused (the firmware has no case for it) [C3] |
| 8 | LITERAL | no change; the firmware logs "unacceptable relocation type" and continues [C3] |
| other | — | refused [C3] |

The carry adjustment is the MIPS ELF convention [C2]; the partner rule is the
firmware's [C3]. A HI16 run that reaches the end of the table with no partner fails.

**Format B** — a compressed stream [C3], [C5]:

1. Header: bytes 0–1 must be zero; byte 2 = *F* (flag-field width in bits); byte 3 =
   *T* (kind-field width in bits). Both must be 1–8 and *F* + *T* + segment width
   (§3.5.2) ≤ 16 [C3].
2. A **flag table** follows at byte 4: its first byte is the table's own length *n_f*
   (including that byte); entries are the bytes after it. A **kind table** follows the
   flag table in the same shape (length *n_k*) [C3]. Index 0 of each table is its
   length byte, so a command selecting index 0 or an index ≥ the table length fails.
3. Commands follow, each a 16-bit little-endian word *c*, possibly followed by
   extension bytes. Fields of *c*, low bits first: flag index (*F* bits); segment
   index (segment width); kind index (*T* bits); the remaining high bits form a
   signed displacement field *d* [C3].
4. The selected flag byte *g* decides the command's meaning [C3]:
   - **Bit 0 clear — set offset segment.** The offset segment becomes the segment
     index. Then, by bits 1–2 of *g*: `00` — the running offset becomes the
     unsigned value of *c* shifted right by (*F* + segment width); `10` (value 4) —
     the running offset becomes the 32-bit little-endian word that follows *c*;
     any other value fails. **Evidence for the `00` rule:** applying it to three
     retail late modules places all 3,327 relocation sites on aligned words and
     every jump-kind site (kinds 6, 7 below) on an existing `j`/`jal` instruction;
     leaving the offset unchanged instead (as a public decompilation reads [C3])
     misplaces 62–73 % of sites per module (maintainer-held measurement, retail
     inputs not in tree; the in-tree black-box fixtures of §8 pin the rule).
   - **Bit 0 set — relocate.** The segment index names the **address segment**; the
     kind index selects the kind from the kind table. The site offset advances by
     bits 1–2 of *g*: `00` — add the sign-extended displacement *d*; `01` (value 2)
     — add a 32-bit signed value whose high 16 bits are *d* and whose low 16 bits
     are the 16-bit word that follows *c*; `10` (value 4) — the offset becomes the
     32-bit word that follows *c*; `11` fails.
   - Then bits 3–5 of *g* select the **low addend** used by kind 4 below: `000` —
     zero; `001` (value 8) — keep the previous addend only if the previous
     relocate command's kind was 4, else zero; `010` (value 16) — a signed 16-bit
     word follows (after any offset extension); any other value fails [C3].
5. Kinds (from the kind table), with *S*, *W*, `sx16` as in format A and *A* the low
   addend [C3]:

| Kind | Result |
| --- | --- |
| 0 | no change |
| 1, 5 | low 16 bits := low 16 of (`sx16(W)` + *S*) |
| 2 | *W* + *S* |
| 3 | jump target field adjusted as format A kind 4; opcode bits unchanged |
| 4 | high 16 bits := ((high16(*W*) × 65536) + `sx16`(*A*) + *S* + 0x8000) ÷ 65536, masked to 16 bits; low 16 unchanged |
| 6 | as kind 3, then the opcode becomes `j` (top 6 bits = 2) |
| 7 | as kind 3, then the opcode becomes `jal` (top 6 bits = 3) |
| other | fails |

   The public decompilation [C3] stores some kinds' results to the value being added
   rather than to the site; this spec writes every result to the site (project
   decision: storing to a segment base would corrupt the module's first word on every
   such relocation and contradicts the same routine's other kinds).
6. The stream ends exactly at the program header's `p_filesz`; a command or extension
   that would read past it fails.

### 3.7 Exports

1. The export table (§3.4) is a sequence of `SceLibraryEntryTable` records [C6]:
   library-name pointer (0 for the system library), version, attribute, record length
   in 32-bit words, variable count, function count, and a pointer to an array of NIDs
   followed by the matching address array (functions first, then variables) [C6].
2. Records are walked by their length field until the table end; a zero length or a
   record crossing the end fails.
3. Every function and variable becomes an `SrPrxExport` with its relocated address.
   System-library entries of note [C4]: `0xD632ACDB` module_start, `0xCEE05613`
   module_stop, `0xF01D73A7` module_info (variable).
4. Library names are read as NUL-terminated strings inside the image; a name not
   terminated within 256 bytes or outside the image fails.

### 3.8 Import stubs

1. The stub table (§3.4) is a sequence of `SceLibraryStubTable` records [C6]: library
   name, version, attribute, length in words, variable count, function count, NID
   array pointer, stub array pointer (and variable table pointer when present).
2. Function stub *k* of a record is 8 bytes at stub array + 8·*k*; each becomes an
   `SrPrxImportStub` (library, NID, stub address) [C6].
3. The loader does not rewrite stub instructions (linking is the caller's job, §0).

### 3.9 Failure policy

1. Validation and relocation happen in a host-side scratch copy of the image; guest
   memory is written only after every step has succeeded (project decision: a
   half-written module is worse than none, and the caller's reservation may be
   released on failure).
2. No input — truncated, malicious, or oversized — may cause an out-of-bounds host
   access, an unbounded loop, or a crash [C7].
3. Failure reasons name the first failing rule (e.g. "format B flag index 0").

## 4. Non-requirements

Decryption/decompression; GP-relative relocation; firmware version checks; module
attribute enforcement (kernel/user); the firmware's own memory placement policy
(the base is an input); resolving imports.

## 5. Clean-room constraints for this item

- The implementation session receives only the §0 allow-list. It must not read
  `tools/prxload.py`, `tools/imports.py`, any other loader in history, or the public
  decompilation [C3] itself — [C3] facts reach it only through this spec.
- The `SEAM:` identifiers in §2 are the only names it must use; decomposition and
  internal naming are its own.
- The derived loader may serve as a black-box oracle (same input and base in, compare
  image bytes out) in the admission review, never as a reference during writing.

## 6. Similarity admission for this item

Candidate: the new `src/rt/prx_loader.c`/`.h`. References: `tools/prxload.py` and the
discarded derived C port (kept outside the tree by the maintainer). Report
token-trigram overlap, longest identical run, and constant-table orderings per campaign
§5.1 item 3 and attach it to the provenance record.

## 7. Provenance record fields (fill at admission)

Classification proposed: `project-authored-independent`. Behaviour sources: this
spec plus [C1]–[C8]. Upstream consulted by the spec session: yes (derived `tools/prxload.py`,
the discarded port, and the public decompilation [C3]); by the implementation
session: no (workspace inventory attached). Maintainer attestation: (maintainer
signs; agents do not).

## 8. Black-box test plan (what the implementation session receives)

The implementation session authors, from this spec alone, a source-owned fixture
builder (Python) that emits synthetic modules and tests that load them through the §2
interface (as `SCHED_SPEC.md` §8 allows for tests authored at implementation time):

1. Format A: every kind in the §3.6 table on known words, including a HI16 run of
   three sharing one partner, a partner of a kind other than LO16 (still the partner),
   the carry case (partner low half ≥ 0x8000), kind 7 (fails), kind 8 (no change), and
   a HI16 run with no partner (fails).
2. Format B: segment width 1 and 2; flag bits `00`/`01`/`10` offset forms; the
   set-offset-segment `00` rule with nonzero displacement bits (pins §3.6-B.4);
   addend modes `000`/`001` (after kind 4 and after another kind)/`010`; kinds 1–7;
   index 0 and out-of-range indices (fail); nonzero header bytes 0–1 (fail);
   truncated extension words (fail).
3. Layout: two segments with BSS tails (zero-filled), overlapping segments (fail),
   five loadable segments (fail), `ET_EXEC` at base 0 and at a nonzero base (fail),
   `~PSP` input (fail with the decryption reason).
4. Module info via `p_paddr` and via the section, agreeing and disagreeing (fail);
   name without NUL (fail).
5. Exports: system library with module_start/module_stop/module_info, a named
   library with functions and variables, a zero-length record (fail).
6. Imports: two libraries' stubs reported with correct stub addresses.
7. Failure atomicity: every failing case leaves a guard-filled guest arena unchanged.
8. Fuzz: 10,000 random mutations of a valid fixture under a time bound never crash
   or read outside the input.
9. Build on Windows (MinGW gcc) and Linux gcc with `-Wall -Wextra -Werror`.

Admission-time checks run by the maintainer's session (not given to the
implementation session): byte parity with the derived loader on the retail late
modules at their manifest bases, and the in-game run of the late-module route.

## 9. Citation catalog

- [C1] ELF: *Tool Interface Standard (TIS) Executable and Linking Format (ELF)
  Specification, Version 1.2* — <https://refspecs.linuxfoundation.org/elf/elf.pdf>
  (headers, program headers, `PT_LOAD`, segment file/memory sizes, entry).
- [C2] MIPS ELF: *System V Application Binary Interface, MIPS RISC Processor
  Supplement, 3rd Edition* — <https://refspecs.linuxfoundation.org/elf/mipsabi.pdf>
  (relocation kinds 16/32/26/HI16/LO16, HI16/LO16 pairing and carry adjustment, word
  encoding).
- [C3] uofw (unofficial PSP firmware reimplementation), `src/kd/loadcore/loadelf.c`
  at commit `5e192e75a83d043d5a65db21128d62758e5741f5` —
  <https://github.com/uofw/uofw/blob/5e192e75a83d043d5a65db21128d62758e5741f5/src/kd/loadcore/loadelf.c>
  (PSP relocation program-header types, format A record fields, format B header,
  tables, command fields, segment-width rule, kind behaviours, refusal of LITERAL,
  `~PSP` handling). Read by the spec session only; divergences from it are recorded
  in §3.6 with evidence.
- [C4] PSPSDK public headers at commit `654ac51fc73fbf7ad9350fac90e39e66e7a0b1c6`
  (<https://github.com/pspdev/pspsdk>): `src/user/pspmoduleinfo.h` (`SceModuleInfo`
  layout), `src/kernel/psploadcore.h` (module structures), and the system-library
  NIDs for module_start / module_stop / module_info as used by PSPSDK's
  `tools/psp-build-exports.c`.
- [C5] PSP Developer Wiki, *PRX File Format* —
  <https://www.psdevwiki.com/psp/PRX_File_Format> (names the two relocation segment
  types and the 4-byte header of the compressed one).
- [C6] PSPSDK `src/kernel/psploadcore.h` at the [C4] commit: `SceLibraryEntryTable`
  and `SceLibraryStubTable` field order and widths.
- [C7] `AGENTS.md` §6 — fail-closed evidence rules; malformed input fails visibly.
- [C8] `src/rt/recomp.h` — guest-memory translation and accessors (given, unchanged).
