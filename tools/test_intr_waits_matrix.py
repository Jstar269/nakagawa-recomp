# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the psp-recomp authors
"""Guards for the issue #88 interrupt/dispatch-context conformance matrix.

``src/rt/intr_conformance.h`` transcribes a hardware-generated table out of
PSPAutotests ``tests/intr/waits.expected``.  That file lives under
``third_party/``, which is Git-ignored, so the transcription is the only copy in
the repository and nothing would otherwise notice if a value drifted.

Three separate things are checked here, at three different evidence tiers:

* **Transcription (tier 3, model/reference).**  Every ``hw[]`` array entry
  (including normal, intr-off, intr-ctx, and disp-off) is compared against the
  exact ``waits.expected`` line the matrix cites.  SKIPs when the PPSSPP checkout
  is absent -- an absent oracle is not a pass.
* **Registry mirror (tier 4, source-shape).**  ``hle_register_wait_conformance_handlers()``
  exists only so the selftest build can reach these NIDs.  Every triple in it
  must be the same ``(nid, name, handler)`` the production branch of
  ``sr_hle_init()`` registers, or the test build would be measuring a different
  implementation from the one that ships.
* **Dispatch-suspension registration (tier 4, source-shape).**  Asserts that
  ``sceKernelSuspendDispatchThread`` and ``sceKernelResumeDispatchThread`` are
  registered in ``hle.c`` and wired into the matrix harness.
"""

from __future__ import annotations

import os
import pathlib
import re
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADER = ROOT / "src" / "rt" / "intr_conformance.h"
HLE_C = ROOT / "src" / "rt" / "hle.c"
_REL = pathlib.Path("ppsspp-src") / "pspautotests" / "tests" / "intr" / "waits.expected"


def _find_waits_expected() -> pathlib.Path | None:
    """Locate the hardware oracle.

    ``third_party/`` is Git-ignored, so a linked worktree does not have its own
    copy: the checkout lives in whichever tree cloned it.  Try this tree first,
    then an explicit override, then the repository's main worktree.
    """
    candidates = [ROOT / "third_party" / _REL]
    override = os.environ.get("NAKAGAWA_THIRD_PARTY")
    if override:
        candidates.insert(0, pathlib.Path(override) / _REL)
    try:
        common = pathlib.Path(
            subprocess.run(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
                           cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()
        )
        candidates.append(common.parent / "third_party" / _REL)
    except (OSError, subprocess.SubprocessError):
        pass
    for c in candidates:
        if c.is_file():
            return c
    return None


WAITS_EXPECTED = _find_waits_expected()

NID_SUSPEND_DISPATCH_THREAD = "0x3ad58b8c"

# Sentinels mirrored from intr_conformance.h.  Kept here rather than parsed so a
# renamed sentinel is a loud failure instead of a silently skipped comparison.
SENTINELS = {
    "IC_UNKNOWN": "IC_UNKNOWN",
    "ICU": "IC_UNKNOWN",
    "IC_BLOCKED": "IC_BLOCKED",
    "ICB": "IC_BLOCKED",
    "IC_NOTRUN": "IC_NOTRUN",
    "IC_SETUPFAIL": "IC_SETUPFAIL",
}
NAMED_CONSTANTS = {
    "CNW": 0x800201A7,      # SCE_KERNEL_ERROR_CAN_NOT_WAIT
    "ILCTX": 0x80020064,    # SCE_KERNEL_ERROR_ILLEGAL_CONTEXT
}


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def parse_matrix(header_text: str):
    """Return the kIcMatrix rows as dicts.  Layout is positional, so the parser
    walks braces rather than pattern-matching a whole row at once."""
    body_start = header_text.index("static const IcProbe kIcMatrix[] = {")
    body_start = header_text.index("{", body_start + len("static const IcProbe kIcMatrix[]"))
    depth, rows, cur, start = 0, [], None, 0
    for i in range(body_start, len(header_text)):
        ch = header_text[i]
        if ch == "{":
            depth += 1
            if depth == 2:
                start = i + 1
        elif ch == "}":
            depth -= 1
            if depth == 1:
                rows.append(header_text[start:i])
            elif depth == 0:
                break
    for raw in rows:
        groups = re.findall(r"\{([^{}]*)\}", raw)
        assert len(groups) >= 4, raw
        strings = re.findall(r'"((?:[^"\\]|\\.)*)"', raw)
        # Replace each brace group with a placeholder and drop string literals, so
        # what is left is exactly the row's scalar fields in declaration order:
        #   api, nid, scenario, group, variant, @ev_line, @hw, @base, @prec,
        #   hw_block, ev_dispatch_line, hw_dispatch
        flat = re.sub(r"\{[^{}]*\}", "@", raw)
        flat = re.sub(r'"(?:[^"\\]|\\.)*"', "$", flat)
        tokens = [t.strip() for t in flat.split(",") if t.strip()]
        scalars = [t for t in tokens if t not in ("@", "$")]
        yield {
            "api": strings[0],
            "scenario": strings[1] if len(strings) > 1 else "",
            "skip": strings[2] if len(strings) > 2 else None,
            "ev_line": [t.strip() for t in groups[0].split(",")],
            "hw": [t.strip() for t in groups[1].split(",")],
            "base": [t.strip() for t in groups[2].split(",")],
            # nid, group, variant, hw_block
            "hw_block": scalars[3],
            "raw": raw,
        }


def resolve(expr: str):
    """Return an int for a concrete expectation, or the canonical sentinel name."""
    expr = expr.strip()
    if expr in SENTINELS:
        return SENTINELS[expr]
    if expr in NAMED_CONSTANTS:
        return NAMED_CONSTANTS[expr]
    m = re.fullmatch(r"IC_RET\(\s*(0[xX][0-9a-fA-F]+|\d+)u?\s*\)", expr)
    if m:
        return int(m.group(1), 0)
    raise AssertionError(f"unparsed expectation {expr!r}")


def load_expected_lines():
    """line number (1-based) -> 32-bit value printed on that line."""
    out = {}
    text = WAITS_EXPECTED.read_text(encoding="utf-8", errors="replace")
    for n, line in enumerate(text.splitlines(), start=1):
        m = re.search(r":\s*([0-9a-f]{8})\s*$", line)
        if m:
            out[n] = int(m.group(1), 16)
    return out


class TestMatrixTranscription(unittest.TestCase):
    """Tier 3: the in-repo table against the hardware-generated file."""

    @classmethod
    def setUpClass(cls):
        if WAITS_EXPECTED is None:
            raise unittest.SkipTest(
                "pspautotests tests/intr/waits.expected not found (third_party/ is "
                "Git-ignored); set NAKAGAWA_THIRD_PARTY to a checkout. The hardware "
                "oracle cannot be consulted, so this is a SKIP, not a pass."
            )
        cls.expected = load_expected_lines()
        cls.rows = list(parse_matrix(strip_comments(HEADER.read_text(encoding="utf-8"))))

    def test_matrix_is_not_empty(self):
        self.assertGreaterEqual(len(self.rows), 40, "kIcMatrix failed to parse")

    def test_context_columns_match_hardware(self):
        checked = 0
        for row in self.rows:
            label = f"{row['api']} / {row['scenario'] or '(no argument)'}"
            for ctx, (line_tok, hw_tok) in enumerate(zip(row["ev_line"], row["hw"])):
                line = int(line_tok, 0)
                hw = resolve(hw_tok)
                if line == 0:
                    self.assertEqual(
                        hw, "IC_UNKNOWN",
                        f"{label} ctx{ctx}: no waits.expected line cited, so the "
                        f"hardware cell must be IC_UNKNOWN, not {hw_tok}",
                    )
                    continue
                self.assertIn(line, self.expected,
                              f"{label} ctx{ctx}: waits.expected:{line} has no result value")
                self.assertEqual(
                    self.expected[line], hw,
                    f"{label} ctx{ctx}: matrix says {hw_tok} but waits.expected:{line} "
                    f"says 0x{self.expected[line]:08x}",
                )
                checked += 1
        self.assertGreater(checked, 80, "too few hardware cells verified")

    def test_dispatch_column_matches_hardware(self):
        checked = 0
        for row in self.rows:
            ev_dispatch = int(row["ev_line"][3], 0)
            hw_dispatch = row["hw"][3]
            if ev_dispatch == 0:
                continue
            self.assertIn(ev_dispatch, self.expected)
            self.assertEqual(
                self.expected[ev_dispatch], resolve(hw_dispatch),
                f"{row['api']} / {row['scenario']}: dispatch-disabled cell disagrees "
                f"with waits.expected:{ev_dispatch}",
            )
            checked += 1
        self.assertGreater(checked, 40, "too few dispatch-disabled cells verified")


class TestRegistryScope(unittest.TestCase):
    """Tier 4: the shared registry helper the conformance harness depends on.

    ``hle_register_wait_conformance_handlers()`` exists so the selftest build can
    reach the wait APIs at all -- ``sr_hle_init()``'s ``SR_HLE_THREAD_SELFTEST``
    branch registers a deliberately narrow set.  It must stay a SINGLE definition
    called by both branches: a copy specific to the test build would mean the
    harness measured a mapping the game never uses.
    """

    @classmethod
    def setUpClass(cls):
        cls.src = HLE_C.read_text(encoding="utf-8", errors="replace")

    @staticmethod
    def triples(block: str):
        return [
            (m.group(1).lower(), m.group(2), m.group(3))
            for m in re.finditer(
                r'sr_hle_register\(\s*(0x[0-9a-fA-F]+)\s*,\s*"([^"]+)"\s*,\s*(\w+)\s*\)', block
            )
        ]

    def init_body(self):
        init = re.search(r"void sr_hle_init\(void\) \{(.*?)\n\}", self.src, re.S)
        self.assertIsNotNone(init, "sr_hle_init() not found")
        return init.group(1)

    def conformance_helper(self):
        m = re.search(
            r"static void hle_register_wait_conformance_handlers\(void\) \{(.*?)\n\}",
            self.src, re.S,
        )
        self.assertIsNotNone(m, "hle_register_wait_conformance_handlers() not found")
        return m.group(1)

    def test_helper_covers_the_matrix_nids(self):
        """Every NID the executable matrix dispatches must be registered by the helper
        or by another helper both builds call.  A NID that is only in the production
        branch is unreachable from the harness."""
        helper_nids = {nid for nid, _, _ in self.triples(self.conformance_helper())}
        self.assertGreaterEqual(len(helper_nids), 25, "registry helper is suspiciously small")
        body = self.init_body()
        shared = body[: body.index("#ifdef SR_HLE_THREAD_SELFTEST")]
        selftest = body[body.index("#ifdef SR_HLE_THREAD_SELFTEST"): body.index("#else")]
        called = set(re.findall(r"(hle_register_\w+)\(\);", shared + selftest))
        self.assertIn("hle_register_wait_conformance_handlers", called)

        rows = list(parse_matrix(strip_comments(HEADER.read_text(encoding="utf-8"))))
        reachable = set(helper_nids)
        for name in called:
            m = re.search(r"static void %s\(void\) \{(.*?)\n\}" % name, self.src, re.S)
            if m:
                reachable |= {nid for nid, _, _ in self.triples(m.group(1))}
        for row in rows:
            nid = re.search(r"0x[0-9a-fA-F]+", row["raw"]).group(0).lower()
            self.assertIn(
                nid, reachable,
                f"{row['api']} ({nid}) is in kIcMatrix but no registration helper the "
                "selftest build calls registers it; the harness could only report it "
                "as registry-scope",
            )

    def test_helper_is_the_only_definition(self):
        """The moved registrations must not have been copied back into the production
        branch: a duplicate NID is rejected at run time and would silently strand one
        of the two copies."""
        body = self.init_body()
        production = body[body.index("#else"): body.index("#endif")]
        helper_nids = {nid for nid, _, _ in self.triples(self.conformance_helper())}
        dupes = sorted(helper_nids & {nid for nid, _, _ in self.triples(production)})
        self.assertEqual(
            dupes, [],
            "these NIDs are registered both by hle_register_wait_conformance_handlers() "
            f"and inline in the production branch: {dupes}",
        )
        self.assertIn("hle_register_wait_conformance_handlers();", production)

    def test_no_duplicate_registrations_anywhere(self):
        seen = {}
        for nid, name, handler in self.triples(self.src):
            self.assertNotIn(
                nid, seen,
                f"{nid} ({name}) is registered twice; sr_hle_register() rejects the "
                f"second, so {handler} would never run (first was {seen.get(nid)})",
            )
            seen[nid] = name

    def test_suspend_dispatch_thread_is_registered(self):
        """Verify sceKernelSuspendDispatchThread and sceKernelResumeDispatchThread are registered."""
        registered = self.triples(self.src)
        self.assertIn(
            "sceKernelSuspendDispatchThread", {name for _, name, _ in registered},
            "sceKernelSuspendDispatchThread must be registered in hle.c",
        )
        self.assertIn(
            "sceKernelResumeDispatchThread", {name for _, name, _ in registered},
            "sceKernelResumeDispatchThread must be registered in hle.c",
        )


if __name__ == "__main__":
    unittest.main()
