# 0003: LLE first; HLE only at the kernel and hardware boundary

Date: 2026-09-16
Status: Accepted (owner)

## Context

A game ships modules of its own (font, movie and codec libraries) on its disc. Nakagawa can replace their
behaviour with host code (HLE), or run the game's own code and model only what lies below it (LLE). HLE is quicker
to write, but every replaced library is a second implementation that can differ from the real one.

## Decision

Run guest code wherever that needs no firmware: a game's own modules execute as translated code. HLE stays only
at the kernel and hardware boundary, where the PSP's own system software would otherwise be required. Firmware is
never decrypted or run.

## Consequences

- Host replacements of game-shipped libraries shrink over time instead of growing; each one carries an issue to
  move it to LLE.
- Hardware behaviour below the boundary is measured on a real PSP and labelled synthetic until it is.
- Work that only adds HLE coverage ranks below work that makes more guest code run.

## Links

- [`docs/LLE_FIDELITY_ARCHITECTURE.md`](../LLE_FIDELITY_ARCHITECTURE.md).
- Example: [#286](https://github.com/Jstar269/nakagawa-recomp/issues/286) (run the guest PSMF player PRX).
