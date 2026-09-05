# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Focused regression coverage for Boot #11 transport qualification.

Proves:
- top-level help category output alone => FAIL
- help module containing ldstart + prompt => shell PASS
- missing prompt => FAIL
- missing ldstart => FAIL
- host0 listing missing frozen FPU filename => FAIL
- complete shell + host0 round trips => PASS
"""

from __future__ import annotations

from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
# Locate boot11_transport_qualify.sh
CANDIDATE_SCRIPTS = [
    HERE / "boot11_transport_qualify.sh",
    HERE.parent / "tools" / "boot11_transport_qualify.sh",
    Path("/mnt/c/nk/reports/psp-hardware-gap-closure-2026-09-03/tools/boot11_transport_qualify.sh"),
    Path("C:/nk/reports/psp-hardware-gap-closure-2026-09-03/tools/boot11_transport_qualify.sh"),
]
SCRIPT_PATH = next((p for p in CANDIDATE_SCRIPTS if p.is_file()), None)
if not SCRIPT_PATH:
    raise RuntimeError("Cannot find boot11_transport_qualify.sh")

SAMPLE_TOP_LEVEL_HELP = """\
help
Command Categories

thread     - Commands to manipulate threads
module     - Commands to handle modules
memory     - Commands to manipulate memory
fileio     - Commands to handle file io
debugger   - Debug commands
misc       - Miscellaneous commands (e.g. USB, exit)

Type 'help category' for more information
host0:/> \
"""

SAMPLE_HELP_MODULE = """\
help module
Category module

modlist    - List the currently loaded modules
modinfo    - Print info about a module
modstop    - Stop a running module
modunld    - Unload a module (must be stopped)
modstun    - Stop and unload a module
modload    - Load a module
modstart   - Start a module
modexec    - LoadExec a module
modaddr    - Display info about the module at a specified address
ldstart    - Load and start a module
kill       - Kill a module and all it's threads
debug      - Start a module under GDB
modexp     - List the exports from a module
modimp     - List the imports in a module
modfindx   - Find a module's export address
modfindi   - Find a module's import address
apihook    - Hook a user mode API call
apihooks   - Hook a user mode API call with sleep
apihp      - Print the user mode API hooks
apihd      - Delete an user mode API hook
host0:/> \
"""

SAMPLE_HOST0_LISTING = """\
ls host0:/
host0:/> Listing directory host0:/
drwxr-xr-x     4096 05-09-2026 00:07 .
drwxrwxrwx     4096 05-09-2026 00:43 ..
-rwxr-xr-x   184066 04-09-2026 20:55 phaseb.prx
-rwxr-xr-x   172218 04-09-2026 20:55 probe_audio_query.prx
-rwxr-xr-x   172290 04-09-2026 20:55 probe_cache_alias.prx
-rwxr-xr-x   173538 04-09-2026 20:55 probe_fpu_vector.prx
-rwxr-xr-x   173402 04-09-2026 20:55 probe_io_matrix.prx
host0:/> \
"""


class TestBoot11TransportQualify(unittest.TestCase):
    def run_eval(self, shell_content: str | None = None, host0_content: str | None = None) -> tuple[int, str]:
        with tempfile.TemporaryDirectory() as td:
            args = ["sh", str(SCRIPT_PATH)]
            if shell_content is not None:
                sf = Path(td) / "shell.txt"
                sf.write_text(shell_content, encoding="utf-8")
                args.extend(["--test-shell-input", str(sf)])
            if host0_content is not None:
                hf = Path(td) / "host0.txt"
                hf.write_text(host0_content, encoding="utf-8")
                args.extend(["--test-host0-input", str(hf)])

            res = subprocess.run(args, capture_output=True, text=True)
            return res.returncode, res.stdout + res.stderr

    def test_top_level_help_category_output_alone_fails(self) -> None:
        rc, out = self.run_eval(shell_content=SAMPLE_TOP_LEVEL_HELP)
        self.assertNotEqual(rc, 0)
        self.assertIn("SHELL_ROUND_TRIP = FAIL", out)

    def test_help_module_containing_ldstart_and_prompt_passes(self) -> None:
        rc, out = self.run_eval(shell_content=SAMPLE_HELP_MODULE)
        self.assertEqual(rc, 0)
        self.assertIn("SHELL_ROUND_TRIP = PASS", out)

    def test_missing_prompt_fails(self) -> None:
        # Prompt host0:/> stripped
        no_prompt = SAMPLE_HELP_MODULE.replace("host0:/>", "")
        rc, out = self.run_eval(shell_content=no_prompt)
        self.assertNotEqual(rc, 0)
        self.assertIn("SHELL_ROUND_TRIP = FAIL", out)

    def test_missing_ldstart_fails(self) -> None:
        # ldstart line stripped
        lines = [line for line in SAMPLE_HELP_MODULE.splitlines() if "ldstart" not in line]
        no_ldstart = "\n".join(lines) + "\n"
        rc, out = self.run_eval(shell_content=no_ldstart)
        self.assertNotEqual(rc, 0)
        self.assertIn("SHELL_ROUND_TRIP = FAIL", out)

    def test_host0_listing_missing_frozen_fpu_filename_fails(self) -> None:
        # probe_fpu_vector.prx missing from listing
        lines = [line for line in SAMPLE_HOST0_LISTING.splitlines() if "probe_fpu_vector.prx" not in line]
        missing_fpu = "\n".join(lines) + "\n"
        rc, out = self.run_eval(host0_content=missing_fpu)
        self.assertNotEqual(rc, 0)
        self.assertIn("HOST0_ROUND_TRIP = FAIL", out)

    def test_complete_shell_and_host0_round_trips_passes(self) -> None:
        rc, out = self.run_eval(shell_content=SAMPLE_HELP_MODULE, host0_content=SAMPLE_HOST0_LISTING)
        self.assertEqual(rc, 0)
        self.assertIn("SHELL_ROUND_TRIP = PASS", out)
        self.assertIn("HOST0_ROUND_TRIP = PASS", out)
        self.assertIn("TRANSPORT_QUALIFIED = YES", out)


if __name__ == "__main__":
    unittest.main()
