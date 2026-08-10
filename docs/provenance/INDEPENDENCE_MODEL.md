# Independence model

**Status: engineering provenance framework, not legal advice.** This defines how Nakagawa
classifies the origin of its own implementation, how a classification may change, and what evidence
each change requires. It does not clear anything for publication; that remains
[#98](https://github.com/Jstar269/nakagawa-recomp/issues/98),
[#99](https://github.com/Jstar269/nakagawa-recomp/issues/99),
[#102](https://github.com/Jstar269/nakagawa-recomp/issues/102),
[#104](https://github.com/Jstar269/nakagawa-recomp/issues/104) plus qualified human review.

Companions: [NOTICE.md](../../NOTICE.md) (what the project *tells the world*),
[LEGAL_REWRITE_ASSESSMENT.md](../LEGAL_REWRITE_ASSESSMENT.md) (risk posture),
[IMPLEMENTATION_PROVENANCE.json](IMPLEMENTATION_PROVENANCE.json) (the machine-readable ledger),
[INDEPENDENCE_BACKLOG.md](INDEPENDENCE_BACKLOG.md) (what to do next).

## Goal, stated precisely

The objective is **not** to stop crediting PPSSPP, and it is **not** to make derived code look
original. It is to reach a state where each of these sentences is either true and provable, or
explicitly marked false:

1. *Nakagawa does not require PPSSPP source or an installed PPSSPP binary to build or run.*
2. *For subsystem X, the shipped implementation is authored from documented PSP behavior,
   measured hardware, and source-owned tests.*
3. *Where (2) is false, the file, the ledger, and NOTICE.md all say so, and the upstream notice is
   preserved.*

**Sentence 1 must be stated with its scope or it is an overclaim.** Measured at the audit commit:

| activity | needs PPSSPP? | why |
| --- | --- | --- |
| build the runtime | no | nothing in the build reads `third_party/ppsspp*` |
| run the game | no | `driver.c` accepts `none` for the reference-trace argument |
| `make verify` | **yes** | the codegen and microtest gates consume PPSSPP-captured golden traces and report BLOCKED without them |
| regenerate `src/rt/nid_names.h` | no *(was yes)* | IND-1 replaced the PPSSPP scrape with the tracked `tools/nid_corpus.json` |

So the honest present-tense claim is "build and run", not "build, run and verify". One `yes` row
remains. Retiring it is a longer-term question for the oracle gates — the trace *format* is
documented in `tools/TRACE_FORMAT.md` and consuming an oracle is not derivation, but a gate that
cannot run without upstream output is still an upstream dependency.

Sentence 2 is true for a minority of the runtime today. Sentence 3 is the invariant this ledger
exists to hold.

**Never claim "clean room" or "zero PPSSPP influence".** Those are terms of art about a development
process this project did not follow and cannot retroactively adopt. The defensible claim is
narrower and still valuable: *this unit's behavior is independently established, and its expression
is project-authored.*

## Classification vocabulary

Exactly one of these applies to each ledger record. They are not a severity scale and must not be
collapsed.

| Classification | Meaning |
| --- | --- |
| `project-authored-independent` | Written from public ABI/spec facts, hardware measurement, PSPAutotests, source-owned tests, or mathematical derivation. No upstream implementation was translated or adapted. |
| `behavior-informed` | Project-authored expression, but another implementation (usually PPSSPP) was consulted, compared against, or used to *decide what the behavior is*. Not the same as derived code — and not automatically safe to relabel. |
| `derived-translated` | Protectable implementation, structure, algorithmic expression, or substantial logic translated or adapted from an upstream implementation. |
| `derived-data` | Tables, assets, or data whose bytes or whose selection originate in another project. |
| `generated-project-owned` | Deterministically generated from project-owned mathematical or source definitions; regenerable from inputs this project controls. |
| `upstream-third-party` | Ordinary external dependency or vendored component, license and provenance retained. |
| `unresolved` | Evidence is insufficient to classify honestly. This is a real answer, not a placeholder to avoid. |

### The distinction that does the work

These are **not** equivalent, and conflating them is how a project talks itself into a false
independence claim:

- *"PPSSPP does X, so translate its implementation of X."* → `derived-translated`.
- *"PPSSPP does X; hardware and PSPAutotests independently establish X; implement X from the
  behavioral contract."* → `behavior-informed`, or `project-authored-independent` once the upstream
  reference is no longer load-bearing.

A citation like `/* matches PPSSPP Clipper::ProcessTriangle */` is evidence **against** independence
until an equivalent hardware or specification citation replaces it. Do not delete such comments to
improve a classification. Earn the replacement citation first, then change both together.

### AI assistance is provenance-neutral

Project policy discloses AI assistance ([docs/AI_USAGE.md](../AI_USAGE.md)). For this ledger, model
output is treated exactly like human output:

- *"Here is PPSSPP's function; rewrite it so it looks original."* is `derived-translated`, whoever
  or whatever typed it.
- *"Here is the hardware/ABI/test contract; implement it."* can be `project-authored-independent`,
  subject to the same review as human work.

If upstream source was in the authoring context, the record says so. That fact is recorded in
`behavior_sources` and, where it matters, in `uncertainty`.

## Evidence tiers for independence claims

Reuses the repository's existing evidence discipline (see [AGENTS.md](../../AGENTS.md)); these are
the *provenance* analogues.

| Tier | Evidence | Sufficient to claim |
| --- | --- | --- |
| H | Measured on real Allegrex hardware via PSPLINK, with model/firmware/PRX digest recorded | `project-authored-independent` for the measured contract |
| S | Public specification, PSP ABI fact, PSPSDK declaration, or mathematical identity that can be recomputed by a third party | `project-authored-independent` for the specified contract |
| A | PSPAutotests-derived expected result, where license permits its use | `project-authored-independent` for the covered cases |
| R | Renderer/interpreter agreement, differential fuzz, or PPSSPP comparison | `behavior-informed` only — **never** sufficient for an independence claim |
| N | No independent evidence; behavior is believed because upstream does it | `derived-translated` or `unresolved` |

Tier R localizes a divergence. It does not establish what the PSP does. Renderer agreement between
`ge.c` and `ge_gpu.c` proves the two agree with each other, and both descend from the same upstream
model.

## Replacement process

Wholesale rewrites are forbidden. Each candidate moves through five phases, and a phase may not be
skipped because the unit "looks obvious".

**Phase A — behavioral specification.** Write the guest-visible contract from independent evidence:
inputs, outputs, error values and their precedence, state transitions, edge cases, and any
guest-observable ordering or timing. If the contract cannot be written without consulting upstream
implementation, the candidate is not ready; file a hardware question instead.

**Phase B — source-owned regression corpus.** Tests must not simply assert agreement with PPSSPP.
Acceptable: PSPLINK hardware oracle records, PSPAutotests-derived expectations where licensing
permits, PSPSDK ABI declarations, mathematical identities, synthetic or homebrew fixtures built from
source this project owns.

**Phase C — implementation.** Implement from the Phase A contract. Do not place the upstream
function beside the new one and paraphrase it. If upstream source is consulted during
implementation, record that truthfully — the result is `behavior-informed`, not
`project-authored-independent`.

**Phase D — provenance review.** Check for accidental structural copying where practical (identical
helper decomposition, identical constant ordering, identical control-flow shape with renamed
identifiers). Record: authoring process, behavior sources, upstream source consulted or deliberately
not consulted, tests, hardware fixtures, and the commit that replaced the old code.

**Phase E — attribution retirement.** A classification may change **only** when protectable derived
implementation is genuinely gone from the shipped file. Historic attribution is never deleted merely
because current code is new: `NOTICE.md` and Git history must continue to explain the lineage
accurately, and the ledger keeps the prior classification in `replacement_state`.

## Prioritization

Candidates are ranked by

```text
independence leverage x confidence x evidence availability / replacement risk
```

- **Independence leverage** — does this remove a real derived unit, or retire a required upstream
  dependency? A PPSSPP name in a comment is not leverage by itself.
- **Confidence** — can the contract be stated completely today?
- **Evidence availability** — does hardware/spec/test evidence exist now, or must it be collected?
- **Replacement risk** — blast radius, guest-visible surface, and how a regression would present.

Deliberately *low* priority regardless of score: anything owned by another active agent lane,
anything blocked on unresolved licensing (`pgf.c` while #98 is open), and anything requiring
speculative PSP semantics.

## Rules that override the score

1. Do not change `assets/vfpu/*.dat` to remove PPSSPP provenance. Bit-exactness is the requirement;
   provenance cosmetics are not. If independent regeneration cannot be proven, the ledger says so.
2. Do not generate replacement fonts and call #99 solved.
3. Do not touch a subsystem another agent lane currently owns. Audit it, record it, defer.
4. Do not run history rewriting. #102 owns that.
5. Do not remove or weaken an upstream notice on the strength of this engineering audit alone.
6. Never invent a `Signed-off-by:` identity.

## Ledger maintenance

[IMPLEMENTATION_PROVENANCE.json](IMPLEMENTATION_PROVENANCE.json) is the machine-readable record.
`tools/test_provenance_ledger.py` enforces that:

- every tracked file under `src/` is covered by exactly one record (directly or by a directory
  record);
- every classification is in the closed vocabulary above;
- every file whose header declares `Derived from <project>` has a record whose classification is
  `derived-translated`, `derived-data`, `behavior-informed`, or `unresolved` — never
  `project-authored-independent`;
- every record naming an upstream project also carries a license field.

The gate catches drift; it cannot catch a dishonest record. Classification remains a human judgment
that the ledger merely makes auditable.
