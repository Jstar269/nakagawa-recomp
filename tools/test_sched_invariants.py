# SPDX-License-Identifier: GPL-2.0-or-later

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

import re
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


if __name__ == "__main__":
    unittest.main()
