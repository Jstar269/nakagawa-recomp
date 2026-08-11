# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Guard the runtime C PGD decryptor (src/rt/pgd.c) against the Python oracle.

``src/rt/pgd.c`` is a hand port of ``tools/pgd_decrypt.py``. This test compiles
the C implementation standalone and always runs its AES FIPS-197 self-test.
Real-file comparison additionally requires a local GAMEDATA.BDL and the
``HST_PGD_VKEY_HEX`` environment variable.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pgd_test_keys  # noqa: E402,F401  (sets SR_PGD_KEYS; inherited by subprocesses)


ROOT = Path(__file__).resolve().parent.parent
PGD_C = ROOT / "src" / "rt" / "pgd.c"
VKEY_ENV = "HST_PGD_VKEY_HEX"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


def _private_bdl_path() -> Path:
    configured = os.environ.get("HST_GAMEDATA_BDL")
    if configured:
        return Path(configured).expanduser()
    candidates = (
        ROOT / "memstick" / "PSP" / "SAVEDATA" / "UCUS98701GAMEDATA" / "GAMEDATA.BDL",
        ROOT / "fs" / "ms0__PSP_SAVEDATA_UCUS98701GAMEDATA_GAMEDATA.BDL",
    )
    return next((path for path in candidates if path.is_file()), candidates[0])


def _private_version_key_hex() -> str | None:
    raw = os.environ.get(VKEY_ENV)
    if raw is None or not raw.strip():
        return None
    raw = raw.strip()
    if re.fullmatch(r"[0-9a-fA-F]{32}", raw) is None:
        raise RuntimeError(f"{VKEY_ENV} must contain exactly 32 hexadecimal characters")
    return raw


BDL = _private_bdl_path()
VKEY_HEX = _private_version_key_hex()
PRIVATE_INPUTS_AVAILABLE = BDL.is_file() and VKEY_HEX is not None


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestPgdC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert CC is not None
        cls.tmp = tempfile.mkdtemp(prefix="pgdc_")
        cls.exe = os.path.join(cls.tmp, "pgd_test.exe")
        result = subprocess.run(
            [CC, "-O2", "-DSR_PGD_TEST", "-o", cls.exe, str(PGD_C)],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise unittest.SkipTest("pgd.c did not compile:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _run(self, *args):
        return subprocess.run([self.exe, *args], capture_output=True, text=True)

    def test_aes_selftest_passes(self):
        output = self._run().stdout
        self.assertIn("AES FIPS-197 selftest: OK", output)

    @unittest.skipUnless(
        PRIVATE_INPUTS_AVAILABLE,
        f"private GAMEDATA.BDL and {VKEY_ENV} are both required",
    )
    def test_blocks_match_python_reference(self):
        import pgd_decrypt as P

        assert VKEY_HEX is not None
        output = self._run(str(BDL), VKEY_HEX).stdout
        self.assertIn("open OK", output, output)
        match = re.search(
            r"data_size=(\d+) block_size=(\d+) data_offset=(\d+)",
            output,
        )
        self.assertIsNotNone(match, output)
        assert match is not None
        c_data_size, c_block_size, c_data_offset = (
            int(match.group(index)) for index in (1, 2, 3)
        )

        with BDL.open("rb") as stream:
            pgd = P.Pgd(stream.read(0x90), bytes.fromhex(VKEY_HEX))
            self.assertEqual(
                (c_data_size, c_block_size, c_data_offset),
                (pgd.data_size, pgd.block_size, pgd.data_offset),
            )
            for line in output.splitlines():
                block_match = re.match(r"block (\d+) first16:\s*([0-9a-f ]+)", line)
                if not block_match:
                    continue
                block = int(block_match.group(1))
                c_bytes = bytes(int(value, 16) for value in block_match.group(2).split())
                stream.seek(pgd.data_offset + block * pgd.block_size)
                python_bytes = pgd.decrypt_block(block, stream.read(pgd.block_size))[:16]
                self.assertEqual(c_bytes, python_bytes, f"block {block} C/Python mismatch")


if __name__ == "__main__":
    unittest.main()
