# SPDX-License-Identifier: GPL-3.0-or-later

import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HLE_SOURCE = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
SCHED_SOURCE = (ROOT / "src" / "rt" / "sched.c").read_text(encoding="utf-8")
RECOMP_H_SOURCE = (ROOT / "src" / "rt" / "recomp.h").read_text(encoding="utf-8")


def strip_comments(source):
    source = re.sub(r"/\*.*?\*/", " ", source, flags=re.S)
    return re.sub(r"//[^\n]*", " ", source)


def function_body(source, name):
    for name_match in re.finditer(rf"\b{name}\s*\(", source):
        depth = 0
        close_paren = None
        for pos in range(name_match.end() - 1, len(source)):
            if source[pos] == "(":
                depth += 1
            elif source[pos] == ")":
                depth -= 1
                if depth == 0:
                    close_paren = pos
                    break
        if close_paren is None:
            continue
        rest = source[close_paren + 1 :].lstrip(" \t\r\n")
        if not rest.startswith("{"):
            continue
        start = close_paren + 1 + (len(source[close_paren + 1 :]) - len(rest))
        depth = 0
        for pos in range(start, len(source)):
            char = source[pos]
            if char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return source[start : pos + 1]
    raise AssertionError(f"{name} not found")


class CallbackCorrectnessTests(unittest.TestCase):
    def test_callback_waits_pump_callbacks(self):
        for source, name in (
            (HLE_SOURCE, "h_DelayThreadCB"),
            (HLE_SOURCE, "h_WaitSemaCB"),
            (HLE_SOURCE, "h_WaitEventFlagCB"),
            (HLE_SOURCE, "h_WaitThreadEndCB"),
            (HLE_SOURCE, "h_IoWaitAsyncCB"),
            (SCHED_SOURCE, "sched_thread_sleep_cb"),
        ):
            body = strip_comments(function_body(source, name))
            self.assertIn("sr_thread_dispatch_callbacks", body)

    def test_vblank_does_not_dispatch_generic_callbacks(self):
        body = strip_comments(function_body(HLE_SOURCE, "sr_vblank_dispatch_registered"))
        self.assertNotIn("dispatch(", body)

    def test_callback_objects_are_dynamic_and_named(self):
        self.assertNotIn("S_CALLBACKS_CAP", HLE_SOURCE)
        self.assertIn("char name[32]", HLE_SOURCE)
        self.assertIn("realloc(s_callbacks", HLE_SOURCE)
        body = strip_comments(function_body(HLE_SOURCE, "sr_callback_table_register"))
        self.assertIn("s_callbacks_len++", body)
        self.assertIn("0x80020001u", body)
        self.assertIn("0x800200D3u", body)

    def test_callback_abi_and_context_preservation_source_shape(self):
        """Structural companion to CallbackDispatchBehaviourTests below.

        The compiled probe proves what the helpers *do*; this greps what they
        must keep *out* of the dispatch path (a callback-global GP override, a
        wiped register file).  Only the negative properties live here, so a
        refactor cannot satisfy them by accident.
        """
        body = strip_comments(function_body(RECOMP_H_SOURCE, "sr_callback_dispatch_one"))
        self.assertNotIn("cpu->r[28]", body)
        self.assertNotIn("memset(cpu", body)

    def test_dispatcher_has_no_gp_override_or_arbitrary_pass_cap(self):
        body = strip_comments(function_body(HLE_SOURCE, "sr_thread_dispatch_callbacks"))
        self.assertIn("for (;;)", body)
        self.assertNotIn("sr_gp_for_callbacks", body)
        self.assertNotIn("S_CALLBACKS_CAP", body)
        self.assertIn("sr_callback_table_unregister(uid)", body)

    def test_dispatcher_selects_and_resolves_by_uid(self):
        # The dispatcher must pick one pending callback and re-resolve it by UID each
        # iteration, not carry a slot cursor across a dispatch. A callback body runs
        # guest code that can reallocate/mutate the table (register, unregister, or
        # delete-and-replace in the same slot), so a slot index cannot survive a
        # dispatch. This locks in the UID-based rescan against a slot-based regression.
        body = strip_comments(function_body(HLE_SOURCE, "sr_thread_dispatch_callbacks"))
        self.assertIn("selected_uid", body)
        self.assertIn("sr_callback_find_in_table(selected_uid)", body)

    def test_check_callback_is_boolean(self):
        body = strip_comments(function_body(HLE_SOURCE, "h_CheckCallback"))
        self.assertIn("sr_thread_dispatch_callbacks() > 0 ? 1u : 0u", body)

    def test_get_callback_count_is_observational(self):
        body = strip_comments(function_body(HLE_SOURCE, "h_GetCallbackCount"))
        self.assertIn("return s_callbacks[idx].notify_count", body)
        self.assertNotIn("pending =", body)
        self.assertNotIn("notify_count =", body)
        self.assertNotIn("notify_arg =", body)

    def test_cancel_clears_all_pending_state_and_returns_zero(self):
        body = strip_comments(function_body(HLE_SOURCE, "h_CancelCallback"))
        self.assertIn("0x800201A1u", body)
        self.assertIn("pending = 0", body)
        self.assertIn("notify_count = 0", body)
        self.assertIn("notify_arg = 0", body)
        self.assertRegex(body, r"return\s+0\s*;")

    def test_refer_status_uses_real_name_and_nonzero_size_gate(self):
        body = strip_comments(function_body(HLE_SOURCE, "h_ReferCallbackStatus"))
        self.assertIn("MEM_R32(infop) != 0", body)
        self.assertIn("cb->name[i]", body)
        self.assertIn("MEM_W32(infop + 0, 56u)", body)
        self.assertNotIn("snprintf", body)

    def test_exit_game_does_not_fire_registered_exit_callback(self):
        body = strip_comments(function_body(HLE_SOURCE, "h_ExitGame"))
        self.assertNotIn("sr_fire_exit_callbacks", body)
        self.assertNotIn("sr_fire_exit_callbacks", HLE_SOURCE)
        reg = strip_comments(function_body(HLE_SOURCE, "h_RegisterExitCallback"))
        self.assertIn("0x03090510u", reg)
        self.assertIn("0x800200D2u", reg)

    def test_power_callback_is_real_slot_registration(self):
        power_source = (ROOT / "src" / "rt" / "hle_power.c").read_text(encoding="utf-8")
        body = strip_comments(function_body(power_source, "h_PowerRegisterCallback"))
        self.assertIn("s_power_cb_slots[16]", power_source)
        for code in ("0x80000020u", "0x80000022u", "0x80000100u", "0x80000102u"):
            self.assertIn(code, body)
        self.assertIn("sr_callback_notify(cb_uid, 0x000010E4u)", body)
        self.assertIn(
            'sr_hle_register(0x04b7766e, "scePowerRegisterCallback", h_PowerRegisterCallback);',
            HLE_SOURCE,
        )

    def test_umd_registration_and_cb_wait_are_distinct(self):
        reg = strip_comments(function_body(HLE_SOURCE, "h_UmdRegisterUMDCallBack"))
        self.assertIn("sr_callback_is_valid", reg)
        self.assertIn("0x80010016u", reg)
        wait = strip_comments(function_body(HLE_SOURCE, "h_UmdWaitDriveStat"))
        self.assertNotIn("sr_umd_signal_ready", wait)
        cbwait = strip_comments(function_body(HLE_SOURCE, "h_UmdWaitDriveStatCB"))
        self.assertIn("sr_thread_dispatch_callbacks", cbwait)
        self.assertIn(
            'sr_hle_register(0x4a9e5e29, "sceUmdWaitDriveStatCB", h_UmdWaitDriveStatCB);',
            HLE_SOURCE,
        )

    def test_wait_thread_end_returns_exit_status_and_zeros_timeout(self):
        for name in ("h_WaitThreadEnd", "h_WaitThreadEndCB"):
            body = strip_comments(function_body(HLE_SOURCE, name))
            self.assertIn("h_wait_thread_status", body)
            self.assertIn("0x80020197u", body)
            self.assertIn("MEM_W32(toptr, 0)", body)
            self.assertIn("0x800201A8u", body)
        helper = strip_comments(function_body(HLE_SOURCE, "h_wait_thread_status"))
        self.assertIn("sched_thread_exit_status", helper)

    def test_terminate_delete_delegates_owned_callback_cleanup_to_scheduler(self):
        body = strip_comments(function_body(HLE_SOURCE, "h_TerminateDeleteThread"))
        self.assertNotIn("sr_callback_unregister_owner(A0)", body)
        self.assertIn("sched_terminate_thread(A0)", body)
        sched = strip_comments((ROOT / "src" / "rt" / "sched.c").read_text(encoding="utf-8"))
        self.assertIn("sr_callback_unregister_owner(t->uid)", sched)


class CallbackDispatchBehaviourTests(unittest.TestCase):
    """Run sr_callback_dispatch_one for real instead of grepping its source.

    The previous version of test_callback_abi_and_context_preservation asserted
    the C source of src/rt/recomp.h line by line ($a0 assignment strings, a
    ``CpuState save = *cpu`` literal, statement order).  A refactor that renamed
    locals or restructured the helpers could fail it while behaving correctly,
    and a behavioural break could still pass it.  These tests compile
    recomp.h's callback seam and execute it: the dispatcher is injected the
    same way hle.c injects the real dispatch(), so the production helpers run
    without linking the runtime.
    """

    # Self-contained probe: record the register file exactly as the PSP callback
    # ABI delivers it ($a0/$a1/$a2 arguments, inherited $gp, pinned $ra), return
    # $v0 = 0x2a, and print the interrupted context after the helper restores it.
    PROBE_SOURCE = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "recomp.h"

static void guest_callback(CpuState *cpu) {
    printf("cb a0=%08x a1=%08x a2=%08x gp=%08x ra=%08x\n",
           cpu->r[4], cpu->r[5], cpu->r[6], cpu->r[28], cpu->r[31]);
    cpu->r[2] = 0x0000002au;  /* the callback's $v0 */
}

/* Stand-in for recompiled code, injected exactly the way hle.c injects the
 * real dispatch(): it only runs the guest body.  A real `jal` leaves $ra =
 * call+8; it never touches $gp. */
static void stand_in_dispatch(CpuState *cpu, uint32_t entry) {
    (void)entry;
    cpu->r[31] = 0xfeed0008u;
    guest_callback(cpu);
}

/* A second dispatcher that forgets to emulate the `jal`: it leaves $ra alone.
 * The helper must still hand the callback a defined $ra (its own pin), not the
 * interrupted thread's stale one. */
static void bare_dispatch(CpuState *cpu, uint32_t entry) {
    (void)entry;
    guest_callback(cpu);
}

int main(void) {
    CpuState cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[28] = 0x12340000u;   /* interrupted thread's live $gp */
    cpu.r[11] = 0x55555555u;   /* register the ABI must preserve */
    cpu.r[2]  = 0x77777777u;   /* interrupted $v0 */
    cpu.r[31] = 0x08900040u;   /* interrupted $ra */
    cpu.pc    = 0x08900000u;   /* interrupted pc */
    cpu.hi    = 0x11111111u;
    cpu.lo    = 0x22222222u;

    /* 1: jal-like dispatcher (the real shape).  The callback must see the
     * arguments, the inherited $gp, and the jal's $ra -- never a helper-installed
     * callback-global $gp or $ra on top of the dispatcher's own. */
    (void)sr_callback_dispatch_one(
        &cpu, 0x08804000u, 7, 0x0000beefu, 0x0000c0deu, stand_in_dispatch);
    /* 2: dispatcher that does not set $ra: the helper's pin (0) must show. */
    uint32_t ret = sr_callback_dispatch_one(
        &cpu, 0x08804000u, 7, 0x0000beefu, 0x0000c0deu, bare_dispatch);

    printf("ret=%08x\n", ret);
    printf("after r28=%08x r11=%08x v0=%08x hi=%08x lo=%08x ra=%08x pc=%08x\n",
           cpu.r[28], cpu.r[11], cpu.r[2], cpu.hi, cpu.lo, cpu.r[31], cpu.pc);
    return 0;
}
"""

    @classmethod
    def setUpClass(cls):
        cls.cc = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
        if cls.cc is None:
            raise unittest.SkipTest("no C compiler on PATH")
        cls.tmp = tempfile.mkdtemp(prefix="callback_dispatch_probe_")
        try:
            source = Path(cls.tmp) / "callback_probe.c"
            source.write_text(cls.PROBE_SOURCE, encoding="ascii")
            exe = Path(cls.tmp) / (
                "callback_probe.exe" if sys.platform == "win32" else "callback_probe"
            )
            result = subprocess.run(
                [
                    cls.cc, "-std=c11", "-O0", "-Wall", "-Werror",
                    f"-I{ROOT / 'src' / 'rt'}",
                    "-o", str(exe), str(source),
                ],
                capture_output=True,
                text=True,
            )
            if result.returncode != 0:
                raise AssertionError(
                    "callback dispatch probe did not compile:\n"
                    + result.stdout
                    + result.stderr
                )
            cls.exe = str(exe)
        except BaseException:
            shutil.rmtree(cls.tmp, ignore_errors=True)
            raise

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _run_probe(self) -> str:
        result = subprocess.run([self.exe], capture_output=True, text=True, timeout=60)
        if result.returncode != 0:
            raise AssertionError(
                "callback dispatch probe crashed:\n" + result.stdout + result.stderr
            )
        return result.stdout

    def test_abi_args_inherited_gp_and_ra_ownership(self):
        """The callback body must see the PSP ABI: $a0=count, $a1=arg, $a2=common,
        the interrupted thread's $gp (never a callback-global GP), and a $ra owned
        by the dispatch path: the jal's value when the dispatcher sets one, the
        helper's pin (0) when it does not."""
        out = self._run_probe()
        self.assertIn(
            "cb a0=00000007 a1=0000beef a2=0000c0de gp=12340000 ra=feed0008",
            out,
            "the PSP callback ABI delivered by sr_callback_dispatch_one is wrong "
            "(jal-like dispatcher case):\n" + out,
        )
        self.assertIn(
            "cb a0=00000007 a1=0000beef a2=0000c0de gp=12340000 ra=00000000",
            out,
            "the helper must pin $ra=0 before dispatch so a dispatcher that "
            "never emulates the jal still hands the callback a defined $ra:\n" + out,
        )

    def test_interrupted_context_restored_and_return_value_delivered(self):
        """$v0 is observed before the snapshot is restored; every register the
        dispatcher did not own comes back exactly as the interrupted thread had it."""
        out = self._run_probe()
        self.assertIn("ret=0000002a", out, "the callback's $v0 must be the return value:\n" + out)
        self.assertIn(
            "after r28=12340000 r11=55555555 v0=77777777 hi=11111111 lo=22222222 ra=08900040 pc=08900000",
            out,
            "the interrupted context must be fully restored after the dispatch:\n" + out,
        )


if __name__ == "__main__":
    unittest.main()
