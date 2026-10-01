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

`verify_gates.py` keeps v1 traces available to the existing codegen and microtest
comparisons. Because v1 has no evidence-tier field, the verifier reports a v1 trace as
`CORROBORATIVE_ONLY`; it never satisfies the hardware gate.

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
two inputs. This permits a PSP capture to be compared with a source-owned
`LOCAL_COSIM` trace while keeping the evidence tier visible. `verify_gates.py`
reports the validated v2 tier from the strict loader. A
`PPSSPP_CORROBORATIVE` trace is reported as `CORROBORATIVE_ONLY` and cannot
satisfy the hardware gate. That gate reports `STRICT_V2_AGREEMENT` (not a hardware
measurement) when a `PSP_HARDWARE` + `LOCAL_COSIM` pair passes `strict_hardware_diff`;
with no pair supplied it is an optional gate reported as `NOT_RUN`, and with only one
trace supplied it fails. The PSP-side v2 trace producer is in the works under
issue #312 and is not provided by this verifier change.

A strict match proves that the two complete, identically identified streams
agree. The gate's evidence-tier result comes from the `source_tier` metadata;
the comparator does not create or attest to the trace's provenance.

Strict v2 records use the same per-step fields as v1 and additionally reject
malformed PCs, opcodes, register names, memory-write names, values, duplicate
writes, gaps, and out-of-order step numbers.  When records diverge, the
comparator reports the first step and PC plus the differing opcode or the
register/memory write context.

Every contract violation is a named `REJECTED: <path>:<line>: <reason>` line
with exit status 2.  A violation that belongs to the whole stream rather than
one line (an unreadable file, text that is not valid UTF-8, or a stream with
no v2 header) is reported as `REJECTED: <path>: <reason>` without the line
component.  Decimal metadata (`steps` and a record's step index) is
length-bounded before conversion, so an oversized, signed, negative, or
otherwise unusable value is refused by name instead of raising an interpreter
conversion error, and a stream that is not valid UTF-8 text is refused by name
too.

## Strict local trace comparison

The gates that run without any PSP metadata cannot borrow the v2 envelope, so
they opt into a separate local contract instead:

```text
tracediff.py --strict-local <trace-a> <trace-b> [--expect-steps N]
```

`--strict-local` requires, on both inputs, a `# psp-recomp trace` identity header
as the first comment line (an arbitrary comment header, a missing header or a
second identity header is rejected), at least one step record (an empty or
header-only stream is never evidence of equivalence), and step records numbered
contiguously from zero, so duplicates, gaps and reordering are rejected by name.
When both headers declare `start_pc`, they must agree. The two streams must always
carry the same number of records; a pair of different lengths is rejected as
`step count differs` even without `--expect-steps`. `--expect-steps N` adds the
caller's own required length: a stream with fewer than `N` records is rejected as
incomplete coverage and one with more is rejected as over-coverage, so equal
lengths cannot substitute for declared coverage. `N` must be a positive decimal
count.

`codegen_gate.py` and `microtest_gate.py` are the callers: each proves the oracle
supplied every record below the exit syscall before truncating, refuses a fixture
whose exit syscall is at step 0 (a non-semantic probe, never a match), and passes
the required record count to the comparator. The default two-argument
invocation keeps its permissive v1 equality behavior for maintained informational
callers; it is not proof that execution matched.
