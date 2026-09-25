# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
"""Clean-room black-box tests for src/rt/prx_loader.c.

Written only from docs/cleanroom/PRX_LOADER_SPEC.md plus general knowledge
of C/ELF/MIPS. The fixture builder below emits synthetic ELF32 PSP modules
purely from the spec. Tests compile a small C harness together with
src/rt/prx_loader.c using gcc from PATH (gcc -std=c11 -Wall -Wextra -Werror);
the harness defines sr_prx_guest_write over a guard-filled arena.

Covers spec section 8 items 1-8. Skips (not fails) if gcc is missing.
"""

import os
import random
import shutil
import struct
import subprocess
import tempfile
import time
import unittest

PT_LOAD = 1
PT_REL_A = 0x700000A0
PT_REL_B = 0x700000A1

ET_PSP = 0xFFA0

BASE = 0x10000
ARENA_SIZE = 0x30000


# ---------------------------------------------------------------------------
# Spec-mirror arithmetic helpers (derived from the spec text, not from any
# loader implementation).
# ---------------------------------------------------------------------------

def sx16(w):
    v = w & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def exp_a16(w, s):
    return (w & 0xFFFF0000) | (((sx16(w) + s) & 0xFFFFFFFF) & 0xFFFF)


def exp_a32(w, s):
    return (w + s) & 0xFFFFFFFF


def exp_jump(w, site, s):
    field = w & 0x03FFFFFF
    t = ((site & 0xF0000000) | ((field << 2) & 0x0FFFFFFF)) + s
    t &= 0xFFFFFFFF
    return (w & 0xFC000000) | ((t >> 2) & 0x03FFFFFF)


def exp_hi16(hi_word, partner_word, s):
    # Revised spec section 3.6 format A kind 5: V = (low16(HI)*65536)
    # + sx16(partner low16) + S; result low16 = (V+0x8000)>>16.
    # A lui keeps its immediate in the low 16 bits; high 16 unchanged.
    v = (((hi_word & 0xFFFF) << 16) + sx16(partner_word) + s) & 0xFFFFFFFF
    nlo = ((v + 0x8000) & 0xFFFFFFFF) >> 16 & 0xFFFF
    return (hi_word & 0xFFFF0000) | nlo


def exp_b_kind4(w, addend, s):
    # Revised spec section 3.6 format B kind 4: same rule as A kind 5,
    # result to the low 16 bits, high 16 unchanged.
    v = (((w & 0xFFFF) << 16) + sx16(addend & 0xFFFFFFFF) + s) & 0xFFFFFFFF
    nlo = ((v + 0x8000) & 0xFFFFFFFF) >> 16 & 0xFFFF
    return (w & 0xFFFF0000) | nlo


# ---------------------------------------------------------------------------
# Fixture builder: synthetic ELF32 PSP modules, purely from the spec.
#
# Module-info block layout (spec section 3.4 order): attr u16 @0, version
# 2 bytes @2, name 28 bytes @4, gp u32 @32, exp_start @36, exp_end @40,
# imp_start @44, imp_end @48 (52 bytes total, little-endian).
#
# Export records (revised spec section 3.7): 16-byte head:
# libname u32 @0, version 2 bytes @4, attribute u16 @6, len(u8 words) @8,
# nvar(u8) @9, nfunc(u16) @10, entrytable u32 @12. Entry table is
# (nfunc+nvar) NIDs followed by (nfunc+nvar) addresses, functions first.
# Bytes beyond offset 16 are ignored.
#
# Import stub records (revised spec section 3.8): 20-byte head
# (24 with vartable): libname u32 @0, version 2 bytes @4, attribute u16 @6,
# len(u8 words) @8, nvar(u8) @9, nfunc(u16) @10, nidtable u32 @12,
# stubtable u32 @16 [, vartable u32 @20, only when len>=6, unused].
# ---------------------------------------------------------------------------

def modinfo_block(attr=0, ver=(0, 0), name=b"testmod", gp=0,
                  exp_start=0, exp_end=0, imp_start=0, imp_end=0,
                  nonul=False):
    if nonul:
        raw = b"A" * 28
    else:
        if len(name) > 27:
            raise ValueError("name too long")
        raw = name + b"\x00" * (28 - len(name))
    return (struct.pack("<H", attr) + bytes([ver[0], ver[1]]) + raw
            + struct.pack("<5I", gp, exp_start, exp_end, imp_start, imp_end))


def export_record(libptr, nvar, nfunc, etab, version=0, attr=0, rec_len=4,
                  extra=b""):
    # Revised §3.7 head: <IHHBBHI (16 bytes). rec_len in words (>=4).
    head = struct.pack("<IHHBBHI", libptr, version, attr, rec_len, nvar,
                       nfunc, etab)
    pad = rec_len * 4 - len(head)
    assert pad >= 0
    tail = bytes(extra[:pad]) if extra else b""
    tail = tail + b"\x00" * (pad - len(tail))
    return head + tail


def import_record(libptr, nvar, nfunc, nidtab, stubtab, vartable=None,
                  version=0, attr=0):
    if vartable is None:
        # len 5 -> 20 bytes.
        return struct.pack("<IHHBBHII", libptr, version, attr, 5, nvar,
                           nfunc, nidtab, stubtab)
    # len 6 -> 24 bytes; vartable present but unused.
    return struct.pack("<IHHBBHIII", libptr, version, attr, 6, nvar, nfunc,
                       nidtab, stubtab, vartable)


def reloc_a(offset, kind, oseg=0, aseg=0):
    return struct.pack("<II", offset, kind | (oseg << 8) | (aseg << 16))


def b_cmd(flag_idx, seg_idx, kind_idx, d, f, t, segw):
    dw = 16 - f - segw - t
    mask = lambda v, b: v & ((1 << b) - 1) if b > 0 else 0
    c = (mask(flag_idx, f) | (mask(seg_idx, segw) << f)
         | (mask(kind_idx, t) << (f + segw))
         | (mask(d, dw) << (f + segw + t)))
    return struct.pack("<H", c)


class Module:
    """A synthetic module. Loads: list of dicts with vaddr, data (bytes),
    memsz, flags, and paddr handling for segment 0 (paddr='auto:<off>' sets
    p_paddr to the segment file offset plus <off>; a plain int is used
    verbatim). Relocs: list of (ptype, bytes). Sections: list of dicts
    (name, type, flags, addr, data)."""

    def __init__(self, e_type=ET_PSP, e_entry=0):
        self.e_type = e_type
        self.e_entry = e_entry
        self.loads = []
        self.relocs = []
        self.sections = []

    def add_seg(self, vaddr, data, memsz=None, flags=6, paddr="auto:0"):
        self.loads.append({"vaddr": vaddr, "data": bytes(data),
                           "memsz": len(data) if memsz is None else memsz,
                           "flags": flags, "paddr": paddr})
        return len(self.loads) - 1

    def add_reloc(self, ptype, blob):
        self.relocs.append((ptype, bytes(blob)))

    def add_section(self, name, stype, addr, data, flags=0):
        self.sections.append({"name": name, "type": stype, "flags": flags,
                              "addr": addr, "data": bytes(data)})

    def build(self):
        nload = len(self.loads)
        nrel = len(self.relocs)
        nph = nload + nrel
        phoff = 52
        off = phoff + 32 * nph
        segs = []
        for ld in self.loads:
            segs.append({"off": off, "data": ld["data"]})
            off += len(ld["data"])
        rels = []
        for (pt, blob) in self.relocs:
            rels.append({"off": off, "data": blob, "pt": pt})
            off += len(blob)
        # Section data (plus string table) live after reloc blobs.
        secblobs = []
        shstr = b"\x00"
        name_off = {}
        for sc in self.sections:
            name_off[sc["name"]] = len(shstr)
            shstr += sc["name"].encode() + b"\x00"
        shstr_off = None
        if self.sections:
            shstr_off = off
            off += len(shstr)
            for sc in self.sections:
                secblobs.append({"off": off, "data": sc["data"]})
                off += len(sc["data"])
        shoff = 0
        shnum = 0
        shstrndx = 0
        if self.sections:
            # Null section + one per section + shstrtab section.
            shnum = 1 + len(self.sections) + 1
            shstrndx = shnum - 1
            shoff = off
            off += 40 * shnum
        eh = struct.pack(
            "<16sHHIIIIIHHHHHH",
            bytes([0x7F, 0x45, 0x4C, 0x46, 1, 1, 1, 0]) + b"\x00" * 8,
            self.e_type, 8, 1, self.e_entry, phoff, shoff, 0,
            52, 32, nph, 40, shnum, shstrndx)
        out = bytearray(eh)
        assert len(out) == phoff
        for idx, ld in enumerate(self.loads):
            paddr = ld["paddr"]
            if isinstance(paddr, str) and paddr.startswith("auto:"):
                paddr = segs[idx]["off"] + int(paddr[5:])
            out += struct.pack("<8I", PT_LOAD, segs[idx]["off"], ld["vaddr"],
                               paddr, len(ld["data"]), ld["memsz"],
                               ld["flags"], 4)
        for rb in rels:
            out += struct.pack("<8I", rb["pt"], rb["off"], 0, 0,
                               len(rb["data"]), len(rb["data"]), 0, 1)
        for sg in segs:
            out += sg["data"]
        for rb in rels:
            out += rb["data"]
        if self.sections:
            out += shstr
            for sb in secblobs:
                out += sb["data"]
            assert len(out) == shoff
            out += b"\x00" * 40
            for idx, sc in enumerate(self.sections):
                out += struct.pack("<10I", name_off[sc["name"]], sc["type"],
                                   sc["flags"], sc["addr"],
                                   secblobs[idx]["off"], len(sc["data"]),
                                   0, 0, 4, 0)
            out += struct.pack("<10I", 0, 3, 0,
                               0, shstr_off, len(shstr), 0, 0, 1, 0)
            assert len(out) == off
        return bytes(out)


def guest(seg_vaddr, seg_off, base=BASE):
    return (base + seg_vaddr + seg_off) & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# C harness (embedded). Defines sr_prx_guest_write over a guard-filled arena
# and prints results for the Python tests to check.
# ---------------------------------------------------------------------------

HARNESS_SRC = r"""
#include "prx_loader.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#define ARENA_SIZE 0x30000u
#define GUARD 0xAAu

static unsigned char g_arena[ARENA_SIZE];

int sr_prx_guest_write(uint32_t guest_addr, const void *src, uint32_t n) {
    uint64_t end = (uint64_t)guest_addr + (uint64_t)n;
    if (end > (uint64_t)ARENA_SIZE) {
        return 1;
    }
    if (n > 0) {
        memcpy(g_arena + guest_addr, src, n);
    }
    return 0;
}

static int arena_all_guard(void) {
    uint32_t i;
    for (i = 0; i < ARENA_SIZE; i++) {
        if (g_arena[i] != GUARD) {
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    const char *path;
    unsigned long base;
    int use_file = 0;
    FILE *f = NULL;
    long fsz = 0;
    unsigned char *blob = NULL;
    size_t got = 0;
    SrPrxImage img;
    char err[256];
    int rc;
    uint32_t i, k;
    if (argc < 3) {
        printf("RESULT fail\nERR usage\nARENA_CLEAN 1\n");
        return 0;
    }
    if (strcmp(argv[1], "--fuzz-stream") == 0) {
        base = strtoul(argv[2], NULL, 0);
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        while (1) {
            uint32_t len = 0;
            if (fread(&len, 1, 4, stdin) != 4) {
                break;
            }
            blob = (unsigned char *)malloc(len > 0 ? (size_t)len : 1);
            if (blob == NULL) {
                printf("RESULT fail\nERR harness out of memory\n");
                return 1;
            }
            if (len > 0 && fread(blob, 1, (size_t)len, stdin) != (size_t)len) {
                free(blob);
                printf("RESULT fail\nERR harness short read\n");
                return 1;
            }
            memset(g_arena, GUARD, sizeof g_arena);
            memset(&img, 0, sizeof img);
            memset(err, 0, sizeof err);
            rc = sr_prx_load_from_memory(blob, (size_t)len, (uint32_t)base, &img, err, sizeof err);
            (void)rc;
            sr_prx_image_free(&img);
            free(blob);
        }
        printf("RESULT ok\n");
        return 0;
    }
    path = argv[1];
    base = strtoul(argv[2], NULL, 0);
    if (argc >= 4 && strcmp(argv[3], "file") == 0) {
        use_file = 1;
    }
    memset(g_arena, GUARD, sizeof g_arena);
    memset(&img, 0, sizeof img);
    memset(err, 0, sizeof err);
    if (use_file) {
        rc = sr_prx_load(path, (uint32_t)base, &img, err, sizeof err);
    } else {
        f = fopen(path, "rb");
        if (f == NULL) {
            printf("RESULT fail\nERR cannot open fixture\nARENA_CLEAN 1\n");
            return 0;
        }
        if (fseek(f, 0, SEEK_END) != 0) {
            printf("RESULT fail\nERR cannot stat fixture\nARENA_CLEAN 1\n");
            fclose(f);
            return 0;
        }
        fsz = ftell(f);
        rewind(f);
        if (fsz < 0) {
            printf("RESULT fail\nERR cannot stat fixture\nARENA_CLEAN 1\n");
            fclose(f);
            return 0;
        }
        blob = (unsigned char *)malloc((size_t)fsz > 0 ? (size_t)fsz : 1);
        if (blob == NULL) {
            printf("RESULT fail\nERR harness out of memory\nARENA_CLEAN 1\n");
            fclose(f);
            return 0;
        }
        got = fread(blob, 1, (size_t)fsz, f);
        fclose(f);
        if (got != (size_t)fsz) {
            printf("RESULT fail\nERR harness short read\nARENA_CLEAN 1\n");
            free(blob);
            return 0;
        }
        rc = sr_prx_load_from_memory(blob, got, (uint32_t)base, &img, err,
                                     sizeof err);
        free(blob);
    }
    if (rc != 0) {
        printf("RESULT fail\nERR %s\nARENA_CLEAN %d\n", err,
               arena_all_guard());
        sr_prx_image_free(&img);
        return 0;
    }
    printf("RESULT ok\n");
    printf("MODNAME %s\n", img.modname);
    printf("ATTR %u VER %u %u GP 0x%08X\n", img.attr, img.version[0],
           img.version[1], img.gp);
    printf("ENTRY 0x%08X HASSTART %d MODSTART 0x%08X\n", img.entry,
           img.has_mod_start, img.mod_start);
    printf("START 0x%08X END 0x%08X\n", img.start, img.end);
    printf("NSEG %u\n", img.nseg);
    for (i = 0; i < img.nseg; i++) {
        printf("SEG %u %u 0x%08X %u %u\n", i, img.segs[i].file_offset,
               img.segs[i].guest_addr, img.segs[i].file_size,
               img.segs[i].mem_size);
    }
    printf("MODINFO 0x%08X\n", img.modinfo_addr);
    printf("NEXP %u\n", img.nexp);
    for (k = 0; k < img.nexp; k++) {
        printf("EXP %u lib=%s nid=0x%08X addr=0x%08X func=%d\n", k,
               img.exps[k].lib, img.exps[k].nid, img.exps[k].addr,
               img.exps[k].is_func);
    }
    printf("NIMP %u\n", img.nimp);
    for (k = 0; k < img.nimp; k++) {
        printf("IMP %u lib=%s nid=0x%08X stub=0x%08X\n", k, img.imps[k].lib,
               img.imps[k].nid, img.imps[k].stub_addr);
    }
    for (i = 0; i < img.nseg; i++) {
        uint64_t end = (uint64_t)img.segs[i].guest_addr + img.segs[i].mem_size;
        uint32_t j;
        if (end > ARENA_SIZE) {
            printf("MEM %u TRUNCATED\n", i);
            continue;
        }
        printf("MEM %u ", i);
        for (j = 0; j < img.segs[i].mem_size; j++) {
            printf("%02X", g_arena[img.segs[i].guest_addr + j]);
        }
        printf("\n");
    }
    printf("ARENA_CLEAN 1\n");
    sr_prx_image_free(&img);
    return 0;
}
"""


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def parse_output(text):
    info = {"exps": [], "imps": [], "segs": [], "mems": []}
    for line in text.splitlines():
        p = line.strip().split()
        if not p:
            continue
        if p[0] == "RESULT" and len(p) > 1:
            info["result"] = p[1]
        elif p[0] == "ERR":
            info["err"] = line[4:].strip()
        elif p[0] == "MODNAME" and len(p) > 1:
            info["modname"] = p[1]
        elif p[0] == "ATTR":
            info["attr"] = int(p[1])
            info["ver"] = (int(p[3]), int(p[4]))
            info["gp"] = int(p[6], 16)
        elif p[0] == "ENTRY":
            info["entry"] = int(p[1], 16)
            info["hasstart"] = int(p[3])
            info["modstart"] = int(p[5], 16)
        elif p[0] == "START":
            info["start"] = int(p[1], 16)
            info["end"] = int(p[3], 16)
        elif p[0] == "NSEG":
            info["nseg"] = int(p[1])
        elif p[0] == "SEG":
            info["segs"].append({"fileoff": int(p[2]),
                                 "guest": int(p[3], 16),
                                 "filesz": int(p[4]), "memsz": int(p[5])})
        elif p[0] == "MODINFO":
            info["modinfo"] = int(p[1], 16)
        elif p[0] == "NEXP":
            info["nexp"] = int(p[1])
        elif p[0] == "EXP":
            kv = {}
            for tok in p[2:]:
                kk, _, vv = tok.partition("=")
                kv[kk] = vv
            info["exps"].append({"lib": kv.get("lib", ""),
                                 "nid": int(kv.get("nid", "0"), 16),
                                 "addr": int(kv.get("addr", "0"), 16),
                                 "func": int(kv.get("func", "0"))})
        elif p[0] == "NIMP":
            info["nimp"] = int(p[1])
        elif p[0] == "IMP":
            kv = {}
            for tok in p[2:]:
                kk, _, vv = tok.partition("=")
                kv[kk] = vv
            info["imps"].append({"lib": kv.get("lib", ""),
                                 "nid": int(kv.get("nid", "0"), 16),
                                 "stub": int(kv.get("stub", "0"), 16)})
        elif p[0] == "MEM" and len(p) > 2 and p[2] != "TRUNCATED":
            info["mems"].append(bytes.fromhex(p[2]))
        elif p[0] == "ARENA_CLEAN":
            info["arena_clean"] = int(p[1])
    return info


class PrxTestBase(unittest.TestCase):
    EXE = None
    TMP = None

    @classmethod
    def setUpClass(cls):
        if PrxTestBase.EXE is not None and os.path.exists(PrxTestBase.EXE):
            cls.TMP = PrxTestBase.TMP
            cls.EXE = PrxTestBase.EXE
            return
        if shutil.which("gcc") is None:
            raise unittest.SkipTest("gcc not on PATH")
        root = repo_root()
        src = os.path.join(root, "src", "rt", "prx_loader.c")
        if not os.path.exists(src):
            raise unittest.SkipTest("prx_loader.c not built yet")
        PrxTestBase.TMP = tempfile.mkdtemp(prefix="prxclean_")
        cls.TMP = PrxTestBase.TMP
        hc = os.path.join(cls.TMP, "harness.c")
        with open(hc, "w") as f:
            f.write(HARNESS_SRC)
        PrxTestBase.EXE = os.path.join(cls.TMP, "harness.exe")
        cls.EXE = PrxTestBase.EXE
        r = subprocess.run(
            ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", os.path.join(root, "src", "rt"),
             hc, src, "-o", cls.EXE],
            capture_output=True, text=True, timeout=300)
        if r.returncode != 0:
            raise unittest.SkipTest("harness build failed: %s" % r.stderr)

    def run_load(self, blob, base=BASE, mode="mem", timeout=60):
        fx = os.path.join(self.TMP, "fix.prx")
        with open(fx, "wb") as f:
            f.write(blob)
        args = [self.EXE, fx, "0x%X" % base]
        if mode == "file":
            args.append("file")
        r = subprocess.run(args, capture_output=True, text=True,
                           errors="replace", timeout=timeout)
        self.assertEqual(r.returncode, 0, "harness crashed: %r" % r.stderr)
        info = parse_output(r.stdout)
        self.assertIn("result", info, "harness gave no RESULT: %r" % r.stdout)
        return info

    def assert_fail(self, blob, base=BASE, needle=None):
        info = self.run_load(blob, base)
        self.assertEqual(info.get("result"), "fail",
                         "expected failure, got success")
        if needle is not None:
            self.assertIn(needle, info.get("err", ""),
                          "err %r lacks %r" % (info.get("err"), needle))
        return info

    def seg_u32(self, info, seg, off):
        mem = info["mems"][seg]
        return struct.unpack_from("<I", mem, off)[0]


# ---------------------------------------------------------------------------
# Item 1: format A
# ---------------------------------------------------------------------------

def basic_seg_with_words(words, vaddr=0x1000, name=b"amod", bss=0):
    """Segment 0 data = modinfo (empty tables) + words list at 4-byte steps."""
    data = bytearray(modinfo_block(name=name))
    offs = []
    for w in words:
        offs.append(len(data))
        data += struct.pack("<I", w)
    if bss:
        memsz = len(data) + bss
    else:
        memsz = len(data)
    return bytes(data), offs, memsz


class TestFormatA(PrxTestBase):
    def test_a_kinds_basic(self):
        words = [0x11111111, 0xABCD0001, 0x00000007, 0x08000010,
                 0x24640005, 0x22222222]
        data, offs, memsz = basic_seg_with_words(words)
        tab = b"".join([
            reloc_a(offs[0], 0),
            reloc_a(offs[1], 1),
            reloc_a(offs[2], 2),
            reloc_a(offs[3], 4),
            reloc_a(offs[4], 6),
            reloc_a(offs[5], 8),
        ])
        m = Module()
        m.add_seg(0x1000, data, memsz)
        m.add_reloc(PT_REL_A, tab)
        info = self.run_load(m.build())
        self.assertEqual(info.get("result"), "ok", info.get("err"))
        s = BASE + 0x1000
        self.assertEqual(self.seg_u32(info, 0, offs[0]), words[0])
        self.assertEqual(self.seg_u32(info, 0, offs[1]),
                         exp_a16(words[1], s))
        self.assertEqual(self.seg_u32(info, 0, offs[2]),
                         exp_a32(words[2], s))
        self.assertEqual(self.seg_u32(info, 0, offs[3]),
                         exp_jump(words[3], s + offs[3], s))
        self.assertEqual(self.seg_u32(info, 0, offs[4]),
                         exp_a16(words[4], s))
        self.assertEqual(self.seg_u32(info, 0, offs[5]), words[5])

    def test_a_hi16_run_of_three(self):
        hi = [0x3C010000, 0x3C021234, 0x3C03FFFF]
        partner = 0x24210034
        words = hi + [partner]
        data, offs, memsz = basic_seg_with_words(words)
        tab = b"".join([reloc_a(o, 5) for o in offs[:3]]
                       + [reloc_a(offs[3], 6)])
        m = Module()
        m.add_seg(0x1000, data, memsz)
        m.add_reloc(PT_REL_A, tab)
        info = self.run_load(m.build())
        self.assertEqual(info.get("result"), "ok", info.get("err"))
        s = BASE + 0x1000
        for i in range(3):
            self.assertEqual(self.seg_u32(info, 0, offs[i]),
                             exp_hi16(hi[i], partner, s))
        self.assertEqual(self.seg_u32(info, 0, offs[3]),
                         exp_a16(partner, s))

    def test_a_hi16_partner_other_kind(self):
        # Partner of kind 2 (32-bit) still counts as the partner.
        hi = [0x3C010001, 0x3C020002]
        partner = 0x00001000
        data, offs, memsz = basic_seg_with_words(hi + [partner])
        tab = (reloc_a(offs[0], 5) + reloc_a(offs[1], 5)
               + reloc_a(offs[2], 2))
        m = Module()
        m.add_seg(0x1000, data, memsz)
        m.add_reloc(PT_REL_A, tab)
        info = self.run_load(m.build())
        self.assertEqual(info.get("result"), "ok", info.get("err"))
        s = BASE + 0x1000
        self.assertEqual(self.seg_u32(info, 0, offs[0]),
                         exp_hi16(hi[0], partner, s))
        self.assertEqual(self.seg_u32(info, 0, offs[1]),
                         exp_hi16(hi[1], partner, s))
        self.assertEqual(self.seg_u32(info, 0, offs[2]),
                         exp_a32(partner, s))

    def test_a_hi16_carry(self):
        hi = 0x3C010000
        partner = 0x24218000  # low half 0x8000: carry case
        data, offs, memsz = basic_seg_with_words([hi, partner])
        tab = reloc_a(offs[0], 5) + reloc_a(offs[1], 1)
        m = Module()
        m.add_seg(0x1000, data, memsz)
        m.add_reloc(PT_REL_A, tab)
        info = self.run_load(m.build())
        self.assertEqual(info.get("result"), "ok", info.get("err"))
        s = BASE + 0x1000
        want = exp_hi16(hi, partner, s)
        # Carry must actually trigger for this vector (upper half bumps).
        # Revised V uses low16(HI)*65536.
        v = (((hi & 0xFFFF) << 16) + sx16(partner) + s) & 0xFFFFFFFF
        self.assertNotEqual((v >> 16) & 0xFFFF, (v + 0x8000) >> 16 & 0xFFFF)
        self.assertEqual(self.seg_u32(info, 0, offs[0]), want)

    def test_a_kind7_fails(self):
        data, offs, memsz = basic_seg_with_words([0x00621821])
        m = Module()
        m.add_seg(0x1000, data, memsz)
        m.add_reloc(PT_REL_A, reloc_a(offs[0], 7))
        self.assert_fail(m.build(), needle="GPREL16")

    def test_a_kind3_fails(self):
        data, offs, memsz = basic_seg_with_words([0x00621821])
        m = Module()
        m.add_seg(0x1000, data, memsz)
        m.add_reloc(PT_REL_A, reloc_a(offs[0], 3))
        self.assert_fail(m.build(), needle="unknown kind")

    def test_a_hi16_no_partner_fails(self):
        data, offs, memsz = basic_seg_with_words([0x3C010000])
        m = Module()
        m.add_seg(0x1000, data, memsz)
        m.add_reloc(PT_REL_A, reloc_a(offs[0], 5))
        self.assert_fail(m.build(), needle="without partner")

    def test_a_bad_segment_index_fails(self):
        data, offs, memsz = basic_seg_with_words([0x1])
        m = Module()
        m.add_seg(0x1000, data, memsz)
        m.add_reloc(PT_REL_A, reloc_a(offs[0], 2, oseg=3, aseg=0))
        self.assert_fail(m.build(), needle="offset segment")
        m2 = Module()
        m2.add_seg(0x1000, data, memsz)
        m2.add_reloc(PT_REL_A, reloc_a(offs[0], 2, oseg=0, aseg=3))
        self.assert_fail(m2.build(), needle="address segment")

    def test_a_site_out_of_range_fails(self):
        data, offs, memsz = basic_seg_with_words([0x1])
        m = Module()
        m.add_seg(0x1000, data, memsz)
        m.add_reloc(PT_REL_A, reloc_a(memsz + 4, 2))
        self.assert_fail(m.build(), needle="site out of range")


# ---------------------------------------------------------------------------
# Item 2: format B
# ---------------------------------------------------------------------------

def b_stream(f, t, flags, kinds, cmds):
    nf = len(flags) + 1
    nk = len(kinds) + 1
    return bytes([0, 0, f, t, nf] + flags + [nk] + kinds) + cmds


class TestFormatB(PrxTestBase):
    F = 3
    T = 2
    # Flag entries used across tests (indices into the flag table).
    SET00 = 0x00  # set offset segment, offset := c >> (F+segw)
    SET32 = 0x04  # set offset segment, offset := following u32
    REL00 = 0x01  # relocate, offset += sign-extended d, addend 000
    REL01 = 0x03  # relocate, offset += (d:ext16), addend 000
    REL10 = 0x05  # relocate, offset := following u32, addend 000
    REL00_KEEP = 0x09  # relocate 00, addend mode 001 (keep iff prev kind 4)
    REL00_EXT = 0x11  # relocate 00, addend := following s16

    def flags(self):
        return [self.SET00, self.SET32, self.REL00, self.REL01, self.REL10,
                self.REL00_KEEP, self.REL00_EXT]

    def test_b_segwidth1_kinds(self):
        # Two segments -> 1-bit segment field; exercise B kinds 1,2,3.
        words = [0x24640002, 0x00000009, 0x0C000020]
        data0, offs, mem0 = basic_seg_with_words(words, vaddr=0x1000)
        data1 = bytes(64)
        f, t, sw = self.F, self.T, 1
        fl = self.flags()
        kinds = [1, 2, 3]
        cmds = b_cmd(2, 0, 0, 0, f, t, sw) + struct.pack("<I", offs[0])
        cmds += b_cmd(3, 1, 1, 0, f, t, sw)  # REL00 kind=1 aseg=1
        cmds += b_cmd(2, 0, 0, 0, f, t, sw) + struct.pack("<I", offs[1])
        cmds += b_cmd(3, 0, 2, 0, f, t, sw)  # REL00 kind=2 aseg=0
        cmds += b_cmd(2, 0, 0, 0, f, t, sw) + struct.pack("<I", offs[2])
        cmds += b_cmd(3, 0, 3, 0, f, t, sw)  # REL00 kind=3 aseg=0
        blob = b_stream(f, t, fl, kinds, cmds)
        m = Module()
        m.add_seg(0x1000, data0, mem0)
        m.add_seg(0x2000, data1)
        m.add_reloc(PT_REL_B, blob)
        info = self.run_load(m.build())
        self.assertEqual(info.get("result"), "ok", info.get("err"))
        s0, s1 = BASE + 0x1000, BASE + 0x2000
        self.assertEqual(self.seg_u32(info, 0, offs[0]),
                         exp_a16(words[0], s1))
        self.assertEqual(self.seg_u32(info, 0, offs[1]),
                         exp_a32(words[1], s0))
        self.assertEqual(self.seg_u32(info, 0, offs[2]),
                         exp_jump(words[2], s0 + offs[2], s0))

    def test_b_segwidth2(self):
        # Three segments -> 2-bit segment field; address segment 2.
        words = [0x00000005]
        data0, offs, mem0 = basic_seg_with_words(words, vaddr=0x1000)
        f, t, sw = self.F, self.T, 2
        fl = self.flags()
        kinds = [2]
        cmds = b_cmd(2, 0, 0, 0, f, t, sw) + struct.pack("<I", offs[0])
        cmds += b_cmd(3, 2, 1, 0, f, t, sw)
        blob = b_stream(f, t, fl, kinds, cmds)
        m = Module()
        m.add_seg(0x1000, data0, mem0)
        m.add_seg(0x2000, bytes(32))
        m.add_seg(0x4000, bytes(32))
        m.add_reloc(PT_REL_B, blob)
        info = self.run_load(m.build())
        self.assertEqual(info.get("result"), "ok", info.get("err"))
        self.assertEqual(self.seg_u32(info, 0, offs[0]),
                         exp_a32(words[0], BASE + 0x4000))

    def test_b_set00_rule_with_displacement_bits(self):
        # Pins section 3.6-B.4: SET with form 00 takes c >> (F+segw),
        # including kind/displacement bits, as the new running offset.
        data0 = bytearray(modinfo_block(name=b"bset"))
        data0 += b"\x11" * 8   # word @52 must stay untouched
        data0 += b"\x22" * 8   # word @60 is the real site
        data0 = bytes(data0)
        f, t, sw = self.F, self.T, 1
        fl = self.flags()
        kinds = [2]
        want_off = 60
        # c >> 3 must equal 60 -> low 3 bits are flag idx 1, seg 0;
        # upper bits (kind idx, d) ride along: c = (60 << 3) | 1.
        c = (want_off << (f + sw)) | 1
        self.assertNotEqual(c >> 8, 0, "need nonzero displacement bits")
        cmds = struct.pack("<H", c)
        cmds += b_cmd(3, 0, 1, 0, f, t, sw)  # REL00 kind=2, d=0
        blob = b_stream(f, t, fl, kinds, cmds)
        m = Module()
        m.add_seg(0x1000, data0, len(data0))
        m.add_reloc(PT_REL_B, blob)
        info = self.run_load(m.build())
        self.assertEqual(info.get("result"), "ok", info.get("err"))
        s0 = BASE + 0x1000
        self.assertEqual(self.seg_u32(info, 0, 52), 0x11111111,
                         "offset 52 must be untouched")
        self.assertEqual(self.seg_u32(info, 0, 60),
                         exp_a32(0x22222222, s0))

    def test_b_relocate_offset_forms(self):
        # 00 (add signed d), 01 (d:ext16), 10 (absolute u32).
        words = [0x1, 0x2, 0x3, 0x4]
        data0, offs, mem0 = basic_seg_with_words(
            words + [0] * 60, vaddr=0x1000)
        base_off = offs[0]
        o1, o2, o3 = base_off + 0x10, base_off + 0x20, base_off + 0x30
        raw = bytearray(data0)
        struct.pack_into("<I", raw, o1, 0xAAAA0001)
        struct.pack_into("<I", raw, o2, 0xBBBB0002)
        struct.pack_into("<I", raw, o3, 0xCCCC0003)
        data0 = bytes(raw)
        f, t, sw = self.F, self.T, 1
        fl = self.flags()
        kinds = [2]
        dw = 16 - f - sw - t
        cmds = b_cmd(2, 0, 0, 0, f, t, sw) + struct.pack("<I", o1 - 0x10)
        cmds += b_cmd(3, 0, 1, 0x10, f, t, sw)  # 00: += 0x10 -> o1
        cmds += b_cmd(2, 0, 0, 0, f, t, sw) + struct.pack("<I", o2 + 0x10)
        cmds += b_cmd(4, 0, 1, (1 << dw) - 1, f, t, sw)  # d=-1
        cmds += struct.pack("<H", 0xFFF0)  # full = -16 -> o2
        cmds += b_cmd(2, 0, 0, 0, f, t, sw) + struct.pack("<I", 0xDEAD)
        cmds += b_cmd(5, 0, 1, 0, f, t, sw) + struct.pack("<I", o3)
        # Note: flag idx 5 is REL10 (offset := u32); reused for absolute set.
        blob = b_stream(f, t, fl, kinds, cmds)
        m = Module()
        m.add_seg(0x1000, data0, mem0)
        m.add_reloc(PT_REL_B, blob)
        info = self.run_load(m.build())
        self.assertEqual(info.get("result"), "ok", info.get("err"))
        s0 = BASE + 0x1000
        self.assertEqual(self.seg_u32(info, 0, o1), exp_a32(0xAAAA0001, s0))
        self.assertEqual(self.seg_u32(info, 0, o2), exp_a32(0xBBBB0002, s0))
        self.assertEqual(self.seg_u32(info, 0, o3), exp_a32(0xCCCC0003, s0))

    def test_b_addend_modes(self):
        # kind 4 with 010 (ext), keep-after-4, keep-after-other-kind.
        data0, offs, mem0 = basic_seg_with_words(
            [0x3C010000, 0x3C020000, 0x00000001, 0x3C030000], vaddr=0x1000)
        f, t, sw = self.F, self.T, 1
        fl = self.flags()
        kinds = [4, 2]
        cmds = b""
        # site0: kind4, addend ext 0x20
        cmds += b_cmd(2, 0, 0, 0, f, t, sw) + struct.pack("<I", offs[0])
        cmds += b_cmd(7, 0, 1, 0, f, t, sw) + struct.pack("<H", 0x20)
        # site1: kind4, keep (prev kind was 4 -> keeps 0x20)
        cmds += b_cmd(2, 0, 0, 0, f, t, sw) + struct.pack("<I", offs[1])
        cmds += b_cmd(6, 0, 1, 0, f, t, sw)
        # site2: kind2 (resets prev-kind-4 chain)
        cmds += b_cmd(2, 0, 0, 0, f, t, sw) + struct.pack("<I", offs[2])
        cmds += b_cmd(3, 0, 2, 0, f, t, sw)
        # site3: kind4, keep (prev kind was 2 -> addend 0)
        cmds += b_cmd(2, 0, 0, 0, f, t, sw) + struct.pack("<I", offs[3])
        cmds += b_cmd(6, 0, 1, 0, f, t, sw)
        blob = b_stream(f, t, fl, kinds, cmds)
        m = Module()
        m.add_seg(0x1000, data0, mem0)
        m.add_reloc(PT_REL_B, blob)
        info = self.run_load(m.build())
        self.assertEqual(info.get("result"), "ok", info.get("err"))
        s0 = BASE + 0x1000
        self.assertEqual(self.seg_u32(info, 0, offs[0]),
                         exp_b_kind4(0x3C010000, 0x20, s0))
        self.assertEqual(self.seg_u32(info, 0, offs[1]),
                         exp_b_kind4(0x3C020000, 0x20, s0))
        self.assertEqual(self.seg_u32(info, 0, offs[2]), exp_a32(1, s0))
        self.assertEqual(self.seg_u32(info, 0, offs[3]),
                         exp_b_kind4(0x3C030000, 0, s0))

    def test_b_kinds_5_6_7(self):
        data0, offs, mem0 = basic_seg_with_words(
            [0x24640002, 0x00000000, 0xFFFFFFFF], vaddr=0x1000)
        f, t, sw = self.F, self.T, 1
        fl = self.flags()
        kinds = [5, 6, 7]
        cmds = b_cmd(2, 0, 0, 0, f, t, sw) + struct.pack("<I", offs[0])
        cmds += b_cmd(3, 0, 1, 0, f, t, sw)
        cmds += b_cmd(2, 0, 0, 0, f, t, sw) + struct.pack("<I", offs[1])
        cmds += b_cmd(3, 0, 2, 0, f, t, sw)
        cmds += b_cmd(2, 0, 0, 0, f, t, sw) + struct.pack("<I", offs[2])
        cmds += b_cmd(3, 0, 3, 0, f, t, sw)
        blob = b_stream(f, t, fl, kinds, cmds)
        m = Module()
        m.add_seg(0x1000, data0, mem0)
        m.add_reloc(PT_REL_B, blob)
        info = self.run_load(m.build())
        self.assertEqual(info.get("result"), "ok", info.get("err"))
        s0 = BASE + 0x1000
        self.assertEqual(self.seg_u32(info, 0, offs[0]),
                         exp_a16(0x24640002, s0))
        want6 = exp_jump(0, s0 + offs[1], s0) | (2 << 26)
        self.assertEqual(self.seg_u32(info, 0, offs[1]), want6)
        want7 = exp_jump(0xFFFFFFFF, s0 + offs[2], s0)
        want7 = (want7 & 0x03FFFFFF) | (3 << 26)
        self.assertEqual(self.seg_u32(info, 0, offs[2]), want7)

    def test_b_failures(self):
        f, t, sw = self.F, self.T, 1
        fl = self.flags()
        set32 = b_cmd(2, 0, 0, 0, f, t, sw) + struct.pack("<I", 52)
        rel = lambda fi=3, sg=0, ki=1, d=0: b_cmd(fi, sg, ki, d, f, t, sw)

        def mod_with(cmds):
            m = Module()
            data, _, mem = basic_seg_with_words([0x1], vaddr=0x1000)
            m.add_seg(0x1000, data, mem)
            m.add_reloc(PT_REL_B, cmds)
            return m.build()

        # flag index 0
        self.assert_fail(
            mod_with(b_stream(f, t, fl, [2], b_cmd(0, 0, 0, 0, f, t, sw))),
            needle="flag index 0")
        # flag index out of range (tiny table: only index 1 valid)
        self.assert_fail(
            mod_with(b_stream(f, t, [self.SET00], [2],
                              b_cmd(2, 0, 0, 0, f, t, sw))),
            needle="flag index out of range")
        # kind index 0
        self.assert_fail(
            mod_with(b_stream(f, t, fl, [2],
                              set32 + b_cmd(3, 0, 0, 0, f, t, sw))),
            needle="kind index 0")
        # kind index out of range (tiny table: only index 1 valid)
        tiny_set = (b_cmd(1, 0, 1, 0, f, t, sw) + struct.pack("<I", 52))
        self.assert_fail(
            mod_with(b_stream(f, t, [self.REL10], [2],
                              tiny_set + b_cmd(1, 0, 2, 0, f, t, sw))),
            needle="kind index out of range")
        # nonzero header bytes
        bad = bytearray(b_stream(f, t, fl, [2], b""))
        bad[0] = 1
        m = Module()
        data, _, mem = basic_seg_with_words([0x1], vaddr=0x1000)
        m.add_seg(0x1000, data, mem)
        m.add_reloc(PT_REL_B, bytes(bad))
        self.assert_fail(m.build(), needle="nonzero header")
        # truncated command: one dangling byte after the tables
        self.assert_fail(
            mod_with(bytes([0, 0, f, t, 2, self.SET00, 2, 2, 0xFF])),
            needle="truncated command")
        # truncated extension on absolute relocate
        self.assert_fail(
            mod_with(b_stream(f, t, fl, [2],
                              set32 + b_cmd(5, 0, 1, 0, f, t, sw))),
            needle="truncated extension")
        # truncated addend word
        self.assert_fail(
            mod_with(b_stream(f, t, fl, [2],
                              set32 + b_cmd(7, 0, 1, 0, f, t, sw))),
            needle="truncated addend")
        # bad set-offset form: flag with bits1-2 == 01
        badflag = fl[:6] + [0x02]
        cmds = b_cmd(7, 0, 0, 0, f, t, sw)
        m = Module()
        data, _, mem = basic_seg_with_words([0x1], vaddr=0x1000)
        m.add_seg(0x1000, data, mem)
        m.add_reloc(PT_REL_B, b_stream(f, t, badflag, [2], cmds))
        self.assert_fail(m.build(), needle="bad set-offset form")
        # bad relocate offset form: bits1-2 == 11
        badflag2 = fl[:6] + [0x07]
        cmds = b_cmd(7, 0, 1, 0, f, t, sw)
        m = Module()
        m.add_seg(0x1000, data, mem)
        m.add_reloc(PT_REL_B, b_stream(f, t, badflag2, [2], cmds))
        self.assert_fail(m.build(), needle="bad relocate offset form")
        # bad addend mode: bits3-5 == 011
        badflag3 = fl[:6] + [0x19]
        cmds = set32 + b_cmd(7, 0, 1, 0, f, t, sw)
        m = Module()
        m.add_seg(0x1000, data, mem)
        m.add_reloc(PT_REL_B, b_stream(f, t, badflag3, [2], cmds))
        self.assert_fail(m.build(), needle="bad addend mode")
        # unknown B kind
        cmds = set32 + rel()
        m = Module()
        m.add_seg(0x1000, data, mem)
        m.add_reloc(PT_REL_B, b_stream(f, t, fl, [9], cmds))
        self.assert_fail(m.build(), needle="unknown kind")
        # bad widths
        m = Module()
        m.add_seg(0x1000, data, mem)
        m.add_reloc(PT_REL_B, bytes([0, 0, 9, 2, 1, 1]))
        self.assert_fail(m.build(), needle="width")
        # site out of range
        cmds = b_cmd(2, 0, 0, 0, f, t, sw) + struct.pack("<I", 0x5000)
        cmds += rel()
        self.assert_fail(mod_with(b_stream(f, t, fl, [2], cmds)),
                         needle="site out of range")


# ---------------------------------------------------------------------------
# Item 3: layout
# ---------------------------------------------------------------------------

class TestLayout(PrxTestBase):
    def test_two_segments_bss_zero_filled(self):
        d0 = modinfo_block(name=b"bssmod") + b"\x11" * 16
        d1 = b"\x22" * 8
        m = Module(e_entry=0x40)
        m.add_seg(0x1000, d0, len(d0) + 32)
        m.add_seg(0x2000, d1, len(d1) + 24)
        info = self.run_load(m.build())
        self.assertEqual(info.get("result"), "ok", info.get("err"))
        self.assertEqual(info["nseg"], 2)
        self.assertEqual(info["start"], BASE + 0x1000)
        self.assertEqual(info["end"], BASE + 0x2000 + len(d1) + 24)
        mem0, mem1 = info["mems"]
        self.assertEqual(mem0[:len(d0)], d0)
        self.assertEqual(mem0[len(d0):], b"\x00" * 32)
        self.assertEqual(mem1[:len(d1)], d1)
        self.assertEqual(mem1[len(d1):], b"\x00" * 24)

    def test_overlapping_segments_fail(self):
        m = Module()
        m.add_seg(0x1000, modinfo_block() + bytes(64), 0x1000)
        m.add_seg(0x1800, bytes(64), 0x100)
        self.assert_fail(m.build(), needle="overlapping")

    def test_five_segments_fail(self):
        m = Module()
        for i in range(5):
            m.add_seg(0x1000 + i * 0x1000, bytes(16), 16)
        # Segment 0 still needs module info; failure is about the count.
        self.assert_fail(m.build(), needle="too many")

    def test_exec_base0_ok_nonzero_base_fails(self):
        d0 = modinfo_block(name=b"execmod") + bytes(16)
        m = Module(e_type=2, e_entry=0x1200)
        m.add_seg(0x1000, d0, len(d0))
        info = self.run_load(m.build(), base=0)
        self.assertEqual(info.get("result"), "ok", info.get("err"))
        self.assertEqual(info["entry"], 0x1200)
        self.assert_fail(m.build(), base=BASE, needle="nonzero base")

    def test_psp_magic_fails(self):
        self.assert_fail(b"~PSP" + bytes(128), needle="decryption")

    def test_bad_headers_fail(self):
        good = Module()
        good.add_seg(0x1000, modinfo_block() + bytes(16), 68)
        blob = bytearray(good.build())
        bad = bytearray(blob)
        bad[0] = 0x7E
        self.assert_fail(bytes(bad), needle="ELF")
        bad = bytearray(blob)
        bad[4] = 2
        self.assert_fail(bytes(bad), needle="class")
        bad = bytearray(blob)
        bad[5] = 2
        self.assert_fail(bytes(bad), needle="endianness")
        bad = bytearray(blob)
        bad[18] = 3
        self.assert_fail(bytes(bad), needle="machine")
        bad = bytearray(blob)
        bad[16] = 0x03
        bad[17] = 0x00
        self.assert_fail(bytes(bad), needle="type")
        self.assert_fail(bytes(blob[:20]), needle=None)


# ---------------------------------------------------------------------------
# Item 4: module info
# ---------------------------------------------------------------------------

class TestModinfo(PrxTestBase):
    def mod_with_table_strings(self, extra_sections=()):
        d0 = bytearray(modinfo_block(name=b"mimod", gp=0xDEAD,
                                     ver=(1, 2)))
        d0 += bytes(64)
        m = Module()
        m.add_seg(0x1000, bytes(d0), len(d0))
        for (nm, tp, addr, dd) in extra_sections:
            m.add_section(nm, tp, addr, dd)
        return m

    def test_section_agreeing(self):
        m = self.mod_with_table_strings()
        m.add_section(".rodata.sceModuleInfo", 1, 0x1000, bytes(4))
        info = self.run_load(m.build())
        self.assertEqual(info.get("result"), "ok", info.get("err"))
        self.assertEqual(info["modname"], "mimod")
        self.assertEqual(info["modinfo"], BASE + 0x1000)
        self.assertEqual(info["gp"], 0xDEAD)
        self.assertEqual(info["ver"], (1, 2))

    def test_section_disagreeing_fails(self):
        m = self.mod_with_table_strings()
        m.add_section(".rodata.sceModuleInfo", 1, 0x1010, bytes(4))
        self.assert_fail(m.build(), needle="disagrees")

    def test_name_without_nul_fails(self):
        d0 = modinfo_block(nonul=True) + bytes(16)
        m = Module()
        m.add_seg(0x1000, d0, len(d0))
        self.assert_fail(m.build(), needle="NUL")

    def test_paddr_below_offset_fails(self):
        d0 = modinfo_block() + bytes(16)
        m = Module()
        m.add_seg(0x1000, d0, len(d0), paddr=0)
        self.assert_fail(m.build(), needle="p_paddr")


# ---------------------------------------------------------------------------
# Item 5: exports
# ---------------------------------------------------------------------------

def build_export_fixture(zero_len=False):
    g = BASE + 0x1000
    data = bytearray()
    data += modinfo_block(name=b"expmod")  # placeholder, patched below
    rec0_off = len(data)
    data += b"\x00" * 16
    rec1_off = len(data)
    data += b"\x00" * 16
    etab0_off = len(data)
    data += b"\x00" * 24
    etab1_off = len(data)
    data += b"\x00" * 16
    str_off = len(data)
    data += b"myLib\x00"
    data = bytes(data)
    exp_start, exp_end = g + rec0_off, g + rec1_off + 16
    mod = bytearray(modinfo_block(name=b"expmod", exp_start=exp_start,
                                  exp_end=exp_end))
    data = bytes(mod) + data[52:]
    raw = bytearray(data)
    f0, f1 = g + 0x500, g + 0x600
    vv = g + 0x700
    struct.pack_into("<IHHBBHI", raw, rec0_off, 0, 0, 0, 4, 1, 2,
                     g + etab0_off)
    struct.pack_into("<3I", raw, etab0_off, 0xD632ACDB, 0xCEE05613,
                     0xF01D73A7)
    struct.pack_into("<3I", raw, etab0_off + 12, f0, f1, vv)
    struct.pack_into("<IHHBBHI", raw, rec1_off, g + str_off, 0, 0, 4, 1, 1,
                     g + etab1_off)
    struct.pack_into("<2I", raw, etab1_off, 0x12345678, 0x9ABCDEF0)
    struct.pack_into("<2I", raw, etab1_off + 8, g + 0x800, g + 0x900)
    if zero_len:
        raw[rec1_off + 8] = 0
    return bytes(raw)


class TestExports(PrxTestBase):
    def test_exports(self):
        m = Module(e_entry=0x10)
        m.add_seg(0x1000, build_export_fixture(), 256)
        info = self.run_load(m.build())
        self.assertEqual(info.get("result"), "ok", info.get("err"))
        self.assertEqual(info["nexp"], 5)
        by_nid = {e["nid"]: e for e in info["exps"]}
        self.assertEqual(by_nid[0xD632ACDB]["lib"], "")
        self.assertEqual(by_nid[0xD632ACDB]["func"], 1)
        self.assertEqual(by_nid[0xD632ACDB]["addr"], BASE + 0x1000 + 0x500)
        self.assertEqual(by_nid[0xCEE05613]["addr"], BASE + 0x1000 + 0x600)
        self.assertEqual(by_nid[0xF01D73A7]["func"], 0)
        self.assertEqual(by_nid[0x12345678]["lib"], "myLib")
        self.assertEqual(by_nid[0x12345678]["func"], 1)
        self.assertEqual(by_nid[0x9ABCDEF0]["func"], 0)
        self.assertEqual(info["hasstart"], 1)
        self.assertEqual(info["modstart"], BASE + 0x1000 + 0x500)
        # Entry is separate from module_start.
        self.assertEqual(info["entry"], BASE + 0x10)

    def test_zero_length_record_fails(self):
        m = Module()
        m.add_seg(0x1000, build_export_fixture(zero_len=True), 256)
        self.assert_fail(m.build(), needle="zero length")

    def test_file_api_matches_mem_api(self):
        m = Module()
        m.add_seg(0x1000, build_export_fixture(), 256)
        blob = m.build()
        a = self.run_load(blob, mode="mem")
        b = self.run_load(blob, mode="file")
        self.assertEqual(a["result"], "ok")
        self.assertEqual(b["result"], "ok")
        self.assertEqual(a["exps"], b["exps"])
        self.assertEqual(a["entry"], b["entry"])


# ---------------------------------------------------------------------------
# Item 6: imports
# ---------------------------------------------------------------------------

def build_import_fixture():
    g = BASE + 0x1000
    data = bytearray()
    data += b"\x00" * 52  # modinfo placeholder
    r0 = len(data)
    data += b"\x00" * 20  # len-5 record
    r1 = len(data)
    data += b"\x00" * 24  # len-6 record with vartable
    nid0 = len(data)
    data += b"\x00" * 8
    stub0 = len(data)
    data += b"\x00" * 16
    nid1 = len(data)
    data += b"\x00" * 4
    stub1 = len(data)
    data += b"\x00" * 8
    s0 = len(data)
    data += b"libA\x00"
    s1 = len(data)
    data += b"libB\x00"
    data = bytes(data)
    imp_start, imp_end = g + r0, g + r1 + 24
    mod = modinfo_block(name=b"impmod", imp_start=imp_start, imp_end=imp_end)
    raw = bytearray(mod + data[52:])
    struct.pack_into("<IHHBBHII", raw, r0, g + s0, 0, 0, 5, 0, 2, g + nid0,
                     g + stub0)
    struct.pack_into("<2I", raw, nid0, 0x11111111, 0x22222222)
    struct.pack_into("<IHHBBHIII", raw, r1, g + s1, 0, 0, 6, 0, 1, g + nid1,
                     g + stub1, 0)
    struct.pack_into("<I", raw, nid1, 0x33333333)
    return bytes(raw)


class TestImports(PrxTestBase):
    def test_imports(self):
        m = Module()
        m.add_seg(0x1000, build_import_fixture(), 256)
        info = self.run_load(m.build())
        self.assertEqual(info.get("result"), "ok", info.get("err"))
        self.assertEqual(info["nimp"], 3)
        g = BASE + 0x1000
        # Recompute expected stub addresses from the builder layout.
        build_import_fixture()
        # stub0 area starts right after nid0 (8 bytes): find via structure.
        # Layout: modinfo(52) r0(20) r1(24) nid0(8) stub0(16) nid1(4)
        #         stub1(8) s0 s1.
        stub0 = g + 52 + 20 + 24 + 8
        stub1 = stub0 + 16 + 4
        got = {(e["lib"], e["nid"]): e["stub"] for e in info["imps"]}
        self.assertEqual(got[("libA", 0x11111111)], stub0)
        self.assertEqual(got[("libA", 0x22222222)], stub0 + 8)
        self.assertEqual(got[("libB", 0x33333333)], stub1)


# ---------------------------------------------------------------------------
# Revised-spec pinning: §3.6 HI16/B-kind4 formulas and §3.7/§3.8 layouts.
# These vectors fail against the previous revision's formulas/offsets.
# ---------------------------------------------------------------------------

class TestRevisedPinning(PrxTestBase):
    def test_hi16_uses_low_immediate_not_opcode(self):
        # lui immediate lives in the LOW 16 bits (spec §3.6 kind 5).
        # hi low=0x1234 -> base 0x12340000; old code used 0x3C010000.
        hi = 0x3C011234
        partner = 0x24640020  # low 0x0020
        data, offs, memsz = basic_seg_with_words([hi, partner])
        tab = reloc_a(offs[0], 5) + reloc_a(offs[1], 1)
        m = Module()
        m.add_seg(0x1000, data, memsz)
        m.add_reloc(PT_REL_A, tab)
        info = self.run_load(m.build())
        self.assertEqual(info.get("result"), "ok", info.get("err"))
        s = BASE + 0x1000
        want = exp_hi16(hi, partner, s)
        # Old formula would give a different low half.
        old_v = ((hi & 0xFFFF0000) + sx16(partner) + s) & 0xFFFFFFFF
        old_lo = ((old_v + 0x8000) & 0xFFFFFFFF) >> 16 & 0xFFFF
        self.assertNotEqual(want & 0xFFFF, old_lo,
                            "vector must distinguish revised from old HI16")
        got = self.seg_u32(info, 0, offs[0])
        self.assertEqual(got, want)
        # High 16 (opcode/registers) unchanged, result in low 16.
        self.assertEqual(got & 0xFFFF0000, hi & 0xFFFF0000)
        self.assertEqual(got & 0xFFFF, want & 0xFFFF)

    def test_hi16_carry_with_nonzero_low(self):
        # Carry case with a nonzero lui immediate: low 0x1234,
        # partner low 0x8000 (sx16 = -32768) must bump the upper half.
        hi = 0x3C011234
        partner = 0x24648000
        data, offs, memsz = basic_seg_with_words([hi, partner])
        tab = reloc_a(offs[0], 5) + reloc_a(offs[1], 1)
        m = Module()
        m.add_seg(0x1000, data, memsz)
        m.add_reloc(PT_REL_A, tab)
        info = self.run_load(m.build())
        self.assertEqual(info.get("result"), "ok", info.get("err"))
        s = BASE + 0x1000
        v = (((hi & 0xFFFF) << 16) + sx16(partner) + s) & 0xFFFFFFFF
        self.assertEqual(sx16(partner), -0x8000)
        self.assertNotEqual((v >> 16) & 0xFFFF,
                            (v + 0x8000) >> 16 & 0xFFFF,
                            "carry must trigger for this vector")
        self.assertEqual(self.seg_u32(info, 0, offs[0]),
                         exp_hi16(hi, partner, s))

    def test_b_kind4_writes_low_preserves_high(self):
        # Revised B kind 4: base = low16(W)*65536, result to LOW 16,
        # high preserved. Old code did the opposite (high base, result
        # to high, low preserved).
        w = 0x3C02ABCD
        addend = 0x0020
        data0, offs, mem0 = basic_seg_with_words([w], vaddr=0x1000)
        f, t, sw = 3, 2, 1
        flags = [0x00, 0x04, 0x01, 0x03, 0x05, 0x09, 0x11]
        kinds = [4]
        cmds = b_cmd(2, 0, 0, 0, f, t, sw) + struct.pack("<I", offs[0])
        cmds += b_cmd(7, 0, 1, 0, f, t, sw) + struct.pack("<H", addend)
        blob = b_stream(f, t, flags, kinds, cmds)
        m = Module()
        m.add_seg(0x1000, data0, mem0)
        m.add_reloc(PT_REL_B, blob)
        info = self.run_load(m.build())
        self.assertEqual(info.get("result"), "ok", info.get("err"))
        s0 = BASE + 0x1000
        want = exp_b_kind4(w, addend, s0)
        got = self.seg_u32(info, 0, offs[0])
        self.assertEqual(got, want)
        self.assertEqual(got & 0xFFFF0000, w & 0xFFFF0000,
                         "B kind 4 must preserve high 16")
        # Old behaviour kept the low half untouched; revised changes it
        # (for this vector) and leaves high alone.
        old_base = w & 0xFFFF0000
        old_v = (old_base + sx16(addend) + s0) & 0xFFFFFFFF
        old_hi = (old_v + 0x8000) >> 16 & 0xFFFF
        old_word = (old_hi << 16) | (w & 0xFFFF)
        self.assertNotEqual(got, old_word,
                            "vector must distinguish revised B kind 4")
        self.assertEqual(w & 0xFFFF, 0xABCD)
        self.assertNotEqual(got & 0xFFFF, w & 0xFFFF)

    def test_b_kind4_carry(self):
        w = 0x3C02FFFF
        addend = 0x8000  # sx16 = -32768, forces the +0x8000 carry path
        data0, offs, mem0 = basic_seg_with_words([w], vaddr=0x1000)
        f, t, sw = 3, 2, 1
        flags = [0x00, 0x04, 0x01, 0x03, 0x05, 0x09, 0x11]
        kinds = [4]
        cmds = b_cmd(2, 0, 0, 0, f, t, sw) + struct.pack("<I", offs[0])
        cmds += b_cmd(7, 0, 1, 0, f, t, sw) + struct.pack("<H", addend)
        blob = b_stream(f, t, flags, kinds, cmds)
        m = Module()
        m.add_seg(0x1000, data0, mem0)
        m.add_reloc(PT_REL_B, blob)
        info = self.run_load(m.build())
        self.assertEqual(info.get("result"), "ok", info.get("err"))
        s0 = BASE + 0x1000
        v = (((w & 0xFFFF) << 16) + sx16(addend) + s0) & 0xFFFFFFFF
        self.assertNotEqual((v >> 16) & 0xFFFF,
                            (v + 0x8000) >> 16 & 0xFFFF)
        self.assertEqual(self.seg_u32(info, 0, offs[0]),
                         exp_b_kind4(w, addend, s0))

    def test_export_field_offsets_widths(self):
        # Pins revised §3.7 head: libname u32 @0, version u16 @4,
        # attr u16 @6, len u8 @8, nvar u8 @9, nfunc u16 @10, etab u32 @12.
        # Nonzero version/attr would shift every old-revision u32 field.
        g = BASE + 0x1000
        seg = bytearray()
        seg += b"\x00" * 52  # modinfo placeholder
        r0 = len(seg)
        seg += b"\x00" * 16  # len-4 record
        r1 = len(seg)
        seg += b"\x00" * 24  # len-6 record: 16 head + 8 ignored bytes
        e0 = len(seg)
        seg += b"\x00" * 8  # 1 func: 1 NID + 1 addr
        e1 = len(seg)
        seg += b"\x00" * 16  # 2 entries: 2 NIDs + 2 addrs
        seg = bytes(seg)
        exp_start, exp_end = g + r0, g + r1 + 24
        mod = modinfo_block(name=b"pinxp", exp_start=exp_start,
                            exp_end=exp_end)
        raw = bytearray(mod + seg[52:])
        # rec0: version 0x1234, attr 0x5678, len 4, nvar 0, nfunc 1.
        struct.pack_into("<IHHBBHI", raw, r0, 0, 0x1234, 0x5678, 4, 0, 1,
                         g + e0)
        struct.pack_into("<I", raw, e0, 0xAAAAAAAA)
        struct.pack_into("<I", raw, e0 + 4, g + 0x500)
        # rec1: version 0x2222, attr 0x3333, len 6, nvar 1, nfunc 1,
        # with 8 ignored tail bytes beyond offset 16.
        struct.pack_into("<IHHBBHI", raw, r1, 0, 0x2222, 0x3333, 6, 1, 1,
                         g + e1)
        raw[r1 + 16:r1 + 24] = b"\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
        struct.pack_into("<2I", raw, e1, 0xBBBBBBBB, 0xCCCCCCCC)
        struct.pack_into("<2I", raw, e1 + 8, g + 0x600, g + 0x700)
        m = Module()
        m.add_seg(0x1000, bytes(raw), 256)
        info = self.run_load(m.build())
        self.assertEqual(info.get("result"), "ok", info.get("err"))
        self.assertEqual(info["nexp"], 3)
        by_nid = {e["nid"]: e for e in info["exps"]}
        self.assertEqual(by_nid[0xAAAAAAAA]["addr"], g + 0x500)
        self.assertEqual(by_nid[0xAAAAAAAA]["func"], 1)
        self.assertEqual(by_nid[0xBBBBBBBB]["func"], 1)
        self.assertEqual(by_nid[0xBBBBBBBB]["addr"], g + 0x600)
        self.assertEqual(by_nid[0xCCCCCCCC]["func"], 0)
        self.assertEqual(by_nid[0xCCCCCCCC]["addr"], g + 0x700)
        # Walking used the len byte at offset 8: rec1 must start at
        # r0 + 4*4 = r0+16. Raw byte check pins the offsets.
        self.assertEqual(raw[r0 + 8], 4)
        self.assertEqual(raw[r0 + 9], 0)
        self.assertEqual(struct.unpack_from("<H", raw, r0 + 10)[0], 1)
        self.assertEqual(struct.unpack_from("<I", raw, r0 + 12)[0], g + e0)
        self.assertEqual(raw[r1 + 8], 6)

    def test_export_short_len_fails(self):
        # len 3 (< 4 words) must fail even though the 16-byte head is present.
        g = BASE + 0x1000
        seg = bytearray(modinfo_block(name=b"pinxf"))
        r0 = len(seg)
        seg += struct.pack("<IHHBBHI", 0, 0, 0, 3, 0, 0, g + 0x200)
        seg = bytes(seg)
        exp_start, exp_end = g + r0, g + r0 + 16
        mod = modinfo_block(name=b"pinxf", exp_start=exp_start,
                            exp_end=exp_end)
        raw = bytearray(mod + seg[52:])
        m = Module()
        m.add_seg(0x1000, bytes(raw), 256)
        self.assert_fail(m.build(), needle="too short")

    def test_stub_field_offsets_widths(self):
        # Pins revised §3.8 head: libname u32 @0, version u16 @4,
        # attr u16 @6, len u8 @8, nvar u8 @9, nfunc u16 @10,
        # nidtab u32 @12, stubtab u32 @16, vartab u32 @20 (len>=6).
        g = BASE + 0x1000
        seg = bytearray()
        seg += b"\x00" * 52
        r0 = len(seg)
        seg += b"\x00" * 20  # len-5
        r1 = len(seg)
        seg += b"\x00" * 24  # len-6 with vartable
        n0 = len(seg)
        seg += b"\x00" * 4
        s0a = len(seg)
        seg += b"\x00" * 8
        n1 = len(seg)
        seg += b"\x00" * 4
        s1a = len(seg)
        seg += b"\x00" * 8
        t0 = len(seg)
        seg += b"pinA\x00"
        t1 = len(seg)
        seg += b"pinB\x00"
        seg = bytes(seg)
        imp_start, imp_end = g + r0, g + r1 + 24
        mod = modinfo_block(name=b"pinimp", imp_start=imp_start,
                            imp_end=imp_end)
        raw = bytearray(mod + seg[52:])
        struct.pack_into("<IHHBBHII", raw, r0, g + t0, 0x1111, 0x2222, 5,
                         0, 1, g + n0, g + s0a)
        struct.pack_into("<I", raw, n0, 0xDDDDDDDD)
        struct.pack_into("<IHHBBHIII", raw, r1, g + t1, 0x3333, 0x4444, 6,
                         0, 1, g + n1, g + s1a, 0xDEADBEEF)
        struct.pack_into("<I", raw, n1, 0xEEEEEEEE)
        m = Module()
        m.add_seg(0x1000, bytes(raw), 256)
        info = self.run_load(m.build())
        self.assertEqual(info.get("result"), "ok", info.get("err"))
        self.assertEqual(info["nimp"], 2)
        got = {(e["lib"], e["nid"]): e["stub"] for e in info["imps"]}
        self.assertEqual(got[("pinA", 0xDDDDDDDD)], g + s0a)
        self.assertEqual(got[("pinB", 0xEEEEEEEE)], g + s1a)
        # Raw byte checks pin the field offsets/widths.
        self.assertEqual(struct.unpack_from("<H", raw, r0 + 4)[0], 0x1111)
        self.assertEqual(struct.unpack_from("<H", raw, r0 + 6)[0], 0x2222)
        self.assertEqual(raw[r0 + 8], 5)
        self.assertEqual(raw[r0 + 9], 0)
        self.assertEqual(struct.unpack_from("<H", raw, r0 + 10)[0], 1)
        self.assertEqual(struct.unpack_from("<I", raw, r0 + 12)[0], g + n0)
        self.assertEqual(struct.unpack_from("<I", raw, r0 + 16)[0], g + s0a)
        self.assertEqual(raw[r1 + 8], 6)
        self.assertEqual(struct.unpack_from("<I", raw, r1 + 20)[0],
                         0xDEADBEEF)

    def test_stub_short_len_fails(self):
        # len 4 (< 5 words) must fail even though the 20-byte head is present.
        g = BASE + 0x1000
        seg = bytearray(modinfo_block(name=b"pinisf"))
        r0 = len(seg)
        seg += struct.pack("<IHHBBHII", 0, 0, 0, 4, 0, 0, g + 0x200,
                           g + 0x300)
        seg = bytes(seg)
        imp_start, imp_end = g + r0, g + r0 + 20
        mod = modinfo_block(name=b"pinisf", imp_start=imp_start,
                            imp_end=imp_end)
        raw = bytearray(mod + seg[52:])
        m = Module()
        m.add_seg(0x1000, bytes(raw), 256)
        self.assert_fail(m.build(), needle="too short")


# ---------------------------------------------------------------------------
# Item 7: failure atomicity — every failing case leaves the guard arena clean
# ---------------------------------------------------------------------------

class TestAtomicity(PrxTestBase):
    def failing_blobs(self):
        blobs = []
        d = basic_seg_with_words([0x1])[0]
        m = Module()
        m.add_seg(0x1000, d, len(d))
        m.add_reloc(PT_REL_A, reloc_a(52, 7))
        blobs.append(m.build())  # A kind 7
        m = Module()
        m.add_seg(0x1000, d, len(d))
        m.add_reloc(PT_REL_A, reloc_a(52, 5))
        blobs.append(m.build())  # HI16 without partner
        m = Module()
        m.add_seg(0x1000, d, len(d))
        m.add_reloc(PT_REL_B, b_stream(2, 2, [0x01], [2],
                                       b_cmd(0, 0, 0, 0, 2, 2, 1)))
        blobs.append(m.build())  # B flag index 0
        m = Module()
        m.add_seg(0x1000, modinfo_block() + bytes(64), 0x1000)
        m.add_seg(0x1800, bytes(64), 0x100)
        blobs.append(m.build())  # overlap
        m = Module()
        for i in range(5):
            m.add_seg(0x1000 + i * 0x1000, bytes(16), 16)
        blobs.append(m.build())  # too many segments
        me = Module(e_type=2, e_entry=0)
        me.add_seg(0x1000, modinfo_block() + bytes(16), 68)
        blobs.append(me.build())  # ET_EXEC (fails at nonzero base)
        blobs.append(b"~PSP" + bytes(64))  # container
        blobs.append(bytes(100))  # not ELF
        blobs.append(modinfo_block(nonul=True) + bytes(200))  # not ELF
        d = modinfo_block(nonul=True) + bytes(16)
        m = Module()
        m.add_seg(0x1000, d, len(d))
        blobs.append(m.build())  # name without NUL
        m = Module()
        m.add_seg(0x1000, build_export_fixture(zero_len=True), 256)
        blobs.append(m.build())  # export zero length
        good = Module()
        good.add_seg(0x1000, modinfo_block() + bytes(64), 116)
        full = good.build()
        blobs.append(full[:52 + 32])  # truncated program headers
        blobs.append(full[:10])  # truncated header
        return blobs

    def test_failures_leave_arena_clean(self):
        for idx, blob in enumerate(self.failing_blobs()):
            info = self.run_load(blob, base=BASE)
            self.assertEqual(info.get("result"), "fail",
                             "blob %d unexpectedly loaded" % idx)
            self.assertEqual(info.get("arena_clean"), 1,
                             "blob %d dirtied the arena" % idx)


# ---------------------------------------------------------------------------
# Item 8: fuzz — 10,000 random mutations never crash nor hang
# ---------------------------------------------------------------------------

class TestFuzz(PrxTestBase):
    def test_fuzz_10000_mutations(self):
        m = Module(e_entry=0x40)
        d0 = (modinfo_block(name=b"fuzzmod") + struct.pack("<4I", 1, 2, 3, 4)
              + b"libX\x00")
        m.add_seg(0x1000, d0, len(d0) + 16)
        m.add_seg(0x2000, bytes(48), 64)
        m.add_reloc(PT_REL_A, reloc_a(52, 2) + reloc_a(56, 1))
        seed_blob = m.build()
        rng = random.Random(0xC10A4E)
        n = 10000
        t0 = time.monotonic()
        stream = bytearray()
        for _i in range(n):
            mut = bytearray(seed_blob)
            op = rng.randrange(4)
            if op == 0 and len(mut) > 0:
                for _ in range(rng.randrange(1, 9)):
                    mut[rng.randrange(len(mut))] = rng.randrange(256)
            elif op == 1 and len(mut) > 1:
                mut = mut[:rng.randrange(len(mut))]
            elif op == 2:
                mut += bytes(rng.randrange(256) for _ in range(
                    rng.randrange(1, 32)))
            else:
                if len(mut) > 0:
                    pos = rng.randrange(len(mut))
                    mut[pos:pos] = bytes(rng.randrange(256) for _ in range(
                        rng.randrange(0, 8)))
            stream += struct.pack("<I", len(mut)) + mut
        r = subprocess.run([self.EXE, "--fuzz-stream", "0x%X" % BASE],
                           input=bytes(stream),
                           capture_output=True, timeout=60)
        out_text = r.stdout.decode("ascii", errors="replace") if r.stdout else ""
        crashes = 0 if (r.returncode == 0 and "RESULT ok" in out_text) else 1
        dt = time.monotonic() - t0
        print("\nfuzz: %d mutations in %.1fs, crashes/hangs=%d"
              % (n, dt, crashes))
        self.assertEqual(crashes, 0, "%d fuzz crashes/hangs: %s" % (crashes, r.stderr))
        self.assertLess(dt, 540, "fuzz exceeded time bound")


if __name__ == "__main__":
    unittest.main(verbosity=2)
