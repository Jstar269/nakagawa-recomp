# 0005: Non-retail discs are accepted, labelled and keyed by content hash

Date: 2026-10-29
Status: Accepted (owner); implementation open

## Context

Owners also have prerelease and fan-translated discs. Some carry a DISC_ID that is not in the retail form, or
PARAM.SFO and ISO 9660 quirks. Refusing them blocks real games; renaming or repairing them would change the
user's disc.

## Decision

A disc that is not a retail release is accepted and labelled as such in the player. Its per-title folders and
packages are keyed by a hash of its content, not by a retail disc ID it does not have. Nakagawa never alters the
disc image or the data read from it; quirks are handled when reading.

## Consequences

- Two different dumps that share a DISC_ID get separate packages and saves.
- Read-time tolerance for quirks keeps the raw bytes and records a note, as #1667 does for PARAM.SFO and the PVD.
- The naming of content-keyed folders needs its own change before such discs build.

## Links

- [#1667](https://github.com/Jstar269/nakagawa-recomp/pull/1667) (quirks that keep disc identity, and the
  non-retail ID case it left for this decision); issue
  [#308](https://github.com/Jstar269/nakagawa-recomp/issues/308).
