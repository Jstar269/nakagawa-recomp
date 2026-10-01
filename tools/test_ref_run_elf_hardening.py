# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Malformed-input regression tests for the standalone reference ELF runner."""

import os
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


@unittest.skipUnless(CC, "no C++ compiler on PATH")
class TestReferenceRunnerTerminationContract(unittest.TestCase):
    """The CLI's process status must mean what it says about *why* it stopped.

    The runner used to return 0 for an unimplemented instruction, a memory fault,
    a break and an exhausted instruction budget alike, so a caller could not tell
    a real comparison from a run that never executed what it claimed. These cases
    drive the real CLI with synthetic source-owned ELFs and assert the process
    status AND the structured stop metadata together.
    """

    OPCODE_SYSCALL = 0x0000000C
    OPCODE_NOP = 0x00000000
    OPCODE_BREAK = 0x0000000D
    OPCODE_UNIMPLEMENTED = 0xF0000000  # major opcode 0x3C is not modeled
    OPCODE_JR_RA = 0x03E00008            # jr $r31

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory(prefix="ref_runner_contract_")
        root = Path(cls.tmp.name)
        exe = root / ("run_elf.exe" if os.name == "nt" else "run_elf")
        compiled = subprocess.run(
            [CC, "-std=c++17", "-O1", "-fno-exceptions",
             f"-I{(ROOT / 'src' / 'ref').as_posix()}",
             f"-I{(ROOT / 'src' / 'rt').as_posix()}",
             str(ROOT / "src" / "ref" / "run_elf.cpp"),
             str(ROOT / "src" / "ref" / "interp.cpp"),
             "-o", str(exe)],
            cwd=ROOT, capture_output=True, text=True,
        )
        if compiled.returncode != 0:
            cls.tmp.cleanup()
            raise AssertionError(f"reference runner build failed:\n{compiled.stderr}")
        cls.exe = exe
        cls.root = root

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def write_elf(self, name: str, words: list[int], entry: int = 0x08000010) -> Path:
        """Build a minimal PT_LOAD ELF whose entry points at the given words."""
        payload = b"".join(word.to_bytes(4, "little") for word in words)
        elf = bytearray(84 + len(payload))
        elf[0:4] = b"\x7fELF"
        elf[24:28] = entry.to_bytes(4, "little")
        elf[28:32] = (52).to_bytes(4, "little")
        elf[42:44] = (32).to_bytes(2, "little")
        elf[44:46] = (1).to_bytes(2, "little")
        elf[52:56] = (1).to_bytes(4, "little")          # PT_LOAD
        elf[56:60] = (84).to_bytes(4, "little")         # p_offset
        elf[60:64] = entry.to_bytes(4, "little")        # p_vaddr
        elf[68:72] = len(payload).to_bytes(4, "little")  # p_filesz
        elf[72:76] = max(0x1000, len(payload)).to_bytes(4, "little")  # p_memsz
        elf[84:] = payload
        path = self.root / name
        path.write_bytes(bytes(elf))
        return path

    def write_init(self, name: str, r31: int = 0) -> Path:
        path = self.root / name
        path.write_text(
            f"# psp-recomp trace v1 oracle=interp target=fixture "
            f"start_pc=0x08900000\n# init r31={r31:08x} r1=00000010\n",
            encoding="ascii",
        )
        return path

    def run_cli(self, elf: Path, init: Path, *extra: str, env_extra: dict | None = None):
        out = self.root / f"{elf.stem}-out.trace"
        env = dict(os.environ)
        env.pop("SR_BREAK_FATAL", None)
        if env_extra:
            env.update(env_extra)
        proc = subprocess.run([str(self.exe), str(elf), str(init), str(out), *extra],
                              capture_output=True, text=True, env=env, timeout=120)
        metadata = {}
        for line in proc.stderr.splitlines():
            if line.startswith("run_elf: ") and "=" in line:
                for token in line.split()[1:]:
                    name, _, value = token.partition("=")
                    metadata[name] = value
        return proc, metadata

    def test_guest_syscall_exit_succeeds_without_an_expectation(self) -> None:
        elf = self.write_elf("syscall.elf", [self.OPCODE_SYSCALL])
        proc, meta = self.run_cli(elf, self.write_init("syscall-init.trace"))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(meta.get("stop_reason"), "syscall", proc.stderr)
        self.assertEqual(meta.get("executed"), "1", proc.stderr)
        self.assertEqual(meta.get("expected"), "none", proc.stderr)
        self.assertEqual(meta.get("op"), f"0x{self.OPCODE_SYSCALL:08x}", proc.stderr)

    def test_unsupported_opcode_is_nonzero_and_distinct(self) -> None:
        elf = self.write_elf("unimpl.elf", [self.OPCODE_UNIMPLEMENTED])
        proc, meta = self.run_cli(elf, self.write_init("unimpl-init.trace"))
        self.assertEqual(proc.returncode, 3, proc.stderr)
        self.assertEqual(meta.get("stop_reason"), "unimplemented", proc.stderr)
        self.assertIn("UNEXPECTED_STOP", proc.stderr)

    def test_unmapped_fetch_is_nonzero_and_distinct(self) -> None:
        elf = self.write_elf("fault.elf", [self.OPCODE_JR_RA, self.OPCODE_NOP])
        init = self.write_init("fault-init.trace", r31=0x20000000)
        proc, meta = self.run_cli(elf, init)
        self.assertEqual(proc.returncode, 4, proc.stderr)
        self.assertEqual(meta.get("stop_reason"), "memory-fault", proc.stderr)
        self.assertEqual(meta.get("pc"), "0x20000000", proc.stderr)
        # jr ra and its delay slot ran; the faulting fetch did not execute.
        self.assertEqual(meta.get("executed"), "2", proc.stderr)

    def test_break_stop_is_nonzero_and_distinct(self) -> None:
        elf = self.write_elf("break.elf", [self.OPCODE_BREAK])
        proc, meta = self.run_cli(elf, self.write_init("break-init.trace"),
                                  env_extra={"SR_BREAK_FATAL": "1"})
        self.assertEqual(proc.returncode, 5, proc.stderr)
        self.assertEqual(meta.get("stop_reason"), "break", proc.stderr)

    def test_exhausted_budget_is_not_a_successful_exit(self) -> None:
        elf = self.write_elf("budget.elf", [self.OPCODE_NOP] * 8)
        init = self.write_init("budget-init.trace")
        proc, meta = self.run_cli(elf, init, "3")
        self.assertEqual(proc.returncode, 1, proc.stderr)
        self.assertEqual(meta.get("stop_reason"), "step-limit", proc.stderr)
        self.assertEqual(meta.get("executed"), "3", proc.stderr)
        self.assertIn("expected=syscall", proc.stderr)

        proc, meta = self.run_cli(elf, init, "3", "--expect-stop=step-limit")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(meta.get("executed"), "3", proc.stderr)
        self.assertEqual(meta.get("expected"), "step-limit", proc.stderr)

    def test_declared_expectation_must_match_the_observed_stop(self) -> None:
        elf = self.write_elf("mismatch.elf", [self.OPCODE_SYSCALL])
        proc, meta = self.run_cli(elf, self.write_init("mismatch-init.trace"),
                                  "--expect-stop=step-limit")
        self.assertEqual(proc.returncode, 1, proc.stderr)
        self.assertIn("UNEXPECTED_STOP expected=step-limit observed=syscall", proc.stderr)
        self.assertEqual(meta.get("stop_reason"), "syscall", proc.stderr)

    def test_faulting_stop_can_be_declared_explicitly(self) -> None:
        """A caller that EXPECTS a fault still asserts it instead of guessing."""
        elf = self.write_elf("declared-fault.elf", [self.OPCODE_UNIMPLEMENTED])
        proc, meta = self.run_cli(elf, self.write_init("declared-init.trace"),
                                  "--expect-stop", "unimplemented")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(meta.get("stop_reason"), "unimplemented", proc.stderr)
        self.assertEqual(meta.get("expected"), "unimplemented", proc.stderr)

    def test_malformed_instruction_budget_is_a_usage_error(self) -> None:
        elf = self.write_elf("usage.elf", [self.OPCODE_SYSCALL])
        init = self.write_init("usage-init.trace")
        for value in ("abc", "0", "-1", "", " 5", "5x", "99999999999999999999999999"):
            with self.subTest(max_steps=value):
                proc, _ = self.run_cli(elf, init, value)
                self.assertEqual(proc.returncode, 2, proc.stderr)
                self.assertIn("invalid max_steps", proc.stderr)

    def test_unknown_option_and_expectation_are_usage_errors(self) -> None:
        elf = self.write_elf("opt.elf", [self.OPCODE_SYSCALL])
        init = self.write_init("opt-init.trace")
        proc, _ = self.run_cli(elf, init, "--expect-stop=finished")
        self.assertEqual(proc.returncode, 2, proc.stderr)
        self.assertIn("unknown --expect-stop value", proc.stderr)
        proc, _ = self.run_cli(elf, init, "--not-a-flag")
        self.assertEqual(proc.returncode, 2, proc.stderr)
        self.assertIn("unknown option", proc.stderr)


if __name__ == "__main__":
    unittest.main()
