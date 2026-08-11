# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Host-safety regression tests for the standalone C PGD implementation.

The golden vectors below are produced by the independent Python reference
(tools/pgd_decrypt.py) under the synthetic constants in tools/pgd_test_keys.py,
so this still cross-validates the C port against a separate implementation
without either side needing real console keys. Regenerate them the same way if
the synthetic constants ever change.
"""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pgd_test_keys  # noqa: E402,F401  (sets SR_PGD_KEYS; inherited by subprocesses)


ROOT = Path(__file__).resolve().parent.parent
PGD_C = ROOT / "src" / "rt" / "pgd.c"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestPgdHardening(unittest.TestCase):
    def test_malformed_boundaries_and_streaming_equivalence(self):
        assert CC is not None
        source = f'''\\
#include "{PGD_C.as_posix()}"

static int bytes_equal(const uint8_t *a, const uint8_t *b, size_t n) {{
    return memcmp(a, b, n) == 0;
}}

int main(void) {{
    uint32_t aligned = 0xdeadbeefu;
    if (!pgd_validate_sizes(0u, 16u, &aligned) || aligned != 0u) return 1;
    if (!pgd_validate_sizes(1u, 16u, &aligned) || aligned != 16u) return 2;
    if (!pgd_validate_sizes(0xfffffff0u, 16u, &aligned) || aligned != 0xfffffff0u) return 3;
    if (pgd_validate_sizes(0xffffffffu, 16u, &aligned)) return 4;
    if (pgd_validate_sizes(16u, 0u, &aligned)) return 5;
    if (pgd_validate_sizes(16u, 17u, &aligned)) return 6;
    if (pgd_validate_sizes(16u, 16u, NULL)) return 7;
    /* block_size allocation cap: the largest accepted aligned value, the
     * first rejected aligned value, and aligned 32-bit boundary values.
     * Rejection happens here, before sr_pgd_open's two allocations. */
    if (!pgd_validate_sizes(16u, SR_PGD_MAX_BLOCK_SIZE, &aligned)) return 20;
    if (pgd_validate_sizes(16u, SR_PGD_MAX_BLOCK_SIZE + 16u, &aligned)) return 21;
    if (pgd_validate_sizes(16u, 0x7ffffff0u, &aligned)) return 22;
    if (pgd_validate_sizes(16u, 0x80000000u, &aligned)) return 23;
    if (pgd_validate_sizes(16u, 0xfffffff0u, &aligned)) return 24;

    kirk_init();
    uint8_t key[16];
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)(i * 13 + 7);
    uint8_t tmp2[16];
    bbcipher_tmp2(key, tmp2);

    uint8_t sentinel = 0xa5u;
    bbcipher_apply(tmp2, 0u, &sentinel, 0u);
    if (sentinel != 0xa5u) return 8;

    uint8_t data[64];
    for (int i = 0; i < 64; i++) data[i] = (uint8_t)(i * 37 + 64 * 11);
    bbcipher_apply(tmp2, 0u, data, sizeof(data));
    static const uint8_t expected_cipher[64] = {{
        0x6c,0xea,0xbf,0x22,0x21,0x5f,0x08,0x17,0x53,0x26,0xc6,0x3f,0xc5,0x73,0xda,0x0f,
        0x1f,0x64,0x63,0x11,0x46,0x45,0xba,0x9c,0x86,0x8d,0x31,0x6c,0x1f,0x52,0x59,0x41,
        0x9c,0xb6,0xf2,0xe8,0xe4,0x5e,0x11,0x04,0xf4,0xa5,0x3d,0x93,0x9b,0x5a,0xba,0xed,
        0x53,0x35,0xd3,0x48,0xe8,0xb6,0x52,0xfe,0x7e,0x55,0x6b,0xfc,0x88,0xa5,0xd5,0xa0,
    }};
    if (!bytes_equal(data, expected_cipher, sizeof(data))) return 9;

    uint8_t macdata[128], mac[16], vkey[16];
    for (int i = 0; i < 128; i++) macdata[i] = (uint8_t)(i * 5 + 128);
    for (int i = 0; i < 16; i++) vkey[i] = (uint8_t)(i * 9 + 3);
    static const uint8_t expected_mac0[16] = {{
        0x6a,0x44,0xdb,0xf1,0x0b,0x46,0xec,0xff,0x53,0x9e,0x33,0x87,0x46,0xd4,0xef,0x22,
    }};
    static const uint8_t expected_mac1[16] = {{
        0x1a,0xd8,0x77,0x9e,0x7c,0x1e,0x41,0xa1,0x2c,0x40,0xb2,0x58,0xfe,0xeb,0xf7,0xb3,
    }};
    bbmac(macdata, 128, NULL, mac);
    if (!bytes_equal(mac, expected_mac0, sizeof(mac))) return 10;
    bbmac(macdata, 128, vkey, mac);
    if (!bytes_equal(mac, expected_mac1, sizeof(mac))) return 11;

    if (sr_pgd_data_size(NULL) != 0u || sr_pgd_block_size(NULL) != 0u ||
        sr_pgd_data_offset(NULL) != 0u || sr_pgd_block_len(NULL, 0u) != 0u) return 12;

    uint8_t dummy_header[0x90] = {{0}};
    uint8_t dummy_vkey[16] = {{0}};
    if (sr_pgd_open(NULL, dummy_vkey) != NULL) return 13;
    if (sr_pgd_open(dummy_header, NULL) != NULL) return 14;

    FILE *host = tmpfile();
    if (!host) return 15;
    if (pgd_seek_abs(host, UINT64_MAX) == 0) return 16;

    SrPgd pgd = {{0}};
    uint8_t cache[16] = {{0}}, cipher[16] = {{0}};
    pgd.block_size = 16u;
    pgd.align_size = 16u;
    pgd.cached_index = UINT32_MAX;
    pgd.cache_valid = 0;
    pgd.cache = cache;
    pgd.cipher = cipher;
    if (sr_pgd_block(&pgd, host, UINT32_MAX) != NULL) return 17;
    if (sr_pgd_block(NULL, host, 0u) != NULL) return 18;
    if (sr_pgd_block(&pgd, NULL, 0u) != NULL) return 19;
    fclose(host);

    puts("PGD hardening selftest: OK");
    return 0;
}}
'''
        with tempfile.TemporaryDirectory(prefix="pgd_harden_") as tmp:
            src = Path(tmp) / "pgd_hardening_test.c"
            exe = Path(tmp) / "pgd_hardening_test"
            src.write_text(source, encoding="utf-8")
            compiled = subprocess.run(
                [
                    CC,
                    "-std=c11",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(src),
                    "-o",
                    str(exe),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr + run.stdout)
            self.assertIn("PGD hardening selftest: OK", run.stdout)


if __name__ == "__main__":
    unittest.main()
