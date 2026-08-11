# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""End-to-end malformed-PGD coverage against the hardened src/rt/pgd.c (#18).

Forges complete synthetic PGD files with the verified Python reference
(tools/pgd_decrypt.py): the BBCipher stream is a symmetric XOR keystream and
both header BBMacs are computable under public constants plus a test-chosen
version key, so a byte-exact VALID file needs no game data whatsoever. Every
malformed case is then a controlled single-field mutation of a known-valid
file, and tools/pgd_e2e_harness.c reports staged diagnostics (magic / DRM /
MAC80 / MAC70 / size validation / open / per-block reads) so each test
asserts the INTENDED rejection stage, not merely "some error".

Sanitizer coverage: the full scenario matrix is re-run against a
`-fsanitize=address,undefined` build of the same harness with strict runtime
settings (ASan leak detection, halt-on-error, and a 64 MiB max-allocation
ceiling; UBSan halt-on-error with stack traces). The allocation ceiling is a
dynamic backstop for the LARGE aligned boundary block sizes only: if one of
those multi-hundred-MiB values incorrectly reached malloc, the sanitized run
would fail. The first-above-cap value (1 MiB + 16) is below that ceiling and
is instead proven by the production validation-before-allocation control
flow plus the direct pgd_validate_sizes() regression. Windows/MinGW is
explicitly skipped (no ASan runtime); on every other host -- including the
supported Linux CI lane -- a sanitizer compile or smoke-run failure FAILS
the suite rather than skipping.

The unrepresentable-seek case (offset > INT64_MAX) is not reachable through
sr_pgd_block -- data_offset and align_size are both 32-bit so the physical
offset is < 2^33 -- and pgd_seek_abs's direct UINT64_MAX rejection is covered
by tools/test_pgd_hardening.py.
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pgd_test_keys  # noqa: E402,F401  (sets SR_PGD_KEYS before pgd_decrypt resolves them)

import pgd_decrypt as ref  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
HARNESS_C = ROOT / "tools" / "pgd_e2e_harness.c"
RT_DIR = ROOT / "src" / "rt"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")

VKEY = bytes(range(0x40, 0x50))
WRONG_VKEY = bytes(range(0xA0, 0xB0))
HEADER_KEY = bytes((i * 7 + 1) & 0xFF for i in range(16))
DKEY = bytes((i * 11 + 5) & 0xFF for i in range(16))


def _align16(n: int) -> int:
    return (n + 15) & ~15


def forge_pgd(
    plaintext: bytes,
    *,
    block_size: int = 32,
    data_size: int | None = None,
    data_offset: int = 0x90,
    key_index: int = 1,
    vkey: bytes = VKEY,
    payload_blocks: int | None = None,
    file_pad: int = 0,
) -> bytes:
    """Build a complete synthetic PGD file the C implementation accepts.

    `data_size`/`block_size` are written into the encrypted parameter block
    verbatim (so malformed values exercise the real validation path), while
    the ciphertext payload is generated for `plaintext` padded to 16 bytes.
    `payload_blocks` truncates the payload to that many whole blocks;
    `file_pad` appends unencrypted filler so reads past the payload see EOF
    behavior rather than short files.
    """
    if data_size is None:
        data_size = len(plaintext)
    header = bytearray(0x90)
    header[0:4] = b"\x00PGD"
    struct.pack_into("<I", header, 4, key_index)
    struct.pack_into("<I", header, 8, 1)  # drm_type 1: fixed-key path
    header[0x10:0x20] = HEADER_KEY

    params = bytearray(0x30)
    params[0x00:0x10] = DKEY
    struct.pack_into("<I", params, 0x14, data_size & 0xFFFFFFFF)
    struct.pack_into("<I", params, 0x18, block_size & 0xFFFFFFFF)
    struct.pack_into("<I", params, 0x1C, data_offset & 0xFFFFFFFF)
    # XOR stream cipher: "decrypting" the plaintext params encrypts them.
    header[0x30:0x60] = ref.bbcipher_decrypt(HEADER_KEY, vkey, 0, bytes(params))

    mac_type = 3 if key_index > 1 else 1

    def store_mac(mac: bytes) -> bytes:
        # mac_type 3 stores kirk4(mac); the verifier kirk7-decrypts it back.
        return ref.kirk4(mac, 0x63) if mac_type == 3 else mac

    header[0x70:0x80] = store_mac(ref.bbmac(bytes(header[0:0x70]), mac_type, vkey))
    header[0x80:0x90] = store_mac(
        ref.bbmac(bytes(header[0:0x80]), mac_type, ref._k("dnas_1a90"))
    )

    padded = plaintext + b"\x00" * (_align16(len(plaintext)) - len(plaintext))
    payload = bytearray()
    if block_size > 0 and block_size % 16 == 0:
        n_blocks = (len(padded) + block_size - 1) // block_size if padded else 0
        if payload_blocks is not None:
            n_blocks = min(n_blocks, payload_blocks)
        for i in range(n_blocks):
            chunk = padded[i * block_size : (i + 1) * block_size]
            seed = (i * block_size) >> 4
            payload += ref.bbcipher_decrypt(DKEY, vkey, seed, chunk)

    blob = bytearray(header)
    # Pad up to small offsets so the payload lands where advertised; huge
    # offsets (the past-EOF tests) intentionally leave the file short.
    if len(blob) < data_offset <= 0x100000:
        blob += b"\xEE" * (data_offset - len(blob))
    blob += payload
    blob += b"\xEE" * file_pad
    return bytes(blob)


class HarnessMixin:
    """Compiles the harness once per concrete class; the sanitized subclass
    overrides SAN_FLAGS. Not a TestCase itself so it is not collected twice."""

    SAN_FLAGS: tuple[str, ...] = ()
    exe: str

    @classmethod
    def setUpClass(cls) -> None:
        assert CC is not None
        cls.tmp = tempfile.mkdtemp(prefix="pgd_e2e_")
        cls.exe = os.path.join(cls.tmp, "pgd_e2e_harness.exe")
        cmd = [
            CC,
            "-std=c11",
            "-O1",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{RT_DIR}",
            *cls.SAN_FLAGS,
            "-o",
            cls.exe,
            str(HARNESS_C),
        ]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            # Windows/MinGW is excluded up front via skipIf on the sanitized
            # class; on every supported host a sanitizer build failure is a
            # real failure, never a skip.
            raise AssertionError("harness did not compile:\n" + result.stderr)
        if cls.SAN_FLAGS:
            smoke = cls._exec(cls.exe, forge_pgd(b"x" * 16), "probe")
            if smoke.returncode != 0 or "OPEN OK" not in smoke.stdout:
                raise AssertionError(
                    "sanitized harness smoke-run failed:\n" + smoke.stdout + smoke.stderr
                )

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.tmp, ignore_errors=True)

    @staticmethod
    def _exec(exe: str, blob: bytes, *args: str, vkey: bytes = VKEY):
        with tempfile.NamedTemporaryFile(delete=False, suffix=".pgd") as f:
            f.write(blob)
            path = f.name
        try:
            # Strict sanitizer runtime: leaks are errors, first error halts,
            # and allocations above 64 MiB are errors. The allocation ceiling
            # backstops the LARGE aligned boundary block-size cases (which
            # far exceed it); sub-ceiling rejected values such as the
            # first-above-cap block size are proven by control flow and the
            # direct pgd_validate_sizes() tests, not by this setting.
            env = dict(
                os.environ,
                ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:exitcode=99:"
                "max_allocation_size_mb=64:allocator_may_return_null=0",
                UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:exitcode=99",
            )
            return subprocess.run(
                [exe, path, vkey.hex(), *args],
                capture_output=True,
                text=True,
                timeout=120,
                env=env,
            )
        finally:
            os.unlink(path)

    def run_harness(self, blob: bytes, *args: str, vkey: bytes = VKEY) -> str:
        result = self._exec(self.exe, blob, *args, vkey=vkey)
        self.assertNotEqual(result.returncode, 99, "sanitizer reported an error:\n" + result.stderr)
        self.assertIn(result.returncode, (0, 2, 3), result.stdout + result.stderr)
        return result.stdout

    # ---- valid baselines -------------------------------------------------

    def test_valid_file_round_trips_bit_for_bit(self) -> None:
        plaintext = bytes((i * 31 + 3) & 0xFF for i in range(100))
        out = self.run_harness(forge_pgd(plaintext), "readall")
        self.assertIn("DATA " + plaintext.hex(), out)

    def test_valid_mac_type3_file_round_trips(self) -> None:
        plaintext = bytes((i * 5 + 9) & 0xFF for i in range(64))
        out = self.run_harness(forge_pgd(plaintext, key_index=2), "readall")
        self.assertIn("DATA " + plaintext.hex(), out)

    def test_final_partial_block_lengths_and_content(self) -> None:
        # data_size 100, block_size 32 -> align 112 -> blocks 32,32,32,16.
        plaintext = bytes((i * 13 + 1) & 0xFF for i in range(100))
        out = self.run_harness(forge_pgd(plaintext), "readall")
        self.assertIn("BLOCKLEN 0 32", out)
        self.assertIn("BLOCKLEN 2 32", out)
        self.assertIn("BLOCKLEN 3 16", out)
        self.assertNotIn("BLOCKLEN 4", out)
        self.assertIn("DATA " + plaintext.hex(), out)

    # ---- header-stage rejections ----------------------------------------

    def test_truncated_header_is_rejected_by_the_file_wrapper(self) -> None:
        # File-wrapper/harness boundary coverage: sr_pgd_open takes a fixed
        # 0x90-byte pointer and is not length-aware, so the SHORT read is
        # detected by the caller (here the harness, mirroring what any
        # production file wrapper must do) BEFORE sr_pgd_open is invoked.
        # This does not exercise a length check inside pgd.c itself.
        blob = forge_pgd(b"y" * 32)
        for cut in (0, 1, 0x40, 0x8F):
            out = self.run_harness(blob[:cut], "probe")
            self.assertIn(f"SHORT_HEADER {cut}", out)

    def test_bad_magic_rejected_as_magic_not_mac(self) -> None:
        blob = bytearray(forge_pgd(b"y" * 32))
        blob[0] = 0x50
        out = self.run_harness(bytes(blob), "probe")
        self.assertIn("MAGIC BAD", out)
        self.assertIn("OPEN NULL", out)

    def test_unsupported_drm_type_rejected(self) -> None:
        # drm_type 2 needs the per-console fuse key; open must refuse it even
        # with otherwise self-consistent MACs recomputed for the new bytes.
        plaintext = b"y" * 32
        blob = bytearray(forge_pgd(plaintext))
        struct.pack_into("<I", blob, 8, 2)
        mac70 = ref.bbmac(bytes(blob[0:0x70]), 1, VKEY)
        blob[0x70:0x80] = mac70
        blob[0x80:0x90] = ref.bbmac(bytes(blob[0:0x80]), 1, ref._k("dnas_1a90"))
        out = self.run_harness(bytes(blob), "probe")
        self.assertIn("MAGIC OK", out)
        self.assertIn("DRM 2", out)
        self.assertIn("MAC80 OK", out)
        self.assertIn("OPEN NULL", out)

    def test_corrupt_fixed_key_mac_rejected_at_mac80(self) -> None:
        blob = bytearray(forge_pgd(b"y" * 32))
        blob[0x80] ^= 0x01
        out = self.run_harness(bytes(blob), "probe")
        self.assertIn("MAGIC OK", out)
        self.assertIn("MAC80 BAD", out)
        self.assertIn("OPEN NULL", out)

    def test_wrong_version_key_rejected_at_mac70(self) -> None:
        out = self.run_harness(forge_pgd(b"y" * 32), "probe", vkey=WRONG_VKEY)
        self.assertIn("MAC80 OK", out)
        self.assertIn("MAC70 BAD", out)
        self.assertIn("OPEN NULL", out)

    # ---- size/parameter rejections (MACs valid, so the rejection stage is
    # provably the size validation) -----------------------------------------

    def assert_rejected_by_size_validation(self, blob: bytes) -> None:
        out = self.run_harness(blob, "probe")
        self.assertIn("MAC80 OK", out)
        self.assertIn("MAC70 OK", out)
        self.assertIn("VALIDATE BAD", out)
        self.assertIn("OPEN NULL", out)

    def test_zero_block_size_rejected(self) -> None:
        self.assert_rejected_by_size_validation(forge_pgd(b"y" * 32, block_size=0))

    def test_unaligned_block_sizes_rejected(self) -> None:
        for bs in (8, 17, 31, 100):
            with self.subTest(block_size=bs):
                self.assert_rejected_by_size_validation(forge_pgd(b"y" * 32, block_size=bs))

    def test_largest_accepted_aligned_block_size_round_trips(self) -> None:
        # SR_PGD_MAX_BLOCK_SIZE (1 MiB) is the documented allocation-safety
        # cap: the largest accepted value must still open AND decrypt.
        plaintext = bytes((i * 3 + 5) & 0xFF for i in range(64))
        blob = forge_pgd(plaintext, block_size=0x100000)
        out = self.run_harness(blob, "probe")
        self.assertIn("VALIDATE OK", out)
        self.assertIn("OPEN OK", out)
        out = self.run_harness(blob, "readall")
        self.assertIn("DATA " + plaintext.hex(), out)

    def test_first_rejected_aligned_block_size(self) -> None:
        # One 16-byte step past the cap: MACs verify, size validation
        # rejects. Rejection-before-allocation for THIS value (1 MiB + 16,
        # below the 64 MiB ASan allocation ceiling, so the sanitizer would
        # not flag it merely for reaching malloc) is established by the
        # production control flow -- sr_pgd_open calls pgd_validate_sizes
        # before either proportional malloc -- and by the direct
        # pgd_validate_sizes() regression in tools/test_pgd_hardening.py.
        self.assert_rejected_by_size_validation(forge_pgd(b"y" * 32, block_size=0x100010))

    def test_large_aligned_32bit_boundary_block_sizes_rejected(self) -> None:
        # Aligned values near the 32-bit boundary must be rejected by the
        # cap, never allocated. For THESE values (all far above the 64 MiB
        # ASan allocation ceiling) the sanitized run is a genuine dynamic
        # backstop: an attempted malloc would abort the harness (exit 99).
        for bs in (0x7FFFFFF0, 0x80000000, 0xFFFF0000, 0xFFFFFFF0):
            with self.subTest(block_size=bs):
                self.assert_rejected_by_size_validation(forge_pgd(b"y" * 32, block_size=bs))

    def test_data_size_alignment_wrap_rejected(self) -> None:
        # (data_size + 15) & ~15 exceeding UINT32_MAX must fail closed.
        for ds in (0xFFFFFFFF, 0xFFFFFFF1):
            with self.subTest(data_size=ds):
                self.assert_rejected_by_size_validation(forge_pgd(b"y" * 32, data_size=ds))

    def test_huge_data_size_boundary_opens_but_reads_fail_closed(self) -> None:
        # 0xFFFFFFF0 is the largest representable aligned size: parameter
        # validation passes, but every advertised block beyond the actual
        # payload must fail its read rather than fabricate data.
        blob = forge_pgd(b"y" * 32, data_size=0xFFFFFFF0)
        out = self.run_harness(blob, "probe")
        self.assertIn("VALIDATE OK", out)
        self.assertIn("OPEN OK", out)
        out = self.run_harness(blob, "read", "2")
        self.assertIn("BLOCK 2 NULL", out)
        out = self.run_harness(blob, "read", "1000000")
        self.assertIn("BLOCK 1000000 NULL", out)

    # ---- payload-stage failures ------------------------------------------

    def test_advertised_data_larger_than_file_fails_on_missing_block(self) -> None:
        # 4 advertised blocks, only 2 present in the file.
        plaintext = bytes(range(128))
        blob = forge_pgd(plaintext, payload_blocks=2)
        out = self.run_harness(blob, "read", "1")
        self.assertIn("BLOCK 1 len=32", out)
        out = self.run_harness(blob, "read", "2")
        self.assertIn("BLOCK 2 NULL len=32", out)

    def test_truncated_encrypted_payload_fails_the_short_block(self) -> None:
        plaintext = bytes(range(128))
        whole = forge_pgd(plaintext)
        # Cut mid-way through block 2's ciphertext: block 1 still reads,
        # block 2's short fread must return NULL, not partial plaintext.
        cut = whole[: 0x90 + 2 * 32 + 7]
        out = self.run_harness(cut, "read", "1")
        self.assertIn("BLOCK 1 len=32", out)
        out = self.run_harness(cut, "read", "2")
        self.assertIn("BLOCK 2 NULL len=32", out)

    def test_block_index_multiplication_boundaries_reject_cleanly(self) -> None:
        # index * block_size crossing align_size (including 64-bit products
        # far beyond 32 bits) must yield len 0 -> NULL, never a wrapped read.
        plaintext = bytes(range(64))  # align 64 -> blocks 0,1 valid
        blob = forge_pgd(plaintext)
        out = self.run_harness(blob, "read", "1")
        self.assertIn("BLOCK 1 len=32", out)
        for idx in ("2", "3", "0x7fffffff", "0xffffffff"):
            with self.subTest(index=idx):
                out = self.run_harness(blob, "read", idx)
                self.assertIn(f"BLOCK {int(idx, 0)} NULL len=0", out)

    def test_large_data_offset_past_eof_fails_the_read(self) -> None:
        # A physical offset far past EOF must fail via seek/read, not wrap.
        blob = forge_pgd(b"y" * 32, data_offset=0xFFFFFF00)
        out = self.run_harness(blob, "probe")
        self.assertIn("VALIDATE OK", out)
        self.assertIn("OPEN OK", out)
        out = self.run_harness(blob, "read", "0")
        self.assertIn("BLOCK 0 NULL", out)

    # ---- cache state across failures -------------------------------------

    def test_repeated_invalid_reads_do_not_poison_the_cache(self) -> None:
        # File contains blocks 0..1 but advertises 4; block 3 is a valid
        # index whose ciphertext is missing (fread failure path).
        plaintext = bytes(range(128))
        blob = forge_pgd(plaintext, payload_blocks=2)
        out = self.run_harness(blob, "cache", "0", "3")
        self.assertIn("CACHE OK", out)

    def test_zero_data_size_yields_no_blocks(self) -> None:
        blob = forge_pgd(b"", data_size=0)
        out = self.run_harness(blob, "probe")
        self.assertIn("VALIDATE OK", out)
        self.assertIn("OPEN OK", out)
        out = self.run_harness(blob, "readall")
        self.assertIn("DATA \n", out)
        out = self.run_harness(blob, "read", "0")
        self.assertIn("BLOCK 0 NULL len=0", out)

    # ---- reference preservation ------------------------------------------

    def test_c_blocks_match_python_reference_bit_for_bit(self) -> None:
        plaintext = bytes((i * 89 + 17) & 0xFF for i in range(200))
        blob = forge_pgd(plaintext, block_size=48)
        pgd = ref.Pgd(blob[0:0x90], VKEY)
        self.assertEqual(pgd.data_size, 200)
        self.assertEqual(pgd.block_size, 48)
        padded = plaintext + b"\x00" * (_align16(len(plaintext)) - len(plaintext))
        for i in range((len(padded) + 47) // 48):
            chunk = blob[0x90 + i * 48 : 0x90 + (i + 1) * 48]
            expect = pgd.decrypt_block(i, chunk)
            out = self.run_harness(blob, "read", str(i))
            self.assertIn(f"BLOCK {i} len={len(chunk)} {expect.hex()}", out)


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestPgdMalformedE2E(HarnessMixin, unittest.TestCase):
    """Plain build: always runs when a C compiler is present."""


@unittest.skipUnless(CC, "no C compiler on PATH")
@unittest.skipIf(
    sys.platform == "win32",
    "ASan+UBSan runtime unavailable on Windows/MinGW; the Linux CI lane runs this class",
)
class TestPgdMalformedE2ESanitized(HarnessMixin, unittest.TestCase):
    """Same matrix under ASan+UBSan with strict runtime settings.

    On any non-Windows host (including the supported Linux CI lane) a
    sanitizer compile or smoke-run failure fails the suite -- setUpClass
    raises AssertionError, never SkipTest.
    """

    SAN_FLAGS = ("-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g")


if __name__ == "__main__":
    unittest.main()
