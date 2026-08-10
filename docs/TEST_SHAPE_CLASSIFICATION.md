# Source-shape evidence classification

`docs/TEST_MATRIX.json` is generated from the discovered case IDs. Its
source-shape labels are conservative triage metadata, not semantic proof: each
case is marked for source inspection before a test is merged, migrated, or
removed.

The integrated report reviews all 53 files with source/structure indicators.
Mixed source/behavior modules are included in the file-level inventory even when
their executable cases are classified separately. The counts below show both
reviewed files and discovered cases, measured at 712 discovered cases:

| Category | Reviewed files | Cases | Disposition | Meaning |
| --- | ---: | ---: | --- | --- |
| A — legitimate structural invariant | 46 | 389 | `KEEP` | Manifest, provenance, publication, CI, generated-output, or other structure whose shape is itself externally meaningful. |
| B — historical behavioral proxy | 4 | 35 | `RETIRE_WITH_BEHAVIORAL_SEAM` | Callback/UMD/progress/manager checks that currently substitute source markers for production behavior; retained while #76/#181 behavioral seams remain incomplete. |
| C — redundant behavioral proxy | 0 | 0 | `DELETE_CANDIDATE` | No deletion is justified without a stronger production test and mutation evidence. |
| D — only available evidence | 3 | 9 | `KEEP_UNTIL_EXECUTABLE_SEAM` | Codegen/retail-output structure for which no equivalent executable seam is currently proven. |
| E — obsolete invariant | 0 | 0 | `DELETE_CANDIDATE` | No contract was shown to be superseded by current issue/history evidence. |

Cases with no source-shape indicator are `NOT_APPLICABLE`/`KEEP` (279 of 712).

## Reading the `disposition` field

`disposition` in [`TEST_MATRIX.json`](TEST_MATRIX.json) previously restated
`source_shape_classification`: every classified case was emitted as `UNKNOWN`.
That inverted the intended reading — the `UNKNOWN` set was exactly the set of
cases that *had* been categorised, and its size (433 of 712) invited the
conclusion that most of the suite was unreviewed. It never meant that.

The field now carries the review outcome, so the actionable backlog is legible
directly:

| Disposition | Cases | Action |
| --- | ---: | --- |
| `KEEP` | 668 | none |
| `RETIRE_WITH_BEHAVIORAL_SEAM` | 35 | retire once the #76/#181 behavioral seams land |
| `KEEP_UNTIL_EXECUTABLE_SEAM` | 9 | keep until an executable seam exists |
| `DELETE_CANDIDATE` | 0 | none exist; C and E remain the deletion boundary |

The 35 category-B cases are `test_callback_correctness` (15),
`test_progress_tracker` (14), `test_hle_umd_wakeup` (4) and
`test_manager_symbol_docs` (2). The 9 category-D cases are
`test_codegen_continuations` (4), `test_codegen_no_shadow_stubs` (3) and
`test_codegen_retail_allocator` (2). A non-`KEEP` disposition is a review
pointer, not authority to delete a test.

The 51-file count is a generated review set, not a claim that all source
inspection has been exhausted. Assertions inside mixed behavioral/structural
files can receive different evidence grades after manual review; the ten mixed
files are retained at file level without promoting every case to source-shape
evidence. No test is deleted by this report, and no source-shape assertion is
described as PSP semantic proof.
