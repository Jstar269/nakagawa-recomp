# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Canonical publication-eligibility policy.

``assets/public_source_profile.json`` is the single authoritative statement of
which tracked paths may appear in a public candidate tree. This module loads it,
validates it, computes its deterministic digest, and resolves any path to an
explicit disposition.

The governing rule is **UNKNOWN = REJECT**. There is deliberately no rule of the
form "paths under ``src/`` or ``tools/`` are probably project-authored, therefore
public". Publication eligibility is decided by explicit enumeration only:

* a path listed under ``exclude_paths`` / matched by ``exclude_globs`` /
  ``exclude_prefixes`` is ``excluded`` and must never appear in a public tree;
* a path listed under ``include_paths`` is ``included``;
* anything else resolves to ``default_disposition``, which is ``REJECT``.

Exclusion is evaluated first and always wins, so a path can never become public by
being added to two lists.

Adding a new file to the public surface therefore requires a visible, reviewable
one-line edit to the policy. That friction is the control: it forces a human
publication decision at the moment a new path is introduced, which is precisely
what was missing on 2026-08-11.

Publication *eligibility* is kept separate from provenance/licence *metadata*.
This module answers only "may this path be published at all". Notice, licence and
provenance classification remain the job of ``assets/release_manifest.json`` and
the auditor, which reconcile against this policy rather than replacing it.
"""

from __future__ import annotations

import fnmatch
import hashlib
import json
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

#: Policy schema versions this module understands.
SUPPORTED_PROFILE_VERSIONS = frozenset({"2.0.0"})

#: Dispositions a path may resolve to.
INCLUDED = "included"
EXCLUDED = "excluded"
UNCLASSIFIED = "unclassified"

#: The only permitted default. A profile that defaults to anything else is
#: rejected at load time, so the fail-closed property cannot be edited away
#: quietly in a data file.
REQUIRED_DEFAULT_DISPOSITION = "REJECT"

_REQUIRED_KEYS = (
    "name",
    "profile_version",
    "min_tool_version",
    "build_mode",
    "default_disposition",
    "exclude_prefixes",
    "exclude_globs",
    "exclude_paths",
    "include_paths",
)


class PolicyError(Exception):
    """The policy file is missing, malformed, or self-contradictory.

    Every raise site is a hard failure. A publication gate that cannot read its
    own policy must never report success.
    """


@dataclass(frozen=True)
class Resolution:
    """How the policy classifies one path."""

    path: str
    disposition: str
    rule: str
    rationale: str = ""

    @property
    def is_excluded(self) -> bool:
        return self.disposition == EXCLUDED

    @property
    def is_unclassified(self) -> bool:
        return self.disposition == UNCLASSIFIED


def canonical_digest(document: dict) -> str:
    """Deterministic SHA-256 over the policy's *meaning*, not its formatting.

    Sorted keys and separator-normalized JSON, so reindenting the file does not
    invalidate every generated artifact, while any change to a path list does.
    """
    payload = json.dumps(document, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def parse_version(text: str) -> tuple[int, ...]:
    try:
        return tuple(int(part) for part in str(text).split("."))
    except (TypeError, ValueError) as error:
        raise PolicyError(f"malformed version string: {text!r}") from error


class Policy:
    """A loaded, validated publication policy."""

    def __init__(self, document: dict, source: Path | None = None) -> None:
        self.document = document
        self.source = source
        self.name: str = document["name"]
        self.profile_version: str = str(document["profile_version"])
        self.min_tool_version: str = str(document["min_tool_version"])
        self.build_mode: str = document["build_mode"]
        self.default_disposition: str = document["default_disposition"]
        self.exclude_prefixes: tuple[str, ...] = tuple(document["exclude_prefixes"])
        self.exclude_globs: tuple[str, ...] = tuple(document["exclude_globs"])
        self.exclude_paths: frozenset[str] = frozenset(document["exclude_paths"])
        self.include_paths: frozenset[str] = frozenset(document["include_paths"])
        self.exclude_rationale: dict[str, str] = dict(document.get("exclude_rationale", {}))
        self.digest: str = canonical_digest(document)

    # -- resolution ----------------------------------------------------------

    def resolve(self, path: str) -> Resolution:
        """Classify one repository-relative POSIX path.

        Exclusion is checked before inclusion so that a path appearing in both
        lists can never be published. (Load-time validation also rejects such a
        profile outright; this ordering is the belt to that braces.)
        """
        normalized = PurePosixPath(path).as_posix()

        if normalized in self.exclude_paths:
            return Resolution(
                normalized, EXCLUDED, "exclude_paths",
                self.exclude_rationale.get(normalized, ""),
            )
        for pattern in self.exclude_globs:
            if fnmatch.fnmatchcase(normalized, pattern):
                return Resolution(
                    normalized, EXCLUDED, f"exclude_globs:{pattern}",
                    self.exclude_rationale.get(pattern, ""),
                )
        for prefix in self.exclude_prefixes:
            if normalized.startswith(prefix):
                return Resolution(
                    normalized, EXCLUDED, f"exclude_prefixes:{prefix}",
                    self.exclude_rationale.get(prefix, ""),
                )
        if normalized in self.include_paths:
            return Resolution(normalized, INCLUDED, "include_paths")

        return Resolution(
            normalized, UNCLASSIFIED, "default_disposition",
            "path is not classified by the canonical publication policy; "
            "unknown paths are rejected, never assumed publishable",
        )

    # -- compatibility -------------------------------------------------------

    def tool_compatibility_errors(self, tool_version: str) -> list[str]:
        """Version drift is a hard failure, in both directions."""
        errors: list[str] = []
        if self.profile_version not in SUPPORTED_PROFILE_VERSIONS:
            errors.append(
                f"policy profile_version {self.profile_version!r} is not supported by this "
                f"auditor (supported: {', '.join(sorted(SUPPORTED_PROFILE_VERSIONS))})"
            )
        if parse_version(tool_version) < parse_version(self.min_tool_version):
            errors.append(
                f"auditor version {tool_version} is older than the policy's "
                f"min_tool_version {self.min_tool_version}"
            )
        return errors


def load_policy(path: Path) -> Policy:
    """Load and validate the canonical policy, or raise ``PolicyError``."""
    try:
        raw = path.read_text(encoding="utf-8")
    except OSError as error:
        raise PolicyError(f"cannot read publication policy {path}: {error}") from error

    try:
        document = json.loads(raw)
    except json.JSONDecodeError as error:
        raise PolicyError(f"publication policy {path} is not valid JSON: {error}") from error

    if not isinstance(document, dict):
        raise PolicyError(f"publication policy {path} must be a JSON object")

    missing = [key for key in _REQUIRED_KEYS if key not in document]
    if missing:
        raise PolicyError(f"publication policy {path} is missing required keys: {', '.join(missing)}")

    for key in ("exclude_prefixes", "exclude_globs", "exclude_paths", "include_paths"):
        value = document[key]
        if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
            raise PolicyError(f"publication policy {path}: {key} must be a list of strings")

    if document["default_disposition"] != REQUIRED_DEFAULT_DISPOSITION:
        raise PolicyError(
            f"publication policy {path}: default_disposition must be "
            f"{REQUIRED_DEFAULT_DISPOSITION!r} (unknown paths must be rejected, never "
            f"assumed publishable); found {document['default_disposition']!r}"
        )

    policy = Policy(document, source=path)

    # A path present in both lists is a policy authoring error, not something to
    # resolve silently in favour of either side.
    overlap = sorted(policy.include_paths & policy.exclude_paths)
    if overlap:
        raise PolicyError(
            f"publication policy {path}: {len(overlap)} path(s) appear in both include_paths "
            f"and exclude_paths: {', '.join(overlap[:5])}"
        )

    # An include entry that a glob/prefix would exclude is equally contradictory.
    shadowed = sorted(
        candidate for candidate in policy.include_paths
        if any(fnmatch.fnmatchcase(candidate, pattern) for pattern in policy.exclude_globs)
        or any(candidate.startswith(prefix) for prefix in policy.exclude_prefixes)
    )
    if shadowed:
        raise PolicyError(
            f"publication policy {path}: {len(shadowed)} include_paths entr(ies) are also matched "
            f"by an exclude rule: {', '.join(shadowed[:5])}"
        )

    duplicates = _duplicates(document["include_paths"]) | _duplicates(document["exclude_paths"])
    if duplicates:
        raise PolicyError(
            f"publication policy {path}: duplicate entries: {', '.join(sorted(duplicates)[:5])}"
        )

    return policy


def _duplicates(items: list[str]) -> set[str]:
    seen: set[str] = set()
    repeated: set[str] = set()
    for item in items:
        if item in seen:
            repeated.add(item)
        seen.add(item)
    return repeated
