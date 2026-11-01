# SPDX-License-Identifier: GPL-3.0-or-later

import re
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hle_sources  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
HLE_SOURCE = hle_sources.text()


def function_body(name):
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", HLE_SOURCE)
    if not match:
        raise AssertionError(f"{name} not found in src/rt/hle.c")
    start = match.end() - 1
    depth = 0
    for pos in range(start, len(HLE_SOURCE)):
        char = HLE_SOURCE[pos]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return HLE_SOURCE[start : pos + 1]
    raise AssertionError(f"{name} has no closing brace")


class UmdWakeupIsolationTests(unittest.TestCase):
    def test_ready_signal_only_wakes_umd_waiters_and_notifies_registered_callback(self):
        body = function_body("sr_umd_signal_ready")
        self.assertIn("sched_wake(0x554D44u)", body)
        # The ready signal restores the measured activated drive state (0x32) before it
        # notifies the callback. The callback argument follows the compiled SDK version
        # (0x22 with none set, 0x32 otherwise); hle_thread_selftest checks both values.
        self.assertRegex(HLE_SOURCE, r"\bUMD_STAT_ACTIVE\s*=\s*0x32u\b")
        self.assertRegex(HLE_SOURCE, r"\bUMD_STAT_DEACTIVATED\s*=\s*0x12u\b")
        self.assertIn("s_umd_drive_stat = UMD_STAT_ACTIVE;", body)
        self.assertIn("g_sdk_version != 0u", body)
        self.assertIn("sr_callback_notify(s_umd_cb_uid, callback_stat)", body)
        self.assertLess(body.index("s_umd_drive_stat = UMD_STAT_ACTIVE;"),
                        body.index("sr_callback_notify(s_umd_cb_uid, callback_stat)"))

    def test_deactivate_notifies_the_deactivated_drive_state(self):
        body = function_body("h_UmdDeactivate")
        self.assertLess(body.index("s_umd_drive_stat = UMD_STAT_DEACTIVATED;"),
                        body.index("sr_callback_notify(s_umd_cb_uid, s_umd_drive_stat)"))
        self.assertNotRegex(body, r"\bsched_thread_wakeup\s*\(")

    def test_callback_registration_validates_kernel_callback_id(self):
        body = function_body("h_UmdRegisterUMDCallBack")
        self.assertIn("sr_callback_is_valid(A0)", body)
        self.assertIn("0x80010016u", body)
        self.assertIn("s_umd_cb_uid = A0", body)

    def test_plain_wait_does_not_create_a_drive_event(self):
        body = function_body("h_UmdWaitDriveStat")
        self.assertNotIn("sr_umd_signal_ready", body)

    def test_cb_wait_consumes_pending_callback(self):
        body = function_body("h_UmdWaitDriveStatCB")
        self.assertIn("sr_thread_has_pending_callbacks", body)
        self.assertIn("sr_thread_dispatch_callbacks", body)


if __name__ == "__main__":
    unittest.main()
