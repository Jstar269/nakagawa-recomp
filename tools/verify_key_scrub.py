#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Check whether any PSP KIRK/amctrl constant is still reachable in Git history.

This contains no key material: it reads the constant *values* from the local key
file (a private local binding) and searches every reachable commit for each value in
the textual encodings the tree has ever used -- contiguous hex, and C/Python byte
arrays with assorted spacing, case, and zero-padding. `git log -S <hex>` alone is
NOT sufficient, because it never matches the `{0x12,0x46,...}` byte-array form.

The verifier fails closed (#377): a history search that did not *complete* is never
reported as clean. Repository preconditions (inside a Git work tree or bare
repository, not shallow, refs with reachable history) are validated once up front,
and every `git log` pickaxe invocation must exit 0 to count as a completed search.
An empty/no-ref repository is unverifiable (exit 2), not clean.

Exit 0  -> every configured constant was searched in every encoding, every search
           completed successfully, and nothing matched (scrub verified / never present).
Exit 3  -> at least one constant is positively reachable (scrub incomplete / not yet run).
Exit 2  -> could not verify (missing/unusable key file, failed repository
           preconditions, or any failed `git` invocation) and there is no
           definitive positive exposure result. Verdict precedence is
           exposure > unverifiable > clean: a confirmed reachable constant
           exits 3 even when other searches failed, with the partial-coverage
           diagnostics printed prominently beside the finding.

Scope: the Git repository containing the current working directory; preconditions
and searches always target that same repository. Run it BEFORE a scrub to confirm
the exposure, and AFTER to confirm it is gone.

Diagnostics never contain constant values or search needles: `git` stderr is
redacted against every configured encoding before display.

Usage:
  python tools/verify_key_scrub.py            # uses keys/pgd_keys.txt or $SR_PGD_KEYS
  python tools/verify_key_scrub.py --keys PATH
"""

from __future__ import annotations

import argparse
from collections.abc import Sequence
import os
import subprocess
import sys
from dataclasses import dataclass
from enum import Enum

GIT_TIMEOUT_SECONDS = 300
_MAX_STDERR_CHARS = 2000


class Verdict(Enum):
    """Outcome of one encoding search. ERROR is never conflated with NOT_FOUND."""

    FOUND = "FOUND"            # a completed search matched this encoding
    NOT_FOUND = "NOT_FOUND"    # a completed search did not match
    ERROR = "ERROR"            # the search did not complete (git failed/missing/timed out)


@dataclass(frozen=True)
class SearchError:
    """Why one history search could not run. `detail` is redacted and needle-free."""

    operation: str  # command description; never contains the needle
    exit_code: int | None
    detail: str


def key_values(path: str) -> dict[str, bytes]:
    """name -> 16 raw bytes, parsed from the `name = hex` key file."""
    values: dict[str, bytes] = {}
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            name, _, value = line.partition("=")
            name, value = name.strip(), value.strip()
            if len(value) == 32:
                try:
                    values[name] = bytes.fromhex(value)
                except ValueError:
                    pass
    return values


def encodings(raw: bytes) -> list[str]:
    """Every textual spelling of `raw` this codebase has plausibly used."""
    forms: set[str] = set()
    forms.add(raw.hex())                      # lowercase contiguous hex
    forms.add(raw.hex().upper())              # uppercase contiguous hex
    for sep in (",", ", "):                   # C / Python byte arrays
        for pad in (True, False):             # 0x0a vs 0xa (non-padded ints)
            for up in (False, True):          # 0xab vs 0xAB
                parts = []
                for b in raw:
                    h = f"{b:02x}" if pad else f"{b:x}"
                    if up:
                        h = h.upper()
                    parts.append("0x" + h)
                forms.add(sep.join(parts))
                # Python bytes([...]) also renders small values as bare ints (1, not 0x01).
    # Bare-int Python list form (bytes([0x27, 0x74, 1, 2, ...])): decimal for <16, hex else.
    forms.add(", ".join(str(b) if b < 16 else f"0x{b:02x}" for b in raw))
    forms.add(", ".join(str(b) if b < 16 else f"0x{b:02X}" for b in raw))
    return sorted(forms)


def _redact(text: str, redact_needles: Sequence[str]) -> str:
    """Remove every configured encoding (and so every key value) from `text`."""
    for needle in redact_needles:
        if needle:
            text = text.replace(needle, "[redacted]")
    return text


def _as_text(value: object) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    return str(value)


def _git(args: list[str]) -> subprocess.CompletedProcess[str] | None:
    """Run a read-only probe `git` command; None when git is missing or fails.

    Catches the same failure family as the search path so a broken probe
    becomes a documented exit-2 precondition failure, never a traceback.
    """
    try:
        return subprocess.run(
            ["git", *args], capture_output=True, text=True, check=False, timeout=GIT_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.SubprocessError, ValueError):
        return None


def check_repository() -> str | None:
    """Validate the Git preconditions for searching this repository's history.

    Scope is the repository containing the current working directory. Returns
    None when a `git log --all` search can meaningfully cover the reachable
    history, or a diagnostic (never containing key material) naming the failed
    precondition. Every failure maps to exit 2 so an unrunnable search can
    never be read as "clean".
    """
    inside = _git(["rev-parse", "--is-inside-work-tree"])
    if inside is None or inside.returncode != 0 or inside.stdout.strip() != "true":
        bare = _git(["rev-parse", "--is-bare-repository"])
        if bare is None or bare.returncode != 0 or bare.stdout.strip() != "true":
            return "not inside a Git work tree or bare repository; run from the checkout (or mirror clone) to verify"
    git_dir = _git(["rev-parse", "--git-dir"])
    if git_dir is None or git_dir.returncode != 0:
        return "git rev-parse --git-dir failed; there is no usable Git repository here"
    shallow = _git(["rev-parse", "--is-shallow-repository"])
    if shallow is None or shallow.returncode != 0:
        return "cannot determine whether this clone is shallow (git rev-parse --is-shallow-repository failed)"
    if shallow.stdout.strip() == "true":
        return "shallow clone: reachable history is truncated here, so no clean verdict would be trustworthy"
    refs = _git(["for-each-ref", "--format=%(refname)"])
    if refs is None or refs.returncode != 0:
        return "git for-each-ref failed; cannot enumerate refs to search"
    if not refs.stdout.strip():
        return "repository has no refs; there is no reachable history to verify (empty repository?)"
    return None


def _diagnostic(text: object, redact_needles: Sequence[str]) -> str:
    """Redact every configured encoding from `text` and bound its length."""
    detail = _redact(_as_text(text).strip(), redact_needles)
    if len(detail) > _MAX_STDERR_CHARS:
        detail = detail[:_MAX_STDERR_CHARS] + "...(truncated)"
    return detail


def search_history(needle: str, redact_needles: Sequence[str]) -> tuple[Verdict, SearchError | None]:
    """Pickaxe-search every reachable commit for one textual encoding.

    Returns (FOUND, None) or (NOT_FOUND, None) only when `git log` ran and
    exited 0. Any nonzero exit, timeout, or missing `git` returns
    (ERROR, SearchError); ERROR never reads as "clean" (#377), though a
    definitive exposure still outranks it in the final verdict.
    """
    operation = "git log --all -S <needle redacted> --oneline --source"
    try:
        result = subprocess.run(
            ["git", "log", "--all", "-S", needle, "--oneline", "--source"],
            capture_output=True, text=True, check=False, timeout=GIT_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired:
        return Verdict.ERROR, SearchError(operation, None, f"git command timed out after {GIT_TIMEOUT_SECONDS}s")
    except (OSError, subprocess.SubprocessError, ValueError) as exc:
        # Any raise out of subprocess.run (missing git, spawn failure, ...) means
        # the search did not complete; it must never surface as "clean" (#377).
        # The exception text can embed the full command including the needle,
        # so it is redacted and bounded like any other diagnostic.
        detail = _diagnostic(exc, redact_needles) or "(exception carried no detail)"
        return Verdict.ERROR, SearchError(operation, None, f"git could not be executed: {detail}")
    if result.returncode != 0:
        detail = _diagnostic(result.stderr, redact_needles) or "(git produced no stderr)"
        return Verdict.ERROR, SearchError(operation, result.returncode, detail)
    if result.stdout.strip():
        return Verdict.FOUND, None
    return Verdict.NOT_FOUND, None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--keys", default=os.environ.get("SR_PGD_KEYS") or os.path.join("keys", "pgd_keys.txt"))
    args = parser.parse_args(argv)

    if not os.path.isfile(args.keys):
        print(f"key file not found: {args.keys} (private local binding)", file=sys.stderr)
        return 2

    values = key_values(args.keys)
    if not values:
        print(f"no usable constants parsed from {args.keys}", file=sys.stderr)
        return 2

    # Preconditions once, up front: a search that cannot run must never be
    # conflated with a completed search that found nothing (#377).
    repo_problem = check_repository()
    if repo_problem is not None:
        print(f"cannot verify history: {repo_problem}", file=sys.stderr)
        return 2

    all_needles = sorted({form for raw in values.values() for form in encodings(raw)})

    exposed: list[str] = []
    unverifiable: list[str] = []
    errors: list[tuple[str, SearchError]] = []
    for name, raw in values.items():
        found = False
        failed = False
        for form in encodings(raw):
            verdict, error = search_history(form, all_needles)
            if error is not None:
                failed = True
                errors.append((name, error))
                continue
            if verdict is Verdict.FOUND:
                found = True
        if found:
            exposed.append(name)
            print(f"  {name}: REACHABLE in history")
        elif failed:
            unverifiable.append(name)
            print(f"  {name}: UNVERIFIABLE (a history search did not complete; diagnostics on stderr)")
        else:
            print(f"  {name}: clean")

    # Verdict precedence: exposure > unverifiable > clean. A definitive
    # positive finding must never be hidden behind a later search failure,
    # but the partial-coverage diagnostics print prominently either way.
    if errors:
        print("\nHistory verification could not be completed; these searches failed:", file=sys.stderr)
        for name, error in errors:
            print(f"  {name}: {error.operation} -> {error.detail}", file=sys.stderr)
        if unverifiable:
            print(f"\n{len(unverifiable)} constant(s) could not be verified: {', '.join(unverifiable)}", file=sys.stderr)
        if exposed:
            print(f"{len(exposed)} constant(s) are still reachable in Git history: {', '.join(exposed)}", file=sys.stderr)
            print(
                "History still exposes the keys and coverage was partial; fix the failures above, "
                "re-run after scrubbing. See docs/KEY_HISTORY_SCRUB.md.",
                file=sys.stderr,
            )
        else:
            print("No clean verdict is possible until every search completes; fix the failures above and re-run.", file=sys.stderr)

    if exposed:
        print(f"\n{len(exposed)} constant(s) still reachable in Git history: {', '.join(exposed)}")
        print("History still exposes the keys. See docs/KEY_HISTORY_SCRUB.md.")
        return 3
    if errors:
        return 2
    print("\nNo PSP constant is reachable in any commit. History is clean.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
