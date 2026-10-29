# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""The failure-code taxonomy and everything that emits a code agree.

``assets/failure_codes.json`` is the machine-readable list, ``docs/FAILURE_CODES.md`` explains
each code, and the emitters are the bring-up (``nk_cli.py``, ``title_qualification.py``), the
tool routes, game-file staging, built-in decryption, the disc checks, the player's problem
report and the library tools. Each test reads the emitter's source, so a code added anywhere
without its entry fails here.
"""

from __future__ import annotations

import ast
import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

from nk_core import decrypt_tool  # noqa: E402

REGISTRY = json.loads((ROOT / "assets" / "failure_codes.json").read_text(encoding="utf-8"))
DOC = (ROOT / "docs" / "FAILURE_CODES.md").read_text(encoding="utf-8")
CODE = re.compile(r"^[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)+$")
C_CODE = re.compile(r'"([A-Z][A-Z0-9]*(?:_[A-Z0-9]+)+)"')


def codes_in(*vocabularies: str) -> set[str]:
    return {code for code, entry in REGISTRY["codes"].items()
            if set(entry["in"]) & set(vocabularies)}


def _tree(relative: str) -> ast.Module:
    return ast.parse((ROOT / relative).read_text(encoding="utf-8"))


def _values(node: ast.AST | None) -> set[str]:
    """The code constants an expression can evaluate to (not the ones it only compares)."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str) and CODE.match(node.value):
        return {node.value}
    if isinstance(node, ast.IfExp):
        return _values(node.body) | _values(node.orelse)
    if isinstance(node, ast.BoolOp):
        return set().union(*(_values(value) for value in node.values))
    if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) \
            and node.func.attr == "get" and isinstance(node.func.value, ast.Dict):
        # {kind: code, ...}.get(kind, default)
        found = set().union(*(_values(value) for value in node.func.value.values))
        return found | (_values(node.args[1]) if len(node.args) > 1 else set())
    return set()


def _assigned(tree: ast.Module, names: set[str], keys: set[str], calls: dict[str, int],
              keywords: set[str]) -> set[str]:
    """Codes a module assigns to ``names``, stores under ``keys`` or passes to ``calls``."""
    found: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            func = node.func
            name = func.id if isinstance(func, ast.Name) else getattr(func, "attr", None)
            if name in calls and len(node.args) > calls[name]:
                found |= _values(node.args[calls[name]])
            for keyword in node.keywords:
                if keyword.arg in keywords:
                    found |= _values(keyword.value)
        elif isinstance(node, ast.Dict):
            for key, value in zip(node.keys, node.values, strict=True):
                if isinstance(key, ast.Constant) and key.value in keys:
                    found |= _values(value)
        elif isinstance(node, (ast.Assign, ast.AnnAssign)):
            targets = node.targets if isinstance(node, ast.Assign) else [node.target]
            for target in targets:
                if isinstance(target, ast.Name) and target.id in names:
                    found |= _values(node.value)
                elif isinstance(target, ast.Subscript) and isinstance(target.slice, ast.Constant) \
                        and target.slice.value in keys:
                    found |= _values(node.value)
                elif isinstance(target, ast.Tuple):
                    for index, element in enumerate(target.elts):
                        if isinstance(element, ast.Name) and element.id in names:
                            value = node.value
                            found |= _values(value.elts[index] if isinstance(value, ast.Tuple)
                                             else value)
    return found


def _module_dict(tree: ast.Module, name: str) -> ast.Dict:
    for node in tree.body:
        targets = node.targets if isinstance(node, ast.Assign) else \
            [node.target] if isinstance(node, ast.AnnAssign) else []
        if any(isinstance(target, ast.Name) and target.id == name for target in targets):
            assert isinstance(node.value, ast.Dict), name
            return node.value
    raise AssertionError(f"{name} is not a module-level dict")


def _c_function(relative: str, name: str) -> str:
    text = (ROOT / relative).read_text(encoding="utf-8")
    start = re.search(rf"^[^\n;]*\b{re.escape(name)}\s*\([^;{{]*\)\s*\{{", text, re.M)
    assert start, f"{name} not found in {relative}"
    depth, index = 0, start.end() - 1
    for index in range(start.end() - 1, len(text)):
        depth += {"{": 1, "}": -1}.get(text[index], 0)
        if depth == 0:
            break
    return text[start.start():index + 1]


#: Python modules whose exceptions carry a route code (the ``route`` vocabulary).
ROUTE_SOURCES = (
    "tools/nk_cli.py", "tools/title_codegen_plan.py", "tools/package_notices.py",
    "tools/imports.py", "tools/psp_import_table.py", "tools/analyze.py",
    "tools/nk_core/iso_inspect.py",
)
ROUTE_EXCEPTIONS = {
    "PackageRouteError": 0, "ImportTableError": 0, "ExperimentalModuleIntakeError": 0,
    "ExperimentalProfileBuildError": 0,
}

#: Player and core sources whose literals the problem report can show.
PLAYER_SOURCES = (
    sorted(str(path.relative_to(ROOT).as_posix()) for path in (ROOT / "src" / "player").glob("*.c"))
    + ["src/core/nk_launch.c", "src/core/nk_disc_expand.c", "src/core/nk_disc_expand.h",
       "src/core/nk_psp_crypto.c", "src/core/nk_iso.c"]
)
#: Uppercase literals in those sources that are not failure codes.
PLAYER_NOT_CODES = {
    # preflight check names (the bring-up report's preflight_checks[].code)
    "AUDIO_OUTPUT", "DATA_ROOT", "DISC_SFO", "GUEST_MODULES", "RUNTIME_PACKAGE", "SYSTEM_FONTS",
    "TITLE_PROFILE",
    # PARAM.SFO keys, disc folders and placeholders
    "DISC_ID", "DISC_VERSION", "PSP_GAME",
    # the success status of a decrypt (nk_psp_status_name for NK_PSP_OK)
    "VALIDATED_DECRYPTED",
    # gamepad and pointer names, UI actions and the UI test harness's tokens
    "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT", "DPAD_UP", "LEFT_SHOULDER", "RIGHT_SHOULDER",
    "MOUSE_DOWN", "MOUSE_UP", "OPEN_FOLDER", "ASSERT_GAME_RUNNING", "INVALIDATE_PACKAGE_CACHE",
    "MOUNT_ART_ISO", "SATURATE_ART_CACHE", "WAIT_ART_DELAY_ACTIVE", "UI_TEST_LONG_ERROR",
}
#: Environment variable names.
PLAYER_NOT_CODE_PREFIXES = ("NK_", "SR_", "XDG_", "NAKAGAWA_", "PLAYER_UI_TEST_")


class RegistryShapeTests(unittest.TestCase):
    def test_every_code_has_one_known_stage_and_known_places(self) -> None:
        stages = REGISTRY["stages"]
        self.assertEqual(stages, ["inspect", "decrypt", "import", "analyze", "codegen", "compile",
                                  "package", "run", "host"])
        self.assertEqual(set(REGISTRY["report_stages"]) - set(stages), set())
        for code, entry in REGISTRY["codes"].items():
            with self.subTest(code=code):
                self.assertRegex(code, r"^[A-Z][A-Z0-9_]*[A-Z0-9]$")
                self.assertIn(entry["stage"], stages)
                self.assertTrue(entry["in"])
                self.assertLessEqual(set(entry["in"]), set(REGISTRY["vocabularies"]))
        self.assertEqual(set(REGISTRY["retired"]) & set(REGISTRY["codes"]), set(),
                         "a retired code is never reused")

    def test_no_code_is_an_unclassified_bucket(self) -> None:
        for code in REGISTRY["codes"]:
            self.assertNotIn("UNCLASSIFIED", code)


class DocumentTests(unittest.TestCase):
    def _rows(self) -> dict[str, tuple[str, list[str]]]:
        rows: dict[str, tuple[str, list[str]]] = {}
        stage = None
        for line in DOC.splitlines():
            heading = re.match(r"^## (\S+)$", line)
            if heading:
                stage = heading.group(1) if heading.group(1) in REGISTRY["stages"] else None
                continue
            row = re.match(r"^\| `([A-Z][A-Z0-9_]*)` \| ([a-z, ]+) \| (.+) \|$", line)
            if stage and row:
                code, where, meaning = row.groups()
                self.assertNotIn(code, rows, f"{code} is listed twice")
                self.assertTrue(meaning.strip(), code)
                rows[code] = (stage, [place.strip() for place in where.split(",")])
        return rows

    def test_the_document_lists_every_code_with_its_stage_and_places(self) -> None:
        documented = self._rows()
        registered = {code: (entry["stage"], entry["in"])
                      for code, entry in REGISTRY["codes"].items()}
        self.assertEqual(sorted(set(registered) - set(documented)), [], "missing from the doc")
        self.assertEqual(sorted(set(documented) - set(registered)), [], "missing from the JSON")
        for code, row in documented.items():
            self.assertEqual(row, registered[code], code)

    def test_the_document_lists_every_stop_boundary(self) -> None:
        section = DOC.split("## Stop boundaries", 1)[1].split("\n## ", 1)[0]
        documented = dict(re.findall(r"^\| `([a-z0-9<>-]+)` \| (\d+) \|", section, re.M))
        boundaries = REGISTRY["stop_boundaries"]
        expected = {name: str(entry["issue"]) for name, entry in boundaries["names"].items()}
        expected.update({prefix + "<name>": str(entry["issue"])
                         for prefix, entry in boundaries["prefixes"].items()})
        self.assertEqual(documented, expected)


class BringupReportTests(unittest.TestCase):
    def test_the_report_schema_enum_is_the_report_vocabulary(self) -> None:
        schema = json.loads((ROOT / "assets" / "bringup_report.schema.json")
                            .read_text(encoding="utf-8"))
        enum = schema["properties"]["failure_class"]["enum"]
        self.assertEqual(len(enum), len(set(enum)))
        self.assertEqual(set(enum) - {"NONE"}, codes_in("report"))

    def test_every_class_the_bringup_writes_is_a_report_code(self) -> None:
        nk_cli = _tree("tools/nk_cli.py")
        emitted = _assigned(nk_cli, {"failure", "failure_class"}, {"failure_class"},
                            {"fail_stage": 2, "_fail_bringup": 2}, {"failure_class"})
        stage_defaults = _module_dict(nk_cli, "_STAGE_FAILURE_CLASS")
        emitted |= set().union(*(_values(value) for value in stage_defaults.values))
        messages = _module_dict(nk_cli, "_ANALYZER_BOUNDARY_MESSAGES")
        emitted |= set().union(*(_values(key) for key in messages.keys))
        qualification = _tree("tools/title_qualification.py")
        emitted |= _assigned(qualification, {"failure", "failure_class"}, {"failure_class"},
                             {"_stage_fail": 2}, {"failure_class"})
        self.assertGreater(len(emitted), 30, "the census found the emit sites")
        self.assertEqual(sorted(emitted - codes_in("report")), [])

    def test_the_analyze_stage_names_each_route_code_it_can_meet(self) -> None:
        nk_cli = _tree("tools/nk_cli.py")
        messages = _module_dict(nk_cli, "_ANALYZER_BOUNDARY_MESSAGES")
        named = set().union(*(_values(key) for key in messages.keys))
        self.assertIn("ANALYZER_INTERNAL_ERROR", named)
        self.assertLessEqual(named, codes_in("report"))


class RouteTests(unittest.TestCase):
    def _census(self) -> set[str]:
        found: set[str] = set()
        for relative in ROUTE_SOURCES:
            tree = _tree(relative)
            # raise PackageRouteError("CODE", ...), super().__init__("CODE", ...) in a subclass,
            # IsoInspectionError(..., boundary_code="CODE"), ImportTableError(..., code="CODE")
            # and title_codegen_plan's printed prefix for an error with no route code
            found |= _assigned(tree, {"prefix"}, set(), {**ROUTE_EXCEPTIONS, "__init__": 0},
                               {"boundary_code", "code"})
            for node in tree.body:
                # named code constants: CODE_REGIONS_MISMATCH = "...", PBP_SFO_INVALID = "..."
                if isinstance(node, ast.Assign) and len(node.targets) == 1 \
                        and isinstance(node.targets[0], ast.Name):
                    name = node.targets[0].id
                    value = _values(node.value)
                    if value and (name.startswith("CODE_") or name.endswith("_CODE")
                                  or value == {name}):
                        found |= value
        return found

    def test_every_route_code_is_registered_and_every_registered_one_is_raised(self) -> None:
        census = self._census()
        self.assertEqual(sorted(census - codes_in("route")), [], "raised but not registered")
        self.assertEqual(sorted(codes_in("route") - census), [], "registered but never raised")


class StagingTests(unittest.TestCase):
    def test_every_staging_code_is_registered(self) -> None:
        census = _assigned(_tree("tools/nk_core/prep_engine.py"), set(), set(), {},
                           {"error_code"})
        self.assertEqual(census, codes_in("staging"))


class DecryptTests(unittest.TestCase):
    def _status_names(self) -> dict[str, str]:
        body = _c_function("src/core/nk_psp_crypto.c", "nk_psp_status_name")
        return dict(re.findall(r'case (NK_PSP_[A-Z_]+): return "([A-Z_]+)";', body))

    def test_every_engine_status_is_a_decrypt_code(self) -> None:
        names = set(self._status_names().values()) - {"VALIDATED_DECRYPTED"}
        body = _c_function("src/core/nk_psp_crypto.c", "nk_psp_status_name")
        names |= set(re.findall(r'default: return "([A-Z_]+)";', body))
        player = _c_function("src/player/player_state.c", "player_decrypt_disc_file")
        names |= set(re.findall(r'return "([A-Z_]+)";', player)) - {"VALIDATED_DECRYPTED"}
        self.assertGreater(len(names), 12)
        self.assertEqual(sorted(names - codes_in("decrypt")), [])

    def test_nk_decrypt_exit_codes_match_the_tooling(self) -> None:
        names = self._status_names()
        body = _c_function("src/core/nk_decrypt_main.c", "print_diagnostic")
        exits = {int(code): names[status]
                 for status, code in re.findall(r"case (NK_PSP_[A-Z_]+): exit_code = (\d+);",
                                                body)}
        default = re.search(r"default: rc = (NK_PSP_[A-Z_]+); exit_code = (\d+);", body)
        self.assertIsNotNone(default)
        exits[int(default.group(2))] = names.get(default.group(1), "INTERNAL_ERROR")
        self.assertEqual(exits, decrypt_tool.EXIT_STATUS_CODES)
        self.assertLessEqual(set(exits.values()), codes_in("decrypt"))
        self.assertIsNone(decrypt_tool.status_code(0))
        self.assertEqual(decrypt_tool.status_code(2), "INTERNAL_ERROR")


class DiscTests(unittest.TestCase):
    def test_every_disc_refusal_is_a_disc_code(self) -> None:
        header = (ROOT / "src" / "core" / "nk_disc_expand.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "core" / "nk_disc_expand.c").read_text(encoding="utf-8")
        found = set(re.findall(r'#define NK_DISC_[A-Z_]+\s+"([A-Z_]+)"', header + source))
        iso = (ROOT / "src" / "core" / "nk_iso.c").read_text(encoding="utf-8")
        found |= set(re.findall(r'"((?:PBP|DISC)_[A-Z_]+)"', iso)) - {"DISC_ID", "DISC_VERSION"}
        self.assertGreater(len(found), 18)
        self.assertEqual(sorted(found - codes_in("disc")), [])


class PlayerTests(unittest.TestCase):
    def test_every_code_the_problem_report_can_show_is_registered(self) -> None:
        literals: set[str] = set()
        for relative in PLAYER_SOURCES:
            literals |= set(C_CODE.findall((ROOT / relative).read_text(encoding="utf-8")))
        candidates = {literal for literal in literals
                      if literal not in PLAYER_NOT_CODES
                      and not literal.startswith(PLAYER_NOT_CODE_PREFIXES)}
        shown = codes_in("player", "disc", "decrypt")
        self.assertEqual(sorted(candidates - shown), [],
                         "a new uppercase literal is a registered code or a listed non-code")
        self.assertEqual(sorted({code for code in codes_in("player") if CODE.match(code)}
                                - literals), [], "registered but no longer in the player")


class LibraryTests(unittest.TestCase):
    def test_library_records_use_registered_codes(self) -> None:
        found: set[str] = set()
        for relative in ("tools/library_build.py", "tools/library_sweep.py"):
            found |= _assigned(_tree(relative), {"boundary_code", "failure_class"},
                               {"failure_class", "boundary_code", "identify_error",
                                "first_failure_code"}, {}, set())
        sweep = _tree("tools/library_sweep.py")
        for node in sweep.body:
            if isinstance(node, ast.Assign) and any(
                    isinstance(target, ast.Name) and target.id == "_PUBLIC_SWEEP_BLOCKER_CODES"
                    for target in node.targets):
                found |= {element.value for element in ast.walk(node.value)
                          if isinstance(element, ast.Constant) and isinstance(element.value, str)}
        self.assertGreater(len(found), 5)
        self.assertEqual(sorted(found - codes_in("library", "report", "decrypt")), [])


class StopBoundaryTests(unittest.TestCase):
    def test_every_stop_boundary_the_runtime_writes_is_registered(self) -> None:
        body = _c_function("src/rt/recomp.c", "dispatch_semantic_boundary_name")
        names = set(re.findall(r'return "([a-z0-9-]+)";', body))
        recomp = (ROOT / "src" / "rt" / "recomp.c").read_text(encoding="utf-8")
        names |= set(re.findall(r'return "(interpreter-rejection)";', recomp))
        hle = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
        names |= set(re.findall(r'#define HLE_LIBFONT_STARTUP_BOUNDARY\s+"([a-z0-9-]+)"', hle))
        prefixes = set(re.findall(r'"(unsupported-import-)"', hle))
        boundaries = REGISTRY["stop_boundaries"]
        self.assertEqual(names, set(boundaries["names"]))
        self.assertEqual(prefixes, set(boundaries["prefixes"]))


class LiveCodeTests(unittest.TestCase):
    def test_every_registered_code_still_appears_in_an_emitter(self) -> None:
        text = ""
        for path in list((ROOT / "tools").rglob("*.py")) + list((ROOT / "src").rglob("*.[ch]")):
            if path.name.startswith("test_") or "selftest" in path.name:
                continue
            text += path.read_text(encoding="utf-8", errors="replace")
        quoted = set(re.findall(r"[\"']([A-Z][A-Z0-9_]*)[\"']", text))
        self.assertEqual(sorted(set(REGISTRY["codes"]) - quoted), [],
                         "move a code no emitter uses to \"retired\"")


if __name__ == "__main__":
    unittest.main()
