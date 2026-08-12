# Public-source publication gates

This is the engineering gate definition for changes to the active sanitized
public repository and for any fresh source candidate or release export derived
from it. It is not legal advice, legal clearance, a binary-release approval, or
evidence that a title runtime is playable. The machine-readable policy and the
exact tree being assessed are authoritative; this page explains the decision
boundary.

## Scope

The active public source tree, and any candidate derived from it, contains
generic recompiler/runtime source, synthetic fixtures, and reviewed third-party
notices. It must not contain game binaries or assets,
decrypted modules, generated retail translation units, saves, keys, captures,
oracle traces, private routes, private repository metadata, or derived bytes.
Title-specific HST configuration and private engineering/review documents are
excluded by `assets/public_source_profile.json`.

The current public source profile also excludes the lineage-sensitive PGF/font and PGD/amctrl
surfaces, plus the reviewed-but-not-yet-cleared sal063-derived ISO/VFS and SDL
audio implementations. Public-safe builds link explicit unavailable boundaries;
they reject the capability and do not fabricate success.

## Required predicates

Every predicate below must pass for the same exact tree and history under review. A local
pass is not a hosted-CI pass, a hardware pass, a visual pass, a DCO attestation,
or a human legal decision.

1. **Exact tree binding.** The audit binds to the `git write-tree` result for the
   audited index, or to an immutable committed tree. A worktree with untracked or
   modified bytes is not publication evidence.
2. **Explicit policy and provenance.** Every included path is enumerated in the
   profile and has a concrete record in the externally trusted provenance ledger.
   Missing, unresolved, substituted, or self-authorized records fail closed.
3. **Candidate export.** `tools/build_public_export.py` produces one deterministic
   single-commit export. The candidate's policy, ledger, manifest, export digest,
   counts, and excluded paths are re-audited from the materialized bytes.
4. **History and object audit.** `tools/history_audit.py` scans every reachable
   commit, tree path, ref, and blob content in the proposed history. A clean tip
   is not sufficient.
5. **Supply-chain inventory.** The release manifest and SBOM cover the expected
   provenance families (sal063, PPSSPP, PSPSDK, FFmpeg/ATRAC3+, SDL3, Vulkan,
   shadcn/ui, and VFPU) with synchronized notices and lock data.
6. **Build and tests.** The public-safe target builds its generic source
   target and runs the source-owned regression gates. Missing private inputs or
   external oracles are reported as blocked/unavailable, never as passes.
7. **Documentation and governance.** Documentation contains no private/counsel
   work product or stale topology claims. Live repository visibility, rulesets,
   Actions behavior, DCO, and maintainer authorization are verified separately
   against the actual destination repository.

## Reproducible local sequence

```text
python tools/provenance_ledger.py
python tools/policy_sync.py --regen-export
python tools/history_audit.py --json
python tools/verify_sbom.py
python tools/build_public_export.py --public-safe-profile --export-dir <staging>
python tools/publish_audit.py --candidate-root <staging> --candidate-tree --public-scope
```

The repository or export is not cleared merely because these commands are
available. Record the exact commit/tree, outputs, and remaining human/hosted
gates.
