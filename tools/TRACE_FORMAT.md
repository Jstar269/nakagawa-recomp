# CPU-state trace format

## Per-step line

```text
<step> pc=<hpc> op=<hword> <reg-writes> <mem-writes>
```

- `<step>`     decimal step index, starting at 0, monotonically increasing by 1.
- `pc=<hpc>`   the guest program counter of the instruction, `0x` + 8 lowercase hex digits.
- `op=<hword>` the 32-bit instruction word, `0x` + 8 lowercase hex digits.
- `<reg-writes>` zero or more register-change tokens, space-separated, in this order:
  general-purpose `r<n>` (n in 1..31; r0 is never written), then `hi`, `lo`, then float
  `f<n>` (n in 0..31), then `fcr31`, then VFPU `v<n>` (n in 0..127). Each token is
  `name=0x<8 hex>`. Only registers whose value changed in this step are listed. Float and
  VFPU values are the raw 32-bit IEEE-754 bit pattern, not a decimal rendering, so the
  comparison is exact and not subject to printf rounding.
- `<mem-writes>` zero or more memory-write tokens, space-separated, ascending by address:
  `m<size>[<haddr>]=0x<hex>` where `<size>` is 8, 16, or 32, `<haddr>` is `0x` + 8 hex
  digits (guest virtual address), and the value has 2, 4, or 8 hex digits matching the size.

Empty groups contribute no tokens (no trailing spaces). A step that changes nothing but the
PC is still emitted (it has `pc=` and `op=` and no write tokens), so step counts line up.

## Example

```text
0 pc=0x08900100 op=0x27bdfff0 r29=0x09ffff00
1 pc=0x08900104 op=0xafbf000c m32[0x09ffff0c]=0x08900200
2 pc=0x08900108 op=0x8c880000 r8=0x0000002a
```

## Header

The first line of a trace file is a header comment, ignored by the diff tool except that
both files must carry one:

```text
# psp-recomp trace v1 target=<name> oracle=<ppsspp|interp|recomp> start_pc=<hpc> steps=<N>
```

The diff tool only compares the step lines, ignoring this header.

## Strict hardware trace v2

The default `tracediff.py <trace-a> <trace-b>` invocation keeps the v1
local/cosimulation behavior above.  The opt-in strict route is selected with
`tracediff.py --strict-hardware <trace-a> <trace-b>` and requires a complete
v2 identity envelope on both inputs:

```text
# psp-recomp trace v2 source_tier=PSP_HARDWARE fixture_id=branch_delay_v1 cell_id=case_0001 binary_sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef source_commit=0123456789abcdef0123456789abcdef01234567 model=PSP-3000 firmware=6.61 start_pc=0x08900100 steps=3 complete=1
```

The v2 fields are exact and unknown fields are rejected:

| Field | Contract |
| --- | --- |
| `source_tier` | `PSP_HARDWARE`, `LOCAL_COSIM`, or `PPSSPP_CORROBORATIVE`; strict comparison requires at least one `PSP_HARDWARE` input and refuses `PPSSPP_CORROBORATIVE`. |
| `fixture_id`, `cell_id` | Source-owned fixture and exact case identity. |
| `binary_sha256` | Lowercase SHA-256 of the executed guest image or PRX. |
| `source_commit` | Lowercase Git object ID for the source used to build the image. |
| `model`, `firmware` | Measured PSP identity; placeholder values such as `unknown` are rejected. |
| `start_pc` | Lowercase eight-digit guest entry PC. |
| `steps` | Positive declared bound, at most 1,000,000, written as at most seven decimal digits; the stream must contain exactly this many records numbered from zero. |
| `complete` | Must be `1`; a short or over-budget stream is rejected before comparison. |

A strict stream carries exactly one identity header.  The first `#`-prefixed
line must be the v2 envelope, and any later `# psp-recomp trace` header line is
rejected as a duplicate, whether it appears before or after the step records and
whether or not its metadata conflicts with the first header.  Other comments
that carry no trace identity are ignored, as they are by the v1 comparator.

All identity fields other than `source_tier` must match exactly between the
two inputs.  This permits a PSP capture to be compared with a source-owned
`LOCAL_COSIM` trace while keeping the evidence tier visible.  A PPSSPP trace
can still be used with the v1 comparator for corroboration, but it cannot
satisfy the strict PSP hardware route.  A strict match therefore proves only
that the two complete, identically identified streams agree; it does not by
itself create physical PSP evidence.

Strict v2 records use the same per-step fields as v1 and additionally reject
malformed PCs, opcodes, register names, memory-write names, values, duplicate
writes, gaps, and out-of-order step numbers.  When records diverge, the
comparator reports the first step and PC plus the differing opcode or the
register/memory write context.

Every contract violation is a named `REJECTED: <path>:<line>: <reason>` line
with exit status 2.  Decimal metadata (`steps` and a record's step index) is
length-bounded before conversion, so an oversized, signed, negative, or
otherwise unusable value is refused by name instead of raising an interpreter
conversion error, and a stream that is not valid UTF-8 text is refused by name
too.
