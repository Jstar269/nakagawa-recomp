# SPDX-License-Identifier: GPL-2.0-or-later

import inspect
import re
import unittest

import codegen


def require_match(pattern: str, text: str, flags: int = 0) -> "re.Match[str]":
    """Run re.search and assert a match, returning a non-optional Match.

    Pylance cannot narrow re.search() results after unittest's assertIsNotNone,
    so wrap the call to keep the type checker quiet and the intent explicit.
    """
    m = re.search(pattern, text, flags)
    assert m is not None, f"pattern not found: {pattern!r}"
    return m


class RetailAllocatorTranslationTests(unittest.TestCase):
    def test_retail_metadata_manipulating_api_bridges_to_host_allocator(self):
        """All retail entry points that touch dlmalloc headers must stay on the host ABI.

        _memalign_r and _realloc_r do not merely call _malloc_r/_free_r: they directly
        carve, unlink, and rewrite dlmalloc chunks. In dlmalloc header bit 0 is
        PREV_INUSE, while recomp.c uses it for the current block's allocation state.
        Translating either body therefore corrupts the host free list even though the
        malloc/free entry points themselves are bridged.
        """
        source = inspect.getsource(codegen)
        malloc_stub = require_match(r"if hst_profile and a == 0x00010738:.*?continue", source, re.S)
        self.assertIn("s->r[31] == 0x000104d0u", malloc_stub.group(0))
        self.assertIn("MEM_R32(s->r[29] + 0x00000004u)", malloc_stub.group(0))
        self.assertIn("owner_ra == 0x00000bf4u", malloc_stub.group(0))
        self.assertIn("owner_ra == 0x00000c5cu", malloc_stub.group(0))
        self.assertIn("MEM_R32(s->r[29] + 0x0000001cu)", malloc_stub.group(0))
        self.assertIn("sr_newlib_malloc(s->r[5], owner_ra)", malloc_stub.group(0))
        free_stub = require_match(r"if hst_profile and a == 0x0000f538:.*?continue", source, re.S)
        self.assertIn("s->r[31] == 0x00010500u", free_stub.group(0))
        self.assertIn("MEM_R32(s->r[29] + 0x00000004u)", free_stub.group(0))
        self.assertIn("owner_ra == 0x00000a14u", free_stub.group(0))
        self.assertIn("MEM_R32(s->r[29] + 0x0000001cu)", free_stub.group(0))
        self.assertIn("sr_newlib_free(s->r[5], owner_ra)", free_stub.group(0))
        memalign_stub = require_match(r"if hst_profile and a == 0x000101c4:.*?continue", source, re.S)
        self.assertIn(
            "sr_newlib_memalign(s->r[5], s->r[6], s->r[31])", memalign_stub.group(0)
        )
        realloc_stub = require_match(r"if hst_profile and a == 0x00013524:.*?continue", source, re.S)
        self.assertIn(
            "sr_newlib_realloc(s->r[5], s->r[6], s->r[31])",
            realloc_stub.group(0),
        )

    def test_public_wrappers_translate_retail_callees(self):
        class FakeElf:
            def __init__(self, words):
                self.words = words

            def read_at_vaddr(self, addr, size):
                word = self.words.get(addr)
                return word.to_bytes(4, "little") if word is not None else None

        def jal(target):
            return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)

        # Reduced control-flow fixtures for the retail public wrappers:
        # malloc -> _getreent -> _malloc_r and free -> _getreent -> _free_r.
        elf = FakeElf({
            0x104B0: jal(0x0FE3C),
            0x104B4: 0,
            0x104B8: jal(0x10738),
            0x104BC: 0,
            0x104C0: 0x03E00008,
            0x104C4: 0,
            0x104E0: jal(0x0FE3C),
            0x104E4: 0,
            0x104E8: jal(0x0F538),
            0x104EC: 0,
            0x104F0: 0x03E00008,
            0x104F4: 0,
        })
        ranges = [(0x0F000, 0x10800)]
        known = {0x0F538, 0x0FE3C, 0x104B0, 0x104E0, 0x10738}
        malloc_text = "\n".join(codegen.emit_function(elf, 0x104B0, ranges, known))
        free_text = "\n".join(codegen.emit_function(elf, 0x104E0, ranges, known))
        self.assertIn("f_0000fe3c(s);", malloc_text)
        self.assertIn("f_00010738(s);", malloc_text)
        self.assertIn("f_0000fe3c(s);", free_text)
        self.assertIn("f_0000f538(s);", free_text)


if __name__ == "__main__":
    unittest.main()
