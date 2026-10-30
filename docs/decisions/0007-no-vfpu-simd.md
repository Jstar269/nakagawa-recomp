# 0007: VFPU SIMD fast paths are not pursued

Date: 2026-10-29
Status: Accepted (measured)

## Context

Issue #292 proposed host SIMD fast paths for VFPU instructions. VFPU semantics are verified bit for bit against the
hardware tables, so any fast path would need the same proof, and it only pays off if VFPU work is a real share of
run time.

## Decision

No VFPU SIMD fast paths. The scalar, table-exact VFPU translation stays the only path.

## Consequences

- The measured share is negligible: on HST's routes, 796 VFPU instructions in about 932 million translated
  instructions, and under 0.1 ms of 75.6 s of translated-code time.
- Performance work goes where the time is, such as the GE.
- The question can reopen if a title's measured VFPU share is material.

## Links

- Issue [#292](https://github.com/Jstar269/nakagawa-recomp/issues/292); measurement in
  [#1650](https://github.com/Jstar269/nakagawa-recomp/pull/1650) (issue
  [#282](https://github.com/Jstar269/nakagawa-recomp/issues/282)).
