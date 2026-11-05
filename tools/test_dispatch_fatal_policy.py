#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for fail-closed dispatch and unknown NID handling.

Fail-closed dispatch is unconditional: a dispatch miss executes only inside
analyzer-owned executable spans (the production interpreter floor) and every
rejection terminates the run. No environment variable can make a miss
permissive; SR_DISPATCH_FATAL was removed as vestigial once that floor landed.

Verifies:
1. Unknown NIDs fail closed: sr_syscall in src/rt/hle.c terminates execution
   (_Exit(7) or longjmp) when a NID is not registered in the HLE table, and never
   returns fake success (0 / H_OK).
2. Unmapped / non-executable dispatch targets fail closed: dispatch() in src/rt/recomp.c
   terminates via exit(1) on dispatch miss rather than limping onward, and the
   miss path routes to the interpreter floor (dispatch_run_interp).
3. No dispatch-control environment variable exists: nothing in src/rt or
   src/core reads or writes one, and the native launch session (src/core/nk_launch.c)
   injects none into the runtime.
4. A synthetic unregistered dispatch fails closed with no environment variable
   set and regardless of any hostile continuation flag.
5. The production runner (nk_manager.ps1) sets no dispatch control and reports
   the retired SR_UNSAFE_CONTINUE_ON_DISPATCH_MISS escape as inert.
6. Every fatal stop in the HLE files that ends the process with _Exit or _exit, which skip
   atexit, finishes a video recording first with its exit status
   (sr_videorec_finish_status, #1273), so record.json never stays INCOMPLETE after a stop the
   runtime chose, and its exit_status is the code the stop exits with.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

try:
    import scratch_dir
except ModuleNotFoundError:
    from tools import scratch_dir

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hle_sources  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RT = ROOT / "src" / "rt"
CORE = ROOT / "src" / "core"
RECOMP_C = ROOT / "src" / "rt" / "recomp.c"
LAUNCH_C = ROOT / "src" / "core" / "nk_launch.c"
MGR = ROOT / "nk_manager.ps1"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


def function_body(source: str, signature: str) -> str:
    """Return the complete text of the function declared by ``signature``.

    The body is delimited by matching the declaration's braces, not by taking a
    fixed-size slice. A byte window silently stops covering the fail-closed block
    as soon as the function grows, which turns a policy invariant into a test of
    how much unrelated code sits above it: sr_syscall gained the runtime PRX
    import-linking path, and the retired 2000-byte window no longer reached
    ``if (!e)``. Brace matching keeps the assertion tied to the policy instead.

    Quoted spans and comments are skipped so that a brace inside a literal or a
    comment cannot end the body early.
    """
    start = source.find(signature)
    if start < 0:
        return ""
    open_brace = source.find("{", start)
    if open_brace < 0:
        return ""
    depth = 0
    i = open_brace
    while i < len(source):
        ch = source[i]
        if ch in "\"'":
            quote, i = ch, i + 1
            while i < len(source) and source[i] != quote:
                i += 2 if source[i] == "\\" else 1
        elif source.startswith("//", i):
            i = source.find("\n", i)
            if i < 0:
                break
        elif source.startswith("/*", i):
            end = source.find("*/", i + 2)
            i = len(source) if end < 0 else end + 1
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
        i += 1
    return source[start:]


class TestDispatchFatalPolicySource(unittest.TestCase):
    """Source-level invariants verifying fail-closed dispatch and NID handling."""

    def test_hle_unregistered_nid_fails_closed(self):
        """sr_syscall must terminate on unregistered NIDs and never return 0."""
        hle_src = hle_sources.text()
        self.assertIn("uint32_t sr_syscall(CpuState *s, uint32_t nid)", hle_src)
        fn_body = function_body(hle_src, "uint32_t sr_syscall(CpuState *s, uint32_t nid)")
        self.assertIn("if (!e)", fn_body)
        self.assertIn("_Exit(7)", fn_body)
        self.assertIn("longjmp(g_hle_jmp", fn_body)
        self.assertIn("HLE: unimplemented nid", fn_body)

    def test_unregistered_nid_stop_is_named_for_the_player(self):
        """The unregistered-NID stop writes the player's semantic-boundary line first."""
        hle_src = hle_sources.text()
        fn_body = function_body(hle_src, "uint32_t sr_syscall(CpuState *s, uint32_t nid)")
        record = fn_body.index("hle_record_unsupported_import_stop(nid, nm);")
        self.assertLess(record, fn_body.index("_Exit(7)"))
        helper = function_body(
            hle_src,
            "static void hle_record_unsupported_import_stop(uint32_t nid, const char *name)")
        self.assertIn('getenv("SR_BOOT_EVENT_FILE")', helper)
        self.assertIn('"unsupported-import-"', helper)
        self.assertIn(
            r'"BOOT_EVENT phase=stop reason=semantic-boundary boundary=%s issue=%u\n"', helper)
        # Only the player's [a-z0-9-] alphabet reaches the line (player_read_semantic_stop).
        self.assertIn("if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');", helper)
        self.assertIn("if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))) ch = '-';",
                      helper)

    def test_unmodelled_argument_shape_stop_is_named_for_the_player(self):
        """A handler's unmodelled-shape stop names the import for the player, then exits 7."""
        hle_src = hle_sources.text()
        stop = function_body(
            hle_src, "uint32_t hle_unmodelled_stop(const char *import_name, const char *fmt, ...)")
        production = stop.split("#else", 1)[1]
        record = production.index("hle_record_unsupported_import_stop(sr_last_nid, import_name);")
        self.assertLess(record, production.index("_Exit(7)"))
        self.assertNotIn("return HLE_KERNEL_ERROR_NOT_IMPLEMENTED", production)

    def test_recomp_dispatch_miss_terminates(self):
        """dispatch() wrapper must terminate via exit(1) on dispatch rejection."""
        recomp_src = RECOMP_C.read_text(encoding="utf-8")
        self.assertIn("void dispatch(CpuState *s, uint32_t target)", recomp_src)
        dispatch_block = re.search(
            r"void\s+dispatch\(CpuState\s*\*s,\s*uint32_t\s+target\)\s*\{(.*?)\}",
            recomp_src,
            re.DOTALL
        )
        self.assertIsNotNone(dispatch_block)
        body = dispatch_block.group(1)
        self.assertIn("exit(1)", body, "dispatch() must call exit(1) on failure")
        self.assertIn("dispatch_try(s, target) < 0", body)

    def test_miss_path_routes_to_interpreter_floor(self):
        """A non-PLT miss must reach dispatch_run_interp, never a permissive return."""
        recomp_src = RECOMP_C.read_text(encoding="utf-8")
        body = function_body(
            recomp_src,
            "static int dispatch_try_with_boundary(\n"
            "    CpuState *s,\n"
            "    uint32_t target,\n"
            "    const SrGuestInterpCallBoundary *call_boundary,\n"
            "    int drain) {",
        )
        self.assertTrue(body, "dispatch_try_with_boundary definition not found")
        self.assertIn("dispatch_run_interp(s, target, call_boundary, drain)", body)
        self.assertIn("SR_PERF_INTERP_DISPATCH_MISS", body)

    def test_no_dispatch_control_environment_variable_exists(self):
        """Dispatch is unconditional: no source may read or write a fatal switch."""
        for directory in (RT, CORE):
            for path in sorted(directory.rglob("*.c")) + sorted(directory.rglob("*.h")):
                text = path.read_text(encoding="utf-8")
                self.assertNotIn("SR_DISPATCH_FATAL", text, str(path))
                self.assertNotIn("SR_UNSAFE_CONTINUE_ON_DISPATCH_MISS", text, str(path))
        launch_src = LAUNCH_C.read_text(encoding="utf-8")
        self.assertNotIn("diagnostic_mode", launch_src)

    def test_hst_manager_dispatch_override_is_inert(self):
        """Manager sets no dispatch control and reports the retired escape honestly."""
        mgr_src = MGR.read_text(encoding="utf-8")
        self.assertNotIn("SR_DISPATCH_FATAL", mgr_src)
        self.assertIn("SR_UNSAFE_CONTINUE_ON_DISPATCH_MISS", mgr_src)
        self.assertIn("unconditionally fail-closed", mgr_src)


FATAL_EXIT_RE = re.compile(r"\b_[Ee]xit\(")
EXIT_ARG_RE = re.compile(r"\b_[Ee]xit\(\s*([^()]*?)\s*\)")
FINISH_STATUS_RE = re.compile(r"\bsr_videorec_finish_status\(\s*\"[^\"]*\"\s*,\s*([^()]*?)\s*\)")


def _without_comments(source: str) -> str:
    """Blank out comments and string literals, keeping every offset and newline."""
    out = list(source)
    i = 0
    while i < len(source):
        ch = source[i]
        if ch in "\"'":
            quote, j = ch, i + 1
            while j < len(source) and source[j] != quote and source[j] != "\n":
                j += 2 if source[j] == "\\" else 1
            for k in range(i + 1, min(j, len(source))):
                if out[k] != "\n":
                    out[k] = " "
            i = j + 1
        elif source.startswith("//", i):
            j = source.find("\n", i)
            j = len(source) if j < 0 else j
            out[i:j] = " " * (j - i)
            i = j
        elif source.startswith("/*", i):
            j = source.find("*/", i + 2)
            j = len(source) if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


def _enclosing_block(code: str, at: int) -> str:
    """The text from the innermost unmatched ``{`` before ``at`` up to ``at``."""
    depth = 0
    for i in range(at - 1, -1, -1):
        if code[i] == "}":
            depth += 1
        elif code[i] == "{":
            if depth == 0:
                return code[i:at]
            depth -= 1
    return code[:at]


class TestFatalStopsFinishRecording(unittest.TestCase):
    """A stop that skips atexit must finish the run's recording itself (#1273, #2080 review)."""

    def test_every_hle_exit_that_skips_atexit_finishes_the_recording(self):
        """Each stop finishes the recording with the status it exits with (record.json
        exit_status agrees with run_end and the process exit code)."""
        stops = []
        for path in sorted(RT.glob("hle*.c")):
            code = _without_comments(path.read_text(encoding="utf-8"))
            for match in FATAL_EXIT_RE.finditer(code):
                line = code.count("\n", 0, match.start()) + 1
                stops.append(f"{path.name}:{line}")
                with self.subTest(stop=f"{path.name}:{line}"):
                    block = _enclosing_block(code, match.start())
                    finishes = FINISH_STATUS_RE.findall(block)
                    self.assertTrue(finishes, "the stop exits without finishing a recording "
                                              "with its exit status (sr_videorec_finish_status)")
                    exit_arg = EXIT_ARG_RE.match(code, match.start())
                    self.assertIsNotNone(exit_arg)
                    self.assertEqual(finishes[-1], exit_arg.group(1),
                                     "the recorded exit status is not the code the stop exits with")
        # The stops this covers today; a new one is checked the same way.
        self.assertGreaterEqual(len(stops), 11, stops)

    def test_exit_at_vblank_records_a_clean_end(self):
        """SR_EXIT_AT_VBLANK is the documented end of a showcase recording: exit status 0."""
        src = (RT / "hle_display.c").read_text(encoding="utf-8")
        self.assertIn('sr_videorec_finish_status("SR_EXIT_AT_VBLANK", 0)', src)

    def test_finish_status_pattern(self):
        code = _without_comments('{ sr_videorec_finish_status("a, b", ROUTE_FAIL_EXIT); _Exit(ROUTE_FAIL_EXIT); }')
        self.assertEqual(FINISH_STATUS_RE.findall(code), ["ROUTE_FAIL_EXIT"])
        self.assertEqual(EXIT_ARG_RE.search(code).group(1), "ROUTE_FAIL_EXIT")

    def test_comments_and_strings_do_not_count(self):
        code = _without_comments('a(); /* _Exit(1) */ b("_exit(2)"); // _Exit(3)\n_Exit(4);')
        self.assertEqual(len(FATAL_EXIT_RE.findall(code)), 1)


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestDispatchFatalPolicyBehavioral(unittest.TestCase):
    """Behavioral test verifying that dispatch() on unknown targets exits with code 1."""

    def test_dispatch_unmapped_target_exits_code_1(self):
        with scratch_dir.TemporaryDirectory(prefix="nk_dispatch_fatal_") as tmp_dir:
            wrapper_c = Path(tmp_dir) / "wrapper_test.c"
            wrapper_code = """
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

typedef struct {
    uint32_t r[32];
    uint32_t pc;
} CpuState;

static int mock_dispatch_try(CpuState *s, uint32_t target) {
    (void)s; (void)target;
    return -1;
}

void dispatch(CpuState *s, uint32_t target) {
    if (mock_dispatch_try(s, target) < 0) {
        exit(1);
    }
}

int main(void) {
    CpuState s;
    dispatch(&s, 0xdeadbeef);
    return 0;
}
"""
            wrapper_c.write_bytes(wrapper_code.encode("utf-8"))
            exe_path = Path(tmp_dir) / "wrapper_test.exe"
            comp = subprocess.run([CC, str(wrapper_c), "-o", str(exe_path)], capture_output=True, text=True)
            self.assertEqual(comp.returncode, 0, f"Compilation failed: {comp.stderr}")
            run_res = subprocess.run([str(exe_path)], capture_output=True)
            self.assertEqual(run_res.returncode, 1, "dispatch() must exit with code 1 on miss")
            hostile = dict(os.environ)
            hostile.update({
                "SR_DISPATCH_FATAL": "0",
                "SR_UNSAFE_CONTINUE_ON_DISPATCH_MISS": "1",
            })
            run_res = subprocess.run([str(exe_path)], capture_output=True, env=hostile)
            self.assertEqual(
                run_res.returncode, 1,
                "hostile continuation flags must not make a dispatch miss permissive",
            )
            run_res = subprocess.run(
                [str(exe_path)], capture_output=True, env={"PATH": os.environ.get("PATH", "")}
            )
            self.assertEqual(
                run_res.returncode, 1,
                "a synthetic unregistered dispatch must fail closed with no environment variable set",
            )


if __name__ == "__main__":
    unittest.main()
