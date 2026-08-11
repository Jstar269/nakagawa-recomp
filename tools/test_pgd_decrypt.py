# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Tests for the PGD/amctrl decryptor (tools/pgd_decrypt.py).

The AES layer is checked against the official NIST FIPS-197 known-answer vector
and always runs without game data. Real-file integration tests require a local
GAMEDATA.BDL plus ``HST_PGD_VKEY_HEX``; they skip when either private input is
unavailable.
"""

import os
from pathlib import Path
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pgd_test_keys  # noqa: E402,F401  (sets SR_PGD_KEYS before pgd_decrypt resolves them)

import pgd_decrypt as P


ROOT = Path(__file__).resolve().parent.parent
VKEY_ENV = "HST_PGD_VKEY_HEX"


def _private_bdl_path() -> Path:
    configured = os.environ.get("HST_GAMEDATA_BDL")
    if configured:
        return Path(configured).expanduser()
    candidates = (
        ROOT / "memstick" / "PSP" / "SAVEDATA" / "UCUS98701GAMEDATA" / "GAMEDATA.BDL",
        ROOT / "fs" / "ms0__PSP_SAVEDATA_UCUS98701GAMEDATA_GAMEDATA.BDL",
    )
    return next((path for path in candidates if path.is_file()), candidates[0])


def _private_version_key() -> bytes | None:
    raw = os.environ.get(VKEY_ENV)
    if raw is None or not raw.strip():
        return None
    raw = raw.strip()
    if re.fullmatch(r"[0-9a-fA-F]{32}", raw) is None:
        raise RuntimeError(f"{VKEY_ENV} must contain exactly 32 hexadecimal characters")
    return bytes.fromhex(raw)


BDL = _private_bdl_path()
VKEY = _private_version_key()
PRIVATE_INPUTS_AVAILABLE = BDL.is_file() and VKEY is not None


class TestAes(unittest.TestCase):
    def test_fips197_vector(self):
        key = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
        pt = bytes.fromhex("00112233445566778899aabbccddeeff")
        ct = bytes.fromhex("69c4e0d86a7b0430d8cdb78070b4c55a")
        rks = P._key_expansion(key)
        self.assertEqual(P.aes_encrypt_block(rks, pt), ct)
        self.assertEqual(P.aes_decrypt_block(rks, ct), pt)

    def test_sbox_is_selfconsistent(self):
        # Computed from GF(2^8); check against FIPS-197 fixed points + bijectivity.
        self.assertEqual(P.SBOX[0x00], 0x63)
        self.assertEqual(P.SBOX[0x53], 0xED)
        self.assertEqual(sorted(P.SBOX), list(range(256)))
        for b in range(256):
            self.assertEqual(P.INV_SBOX[P.SBOX[b]], b)

    def test_cbc_roundtrip(self):
        key = os_urandom_like(16)
        iv = os_urandom_like(16)
        data = os_urandom_like(64)
        enc = P.aes_cbc_encrypt(key, iv, data)
        self.assertEqual(P.aes_cbc_decrypt(key, iv, enc), data)

    def test_cmac_shift(self):
        # No carry: plain left shift by 1.
        self.assertEqual(P._cmac_shift(bytes([0x01] + [0] * 15)), bytes([0x02] + [0] * 15))
        # Carry (MSB set): xor 0x87 into the last byte.
        out = P._cmac_shift(bytes([0x80] + [0] * 15))
        self.assertEqual(out[15], 0x87)
        self.assertEqual(out[0], 0x00)


def os_urandom_like(n):
    # Deterministic pseudo-bytes keep the test reproducible.
    return bytes((i * 37 + 11) & 0xFF for i in range(n))


@unittest.skipUnless(
    PRIVATE_INPUTS_AVAILABLE,
    f"private GAMEDATA.BDL and {VKEY_ENV} are both required",
)
class TestPgdOnRealFile(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with BDL.open("rb") as stream:
            cls.header = stream.read(0x90)
            stream.seek(0x90)
            cls.block0 = stream.read(1024)

    def test_header_macs_verify_and_params_sane(self):
        assert VKEY is not None
        # Constructing Pgd raises unless both header MACs verify.
        pgd = P.Pgd(self.header, VKEY)
        self.assertEqual(pgd.drm_type, 1)
        self.assertEqual(pgd.block_size, 1024)
        self.assertEqual(pgd.data_offset, 0x90)
        self.assertLessEqual(pgd.data_offset + pgd.data_size, BDL.stat().st_size)

    def test_wrong_vkey_rejected(self):
        assert VKEY is not None
        bad = bytes([byte ^ 0xFF for byte in VKEY])
        with self.assertRaises(P.PgdError):
            P.Pgd(self.header, bad)

    def test_block0_decrypt_is_deterministic_and_structured(self):
        assert VKEY is not None
        pgd = P.Pgd(self.header, VKEY)
        pt1 = pgd.decrypt_block(0, self.block0)
        pt2 = pgd.decrypt_block(0, self.block0)
        self.assertEqual(pt1, pt2)
        self.assertEqual(len(pt1), 1024)
        # Real data, not random noise: a meaningful fraction of structural zeros.
        self.assertGreater(pt1.count(0), 40)


if __name__ == "__main__":
    unittest.main()
