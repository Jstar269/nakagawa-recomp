# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Malformed-input regression tests for the native runtime driver (src/rt/driver.c)."""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import title_runtime_config  # noqa: E402


ROOT = Path(__file__).resolve().parent.parent
DRIVER_C = ROOT / "src" / "rt" / "driver.c"
RT_DIR = ROOT / "src" / "rt"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang") or shutil.which("g++") or shutil.which("c++")


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestNativeDriverHardening(unittest.TestCase):
    def test_native_driver_malformed_inputs(self):
        assert CC is not None

        driver_code = DRIVER_C.read_text(encoding="utf-8")
        ph_count = driver_code.count("(uint64_t)e_phoff + (uint64_t)i * phentsize")
        self.assertGreaterEqual(
            ph_count, 2, f"Expected 64-bit ph_offset calculation in both Pass 1 and Pass 2, found {ph_count}"
        )
        self.assertNotIn("elf + e_phoff + (uint32_t)i * phentsize", driver_code)

        source = f'''\\
#define _CRT_SECURE_NO_WARNINGS
#define SR_SELFTEST_ONLY
#include "{DRIVER_C.as_posix()}"

static uint8_t g_mock_ram[0x0c000000];
uint8_t *g_mem = g_mock_ram + 0x08000000;
static int g_mem_init_calls = 0;
static int g_segment_loads = 0;
static int g_check_all_or_nothing = 0;

uint32_t g_sr_debug = 0;
int sr_hit_hle = 0;
int g_sr_metadata_watch = 0;
int g_hle_depth = 0;
CpuState *s_cpu = NULL;
void sr_oor(uint32_t address, uint32_t value, int store) {{
    (void)address; (void)value; (void)store;
}}
uint32_t sr_get_ge_status(void) {{ return 0; }}
uint32_t sched_current_uid(void) {{ return 0; }}
void sr_debug_init_watches(void) {{}}
void sr_perf_init(void) {{}}
void sr_profile_init(void) {{}}
void sr_register_all(void) {{}}
static void dummy_recomp_fn(CpuState *s) {{ (void)s; }}
RecompFn sr_lookup(uint32_t addr) {{
    if (addr == 0x08000010u || addr == (0x08000010u & 0x01ffffffu)) return dummy_recomp_fn;
    return NULL;
}}
int sr_trace_open(const char *path, const char *title, uint32_t entry) {{ (void)path; (void)title; (void)entry; return 0; }}
void sr_trace_close(void) {{}}
void gui_init(const char *title) {{ (void)title; }}
void sched_init(CpuState *cpu) {{ (void)cpu; }}
void sched_run(uint32_t entry, uint32_t arglen, uint32_t argp) {{ (void)entry; (void)arglen; (void)argp; }}

static void check_all_or_nothing_on_exit(void) {{
    if (g_check_all_or_nothing) {{
        if (g_mem_init_calls != 0 || g_segment_loads != 0) {{
            fprintf(stderr, "ALL_OR_NOTHING_FAILED: sr_mem_init_calls=%d sr_segment_loads=%d\\n",
                    g_mem_init_calls, g_segment_loads);
            _Exit(99);
        }}
    }}
}}

void sr_mem_init(void) {{
    g_mem_init_calls++;
    memset(g_mock_ram, 0, sizeof(g_mock_ram));
}}

void sr_load_segment(uint32_t vaddr, const void *data, uint32_t len) {{
    g_segment_loads++;
    uint32_t phys = (uint32_t)(vaddr & 0x1fffffff);
    if (phys < 0x0c000000u && len <= 0x0c000000u - phys) {{
        memcpy(g_mock_ram + phys, data, len);
    }}
}}

static void wr16(uint8_t *v, size_t off, uint16_t x) {{
    memcpy(v + off, &x, sizeof(x));
}}
static void wr32(uint8_t *v, size_t off, uint32_t x) {{
    memcpy(v + off, &x, sizeof(x));
}}
static void make_base_elf(uint8_t *elf, size_t size) {{
    memset(elf, 0, size);
    memcpy(elf, "\\x7f" "ELF", 4);
    wr32(elf, 24, 0x08000010u); /* entry */
}}

int main(int argc, char **argv) {{
    if (argc < 2) return 90;
    const char *mode = argv[1];

    if (strcmp(mode, "seed_valid") == 0) {{
        if (argc < 3) return 91;
        CpuState s;
        memset(&s, 0, sizeof(s));
        for (int i = 0; i < 32; i++) {{ s.r[i] = 0x11110000u + (uint32_t)i; s.fi[i] = 0x22220000u + (uint32_t)i; }}
        s.hi = 0xaaaaaaaau; s.lo = 0xbbbbbbbbu; s.fcr31 = 0xccccccccu;
        seed_from_init(argv[2], &s);
        if (s.r[0] != 0x00000000u || s.r[31] != 0x12345678u) return 1;
        if (s.fi[0] != 0x22220000u || s.fi[31] != 0x89abcdefu) return 2;
        if (s.hi != 0x11111111u || s.lo != 0x22222222u || s.fcr31 != 0x33333333u) return 3;
        if (s.r[1] != 0x11110001u || s.r[2] != 0x11110002u || s.r[3] != 0x11110003u) return 4;
        return 0;
    }}

    if (strcmp(mode, "seed_malformed") == 0) {{
        if (argc < 3) return 92;
        uint32_t val = 0, idx = 0;
        if (!parse_hex32("00000000", &val) || val != 0u) return 101;
        if (!parse_hex32("ffffffff", &val) || val != 0xffffffffu) return 102;
        if (!parse_hex32("0x08000000", &val) || val != 0x08000000u) return 103;
        if (parse_hex32("100000000", &val)) return 104;
        if (parse_hex32("ffffffffffffffff", &val)) return 105;
        if (parse_hex32("100000000000000000000", &val)) return 106;
        if (parse_hex32("0x08000000junk", &val)) return 107;
        if (parse_hex32("+0x08000000", &val)) return 110;
        if (parse_hex32("0x+1234", &val)) return 111;
        if (parse_hex32("0x-0", &val)) return 112;
        if (parse_hex32("0x 1234", &val)) return 113;
        if (parse_hex32(" 1234", &val)) return 114;
        if (parse_hex32("+1234", &val)) return 115;
        if (parse_indexed_register("r+1", 'r', &idx)) return 108;
        if (parse_indexed_register("r-1", 'r', &idx)) return 109;

        CpuState s;
        memset(&s, 0, sizeof(s));
        for (int i = 0; i < 32; i++) {{ s.r[i] = 0x11110000u + (uint32_t)i; s.fi[i] = 0x22220000u + (uint32_t)i; }}
        seed_from_init(argv[2], &s);
        for (int i = 0; i < 32; i++) {{
            if (s.r[i] != (0x11110000u + (uint32_t)i)) return 10 + i;
            if (s.fi[i] != (0x22220000u + (uint32_t)i)) return 50 + i;
        }}
        return 0;
    }}

    if (strcmp(mode, "read_missing") == 0) {{
        size_t len = 999;
        uint8_t *d = read_file("non_existent_file_12345.bin", &len);
        (void)d;
        return 0; /* Expected to exit(2) */
    }}

    if (strcmp(mode, "read_zero") == 0) {{
        if (argc < 3) return 93;
        size_t len = 999;
        uint8_t *d = read_file(argv[2], &len);
        if (len != 0 || !d) return 1;
        free(d);
        return 0;
    }}

    if (strcmp(mode, "elf_truncated_hdr") == 0) {{
        uint8_t elf[40]; make_base_elf(elf, sizeof(elf));
        load_elf(elf, sizeof(elf)); return 10;
    }}

    if (strcmp(mode, "elf_phbounds") == 0) {{
        uint8_t elf[52]; make_base_elf(elf, sizeof(elf));
        wr32(elf, 28, 50); wr16(elf, 42, 32); wr16(elf, 44, 1);
        load_elf(elf, sizeof(elf)); return 11;
    }}

    if (strcmp(mode, "elf_phentsize") == 0) {{
        uint8_t elf[84]; make_base_elf(elf, sizeof(elf));
        wr32(elf, 28, 52); wr16(elf, 42, 4); wr16(elf, 44, 1);
        load_elf(elf, sizeof(elf)); return 12;
    }}

    if (strcmp(mode, "elf_ph_table_out_of_range") == 0) {{
        uint8_t elf[84]; make_base_elf(elf, sizeof(elf));
        wr32(elf, 28, 52); wr16(elf, 42, 65535); wr16(elf, 44, 65535);
        load_elf(elf, sizeof(elf)); return 13;
    }}

    if (strcmp(mode, "elf_segbounds") == 0) {{
        uint8_t elf[84]; make_base_elf(elf, sizeof(elf));
        wr32(elf, 28, 52); wr16(elf, 42, 32); wr16(elf, 44, 1);
        wr32(elf, 52, 1); wr32(elf, 56, 80); wr32(elf, 60, 0x08000010u); wr32(elf, 68, 8); wr32(elf, 72, 8);
        load_elf(elf, sizeof(elf)); return 14;
    }}

    if (strcmp(mode, "elf_filesz") == 0) {{
        uint8_t elf[100]; make_base_elf(elf, sizeof(elf));
        wr32(elf, 28, 52); wr16(elf, 42, 32); wr16(elf, 44, 1);
        wr32(elf, 52, 1); wr32(elf, 56, 84); wr32(elf, 60, 0x08000010u); wr32(elf, 68, 16); wr32(elf, 72, 8);
        load_elf(elf, sizeof(elf)); return 15;
    }}

    if (strcmp(mode, "elf_guest_boundary") == 0) {{
        uint8_t elf[100]; make_base_elf(elf, sizeof(elf));
        wr32(elf, 28, 52); wr16(elf, 42, 32); wr16(elf, 44, 1);
        wr32(elf, 52, 1); wr32(elf, 56, 84); wr32(elf, 60, 0x0bfffff8u); wr32(elf, 68, 16); wr32(elf, 72, 16);
        load_elf(elf, sizeof(elf)); return 16;
    }}

    if (strcmp(mode, "elf_near_uint32_max") == 0) {{
        uint8_t elf[100]; make_base_elf(elf, sizeof(elf));
        wr32(elf, 28, 52); wr16(elf, 42, 32); wr16(elf, 44, 1);
        wr32(elf, 52, 1); wr32(elf, 56, 84); wr32(elf, 60, 0xffffff00u); wr32(elf, 68, 0x200u); wr32(elf, 72, 0x200u);
        load_elf(elf, sizeof(elf)); return 17;
    }}

    if (strcmp(mode, "elf_valid") == 0) {{
        uint8_t elf[88]; make_base_elf(elf, sizeof(elf));
        wr32(elf, 28, 52); wr16(elf, 42, 32); wr16(elf, 44, 1);
        wr32(elf, 52, 1); wr32(elf, 56, 84); wr32(elf, 60, 0x08000010u); wr32(elf, 68, 4); wr32(elf, 72, 8);
        elf[84]=0xAA; elf[85]=0xBB; elf[86]=0xCC; elf[87]=0xDD;
        uint32_t entry = load_elf(elf, sizeof(elf));
        if (entry != 0x08000010u) return 20;
        if (g_mock_ram[0x08000010] != 0xAA || g_mock_ram[0x08000011] != 0xBB ||
            g_mock_ram[0x08000012] != 0xCC || g_mock_ram[0x08000013] != 0xDD) return 21;
        return 0;
    }}

    if (strcmp(mode, "elf_all_or_nothing") == 0) {{
        g_mem_init_calls = 0;
        g_segment_loads = 0;
        g_check_all_or_nothing = 1;
        atexit(check_all_or_nothing_on_exit);

        /* Two segments: segment 0 is valid, segment 1 is malformed (source beyond EOF) */
        uint8_t elf[120]; make_base_elf(elf, sizeof(elf));
        wr32(elf, 28, 52); wr16(elf, 42, 32); wr16(elf, 44, 2);
        /* Segment 0: vaddr 0x08000020, filesz 4, memsz 4, off 116 */
        wr32(elf, 52, 1); wr32(elf, 56, 116); wr32(elf, 60, 0x08000020u); wr32(elf, 68, 4); wr32(elf, 72, 4);
        elf[116]=0x11; elf[117]=0x22; elf[118]=0x33; elf[119]=0x44;
        /* Segment 1: vaddr 0x08000030, filesz 8, memsz 8, off 200 (BEYOND EOF) */
        wr32(elf, 84, 1); wr32(elf, 88, 200); wr32(elf, 92, 0x08000030u); wr32(elf, 100, 8); wr32(elf, 104, 8);

        /* Expect load_elf to reject second segment in Pass 1 and exit(2) without calling sr_mem_init or loading segment 0 */
        load_elf(elf, sizeof(elf));
        return 30;
    }}

    if (strcmp(mode, "image_invalid_base") == 0) {{
        if (argc < 3) return 94;
        char *argv_fake[] = {{ "driver", "--image", argv[2], "zzzz", "0x08000010", "none", "none" }};
        driver_main(7, argv_fake);
        return 40;
    }}

    if (strcmp(mode, "image_invalid_entry") == 0) {{
        if (argc < 3) return 95;
        char *argv_fake[] = {{ "driver", "--image", argv[2], "0x08000000", "zzzz", "none", "none" }};
        driver_main(7, argv_fake);
        return 41;
    }}

    if (strcmp(mode, "image_trailing_junk") == 0) {{
        if (argc < 3) return 96;
        char *argv_fake[] = {{ "driver", "--image", argv[2], "0x08000000junk", "0x08000010", "none", "none" }};
        driver_main(7, argv_fake);
        return 42;
    }}

    if (strcmp(mode, "image_overflow") == 0) {{
        if (argc < 3) return 97;
        char *argv_fake[] = {{ "driver", "--image", argv[2], "100000000", "0x08000010", "none", "none" }};
        driver_main(7, argv_fake);
        return 43;
    }}

    if (strcmp(mode, "image_invalid_span") == 0) {{
        if (argc < 3) return 98;
        char *argv_fake[] = {{ "driver", "--image", argv[2], "0x0bfffff0", "0x08000010", "none", "none" }};
        driver_main(7, argv_fake);
        return 44;
    }}

    if (strcmp(mode, "image_valid") == 0) {{
        if (argc < 3) return 99;
        char *argv_fake[] = {{ "driver", "--image", argv[2], "0x08000000", "0x08000010", "none", "none" }};
        return driver_main(7, argv_fake);
    }}

    if (strcmp(mode, "image_expect_valid") == 0) {{
        if (argc < 3) return 100;
        char *argv_fake[] = {{
            "driver", "--image", argv[2], "0x08000000", "0x08000010", "none", "none",
            "--expect-u32=0x08000020:0x12345678"
        }};
        return driver_main(8, argv_fake);
    }}

    if (strcmp(mode, "image_expect_mismatch") == 0) {{
        if (argc < 3) return 101;
        char *argv_fake[] = {{
            "driver", "--image", argv[2], "0x08000000", "0x08000010", "none", "none",
            "--expect-u32=0x08000020:0x87654321"
        }};
        return driver_main(8, argv_fake);
    }}

    if (strcmp(mode, "image_expect_malformed") == 0) {{
        if (argc < 3) return 102;
        char *argv_fake[] = {{
            "driver", "--image", argv[2], "0x08000000", "0x08000010", "none", "none",
            "--expect-u32=0x08000020"
        }};
        return driver_main(8, argv_fake);
    }}

    if (strcmp(mode, "image_expect_out_of_range") == 0) {{
        if (argc < 3) return 103;
        char *argv_fake[] = {{
            "driver", "--image", argv[2], "0x08000000", "0x08000010", "none", "none",
            "--expect-u32=0xffffffff:0x12345678"
        }};
        return driver_main(8, argv_fake);
    }}

    if (strcmp(mode, "image_expect_unaligned") == 0) {{
        if (argc < 3) return 104;
        char *argv_fake[] = {{
            "driver", "--image", argv[2], "0x08000000", "0x08000010", "none", "none",
            "--expect-u32=0x08000021:0x12345678"
        }};
        return driver_main(8, argv_fake);
    }}

    if (strcmp(mode, "image_expect_duplicate") == 0) {{
        if (argc < 3) return 105;
        char *argv_fake[] = {{
            "driver", "--image", argv[2], "0x08000000", "0x08000010", "none", "none",
            "--expect-u32=0x08000020:0x12345678",
            "--expect-u32=0x08000020:0x12345678"
        }};
        return driver_main(9, argv_fake);
    }}

    return 99;
}}
'''
        with tempfile.TemporaryDirectory(prefix="native_driver_harden_") as tmp:
            tmpdir = Path(tmp)
            src = tmpdir / "harness.c"
            exe = tmpdir / "harness.exe"
            valid_trace = tmpdir / "valid.trace"
            malformed_trace = tmpdir / "malformed.trace"
            zero_file = tmpdir / "zero.bin"
            sample_img = tmpdir / "sample.bin"

            src.write_text(source, encoding="utf-8")
            valid_trace.write_text(
                "# init r0=00000000 f0=22220000 r31=12345678 f31=89abcdef hi=11111111 lo=22222222 "
                "fcr31=33333333 r1=11110001 r2=11110002 r3=11110003\n",
                encoding="ascii",
            )
            malformed_trace.write_text(
                "# init r32=deadbeef f32=feedface r999=12345678 f999=87654321 "
                "r-1=55555555 r1junk=77777777 r2=zzzz r3=100000000\n",
                encoding="ascii",
            )
            zero_file.write_bytes(b"")
            sample_bytes = bytearray(256)
            sample_bytes[0x20:0x24] = (0x12345678).to_bytes(4, "little")
            sample_img.write_bytes(sample_bytes)

            # driver.c reads its fallback entry from the generic title configuration.
            # Build against the generic (no-title) configuration so this harness keeps
            # asserting the unconfigured driver behavior and nothing title-specific.
            title_runtime_config.write_if_changed(
                tmpdir / "sr_title_config.h",
                title_runtime_config.render_header(
                    title_runtime_config.bindings_from_manifest(None)
                ),
            )

            compiled = subprocess.run(
                [CC, "-std=c11", "-O2", f"-I{RT_DIR}", f"-I{tmpdir}", "-Wall", "-Wextra", "-Werror",
                 str(src), str(RT_DIR / "title_config.c"), "-o", str(exe)],
                capture_output=True, text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)

            # 1. Valid seed trace
            seed_valid = subprocess.run([str(exe), "seed_valid", str(valid_trace)], capture_output=True, text=True)
            self.assertEqual(seed_valid.returncode, 0, seed_valid.stderr + seed_valid.stdout)

            # 2. Malformed seed trace (must not index OOB or mutate invalid regs)
            seed_malformed = subprocess.run([str(exe), "seed_malformed", str(malformed_trace)], capture_output=True, text=True)
            self.assertEqual(seed_malformed.returncode, 0, seed_malformed.stderr + seed_malformed.stdout)

            # 3. Read file tests
            read_missing = subprocess.run([str(exe), "read_missing"], capture_output=True, text=True)
            self.assertEqual(read_missing.returncode, 2, read_missing.stderr + read_missing.stdout)

            read_zero = subprocess.run([str(exe), "read_zero", str(zero_file)], capture_output=True, text=True)
            self.assertEqual(read_zero.returncode, 0, read_zero.stderr + read_zero.stdout)

            # 4. Valid ELF load
            valid_elf = subprocess.run([str(exe), "elf_valid"], capture_output=True, text=True)
            self.assertEqual(valid_elf.returncode, 0, valid_elf.stderr + valid_elf.stdout)

            # 5. Malformed ELF cases must exit(2)
            for mode in (
                "elf_truncated_hdr",
                "elf_phbounds",
                "elf_phentsize",
                "elf_ph_table_out_of_range",
                "elf_segbounds",
                "elf_filesz",
                "elf_guest_boundary",
                "elf_near_uint32_max",
                "elf_all_or_nothing",
            ):
                result = subprocess.run([str(exe), mode], capture_output=True, text=True)
                self.assertEqual(result.returncode, 2, f"{mode} expected exit code 2, got {result.returncode}.\nStderr: {result.stderr}\nStdout: {result.stdout}")

            # 6. Valid --image load
            valid_img = subprocess.run([str(exe), "image_valid", str(sample_img)], capture_output=True, text=True)
            self.assertEqual(valid_img.returncode, 0, valid_img.stderr + valid_img.stdout)

            expected_img = subprocess.run(
                [str(exe), "image_expect_valid", str(sample_img)], capture_output=True, text=True
            )
            self.assertEqual(expected_img.returncode, 0, expected_img.stderr + expected_img.stdout)
            self.assertIn(
                "DRIVER_EXPECT_U32 addr=0x08000020 got=0x12345678 expected=0x12345678 status=PASS",
                expected_img.stderr,
            )

            mismatched_img = subprocess.run(
                [str(exe), "image_expect_mismatch", str(sample_img)], capture_output=True, text=True
            )
            self.assertEqual(mismatched_img.returncode, 3, mismatched_img.stderr + mismatched_img.stdout)
            self.assertIn("status=FAIL", mismatched_img.stderr)

            for mode in (
                "image_expect_malformed",
                "image_expect_out_of_range",
                "image_expect_unaligned",
                "image_expect_duplicate",
            ):
                result = subprocess.run([str(exe), mode, str(sample_img)], capture_output=True, text=True)
                self.assertEqual(
                    result.returncode,
                    2,
                    f"{mode} expected exit code 2, got {result.returncode}.\n"
                    f"Stderr: {result.stderr}\nStdout: {result.stdout}",
                )

            # 7. Malformed --image cases must exit(2)
            for mode in (
                "image_invalid_base",
                "image_invalid_entry",
                "image_trailing_junk",
                "image_overflow",
                "image_invalid_span",
            ):
                result = subprocess.run([str(exe), mode, str(sample_img)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 2, f"{mode} expected exit code 2, got {result.returncode}.\nStderr: {result.stderr}\nStdout: {result.stdout}")

            # Quiet mode deliberately checks both freopen results.  The null
            # device exists on supported hosts, so this exercises the success
            # path while the strict compile above guards the warning path.
            quiet_env = os.environ.copy()
            quiet_env["SR_QUIET"] = "1"
            quiet = subprocess.run(
                [str(exe), "image_invalid_base", str(sample_img)],
                capture_output=True,
                text=True,
                env=quiet_env,
            )
            self.assertEqual(quiet.returncode, 2, quiet.stderr + quiet.stdout)


if __name__ == "__main__":
    unittest.main()
