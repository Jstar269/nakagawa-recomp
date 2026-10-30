# Decision records

Each page here records one settled choice: the context, the decision, what follows from it, the date and where it
was made. Read the record before reopening a question. A decision changes only through a new record that says
which one it replaces; the old record then names its successor.

| Record | Decision | Date |
| --- | --- | --- |
| [0001](0001-llvm-ir-backend.md) | The direct backend for #330 lowers to LLVM IR | 2026-10-29 |
| [0002](0002-shared-analysis-ir.md) | One analysis IR feeds both the C and the LLVM backends | 2026-10-29 |
| [0003](0003-lle-first.md) | LLE first; HLE only at the kernel and hardware boundary | 2026-09-16 |
| [0004](0004-per-path-provenance.md) | Provenance is recorded per path | 2026-10-29 |
| [0005](0005-non-retail-discs.md) | Non-retail discs are accepted, labelled and keyed by content hash | 2026-10-29 |
| [0006](0006-register-promotion-opt-in.md) | Register promotion stays opt-in | 2026-10-29 |
| [0007](0007-no-vfpu-simd.md) | VFPU SIMD fast paths are not pursued | 2026-10-29 |

## Writing a record

Copy the shape of an existing page: a title, `Date:` and `Status:` lines, then Context, Decision, Consequences and
Links. Keep it to one page. Number records in order and add a row to the table above.
