# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Unit tests for tools/format_portability_check.py."""

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from tools.format_portability_check import (
    check_format_portability,
    host_long_width,
    main,
    narrow_long_flags,
    resolve_compiler,
)


class TestFormatPortabilityCheck(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.test_root = Path(self.temp_dir.name)
        self.shim_dir = self.test_root / "shim"

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def test_synthetic_bad_uint32_specifier_fails(self) -> None:
        """A synthetic file printing uint32_t with %u must fail the 32-bit newlib check."""
        bad_c = self.test_root / "bad_uint32.c"
        bad_c.write_text(
            "#include <stdint.h>\n"
            "#include <stdio.h>\n"
            "void test_bad(uint32_t val) {\n"
            '    printf("val=%u\\n", val);\n'
            "}\n",
            encoding="utf-8",
        )
        failed, errors = check_format_portability(
            files=[str(bad_c)],
            shim_dir=self.shim_dir,
            narrow_long=False,
        )
        self.assertEqual(failed, 1)
        self.assertEqual(len(errors), 1)
        self.assertIn("error: format", errors[0])

    def test_synthetic_bad_int32_specifier_fails(self) -> None:
        """A synthetic file printing int32_t with %d must fail the 32-bit newlib check."""
        bad_c = self.test_root / "bad_int32.c"
        bad_c.write_text(
            "#include <stdint.h>\n"
            "#include <stdio.h>\n"
            "void test_bad(int32_t val) {\n"
            '    printf("val=%d\\n", val);\n'
            "}\n",
            encoding="utf-8",
        )
        failed, errors = check_format_portability(
            files=[str(bad_c)],
            shim_dir=self.shim_dir,
            narrow_long=False,
        )
        self.assertEqual(failed, 1)
        self.assertEqual(len(errors), 1)
        self.assertIn("error: format", errors[0])

    def test_synthetic_fixed_specifiers_pass(self) -> None:
        """A synthetic file printing uint32_t/int32_t with PRI macros must pass."""
        good_c = self.test_root / "good.c"
        good_c.write_text(
            "#include <errno.h>\n"
            "#include <stdint.h>\n"
            "#include <inttypes.h>\n"
            "#include <stdio.h>\n"
            "void test_good(uint32_t u, int32_t s, size_t n, intmax_t m) {\n"
            '    printf("u=%" PRIu32 " s=%" PRId32 " x=0x%08" PRIx32 "\\n", u, s, u);\n'
            '    printf("cast=%u\\n", (unsigned)u);\n'
            "    /* the shim must carry the C11 limits the core uses */\n"
            '    printf("n=%zu max=%zu ptr=%" PRIdMAX " m=%" PRIdMAX " e=%d\\n", n, (size_t)SIZE_MAX,\n'
            "           (intmax_t)INTPTR_MAX, m, errno);\n"
            "}\n",
            encoding="utf-8",
        )
        failed, errors = check_format_portability(
            files=[str(good_c)],
            shim_dir=self.shim_dir,
            narrow_long=False,
        )
        self.assertEqual(failed, 0, "\n".join(errors))
        self.assertEqual(len(errors), 0)

    def test_cli_invocations(self) -> None:
        """The CLI main() must exit 0 on clean files and 1 on bad files."""
        good_c = self.test_root / "cli_good.c"
        good_c.write_text(
            "#include <stdint.h>\n"
            "#include <inttypes.h>\n"
            "#include <stdio.h>\n"
            "void f(uint32_t x) { printf(\"%\" PRIx32 \"\\n\", x); }\n",
            encoding="utf-8",
        )
        bad_c = self.test_root / "cli_bad.c"
        bad_c.write_text(
            "#include <stdint.h>\n"
            "#include <stdio.h>\n"
            "void f(uint32_t x) { printf(\"%x\\n\", x); }\n",
            encoding="utf-8",
        )

        rc_good = main(["--shim-dir", str(self.shim_dir), "--allow-wide-long", str(good_c)])
        self.assertEqual(rc_good, 0)

        rc_bad = main(["--shim-dir", str(self.shim_dir), "--allow-wide-long", str(bad_c)])
        self.assertEqual(rc_bad, 1)

    def test_missing_compiler_is_the_toolchain_exit_code(self) -> None:
        """A compiler that is not installed is exit 2, the fail-closed contract, never a traceback."""
        good_c = self.test_root / "any.c"
        good_c.write_text("int f(void) { return 0; }\n", encoding="utf-8")
        rc = main(["--compiler", "no-such-c-compiler-7f3a", "--allow-wide-long", str(good_c)])
        self.assertEqual(rc, 2)

    def test_long_width_decides_the_32_bit_flag(self) -> None:
        """An LP64 host compiles the check with -m32; ILP32 and LLP64 hosts need nothing."""
        self.assertEqual(narrow_long_flags(8), ["-m32"])
        self.assertEqual(narrow_long_flags(4), [])
        self.assertIn(host_long_width(resolve_compiler("gcc")), (4, 8))


if __name__ == "__main__":
    unittest.main()
