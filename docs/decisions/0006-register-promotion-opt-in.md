# 0006: Register promotion stays opt-in

Date: 2026-10-29
Status: Accepted (measured)

## Context

`codegen.py --promote-registers` keeps guest registers in C locals inside runs of simple instructions and writes
them back at every boundary. The bar for making it the default was set before measuring: every route still passes,
and HST's guest CPU time improves by at least 5% at its qualified `-O1`.

## Decision

Promotion stays off by default. It remains available as an option and is checked by `cosim-selftest-promoted`.

## Consequences

- Correctness held: the promoted cosimulation matched the baseline, production-smoke passed, and HST and
  PAC-MAN CE passed their routes.
- The gain was small: about 1.3% of HST's guest time outside the GE, and about 0.25% of frame time. Titles built
  at `-O0` ran about 11% slower.
- The next step to measure is dropping write-backs of registers that are dead at the boundary.

## Links

- Issue [#284](https://github.com/Jstar269/nakagawa-recomp/issues/284) and
  [#1643](https://github.com/Jstar269/nakagawa-recomp/pull/1643).
- [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md), "Local register promotion".
