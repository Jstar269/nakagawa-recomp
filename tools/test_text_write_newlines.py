#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

r"""A tool that emits a TRACKED text file must pin its line endings.

Python's text mode translates "\n" to "\r\n" on Windows unless the caller passes
``newline=""`` or ``newline="\n"``. This repository is developed on Windows, its
``.gitattributes`` declares ``eol=lf`` for source, and ``tools/publish_audit.py``
rejects CRLF in a tracked text file as ``TEXT_LINE_ENDING_CRLF``. A generator
that omits the argument therefore produces an artifact that cannot be
regenerated on a Windows host without the publication gate refusing it
afterwards -- which is what ``tools/title_catalog_codegen.py`` did to the
generated title catalog, silently, until someone regenerated it and then had to
work out why the provenance audit was complaining about a file they had not
meaningfully changed.

The scope here is deliberately narrow. Writes into a temporary directory are
invisible to the publication gate and spelling them any particular way is not
worth a rule, so the several hundred such writes across ``tools/test_*.py`` are
not flagged. What is checked is the small set of modules that write artifacts
git tracks. **Adding a generator means adding it to GENERATORS below** -- that
line is the whole maintenance cost of this lint, and it is cheaper than
rediscovering the failure from a CRLF finding in an audit log.
"""

from __future__ import annotations

import ast
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"

#: Modules that write files git tracks. The artifact each one owns is named so a
#: reader can confirm the entry still earns its place.
GENERATORS = {
    "title_catalog_codegen.py": "src/core/generated/nk_title_catalog.{c,h}",
    "policy_sync.py": "assets/public_source_profile.json and PUBLIC_EXPORT.json",
    "provenance_ledger.py": "assets/public_provenance_ledger.json",
}


def _offending_write(node: ast.AST) -> str | None:
    """Describe `node` when it writes text without pinning newlines."""
    if not isinstance(node, ast.Call):
        return None

    pinned = any(kw.arg == "newline" for kw in node.keywords)

    if isinstance(node.func, ast.Attribute) and node.func.attr == "write_text":
        return None if pinned else "write_text() without newline="

    name = None
    if isinstance(node.func, ast.Name):
        name = node.func.id
    elif isinstance(node.func, ast.Attribute):
        name = node.func.attr
    if name != "open":
        return None

    mode = None
    if len(node.args) >= 2 and isinstance(node.args[1], ast.Constant):
        mode = node.args[1].value
    for kw in node.keywords:
        if kw.arg == "mode" and isinstance(kw.value, ast.Constant):
            mode = kw.value.value
    if not isinstance(mode, str) or "b" in mode:
        return None
    if not any(ch in mode for ch in "wxa"):
        return None
    return None if pinned else f"open(..., {mode!r}) without newline="


class TrackedArtifactNewlineTests(unittest.TestCase):
    def test_generators_pin_their_line_endings(self) -> None:
        offenders: list[str] = []
        for filename in sorted(GENERATORS):
            path = TOOLS / filename
            self.assertTrue(
                path.is_file(),
                f"{filename} is listed in GENERATORS but does not exist; remove the "
                f"entry or correct the name",
            )
            tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
            for node in ast.walk(tree):
                described = _offending_write(node)
                if described:
                    offenders.append(f"tools/{filename}:{node.lineno}: {described}")

        self.assertEqual(
            offenders,
            [],
            "These writes translate \\n to \\r\\n on Windows. The artifact is tracked, "
            "so tools/publish_audit.py rejects the result as TEXT_LINE_ENDING_CRLF and "
            "the failure appears only after someone regenerates on Windows. Pass "
            'newline="" to keep the emitted bytes exactly, or write bytes directly:\n  '
            + "\n  ".join(offenders),
        )

    def test_generator_list_covers_the_known_tracked_artifacts(self) -> None:
        """A generated artifact whose producer is unlisted is the gap this lint exists to close."""
        expected_producers = {
            "src/core/generated/nk_title_catalog.c": "title_catalog_codegen.py",
            "assets/public_provenance_ledger.json": "provenance_ledger.py",
            "PUBLIC_EXPORT.json": "policy_sync.py",
        }
        for artifact, producer in expected_producers.items():
            self.assertIn(
                producer, GENERATORS,
                f"{artifact} is a tracked generated artifact but its producer "
                f"{producer} is not covered by this lint",
            )
            self.assertTrue(
                (ROOT / artifact).is_file(),
                f"{artifact} is named here but is not present in the tree",
            )


if __name__ == "__main__":
    unittest.main()
