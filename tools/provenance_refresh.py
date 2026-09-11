#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Regenerate the derived provenance artifacts, in the one order that works.

Refreshing provenance by hand needs four tools run in a specific order, and
three of the ordering constraints are invisible from the tools' own help text.
Getting any of them wrong produces a failure that looks like a *content*
problem -- a stale export, a hash mismatch -- so the natural response is to
re-run the same wrong sequence and watch it not converge. That has cost real
time more than once. The constraints:

1. ``tools/provenance_ledger.py`` reads the **git index**, never the worktree.
   An edit that has not been staged is invisible to it, and a `git restore
   --staged` between steps silently reverts what it is about to hash. So the
   tree must be staged BEFORE the ledger is generated, not after.

2. ``--export-output`` is **silently ignored** when the ledger runs in
   full-build mode. It belongs to the ``refresh-reviewed`` /
   ``admit-new-reviewed`` subcommands. Passing it to a full build and expecting
   ``PUBLIC_EXPORT.json`` to change is the classic non-converging loop.

3. ``PUBLIC_EXPORT.json`` is written only by ``tools/policy_sync.py
   --regen-export``, and it takes ``provenance_ledger_sha256`` from the
   **worktree** ledger while taking ``included_content_sha256`` from the
   **index**. So the freshly generated ledger has to be staged before the
   export is regenerated, or the two disagree by construction.

What this script does NOT do, by design:

* It never creates a provenance record and never approves a blob. Those live in
  the maintainer-controlled authority outside this repository, admission is not
  a hash refresh, and an approval keys on (path, exact sha256). Automating
  either would be automating the attestation itself.
* It refuses to add anything to ``include_paths`` unless you pass
  ``--apply-policy``, because putting a file on the public surface is a
  publication decision, not a build step. Without that flag an unclassified
  path stops the run and is reported.

Typical use, after committing or staging your change:

    python tools/provenance_refresh.py \\
        --implementation-ledger <path-to-private-authority>/IMPLEMENTATION_PROVENANCE.json

Add ``--apply-policy`` when the change legitimately introduces new public paths
and you have decided they belong on the public surface.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
LEDGER = "assets/public_provenance_ledger.json"


def run(command: list[str], *, dry_run: bool, capture: bool = False) -> subprocess.CompletedProcess | None:
    printable = " ".join(command)
    if dry_run:
        print(f"  (dry run) {printable}")
        return None
    print(f"  $ {printable}")
    return subprocess.run(
        command, cwd=ROOT, text=True,
        capture_output=capture, check=False,
    )


def git_add_all(dry_run: bool) -> None:
    """Stage the worktree so the index the generator reads matches it.

    This is a real side effect on the caller's index and is the reason the
    ordering works at all; see constraint 1 in the module docstring.
    """
    result = run(["git", "add", "-A"], dry_run=dry_run, capture=True)
    if result is not None and result.returncode != 0:
        sys.exit(f"git add -A failed:\n{result.stderr}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--implementation-ledger", required=True, type=Path,
        help="the maintainer-controlled detailed development ledger "
             "(IMPLEMENTATION_PROVENANCE.json) from the private authority",
    )
    parser.add_argument(
        "--apply-policy", action="store_true",
        help="add newly introduced public paths to include_paths. Without this a "
             "new path stops the run: adding a file to the public surface is a "
             "publication decision, and policy_sync refuses high-risk paths outright.",
    )
    parser.add_argument("--dry-run", action="store_true",
                        help="print the sequence without changing anything")
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)

    authority = args.implementation_ledger
    if not authority.is_file():
        sys.exit(
            f"implementation ledger not found: {authority}\n"
            f"This is the private authority file; it is never stored in this repository."
        )

    python = sys.executable

    print("[1/6] policy classification")
    check = run([python, "tools/policy_sync.py"], dry_run=False, capture=True)
    if check is not None and check.returncode != 0:
        if not args.apply_policy:
            sys.stdout.write(check.stdout or "")
            sys.stderr.write(check.stderr or "")
            sys.exit(
                "\nUnclassified tracked paths remain. Each one is a publication "
                "decision:\n"
                "  - re-run with --apply-policy to add the routine ones to "
                "include_paths, or\n"
                "  - add them to exclude_paths with a rationale by hand.\n"
                "policy_sync refuses high-risk paths (fonts, binaries, "
                "PGF/PGD/key-adjacent names) either way."
            )
        applied = run([python, "tools/policy_sync.py", "--apply"],
                      dry_run=args.dry_run, capture=True)
        if applied is not None:
            sys.stdout.write(applied.stdout or "")
            if applied.returncode != 0:
                sys.exit(applied.stderr or "policy_sync --apply failed")
    else:
        print("  policy already classifies every tracked path")

    print("[2/6] stage the worktree (the ledger generator reads the INDEX)")
    git_add_all(args.dry_run)

    print("[3/6] regenerate the public provenance ledger from the authority")
    generated = run(
        [python, "tools/provenance_ledger.py",
         "--implementation-ledger", str(authority),
         "--output", LEDGER],
        dry_run=args.dry_run, capture=True,
    )
    if generated is not None:
        sys.stdout.write(generated.stdout or "")
        if generated.returncode != 0:
            sys.stderr.write(generated.stderr or "")
            sys.exit(
                "\nLedger generation failed. An `unresolved` path means a genuinely "
                "new implementation-bearing file needs ADMISSION in the authority -- "
                "its own path-specific record. That is not something this script "
                "will invent, and a hash refresh will not resolve it."
            )

    print("[4/6] stage the ledger (the export reads it from the WORKTREE)")
    git_add_all(args.dry_run)

    print("[5/6] regenerate PUBLIC_EXPORT.json")
    export = run([python, "tools/policy_sync.py", "--regen-export"],
                 dry_run=args.dry_run, capture=True)
    if export is not None:
        sys.stdout.write(export.stdout or "")
        if export.returncode != 0:
            sys.exit(export.stderr or "policy_sync --regen-export failed")
    git_add_all(args.dry_run)

    print("[6/6] audit both legs")
    failures = []
    for label, extra in (("index", []), ("worktree", ["--worktree"])):
        audit = run(
            [python, "tools/publish_audit.py", "--tracked-only", "--public-scope",
             "--provenance-self-consistency", *extra],
            dry_run=args.dry_run, capture=True,
        )
        if audit is None:
            continue
        sys.stdout.write(audit.stdout or "")
        if audit.returncode != 0:
            failures.append(label)

    if args.dry_run:
        print("\ndry run: nothing changed")
        return 0
    if failures:
        print(f"\nprovenance refresh: FAIL ({', '.join(failures)} leg)")
        return 1
    print(
        "\nprovenance refresh: OK\n"
        "Reminder: the hosted attestation fetches the authority from its GitHub "
        "repository at run time, so push the private authority BEFORE pushing this "
        "branch or the hosted gate reports every changed blob as unapproved."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
