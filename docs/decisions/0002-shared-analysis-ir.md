# 0002: One analysis IR feeds both backends

Date: 2026-10-29
Status: Accepted (owner)

## Context

The analyzer decides function boundaries, ownership, delay slots, continuations and indirect transfers. A second
backend could re-derive those facts from the MIPS bytes, but two derivations would drift apart and disagree in
ways that are hard to see.

## Decision

The C emitter and the LLVM IR backend consume the same analysis facts: the canonical CFG report and the analysis
IR built from it (`tools/analysis_ir.py`). Neither backend rediscovers what the analyzer owns.

## Consequences

- A fix to function discovery or ownership reaches both backends at once.
- `analysis_ir.py verify` checks that the IR matches the analyzer's ownership exactly, so a backend can rely on it.
- External annotations (Ghidra, decomp.me, manual notes) are compared with the analyzer and reported, but never
  change a fact either backend sees.

## Links

- Issue [#305](https://github.com/Jstar269/nakagawa-recomp/issues/305) and
  [#1642](https://github.com/Jstar269/nakagawa-recomp/pull/1642); backend use in
  [#1663](https://github.com/Jstar269/nakagawa-recomp/pull/1663).
- [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md), "ProgramImage v1 and CFG ownership observation v1".
