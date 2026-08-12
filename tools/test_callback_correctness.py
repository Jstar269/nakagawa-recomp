# SPDX-License-Identifier: GPL-2.0-or-later

import re
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

    def test_callback_abi_and_context_preservation(self):
        pack = strip_comments(function_body(RECOMP_H_SOURCE, "sr_callback_pack_args"))
        self.assertIn("cpu->r[4] = (uint32_t)notify_count", pack)
        self.assertIn("cpu->r[5] = notify_arg", pack)
        self.assertIn("cpu->r[6] = common_arg", pack)
        body = strip_comments(function_body(RECOMP_H_SOURCE, "sr_callback_dispatch_one"))
        self.assertIn("CpuState save = *cpu", body)
        self.assertIn("*cpu = save", body)
        self.assertIn("cpu->r[31] = 0", body)
        self.assertIn("cpu->pc = entry", body)
        self.assertNotIn("cpu->r[28]", body)
        self.assertNotIn("memset(cpu", body)
        self.assertLess(body.index("uint32_t ret = cpu->r[2]"), body.index("*cpu = save"))

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
        body = strip_comments(function_body(HLE_SOURCE, "h_PowerRegisterCallback"))
        self.assertIn("s_power_cb_slots[16]", HLE_SOURCE)
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


if __name__ == "__main__":
    unittest.main()
