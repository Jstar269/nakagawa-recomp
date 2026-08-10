# SPDX-License-Identifier: GPL-2.0-or-later

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HLE_SOURCE = (ROOT / "src" / "rt" / "hle.c").read_text(encoding="utf-8")


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
        self.assertIn("sr_callback_notify(s_umd_cb_uid, 0x32u)", body)
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
