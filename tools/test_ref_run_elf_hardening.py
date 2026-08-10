# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Malformed-input regression tests for the standalone reference ELF runner."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
RUN_ELF = ROOT / "src" / "ref" / "run_elf.cpp"
CC = shutil.which("g++") or shutil.which("c++") or shutil.which("clang++")


@unittest.skipUnless(CC, "no C++ compiler on PATH")
class TestReferenceRunnerHardening(unittest.TestCase):
    def test_malformed_elf_and_init_trace_inputs(self):
        assert CC is not None
        source = f'''\\
#define SR_SELFTEST_ONLY
#include "{RUN_ELF.as_posix()}"

static void wr16(std::vector<uint8_t> &v, size_t off, uint16_t x) {{
    memcpy(v.data() + off, &x, sizeof(x));
}}
static void wr32(std::vector<uint8_t> &v, size_t off, uint32_t x) {{
    memcpy(v.data() + off, &x, sizeof(x));
}}
static std::vector<uint8_t> base_elf(size_t size) {{
    std::vector<uint8_t> elf(size, 0);
    memcpy(elf.data(), "\\x7f" "ELF", 4);
    wr32(elf, 24, 0x08000010u);
    return elf;
}}

int main(int argc, char **argv) {{
    if (argc < 2) return 90;
    const char *mode = argv[1];
    if (strcmp(mode, "seed") == 0) {{
        if (argc < 3) return 91;
        uint32_t val = 0, idx = 0;
        if (!ParseHex32("00000000", &val) || val != 0u) return 101;
        if (!ParseHex32("ffffffff", &val) || val != 0xffffffffu) return 102;
        if (!ParseHex32("0x08000000", &val) || val != 0x08000000u) return 103;
        if (ParseHex32("100000000", &val)) return 104;
        if (ParseHex32("ffffffffffffffff", &val)) return 105;
        if (ParseHex32("100000000000000000000", &val)) return 106;
        if (ParseHex32("0x08000000junk", &val)) return 107;
        if (ParseHex32("+0x08000000", &val)) return 110;
        if (ParseHex32("0x+1234", &val)) return 111;
        if (ParseHex32("0x-0", &val)) return 112;
        if (ParseHex32("0x 1234", &val)) return 113;
        if (ParseHex32(" 1234", &val)) return 114;
        if (ParseHex32("+1234", &val)) return 115;
        if (ParseIndexedRegister("r+1", 'r', &idx)) return 108;
        if (ParseIndexedRegister("r-1", 'r', &idx)) return 109;

        ref::CpuState s;
        for (int i = 0; i < 32; i++) {{ s.r[i] = 0x11110000u + (uint32_t)i; s.fi[i] = 0x22220000u + (uint32_t)i; }}
        s.hi = 0xaaaaaaaau; s.lo = 0xbbbbbbbbu; s.fcr31 = 0xccccccccu;
        if (!SeedFromInit(argv[2], &s)) return 1;
        if (s.r[31] != 0x12345678u || s.fi[31] != 0x89abcdefu) return 2;
        if (s.hi != 0x11111111u || s.lo != 0x22222222u || s.fcr31 != 0x33333333u) return 3;
        if (s.r[1] != 0x11110001u || s.r[2] != 0x11110002u || s.r[3] != 0x11110003u) return 4;
        return 0;
    }}
    ref::Memory mem;
    if (strcmp(mode, "phbounds") == 0) {{
        auto elf = base_elf(52); wr32(elf, 28, 50); wr16(elf, 42, 32); wr16(elf, 44, 1); LoadElf(elf, &mem); return 10;
    }}
    if (strcmp(mode, "phentsize") == 0) {{
        auto elf = base_elf(84); wr32(elf, 28, 52); wr16(elf, 42, 4); wr16(elf, 44, 1); LoadElf(elf, &mem); return 11;
    }}
    if (strcmp(mode, "segbounds") == 0) {{
        auto elf = base_elf(84); wr32(elf, 28, 52); wr16(elf, 42, 32); wr16(elf, 44, 1);
        wr32(elf, 52, 1); wr32(elf, 56, 80); wr32(elf, 60, 0x08000010u); wr32(elf, 68, 8); wr32(elf, 72, 8);
        LoadElf(elf, &mem); return 12;
    }}
    if (strcmp(mode, "memrange") == 0) {{
        auto elf = base_elf(100); wr32(elf, 28, 52); wr16(elf, 42, 32); wr16(elf, 44, 1);
        wr32(elf, 52, 1); wr32(elf, 56, 84); wr32(elf, 60, 0x0bfffff8u); wr32(elf, 68, 16); wr32(elf, 72, 16);
        LoadElf(elf, &mem); return 13;
    }}
    if (strcmp(mode, "filesz") == 0) {{
        auto elf = base_elf(100); wr32(elf, 28, 52); wr16(elf, 42, 32); wr16(elf, 44, 1);
        wr32(elf, 52, 1); wr32(elf, 56, 84); wr32(elf, 60, 0x08000010u); wr32(elf, 68, 16); wr32(elf, 72, 8);
        LoadElf(elf, &mem); return 14;
    }}
    if (strcmp(mode, "unmapped") == 0) {{
        auto elf = base_elf(88); wr32(elf, 28, 52); wr16(elf, 42, 32); wr16(elf, 44, 1);
        wr32(elf, 52, 1); wr32(elf, 56, 84); wr32(elf, 60, 0x00400000u); wr32(elf, 68, 4); wr32(elf, 72, 8);
        elf[84]=1; elf[85]=2; elf[86]=3; elf[87]=4;
        if (LoadElf(elf, &mem) != 0x08000010u) return 30;
        if (mem.last_fault()) return 31;
        return 0;
    }}
    if (strcmp(mode, "valid") == 0) {{
        auto elf = base_elf(88); wr32(elf, 28, 52); wr16(elf, 42, 32); wr16(elf, 44, 1);
        wr32(elf, 52, 1); wr32(elf, 56, 84); wr32(elf, 60, 0x08000010u); wr32(elf, 68, 4); wr32(elf, 72, 8);
        elf[84]=1; elf[85]=2; elf[86]=3; elf[87]=4;
        if (LoadElf(elf, &mem) != 0x08000010u) return 20;
        if (mem.Read8(0x08000010u)!=1 || mem.Read8(0x08000011u)!=2 || mem.Read8(0x08000012u)!=3 || mem.Read8(0x08000013u)!=4) return 21;
        if (mem.Read32(0x08000014u) != 0u || mem.last_fault()) return 22;
        return 0;
    }}
    return 99;
}}
'''
        with tempfile.TemporaryDirectory(prefix="ref_runner_harden_") as tmp:
            tmpdir = Path(tmp)
            src = tmpdir / "harness.cpp"
            exe = tmpdir / "harness"
            trace = tmpdir / "init.trace"
            src.write_text(source, encoding="utf-8")
            trace.write_text(
                "# init r31=12345678 f31=89abcdef hi=11111111 lo=22222222 "
                "fcr31=33333333 r32=deadbeef f32=feedface r1junk=77777777 "
                "r2=zzzz r3=100000000\\n",
                encoding="ascii",
            )
            compiled = subprocess.run(
                [CC, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", str(src), "-o", str(exe)],
                capture_output=True, text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            seed = subprocess.run([str(exe), "seed", str(trace)], capture_output=True, text=True)
            self.assertEqual(seed.returncode, 0, seed.stderr + seed.stdout)
            valid = subprocess.run([str(exe), "valid"], capture_output=True, text=True)
            self.assertEqual(valid.returncode, 0, valid.stderr + valid.stdout)
            unmapped = subprocess.run([str(exe), "unmapped"], capture_output=True, text=True)
            self.assertEqual(unmapped.returncode, 0, unmapped.stderr + unmapped.stdout)
            for mode in ("phbounds", "phentsize", "segbounds", "memrange", "filesz"):
                result = subprocess.run([str(exe), mode], capture_output=True, text=True)
                self.assertEqual(result.returncode, 2, f"{mode}: {result.stderr}{result.stdout}")


if __name__ == "__main__":
    unittest.main()
