# SPDX-License-Identifier: GPL-3.0-or-later

"""Source-level guards for scheduler / thread-lifecycle invariants (2026-07-18 campaign).

Scheduler semantics are covered by the native ``make sched-selftest`` harness.
The ExitThread production boundary is additionally executed by
``make hle-thread-selftest`` through the registered NID path. These source tests
remain narrow cross-platform structural tripwires against reintroducing behavior
the campaign removed:

* an "anti-starvation" rotation in pick_next() that let a lower-priority READY
  thread preempt a higher-priority READY thread (PSP scheduling is strict priority);
* a thread entry returning without full exit bookkeeping (stranded WaitThreadEnd
  joiners, stale exit status);
* undefined coroutine fallbacks (Windows fiber self-switch, POSIX busy spin);
* literal thread-UID special cases (0x110/0x111/0x115) in executable code -- UID
  allocation drifts between runs, so roles must resolve via sched_*_uid();
* silently clamping a thread-stack request instead of failing the create.

Scope, and what these checks are NOT.

These are secondary diagnostics. Source scanning cannot prove a runtime invariant:
backslash-newline line splicing, a macro alias, an indirect alias or a reordered
guard all defeat a textual scan, and the mere textual presence of an equality check
or an ``abort()`` proves nothing about whether any control flow depends on it.

The coroutine lifecycle rules -- adopt the main coroutine exactly once, never adopt
from inside a child, park only on that one established identity, never self-switch,
destroy each coroutine exactly once and never while it is running -- are therefore
proved by executable instrumentation instead. ``src/rt/sr_coro.c`` counts those
operations as the real implementation performs them when built with
``-DSR_CORO_LIFECYCLE_TEST``, hard-caps adoptions and suppressed self-switches so a
reintroduced defect aborts in milliseconds rather than exhausting host RAM, and
``hle_thread_selftest`` asserts the exact totals. Treat anything below as a cheap
early warning, never as the safety argument.
"""

import os
import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCHED = (ROOT / "src" / "rt" / "sched.c").read_text(encoding="utf-8")
CORO = (ROOT / "src" / "rt" / "sr_coro.c").read_text(encoding="utf-8")
HLE = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")


def strip_comments(source):
    """Remove /* */ and // comments so assertions only see executable text."""
    source = re.sub(r"/\*.*?\*/", " ", source, flags=re.S)
    return re.sub(r"//[^\n]*", " ", source)


def function_body(source, name):
    match = re.search(rf"\b{name}\s*\([^;{{)]*\)\s*\{{", source)
    if not match:
        raise AssertionError(f"{name} not found")
    start = match.end() - 1
    depth = 0
    for pos in range(start, len(source)):
        char = source[pos]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start : pos + 1]
    raise AssertionError(f"{name} has no closing brace")


class PickNextStrictPriorityTests(unittest.TestCase):
    def test_rotation_is_filtered_to_best_priority(self):
        body = strip_comments(function_body(SCHED, "pick_next"))
        self.assertIn("best_pri", body, "pick_next must compute the best runnable priority")
        self.assertRegex(
            body,
            r"priority\s*==\s*best_pri",
            "rotation candidates must be restricted to the best priority",
        )

    def test_anti_starvation_override_is_gone(self):
        body = strip_comments(function_body(SCHED, "pick_next"))
        self.assertNotIn(
            "same_count",
            body,
            "the anti-starvation counter allowed priority inversion; do not reintroduce it",
        )
        self.assertNotIn("SCHED_ROT", strip_comments(SCHED))

    def test_only_ready_threads_are_selectable(self):
        body = strip_comments(function_body(SCHED, "pick_next"))
        self.assertRegex(body, r"state\s*==\s*TH_READY")


class DisplayWaitIsReadinessIndependentTests(unittest.TestCase):
    """The display wait must not consult any other thread's scheduling state.

    Measured on PSP-3001/6.61-ARK (record PSP-DISPLAY-003): with an always-runnable
    lower-priority peer present, the high-priority caller's total wall time moved by
    2 us over 2.006 s and its summed wait by 220 us over 120 waits, with identical
    VCOUNT progression. The syscall does not read the ready queue.

    A superseded candidate blocked the caller whenever any other thread was
    TH_READY, so that a lower-priority asset loader would be scheduled. The
    executable proof lives in sched_selftest.c
    (test_display_wait_ignores_other_ready_threads, which fails against that
    design); this is the cheap textual tripwire against reintroducing it in the
    same place, and nothing more.
    """

    def _wait_bodies(self):
        return {
            name: strip_comments(function_body(SCHED, name))
            for name in ("sched_wait_vblank_start", "sched_wait_vblank",
                         "sched_vblank_block")
        }

    def test_no_ready_state_scan_in_the_display_wait(self):
        for name, body in self._wait_bodies().items():
            self.assertNotIn(
                "TH_READY", body,
                f"{name} must not consult another thread's READY state; "
                "the PSP display wait is readiness-independent (PSP-DISPLAY-003)",
            )

    def test_display_wait_has_no_missed_edge_latch(self):
        self.assertNotIn(
            "vbl_seen", strip_comments(SCHED),
            "the per-thread missed-vblank latch is disproven by PSP-DISPLAY-002; "
            "a late display wait blocks to the next edge",
        )

    def test_the_two_display_nids_have_separate_handlers(self):
        hle = strip_comments(HLE)
        self.assertRegex(
            hle,
            r'sr_hle_register\(0x984c27e7,\s*"sceDisplayWaitVblankStart",\s*h_DisplayWaitVblankStart\)',
            "sceDisplayWaitVblankStart must resolve to the always-block handler",
        )
        self.assertRegex(
            hle,
            r'sr_hle_register\(0x36cdfade,\s*"sceDisplayWaitVblank",\s*h_DisplayWaitVblank\)',
            "sceDisplayWaitVblank must resolve to the in-vblank fast-return handler",
        )


class DisplayObservationTests(unittest.TestCase):
    def test_get_vcount_refreshes_before_returning(self):
        body = strip_comments(function_body(HLE, "h_DisplayGetVcount"))
        self.assertIn(
            "sched_vtime_refresh()",
            body,
            "sceDisplayGetVcount must sample time and latch due display edges",
        )
        self.assertLess(body.index("sched_vtime_refresh()"), body.index("return s_vcount"))

    def test_isvblank_refresh_does_not_advance_turbo_time(self):
        body = strip_comments(function_body(SCHED, "sched_display_is_vblank"))
        self.assertIn(
            "vtime_refresh()",
            body,
            "sceDisplayIsVblank must sample time and latch due display edges",
        )
        self.assertNotIn(
            "scheduler_progress_time()",
            body,
            "a display observation must not inject a turbo scheduler quantum",
        )


class IdleProgressGuardTests(unittest.TestCase):
    """A finite timer is progress even when the display source is not."""

    def test_vblank_watchdog_does_not_abort_finite_waits(self):
        body = strip_comments(function_body(SCHED, "sched_run"))
        self.assertRegex(
            body,
            r"if\s*\(\s*soonest\s*==\s*SCHED_WAIT_FOREVER\s*&&\s*"
            r"s_vbl_count\s*==\s*idle_vbl_mark",
            "the VBLANK watchdog must be scoped to all-infinite waits",
        )


class ThreadLifecycleTests(unittest.TestCase):
    def test_entry_return_routes_through_full_exit(self):
        body = strip_comments(function_body(SCHED, "coro_body"))
        self.assertIn(
            "sched_exit_current",
            body,
            "an entry that returns must exit like sceKernelExitThread(v0): "
            "status recorded, joiners woken, libc state unregistered",
        )

    def test_exit_thread_is_terminal_and_role_based(self):
        body = strip_comments(function_body(HLE, "h_ExitThread"))
        self.assertIn("sched_exit_current", body)
        self.assertIn(
            "sched_exit_current_delete",
            body,
            "ExitDeleteThread must retain the raw delete-exit seam",
        )
        self.assertNotIn(
            "sched_thread_wakeup",
            body,
            "ExitThread may release joiners but must not fabricate a wakeup for an "
            "unrelated launcher thread",
        )
        self.assertNotRegex(
            body,
            r"0x11[0-9]u?\b",
            "h_ExitThread must resolve roles via sched_*_uid(), never literal UIDs",
        )

    def test_stop_unload_self_module_is_terminal_without_role_cases(self):
        body = strip_comments(function_body(HLE, "h_StopUnloadSelfModuleWithStatus"))
        self.assertIn("sched_exit_current_unchecked", body)
        self.assertNotIn(
            "sched_exit_current((int32_t)A0)",
            body,
            "module self-unload remains outside the measured non-delete normalization",
        )
        self.assertNotRegex(body, r"\buid\s*[!=]=", "no per-role survival special cases")
        self.assertNotRegex(body, r"0x11[0-9]u?\b")

    def test_no_executable_literal_uid_comparisons_remain(self):
        for name, source in (("sched.c", SCHED), ("hle.c", HLE)):
            stripped = strip_comments(source)
            self.assertNotRegex(
                stripped,
                r"[!=]=\s*0x11[015]u?\b",
                f"{name} must not compare thread UIDs against historical literals",
            )

    def test_register_libc_thread_uses_role_validity_not_uid_numbers(self):
        """Role treatment must follow a captured role, never a UID's numeric value.

        UIDs are allocated from 0x110 upward, so any role UID is also an ordinary UID.
        A bare ``uid == g_launcher_uid`` therefore grants launcher-only treatment --
        master-reent seeding and a skipped guest reent registration -- to whichever
        thread happens to be allocated that number in a build with no launcher binding.
        """
        body = strip_comments(function_body(SCHED, "register_libc_thread"))
        self.assertIn("sched_uid_is_launcher(uid)", body)
        self.assertIn("init_guest_reent", body)
        self.assertNotRegex(
            body,
            r"uid\s*[!=]=\s*g_(launcher|worker|root)_uid",
            "register_libc_thread must ask sched_uid_is_*(), not compare UID numbers",
        )

        reent_body = strip_comments(function_body(SCHED, "init_guest_reent"))
        self.assertIn("sched_uid_is_root(uid)", reent_body)
        self.assertIn("sched_uid_is_launcher(uid)", reent_body)
        self.assertNotRegex(
            reent_body,
            r"uid\s*[!=]=\s*g_(launcher|worker|root)_uid",
            "init_guest_reent must ask sched_uid_is_*(), not compare UID numbers",
        )

    def test_role_uids_start_uncaptured(self):
        """No role global may be seeded with a value the UID allocator can hand out."""
        stripped = strip_comments(SCHED)
        for role in ("g_root_uid", "g_worker_uid", "g_launcher_uid"):
            self.assertRegex(
                stripped,
                rf"(?m)^uint32_t\s+{role}\s*=\s*SR_ROLE_UID_NONE;",
                f"{role} must start uncaptured, not at a historical allocation",
            )

    def test_no_source_compares_a_uid_against_a_role_global(self):
        """Every role question in the runtime goes through a fail-closed predicate."""
        for name, source in (("sched.c", SCHED), ("hle.c", HLE)):
            stripped = strip_comments(source)
            self.assertNotRegex(
                stripped,
                r"[!=]=\s*g_(root|worker|launcher)_uid\b",
                f"{name} must not compare a UID against a role global directly",
            )
            self.assertNotRegex(
                stripped,
                r"[!=]=\s*sched_(root|worker|launcher)_uid\s*\(\s*\)",
                f"{name} must not compare a UID against a role accessor directly",
            )


class CoroutineFallbackTests(unittest.TestCase):
    def test_no_fiber_self_switch(self):
        stripped = strip_comments(CORO)
        self.assertNotRegex(
            stripped,
            r"SwitchToFiber\s*\(\s*GetCurrentFiber\s*\(\s*\)\s*\)",
            "SwitchToFiber to the running fiber is documented-undefined",
        )

    def test_no_busy_spin_park(self):
        stripped = strip_comments(CORO)
        self.assertNotRegex(
            stripped,
            r"for\s*\(\s*;\s*;\s*\)\s*\{\s*\}",
            "an empty for(;;) park burns a host core",
        )

    def test_self_switch_guard_on_both_backends(self):
        stripped = strip_comments(CORO)
        guards = re.findall(r"to\s*==\s*s_current", stripped)
        self.assertGreaterEqual(
            len(guards), 2, "sr_coro_switch must no-op on self-switch on both backends"
        )

    def test_already_fiber_adoption_uses_documented_condition(self):
        stripped = strip_comments(CORO)
        self.assertIn("ERROR_ALREADY_FIBER", stripped)
        self.assertIn("IsThreadAFiber", stripped)


class LifecycleInstrumentationIsMandatoryTests(unittest.TestCase):
    """The executable proof must not be silently optional.

    This is a presence check on a preprocessor guard, which is exactly the kind of
    claim source text *can* settle: if the #error is absent, a build without
    -DSR_CORO_LIFECYCLE_TEST would produce a selftest with none of its protection
    and no indication that anything was missing.
    """

    def test_selftest_refuses_to_build_without_instrumentation(self):
        selftest = (ROOT / "src" / "rt" / "hle_thread_selftest.c").read_text(encoding="utf-8")
        self.assertIn("#ifndef SR_CORO_LIFECYCLE_TEST", selftest)
        self.assertIn("#error", selftest)

    def test_makefile_defines_it_for_that_target_only(self):
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        # Recipe lines only: a comment mentioning the macro compiles nothing.
        defines = [line for line in makefile.splitlines()
                   if line.startswith("\t") and "-DSR_CORO_LIFECYCLE_TEST" in line]
        self.assertEqual(len(defines), 1,
                         "exactly one build recipe may compile the instrumentation, got "
                         + repr(defines))

    def test_instrumentation_is_macro_guarded_in_production_source(self):
        self.assertIn("#ifdef SR_CORO_LIFECYCLE_TEST", CORO)
        # Every counter helper lives inside the guard; none may be defined unconditionally.
        for symbol in ("lc_note_adopt", "lc_note_create", "lc_note_destroy", "lc_note_switch"):
            self.assertIn(symbol, CORO)
        guard_start = CORO.index("#ifdef SR_CORO_LIFECYCLE_TEST")
        guard_end = CORO.index("#endif /* SR_CORO_LIFECYCLE_TEST */")
        for symbol in ("lc_note_adopt", "lc_note_create", "lc_note_destroy", "lc_note_switch"):
            self.assertTrue(guard_start < CORO.index("static void " + symbol
                                                     if symbol != "lc_note_destroy"
                                                     else "static int " + symbol) < guard_end,
                            symbol + " is defined outside the SR_CORO_LIFECYCLE_TEST guard")


class StackAllocationTests(unittest.TestCase):
    def test_oversized_stack_fails_instead_of_clamping(self):
        self.assertNotIn("clamping to avail", SCHED)
        body = strip_comments(function_body(SCHED, "sched_create_thread_finish"))
        self.assertIn("failing create", body)

    def test_create_thread_maps_failure_to_no_memory(self):
        body = strip_comments(function_body(HLE, "h_CreateThread"))
        self.assertIn("0x80020190", body, "a failed create must surface NO_MEMORY to the guest")


class NovbpaceSemanticsTests(unittest.TestCase):
    """SR_NOVBPACE must enforce value semantics: unset/empty/'0' = paced, '1' = turbo, any other refused."""

    def test_pace_setup_checks_value_semantics_source(self):
        body = strip_comments(function_body(SCHED, "pace_setup"))
        self.assertIn("SR_NOVBPACE", body)
        self.assertIn('strcmp(e, "0")', body)
        self.assertIn('strcmp(e, "1")', body)
        self.assertIn("exit(1)", body)
        self.assertIn("SR_NOVBPACE: invalid value", body)

    def test_pace_setup_binary_execution(self):
        bin_candidates = list((ROOT / "build").glob("**/sched_selftest*.exe"))
        if not bin_candidates:
            bin_candidates = list((ROOT / "build").glob("**/sched_selftest*"))
        if not bin_candidates:
            self.skipTest("sched_selftest executable not found; skipping binary execution test")

        exe = str(bin_candidates[0])

        # Unset -> paced (s_pace_on=1)
        env = {k: v for k, v in os.environ.items() if k != "SR_NOVBPACE"}
        res = subprocess.run([exe, "--test-pace-setup"], env=env, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("s_pace_on=1", res.stdout)

        # Empty string -> paced (s_pace_on=1)
        env["SR_NOVBPACE"] = ""
        res = subprocess.run([exe, "--test-pace-setup"], env=env, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("s_pace_on=1", res.stdout)

        # "0" -> paced (s_pace_on=1)
        env["SR_NOVBPACE"] = "0"
        res = subprocess.run([exe, "--test-pace-setup"], env=env, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("s_pace_on=1", res.stdout)

        # "1" -> turbo (s_pace_on=0)
        env["SR_NOVBPACE"] = "1"
        res = subprocess.run([exe, "--test-pace-setup"], env=env, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("s_pace_on=0", res.stdout)

        # Invalid values -> refused (fail closed)
        for invalid in ("2", "turbo", "false", "true", "yes", "-1"):
            env["SR_NOVBPACE"] = invalid
            res = subprocess.run([exe, "--test-pace-setup"], env=env, capture_output=True, text=True)
            self.assertNotEqual(res.returncode, 0, f"Expected rejection for SR_NOVBPACE={invalid}")
            self.assertIn("SR_NOVBPACE: invalid value", res.stderr)


# ---------------------------------------------------------------------------
# Spec coverage: every scheduler symbol consumed outside sched.c is mapped by
# docs/cleanroom/SCHED_SPEC.md (issue #340).
#
# Surface: functions and globals with external linkage defined at file scope in
# sched.c. Macros are not linkage symbols and are out of scope.
#
# Consumers: C/C++ sources and headers under src/ and tools/ that do not
# #include sched.c, plus non-test Python modules under tools/ (the code generator
# emits calls to some symbols; the memory debugger resolves others by name). A
# white-box TU that #includes sched.c sees the statics directly and is not an
# outside consumer. Test modules (test_*.py) name symbols to assert on source
# text or to drive binaries, so they are not consumers either.
#
# Mapping: a SEAM: marker in SCHED_SPEC.md section 2, or a row of the section 10
# table. Section 10 rows are "| `symbol` | §N | contract |".

SPEC = (ROOT / "docs" / "cleanroom" / "SCHED_SPEC.md").read_text(encoding="utf-8")
RECOMP_H = (ROOT / "src" / "rt" / "recomp.h").read_text(encoding="utf-8")

C_KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "else", "do", "case"}
FUNCTION_HEAD = re.compile(
    r"^(?!static\b|extern\b|typedef\b|return\b)[A-Za-z_][^\n;(]*?\b([A-Za-z_]\w*)\s*\(", re.M)
GLOBAL_DEF = re.compile(
    r"^(?!static\b|extern\b|typedef\b|return\b)[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*"
    r"(?:__attribute__\s*\(\(.*?\)\)\s*)?[=;\[]", re.M)
SEAM_MARKER = re.compile(r"SEAM: (\w+)")
MAP_ROW = re.compile(r"^\|\s*`(\w+)`\s*\|\s*§(\d+(?:\.\d+)?)\s*\|\s*(.+?)\s*\|\s*$", re.M)


def strip_c_comments(source):
    """Blank C comments but keep newlines, so line-anchored patterns still hold."""
    def blank(match):
        return re.sub(r"[^\n]", " ", match.group(0))
    source = re.sub(r"/\*.*?\*/", blank, source, flags=re.S)
    return re.sub(r"//[^\n]*", blank, source)


def scheduler_surface(source):
    """Names with external linkage defined at file scope in sched.c."""
    text = strip_c_comments(source)
    names = set()
    for match in FUNCTION_HEAD.finditer(text):
        end = text.find("\n", match.end())
        line = text[match.start():] if end == -1 else text[match.start():end]
        if line.rstrip().endswith(";"):
            continue  # a prototype, not a definition
        names.add(match.group(1))
    names.update(GLOBAL_DEF.findall(text))
    return names - C_KEYWORDS


def outside_consumers():
    """Yield (path, text) for each file that consumes the scheduler surface outside sched.c."""
    sched_path = ROOT / "src" / "rt" / "sched.c"
    for area in ("src", "tools"):
        for dirpath, dirnames, filenames in os.walk(ROOT / area):
            dirnames[:] = [d for d in dirnames if d != "__pycache__"]
            for name in filenames:
                path = Path(dirpath) / name
                if path == sched_path:
                    continue
                if name.endswith((".c", ".h", ".cc", ".cpp", ".hpp")):
                    text = path.read_text(encoding="utf-8", errors="replace")
                    if re.search(r'^\s*#\s*include\s+"sched\.c"', text, re.M):
                        continue  # white-box fixture, not an outside consumer
                    yield path, strip_c_comments(text)
                elif area == "tools" and name.endswith(".py") and not name.startswith("test_"):
                    yield path, path.read_text(encoding="utf-8", errors="replace")


def consumed_scheduler_symbols(surface):
    if not surface:
        return set()
    pattern = re.compile(r"\b(?:" + "|".join(sorted(re.escape(n) for n in surface)) + r")\b")
    used = set()
    for _path, text in outside_consumers():
        used.update(pattern.findall(text))
    return used & surface


def spec_part(start, end):
    """Text from the start heading to the end heading; empty when the start heading is absent."""
    begin = SPEC.find(start)
    if begin == -1:
        return ""
    stop = SPEC.find(end, begin + 1) if end else -1
    return SPEC[begin:stop] if stop != -1 else SPEC[begin:]


def spec_heading_pattern(section):
    if "." in section:
        return rf"^### {re.escape(section)}\b"
    return rf"^## {re.escape(section)}\. "


class SpecSeamCoverageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.surface = scheduler_surface(SCHED)
        cls.rows = MAP_ROW.findall(spec_part("## 10. ", None))
        cls.seams = set(SEAM_MARKER.findall(spec_part("## 2. Normative interface", "## 3. Behaviour")))

    def test_surface_extraction_finds_the_seam(self):
        for name in ("sched_init", "sched_wait_begin", "sched_thread_info", "s_cpu", "sr_thread_k0"):
            self.assertIn(name, self.surface)
        for name in ("pick_next", "scheduler_service_pending"):
            self.assertNotIn(name, self.surface, "static helpers are not linkage symbols")

    def test_every_consumed_symbol_is_mapped_by_the_spec(self):
        consumed = consumed_scheduler_symbols(self.surface)
        self.assertTrue(consumed, "no outside consumer found; the consumer scan is broken")
        mapped = self.seams | {symbol for symbol, _section, _contract in self.rows}
        missing = sorted(consumed - mapped)
        self.assertEqual(
            missing, [],
            "scheduler symbols consumed outside sched.c but not mapped in SCHED_SPEC.md "
            "(add a section 10 row for each): " + ", ".join(missing))

    def test_mapping_table_is_present(self):
        self.assertIn("\n## 10. ", SPEC, "SCHED_SPEC.md has no section 10 mapping table")
        self.assertTrue(self.rows, "SCHED_SPEC.md section 10 has no mapping rows")

    def test_each_symbol_has_one_row(self):
        symbols = [symbol for symbol, _section, _contract in self.rows]
        duplicates = sorted({s for s in symbols if symbols.count(s) > 1})
        self.assertEqual(duplicates, [], "symbols mapped more than once")

    def test_each_row_points_at_a_real_section(self):
        for symbol, section, _contract in self.rows:
            with self.subTest(symbol=symbol, section=section):
                self.assertRegex(SPEC, re.compile(spec_heading_pattern(section), re.M))

    def test_each_row_names_a_symbol_the_scheduler_declares(self):
        declared = SCHED + "\n" + RECOMP_H
        for symbol, _section, _contract in self.rows:
            with self.subTest(symbol=symbol):
                self.assertRegex(declared, rf"\b{re.escape(symbol)}\b")

    def test_each_row_cites_a_source_or_records_a_decision(self):
        for symbol, _section, contract in self.rows:
            with self.subTest(symbol=symbol):
                self.assertTrue(re.search(r"\[C\d+\]", contract)
                                or re.search(r"project decision", contract, re.I),
                                "the reading rule needs a [Cn] citation or a project decision")


class HostThreadCreatorAuditTests(unittest.TestCase):
    """Only the documented OSK worker may create a native host thread."""

    def test_native_thread_creators_are_allowlisted(self):
        pattern = (
            r"(^|[^[:alnum:]_])(SDL_CreateThread|CreateThread|_beginthread(ex)?|"
            r"pthread_create|thrd_create|std::jthread|std::thread)[[:space:]]*\("
        )
        result = subprocess.run(
            ["git", "grep", "-n", "-I", "-E", pattern, "--", "src/rt", "src/core"],
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        self.assertIn(result.returncode, (0, 1), result.stderr)
        creators = []
        for line in result.stdout.splitlines():
            path, _line_number, _source = line.split(":", 2)
            if "selftest" not in Path(path).name.lower():
                creators.append(path)
        self.assertEqual(
            creators, ["src/rt/osk_text_entry.c"],
            "native thread creation needs a documented owner and guest-state quiescence rule",
        )


class HostCallbackEntryAuditTests(unittest.TestCase):
    """Code that another thread runs without the runtime creating one: SDL audio and timer
    callbacks, event watches, OS thread pools, waits, timers and asynchronous work queues.
    Each registration needs a documented owner and the reason a paused guest cannot observe
    it (docs/ARCHITECTURE.md, "Runtime host-thread audit")."""

    PATTERN = (
        r"(^|[^[:alnum:]_])(SDL_SetAudioPostmixCallback|SDL_SetAudioStream(Get|Put)Callback|"
        r"SDL_SetAudioIterationCallbacks|SDL_OpenAudioDeviceStream|SDL_AddTimer(NS)?|"
        r"SDL_AddEventWatch|SDL_SetEventFilter|SDL_AddHintCallback|SDL_Show(Open|Save)FileDialog|"
        r"SDL_ShowOpenFolderDialog|SDL_ShowFileDialogWithProperties|QueueUserWorkItem|"
        r"CreateThreadpool[[:alnum:]_]*|TrySubmitThreadpoolCallback|RegisterWaitForSingleObject|"
        r"CreateTimerQueueTimer|timeSetEvent|SetTimer|SetWaitableTimer(Ex)?|ReadFileEx|WriteFileEx|"
        r"BindIoCompletionCallback|MFPutWorkItem(Ex|2)?|MFScheduleWorkItem|MFCreateAsyncResult|"
        r"BeginGetEvent)[[:space:]]*\("
    )
    ALLOWED = {
        # The SR_AUDIODUMP postmix writer copies the device's mixed output into a host WAV
        # file; it reads no guest memory and writes no state the guest reads.
        ("src/rt/audio_unavailable.c", "SDL_SetAudioPostmixCallback"),
    }

    def _registrations(self):
        result = subprocess.run(
            ["git", "grep", "-n", "-I", "-o", "-E", self.PATTERN, "--", "src/rt", "src/core"],
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
        )
        self.assertIn(result.returncode, (0, 1), result.stderr)
        found = set()
        for line in result.stdout.splitlines():
            path, _line_number, match = line.split(":", 2)
            if "selftest" in Path(path).name.lower():
                continue
            name = re.sub(r"^[^A-Za-z_]+", "", match).split("(")[0].strip()
            found.add((path, name))
        return found

    def test_callback_entries_are_allowlisted(self):
        self.assertEqual(
            self._registrations(), self.ALLOWED,
            "a host callback entry needs a documented owner and guest-state quiescence rule",
        )

    def test_pattern_refuses_each_entry_kind(self):
        refused = (
            "SDL_SetAudioStreamGetCallback(s, f, u);", "SDL_SetAudioStreamPutCallback(s, f, u);",
            "SDL_SetAudioPostmixCallback(d, f, u);", "SDL_OpenAudioDeviceStream(d, &spec, f, u);",
            "SDL_AddTimer(10, f, u);", "SDL_AddTimerNS(10, f, u);", "SDL_AddEventWatch(f, u);",
            "SDL_SetEventFilter(f, u);", "SDL_ShowOpenFileDialog(f, u, w, 0, 0, 0, 0);",
            "QueueUserWorkItem(f, u, 0);", "CreateThreadpoolWork(f, u, 0);",
            "TrySubmitThreadpoolCallback(f, u, 0);", "RegisterWaitForSingleObject(&h, e, f, u, 0, 0);",
            "CreateTimerQueueTimer(&t, 0, f, u, 0, 0, 0);", "timeSetEvent(1, 0, f, 0, 0);",
            "SetTimer(w, 1, 10, f);", "ReadFileEx(h, b, n, &o, f);", "MFPutWorkItem(q, cb, s);",
            "gen->lpVtbl->BeginGetEvent(gen, cb, 0);",
        )
        allowed = ("SDL_GetAudioStreamQueued(s);", "my_SetTimer_helper(1);", "SDL_PauseAudioDevice(d);")
        for text in refused + allowed:
            with self.subTest(text=text):
                matched = re.search(_posix_classes_to_python(self.PATTERN), text) is not None
                self.assertEqual(matched, text in refused)


def _posix_classes_to_python(pattern):
    return pattern.replace("[:alnum:]", "A-Za-z0-9").replace("[:space:]", " \\t")


class PauseRuntimeAssertionTests(unittest.TestCase):
    def test_time_refresh_aborts_inside_the_pause_loop(self):
        candidates = (
            ROOT / "build" / "hst" / "sched_selftest_generic.exe",
            ROOT / "build" / "hst" / "sched_selftest_generic",
        )
        binary = next((path for path in candidates if path.is_file()), None)
        if binary is None:
            self.skipTest("sched-selftest binary has not been built")
        result = subprocess.run(
            [str(binary), "--pause-forbidden-vtime"], cwd=ROOT, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10, check=False,
        )
        self.assertNotEqual(result.returncode, 0, "vtime_refresh unexpectedly ran while paused")
        self.assertIn(
            "sched pause invariant: vtime_refresh ran while the guest was paused",
            result.stdout,
        )


if __name__ == "__main__":
    unittest.main()
