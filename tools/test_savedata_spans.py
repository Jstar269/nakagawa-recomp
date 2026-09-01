# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SAVEDATA_C = ROOT / "src" / "rt" / "savedata.c"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
SEAM_H = ROOT / "src" / "rt" / "vfs_contained.h"


def _savedata_fn(marker, end_marker):
    """Return the source text of one savedata.c function, by markers."""
    text = SAVEDATA_C.read_text(encoding="utf-8")
    idx = text.find(marker)
    assert idx != -1, "marker not found: " + marker
    end = text.find(end_marker, idx + len(marker))
    assert end != -1, "end marker not found: " + end_marker
    return text[idx:end]


def _strip_c_comments(code):
    """Drop C comments so a source-shape gate judges CODE, not prose.

    The seam's documentation deliberately names the host primitives it refuses
    to let generic logic call; a gate that scanned raw text would fire on the
    explanation instead of the implementation."""
    return re.sub(r"/\*.*?\*/", " ", code, flags=re.S)


# The seam's backends are delimited by banner comments, not by the macro names
# (those also appear in the struct and in the diagnostics helpers).
_SEAM_BANNERS = {
    "SR_CD_BACKEND_WINDOWS": "/* Windows backend: OPEN -> HANDLE",
    "SR_CD_BACKEND_POSIX_AT": "/* POSIX backend: descriptor-relative",
    "SR_CD_BACKEND_NONE": "/* Fail-closed backend for hosts",
}


def _seam_backend(macro):
    """Return the source text of one vfs_contained.h backend block."""
    text = SEAM_H.read_text(encoding="utf-8")
    banner = _SEAM_BANNERS[macro]
    idx = text.index(banner)
    rest = text[idx + len(banner):]
    following = [rest.index(b) for b in _SEAM_BANNERS.values() if b in rest]
    end = idx + len(banner) + (min(following) if following else len(rest))
    return text[idx:end]


def _seam_generic():
    """The host-neutral half: everything above the first backend banner."""
    text = SEAM_H.read_text(encoding="utf-8")
    first = min(text.index(b) for b in _SEAM_BANNERS.values())
    return text[:first]


def _assert_absent(case, needles, code, what):
    """Refuse a bare substring match: `fdopendir(` must not trip a ban on
    `opendir(`, and `AT_REMOVEDIR` must not trip a ban on a remove call."""
    stripped = _strip_c_comments(code)
    for needle in needles:
        # Every banned name is a CALL, so require the call parenthesis: a ban on
        # "stat" must not fire on the word "static".
        pattern = r"\b" + re.escape(needle) + r"\s*\("
        case.assertIsNone(re.search(pattern, stripped),
                          f"{what} must not name {needle!r}")


class TestSavedataSpanPreflight(unittest.TestCase):
    def test_savedata_uses_guest_span_validation(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        self.assertIn("sr_guest_span_readable", text, "savedata.c must use sr_guest_span_readable for preflight checks")
        self.assertIn("sr_guest_span_writable", text, "savedata.c must use sr_guest_span_writable for preflight checks")

    def test_preflight_checks_occur_before_host_file_operations(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        do_save_idx = text.find("static uint32_t do_save")
        self.assertNotEqual(do_save_idx, -1)
        do_save_code = text[do_save_idx:text.find("static int dir_exists", do_save_idx)]

        prepare_idx = do_save_code.find("s_storage.prepare")
        span_check_idx = do_save_code.find("sr_guest_span_readable")
        self.assertNotEqual(prepare_idx, -1)
        self.assertNotEqual(span_check_idx, -1)
        self.assertLess(span_check_idx, prepare_idx, "do_save must validate guest spans BEFORE calling s_storage.prepare")

    def test_do_load_validates_spans_before_mutating_datasize(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        do_load_idx = text.find("static uint32_t do_load")
        self.assertNotEqual(do_load_idx, -1)
        do_load_code = text[do_load_idx:text.find("static uint32_t do_delete", do_load_idx)]

        mutate_idx = do_load_code.find("MEM_W32(param + SDP_dataSize, 0)")
        span_check_idx = do_load_code.find("sr_guest_span_writable")
        self.assertNotEqual(mutate_idx, -1)
        self.assertNotEqual(span_check_idx, -1)
        self.assertLess(span_check_idx, mutate_idx, "do_load must validate guest spans BEFORE mutating SDP_dataSize")

    def test_null_buffer_and_oversized_spans_are_rejected_in_do_save_preflight(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        do_save_idx = text.find("static uint32_t do_save")
        self.assertNotEqual(do_save_idx, -1)
        do_save_code = text[do_save_idx:text.find("static int dir_exists", do_save_idx)]
        self.assertIn("!dataBuf", do_save_code, "do_save must reject dataBuf == 0 when dataSize > 0")
        self.assertIn("dataSize >= 0x04000000", do_save_code, "do_save must reject dataSize >= 64 MiB in preflight check")

    def test_zero_capacity_and_null_buffers_are_rejected_in_do_load_preflight(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        do_load_idx = text.find("static uint32_t do_load")
        self.assertNotEqual(do_load_idx, -1)
        do_load_code = text[do_load_idx:text.find("static uint32_t do_delete", do_load_idx)]
        self.assertIn("!dataBuf", do_load_code, "do_load must reject dataBuf == 0 when loading file data")
        self.assertIn("cap == 0", do_load_code, "do_load must reject cap == 0 when loading file data")
        self.assertIn("cap >= 0x04000000", do_load_code, "do_load must reject cap >= 64 MiB in preflight check")

    def test_load_debug_preview_is_bounded_by_bytes_read(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        do_load_idx = text.find("static uint32_t do_load")
        self.assertNotEqual(do_load_idx, -1)
        do_load_code = text[do_load_idx:text.find("static uint32_t do_delete", do_load_idx)]
        self.assertIn("rd > 0", do_load_code, "debug preview must bound p[0] access by actual bytes read")
        self.assertIn("rd > 3", do_load_code, "debug preview must bound p[3] access by actual bytes read")

    def test_load_file_size_clamping_uses_uint64_comparison(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        do_load_idx = text.find("static uint32_t do_load")
        self.assertNotEqual(do_load_idx, -1)
        do_load_code = text[do_load_idx:text.find("static uint32_t do_delete", do_load_idx)]
        self.assertIn("(uint64_t)cap", do_load_code, "do_load must compare file size against cap using 64-bit unsigned comparison to prevent LP64 narrowing truncation bypass")

    def test_sfo_reader_checks_index_extent_and_bounded_keys(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        sfo_idx = text.find("static void load_sfo_param")
        self.assertNotEqual(sfo_idx, -1)
        sfo_code = text[sfo_idx:text.find("/* ---- modes", sfo_idx)]
        self.assertIn("cnt > ((uint32_t)n - 20u) / 16u", sfo_code)
        self.assertIn("uint64_t keyPos", sfo_code)
        self.assertIn("uint64_t dataPos", sfo_code)
        self.assertIn("unterminated key", sfo_code)
        self.assertNotIn("keyStart + keyOff >=", sfo_code)
        self.assertNotIn("dataStart + dataOff + len >", sfo_code)


    def test_sfo_writer_bounds_string_entries_to_field_capacity(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        self.assertIn("static SfoEnt sfo_str_entry", text)
        self.assertIn("slen >= (size_t)maxlen", text)
        self.assertIn("slen = (size_t)maxlen - 1u", text)
        self.assertIn("if (e[i].len > e[i].maxlen) return;", text)

    def test_sfo_writer_copies_payload_and_terminates_inside_the_slot(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        sfo_idx = text.find("static void sfo_write")
        self.assertNotEqual(sfo_idx, -1)
        sfo_code = text[sfo_idx:text.find("/* ---- ScePspDateTime", sfo_idx)]
        self.assertIn("e[i].fmt == 0x0204", sfo_code)
        self.assertIn("payload = e[i].len > 0 ? e[i].len - 1u : 0u", sfo_code)
        self.assertIn("if (e[i].maxlen > 0) {", sfo_code)
        self.assertIn(r"buf[dataStart + dataOff[i] + payload] = '\0';", sfo_code)

    def test_do_save_local_buffers_match_sfo_field_capacities(self):
        # rd_cstr already reserves the terminator (it fills at most max-1 bytes),
        # so a buffer of exactly the field capacity yields the largest LEGAL
        # payload (maxlen - 1) and nothing more.  The old 129/1025 buffers could
        # only ever produce an over-capacity payload.
        text = SAVEDATA_C.read_text(encoding="utf-8")
        do_save_idx = text.find("static uint32_t do_save")
        self.assertNotEqual(do_save_idx, -1)
        do_save_code = text[do_save_idx:text.find("static int dir_exists", do_save_idx)]
        self.assertIn("title[128]", do_save_code)
        self.assertIn("saveTitle[128]", do_save_code)
        self.assertIn("detail[1024]", do_save_code)
        self.assertNotIn("title[129]", do_save_code)
        self.assertNotIn("saveTitle[129]", do_save_code)
        self.assertNotIn("detail[1025]", do_save_code)


# ---------------------------------------------------------------------------
# Executable evidence over the REAL production implementation.
#
# The harness below contains no copy of sfo_write.  It #includes the actual
# src/rt/savedata.c and drives the actual sr_savedata_execute(SD_SAVE) entry
# point with guest bytes, so the chain under test is the production one:
#
#   guest memory -> MEM_R8 -> rd_cstr -> SfoEnt.len -> dataSize -> calloc
#                -> memcpy / terminator -> emitted PARAM.SFO
#
# The only substitution is the allocator that sfo_write's own calloc() resolves
# to, so a write past the end of that exact allocation lands in a checked red
# zone instead of the host heap.  The mutation test rewrites the production
# source text and rebuilds, so a surviving mutant is a real hole in these
# assertions rather than a bookkeeping detail.
# ---------------------------------------------------------------------------

SFO_HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "recomp.h"
uint8_t *g_mem;
CpuState *s_cpu;
int g_sr_heap_watch, g_hle_depth;
void sr_oor(uint32_t a, uint32_t v, int s) { (void)a; (void)v; (void)s; }
void sr_heap_note_write(uint32_t a, uint32_t w, uint32_t v, uint32_t p)
    { (void)a; (void)w; (void)v; (void)p; }
void sr_heap_note_bulk_write(uint32_t a, uint32_t w, uint32_t p)
    { (void)a; (void)w; (void)p; }
uint32_t sched_current_uid(void) { return 0; }
uint32_t sr_get_ge_status(void) { return 0; }

#define RZ 64
#define RZ_FILL 0xAA
static uint8_t *g_rz_user, *g_rz_raw; static size_t g_rz_bytes;
static uint8_t *g_sfo; static size_t g_sfo_n; static int g_tail_dirty, g_allocs;

static void *rz_calloc(size_t num, size_t size) {
    size_t total = num * size;
    uint8_t *raw = (uint8_t *)malloc(total + 2 * RZ);
    if (!raw) return NULL;
    memset(raw, RZ_FILL, total + 2 * RZ);
    memset(raw + RZ, 0, total);
    g_rz_raw = raw; g_rz_user = raw + RZ; g_rz_bytes = total; g_allocs++;
    return g_rz_user;
}
static void rz_free(void *p) {
    if (p && p == g_rz_user) {
        for (size_t k = 0; k < RZ; k++)
            if (g_rz_user[g_rz_bytes + k] != RZ_FILL) { g_tail_dirty = 1; break; }
        free(g_sfo);
        g_sfo = (uint8_t *)malloc(g_rz_bytes ? g_rz_bytes : 1);
        if (g_sfo) memcpy(g_sfo, g_rz_user, g_rz_bytes);
        g_sfo_n = g_rz_bytes;
        free(g_rz_raw); g_rz_raw = NULL; g_rz_user = NULL;
        return;
    }
    free(p);
}
#define calloc(n, s) rz_calloc((n), (s))
#define free(p)      rz_free((p))

#include "savedata.c"

#undef calloc
#undef free

#define PARAM 0x08800000u
#define ARENA 0x0c000000u

static void gset(uint32_t a, const char *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) sr_w8(a + i, (uint8_t)s[i]);
}
static void gfill(uint32_t a, uint8_t v, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) sr_w8(a + i, v);
}

static int sfo_entry(const uint8_t *b, size_t n, const char *want,
                     uint32_t *len, uint32_t *maxlen, const uint8_t **val) {
    if (!b || n < 20) return 0;
    uint32_t keyStart, dataStart, cnt, dataOff;
    memcpy(&keyStart, b + 8, 4); memcpy(&dataStart, b + 12, 4); memcpy(&cnt, b + 16, 4);
    for (uint32_t i = 0; i < cnt && (size_t)(20u + 16u * i + 16u) <= n; i++) {
        const uint8_t *ix = b + 20 + 16 * i;
        uint16_t ko; memcpy(&ko, ix, 2);
        if ((size_t)keyStart + ko >= n) continue;
        if (strcmp((const char *)(b + keyStart + ko), want)) continue;
        memcpy(len, ix + 4, 4); memcpy(maxlen, ix + 8, 4); memcpy(&dataOff, ix + 12, 4);
        *val = b + dataStart + dataOff;
        return 1;
    }
    return 0;
}

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); \
                                       printf("\n"); } } while (0)

/* One production SD_SAVE.  `off` names the guest SFO field to overfill. */
static void run_save(uint32_t off, uint32_t cap, uint8_t fill, uint32_t fill_len) {
    memset(g_mem - 0x08000000u, 0, ARENA);
    gfill(PARAM, 0, 0x600);
    sr_w32(PARAM + SDP_mode, SD_SAVE);
    gset(PARAM + SDP_gameName, "ULUS99999", 10);
    gset(PARAM + SDP_saveName, "SLOT00", 7);
    gset(PARAM + SDP_sfoTitle, "T", 2);
    gset(PARAM + SDP_sfoSaveTitle, "S", 2);
    gset(PARAM + SDP_sfoDetail, "D", 2);
    gfill(PARAM + off, fill, fill_len);
    if (fill_len < cap) sr_w8(PARAM + off + fill_len, 0);
    g_tail_dirty = 0; g_allocs = 0;
    (void)sr_savedata_execute(PARAM);
}

static void check_field(const char *key, uint32_t want_maxlen) {
    uint32_t len = 0, maxlen = 0; const uint8_t *v = NULL;
    if (!sfo_entry(g_sfo, g_sfo_n, key, &len, &maxlen, &v)) {
        g_fail++; printf("FAIL: emitted SFO carries no %s entry\n", key); return;
    }
    CHECK(maxlen == want_maxlen, "%s maxlen=%u expected %u", key, maxlen, want_maxlen);
    CHECK(len <= maxlen, "%s len=%u must not exceed maxlen=%u", key, len, maxlen);
    CHECK(len >= 1 && v[len - 1] == 0, "%s must be NUL terminated inside its slot", key);
}

int main(void) {
    uint8_t *arena = (uint8_t *)malloc(ARENA);
    if (!arena) return 2;
    memset(arena, 0, ARENA);
    g_mem = arena + 0x08000000u;
    putenv((char *)"SR_MEMSTICK=./sfo_selftest_ms");

    /* 1. Each fixed field driven to exactly its capacity in non-NUL guest bytes.
     *    Before the fix this made SfoEnt.len exceed maxlen and the emission
     *    memcpy ran one byte past the slot -- past the whole allocation for the
     *    last entry, TITLE. */
    run_save(SDP_sfoTitle, 128, 'A', 128);
    CHECK(g_allocs == 1, "production sfo_write must have allocated its buffer");
    CHECK(!g_tail_dirty, "128-byte TITLE must not write past the sfo_write allocation");
    check_field("TITLE", 128);

    run_save(SDP_sfoSaveTitle, 128, 'S', 128);
    CHECK(!g_tail_dirty, "128-byte SAVEDATA_TITLE must not write past the allocation");
    check_field("SAVEDATA_TITLE", 128);
    {   /* the following slot must hold its own value, never a neighbour's spill */
        uint32_t l = 0, m = 0; const uint8_t *v = NULL;
        if (sfo_entry(g_sfo, g_sfo_n, "TITLE", &l, &m, &v))
            CHECK(v[0] == 'T', "TITLE slot must survive an over-long SAVEDATA_TITLE (0x%02x)", v[0]);
    }

    run_save(SDP_sfoDetail, 1024, 'D', 1024);
    CHECK(!g_tail_dirty, "1024-byte SAVEDATA_DETAIL must not write past the allocation");
    check_field("SAVEDATA_DETAIL", 1024);
    {
        uint32_t l = 0, m = 0; const uint8_t *v = NULL;
        if (sfo_entry(g_sfo, g_sfo_n, "SAVEDATA_DIRECTORY", &l, &m, &v))
            CHECK(v[0] == 'U', "SAVEDATA_DIRECTORY slot must survive an over-long DETAIL (0x%02x)", v[0]);
    }

    /* 2. The largest LEGAL payload must survive byte for byte: the bound has to
     *    truncate exactly one byte later than it rejects, or the fix silently
     *    costs a legal character. */
    run_save(SDP_sfoTitle, 128, 'B', 127);
    {
        uint32_t len = 0, maxlen = 0; const uint8_t *v = NULL;
        if (sfo_entry(g_sfo, g_sfo_n, "TITLE", &len, &maxlen, &v)) {
            CHECK(len == 128, "127 payload bytes must round-trip as len=128 (got %u)", len);
            int ok = v[127] == 0;
            for (int i = 0; i < 127; i++) if (v[i] != 'B') ok = 0;
            CHECK(ok, "127 TITLE payload bytes must be preserved byte for byte");
        }
        CHECK(!g_tail_dirty, "the maximum legal TITLE must not touch the red zone");
    }
    run_save(SDP_sfoDetail, 1024, 'E', 1023);
    {
        uint32_t len = 0, maxlen = 0; const uint8_t *v = NULL;
        if (sfo_entry(g_sfo, g_sfo_n, "SAVEDATA_DETAIL", &len, &maxlen, &v)) {
            CHECK(len == 1024, "1023 payload bytes must round-trip as len=1024 (got %u)", len);
            int ok = v[1023] == 0;
            for (int i = 0; i < 1023; i++) if (v[i] != 'E') ok = 0;
            CHECK(ok, "1023 DETAIL payload bytes must be preserved byte for byte");
        }
    }

    /* 3. The eight-entry table is fixed, so its envelope is a compile-time
     *    constant: keySize 115, dataSize 4648, dataStart 264, totalAlloc 4912.
     *    Pin it -- the "no keySize/dataSize/dataStart/totalAlloc overflow is
     *    reachable" argument holds by construction only while that stays true. */
    run_save(SDP_sfoTitle, 128, 'T', 1);
    CHECK(g_sfo_n == 4912, "the fixed SFO envelope must stay 4912 bytes (got %zu)", g_sfo_n);
    CHECK(!g_tail_dirty, "an ordinary save must not touch the red zone");
    check_field("SAVEDATA_DIRECTORY", 64);
    check_field("CATEGORY", 4);

    /* 4. save_rel: the ROOT-RELATIVE path the contained-delete seam walks.
     *    This is the only new savedata.c code on the destructive path, and a
     *    silent defect here would disable DELETE/ERASE rather than announce
     *    itself, so it gets executable coverage and not just a source gate. */
    {
        char rel[256];
        CHECK(save_rel(rel, sizeof(rel), "ULUS00001", "DATA") == 1,
              "an ordinary save must produce a relative path");
        CHECK(strcmp(rel, "PSP/SAVEDATA/ULUS00001DATA") == 0,
              "save_rel produced '%s'", rel);
        /* No host root may leak into a path the seam walks from its own anchor. */
        CHECK(strstr(rel, "memstick") == NULL, "save_rel must not embed the host root");
        CHECK(rel[0] != '/', "save_rel must not produce an absolute path");

        /* The JOINED name is what reaches the host: "NU" and "L" are each a
         * safe component, "NUL" is a device alias. Validating the two guest
         * strings separately would let it through. */
        CHECK(save_rel(rel, sizeof(rel), "NU", "L") == 0,
              "a joined device alias must be refused");
        CHECK(save_rel(rel, sizeof(rel), "CO", "N") == 0,
              "a joined device alias must be refused");
        /* ...while a JOINED name that merely contains those letters stays
         * legal, so the added check does not over-reject. (A game string of
         * exactly "CON" is already refused one layer earlier, by the
         * per-component path_sanitize that has always run.) */
        CHECK(save_rel(rel, sizeof(rel), "CONS", "OLE") == 1,
              "CONSOLE is an ordinary save directory name");
        CHECK(strcmp(rel, "PSP/SAVEDATA/CONSOLE") == 0, "save_rel produced '%s'", rel);
        CHECK(save_rel(rel, sizeof(rel), "CON", "SOLE") == 0,
              "a reserved-device GAME string is refused by the pre-existing component filter");

        CHECK(save_rel(rel, sizeof(rel), "A/B", "C") == 0, "a separator must be refused");
        CHECK(save_rel(rel, sizeof(rel), "..", "X") == 0, "traversal must be refused");
        CHECK(save_rel(rel, sizeof(rel), "ULUS00001", "") == 0, "an empty save must be refused");
        CHECK(save_rel(rel, sizeof(rel), "", "DATA") == 0, "an empty game must be refused");
        CHECK(save_rel(rel, 8, "ULUS00001", "DATA") == 0, "a path that does not fit must be refused");
    }

    printf(g_fail ? "sfo_selftest: %d FAILURE(S)\n" : "sfo_selftest: OK\n", g_fail);
    return g_fail ? 1 : 0;
}
"""


# (find, replace) pairs applied to the real src/rt/savedata.c text.  Every one
# of these changes bytes that ship; the assertions above must kill all of them.
#
# The fix is layered, and the layers are independently sufficient for the
# current call sites: sizing the do_save locals to the field capacity already
# caps rd_cstr at maxlen - 1, so reverting the clamp ALONE cannot overflow, and
# clamping ALONE makes oversized locals harmless.  A single-layer revert
# therefore survives by design.  Each mutant below removes a layer while
# restoring the precondition that layer exists to handle, so every one of them
# is genuinely load bearing.
_OVERSIZED_LOCALS = (
    "char dir[PATH_MAX], fileName[16], title[128], saveTitle[128], detail[1024], saveDir[64];",
    "char dir[PATH_MAX], fileName[16], title[129], saveTitle[129], detail[1025], saveDir[64];",
)
_DROP_CLAMP = (
    "    if (maxlen > 0 && slen >= (size_t)maxlen) {\n"
    "        slen = (size_t)maxlen - 1u;\n"
    "    }\n", "",
)
_DROP_PREFLIGHT = ("        if (e[i].len > e[i].maxlen) return;\n", "")

SFO_MUTANTS = {
    # The whole pre-fix writer: oversized locals, no clamp, no preflight.
    "restore-pre-fix-writer": [_OVERSIZED_LOCALS, _DROP_CLAMP, _DROP_PREFLIGHT],
    # Oversized locals with the preflight still in place: the clamp is what
    # keeps a full-capacity field representable instead of dropping the file.
    "oversize-locals-drop-clamp": [_OVERSIZED_LOCALS, _DROP_CLAMP],
    # Drop the terminator from the accounting: copy len bytes, then terminate
    # after them, so a full slot writes one byte into the next one.
    "terminator-accounting": [
        ("uint32_t payload = e[i].len > 0 ? e[i].len - 1u : 0u;",
         "uint32_t payload = e[i].len;"),
    ],
    # A zero-capacity call site with the terminator guard removed: the slot has
    # no bytes, so the unconditional terminator lands outside it.
    "zero-capacity-unguarded": [
        ('sfo_str_entry("TITLE",              title, 128)',
         'sfo_str_entry("TITLE",              title, 0)'),
        ("            if (e[i].maxlen > 0) {\n"
         "                buf[dataStart + dataOff[i] + payload] = '\\0';\n"
         "            }\n",
         "            buf[dataStart + dataOff[i] + payload] = '\\0';\n"),
    ],
    # Truncate one byte too eagerly: a legal maximum payload must not be lost.
    "over-eager-truncation": [
        ("        slen = (size_t)maxlen - 1u;", "        slen = (size_t)maxlen - 2u;"),
        ("if (maxlen > 0 && slen >= (size_t)maxlen) {",
         "if (maxlen > 1 && slen >= (size_t)maxlen - 1u) {"),
    ],
}


# savedata.c reaches the POSIX contained-delete backend only when the
# translation unit selects a POSIX.1-2008 feature profile. -std=c11 selects
# none, so the flag is supplied here rather than letting the seam's #error stop
# a build that is merely missing a profile.
_POSIX_PROFILE = [] if os.name == "nt" else ["-D_POSIX_C_SOURCE=200809L"]


def _build(tmp, savedata_override=None):
    """Compile the harness against production savedata.c, or a mutated copy."""
    harness = os.path.join(tmp, "sfo_selftest.c")
    with open(harness, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(SFO_HARNESS)
    if savedata_override is not None:
        # A quoted #include resolves next to the including file first, so this
        # copy shadows src/rt/savedata.c for this build only.
        override = os.path.join(tmp, "savedata.c")
        with open(override, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(savedata_override)
    exe = os.path.join(tmp, "sfo_selftest.exe")
    rt = str(ROOT / "src" / "rt")
    build = subprocess.run(
        [CC, "-std=c11", "-O1"] + _POSIX_PROFILE + ["-I", tmp, "-I", rt, "-o", exe, harness,
         os.path.join(rt, "debug.c"), os.path.join(rt, "watchpoints_file.c")],
        capture_output=True, text=True,
    )
    if build.returncode != 0:
        raise AssertionError("sfo selftest harness failed to build:\n" + build.stderr[-4000:])
    return exe


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestSavedataSfoProduction(unittest.TestCase):
    """Executable evidence: the real entry point over the real implementation."""

    def test_production_sfo_write_stays_inside_its_allocation(self):
        with tempfile.TemporaryDirectory(prefix="sfo_prod_") as tmp:
            exe = _build(tmp)
            run = subprocess.run([exe], capture_output=True, text=True, cwd=tmp)
            self.assertEqual(
                run.returncode, 0,
                "production SFO selftest failed:\n" + run.stdout + run.stderr)
            self.assertIn("sfo_selftest: OK", run.stdout)

    def test_every_production_mutant_is_killed(self):
        original = SAVEDATA_C.read_text(encoding="utf-8")
        survivors = []
        for name, edits in SFO_MUTANTS.items():
            mutated = original
            for old, new in edits:
                self.assertIn(old, mutated,
                              "mutant {}: anchor not found: {!r}".format(name, old[:70]))
                mutated = mutated.replace(old, new, 1)
            self.assertNotEqual(mutated, original,
                                "mutant {} changed nothing".format(name))
            with tempfile.TemporaryDirectory(prefix="sfo_mut_") as tmp:
                exe = _build(tmp, savedata_override=mutated)
                run = subprocess.run([exe], capture_output=True, text=True, cwd=tmp)
                if run.returncode == 0:
                    survivors.append(name)
        self.assertEqual(
            survivors, [],
            "production mutants survived the SFO assertions: " + ", ".join(survivors))


class TestWindowsContainmentArchitecture(unittest.TestCase):
    """Source-shape gates for the Windows OPEN -> HANDLE -> FINAL PATH VERIFY ->
    OPERATION architecture (reconstruction of PR #114 plus audit findings
    F114-1..F114-5). These complement the executable junction/hostile fixtures
    in src/rt/vfs_selftest.c: a mutation that reintroduces verify-then-unlink-by-
    name, creates below an unverified mid-path component, or drops the
    component-boundary discipline must fail one of these checks."""

    @staticmethod
    def _windows_branch(code: str) -> str:
        """Return only the #ifdef _WIN32 ... #else segment of a function body."""
        return code.split("#ifdef _WIN32", 1)[1].split("#else", 1)[0]

    def test_do_delete_never_unlinks_by_name_on_windows(self):
        """The Windows guarantee moved, it did not go away.

        do_delete no longer contains a Windows branch at all -- it speaks the
        host-neutral contained-delete seam. The verified-handle disposition it
        used to inline now lives in the seam's Windows backend, so that is where
        F114-1 is pinned."""
        win = _seam_backend("SR_CD_BACKEND_WINDOWS")
        self.assertNotIn("sd_unlink(", win,
                         "the Windows backend must dispose through verified handles (F114-1)")
        self.assertNotIn("_unlink(", win,
                         "the Windows backend must not unlink by name after verification")
        self.assertIn("sr_vfs_delete_contained_leaf(", win,
                      "entries must be deleted through their own verified handle")
        self.assertIn("sr_vfs_dispose_by_handle(", win,
                      "the save directory itself must be removed by handle")
        self.assertIn("FILE_FLAG_OPEN_REPARSE_POINT", win,
                      "the directory disposition must stay pinned to the object, not a link target")
        self.assertIn("sr_vfs_dir_is_contained(", win,
                      "the save directory must be containment-checked before enumeration")

    def test_do_erase_deletes_through_verified_handle(self):
        win = _seam_backend("SR_CD_BACKEND_WINDOWS")
        self.assertIn("sr_vfs_delete_contained_leaf(", win,
                      "ERASE must delete through the handle it verified (F114-1)")
        self.assertNotIn("sd_unlink(", win,
                         "the Windows backend must not fall back to by-name unlink")
        self.assertIn("SR_CD_IS_DIRECTORY", win,
                      "a directory named where a file was required must fail closed")

    def test_mkdirs_resolves_root_before_creating_owned_components(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        mkdirs_idx = text.find("static int mkdirs(const char *path)")
        self.assertNotEqual(mkdirs_idx, -1)
        mkdirs_code = text[mkdirs_idx:text.find("uint32_t sr_savedata_prepare_utility", mkdirs_idx)]
        self.assertIn("ms_canonical_root(", mkdirs_code,
                      "F114-2: canonical root identity must be resolved first")
        self.assertIn("sr_vfs_mkdirs_contained(tail, canonical_root)",
                      mkdirs_code,
                      "F114-2: owned components must be created through the ordered verifier")

    def test_storage_reads_route_through_the_containment_boundary(self):
        text = SAVEDATA_C.read_text(encoding="utf-8")
        for fn_marker, next_marker in (("static int host_write_file", "static int host_read_file"),
                                       ("static int host_read_file", "/* Storage boundary")):
            fn_idx = text.find(fn_marker)
            self.assertNotEqual(fn_idx, -1)
            fn_code = text[fn_idx:text.find(next_marker, fn_idx)]
            win_branch = self._windows_branch(fn_code)
            self.assertIn("sr_vfs_open_contained_utf8(", win_branch,
                          f"{fn_marker} must open through the containment wrapper")
            self.assertNotIn("fopen(", win_branch,
                             f"{fn_marker} must not bypass the verified-handle path on Windows")

    def test_header_pins_boundary_aligned_containment_and_fail_closed_paths(self):
        header = (ROOT / "src" / "rt" / "vfs_path.h").read_text(encoding="utf-8")
        self.assertIn("FINAL PATH VERIFY", header,
                      "the canonical operation order must stay documented at the seam")
        self.assertIn("FILE_DISPOSITION_FLAG_POSIX_SEMANTICS", header,
                      "delete-by-handle must prefer POSIX-semantics disposition (F114-1)")
        self.assertIn("FILE_ATTRIBUTE_REPARSE_POINT", header,
                      "pre-planted links must be rejected before deeper creation (F114-2)")
        self.assertIn("fail closed", header.lower(),
                      "long-path truncation must fail closed, never fall back (F114-3)")
        self.assertIn("documented F114-4 side effect", header,
                      "root creation must remain a single documented side effect (F114-4)")
        self.assertIn('"COM0"', header,
                      "reserved-device edges (COM0/LPT0 non-reserved) must stay pinned (F114-5)")


class TestPortableContainedDeleteArchitecture(unittest.TestCase):
    """Source-shape gates for the host-neutral contained-delete seam.

    The POSIX savedata deletion path used to re-resolve a guest-influenced
    pathname on every step: stat(path) then unlink(path), opendir(dir) then
    rmdir(dir). An actor able to replace an intermediate save-directory
    component redirected the deletion outside the memstick root. These gates
    pin the shape of the repair; src/rt/vfs_contained_selftest.c is the
    executable half, and it demonstrates the old design being redirected on a
    live fixture before showing the new one refusing the same fixture."""

    def test_savedata_destructive_paths_name_no_host_primitive(self):
        """do_delete and do_erase must be host-neutral.

        Not "must not call unlink" -- must not contain a platform conditional
        or a host filesystem call AT ALL. Generic savedata logic asks the seam
        to delete a contained object; which syscall that becomes is the
        backend's business."""
        for fn, end_marker in (("static uint32_t do_delete", "static uint32_t do_erase"),
                               ("static uint32_t do_erase", "/* LIST (11)")):
            code = _savedata_fn(fn, end_marker)
            _assert_absent(self, ("stat", "unlink", "rmdir", "remove", "opendir", "readdir",
                                  "sd_unlink", "sd_rmdir", "DeleteFileA", "DeleteFileW",
                                  "RemoveDirectoryA", "RemoveDirectoryW",
                                  "SetFileInformationByHandle", "CreateFileW"), code, fn)
            self.assertNotIn("#ifdef", code, f"{fn} must not branch on the host")
            self.assertNotIn("#if defined", code, f"{fn} must not branch on the host")
            self.assertNotIn("_WIN32", code, f"{fn} must not branch on the host")

    def test_savedata_defines_no_by_name_delete_primitive(self):
        """Stronger than "do_delete does not call unlink": the file no longer
        DEFINES a by-name delete at all, so the pathname design cannot creep
        back in one call site at a time."""
        text = SAVEDATA_C.read_text(encoding="utf-8")
        for gone in ("#define sd_unlink", "#define sd_rmdir"):
            self.assertNotIn(gone, text,
                             f"{gone} was removed with the pathname delete design")
        _assert_absent(self, ("sd_unlink", "sd_rmdir", "unlink", "rmdir"),
                       text, "savedata.c")

    def test_do_delete_routes_through_the_contained_seam(self):
        code = _savedata_fn("static uint32_t do_delete", "static uint32_t do_erase")
        self.assertIn("sr_cd_root_open(ms_root()", code,
                      "the trusted root must be bound before anything is destroyed")
        self.assertIn("sr_cd_delete_dir_shallow(&root, rel)", code,
                      "DELETE must go through the contained tree-delete entry point")
        self.assertIn("save_rel(", code,
                      "DELETE must pass a ROOT-RELATIVE path, never an absolute pathname")
        self.assertIn("sr_cd_root_close(&root)", code, "the root binding must be released")
        self.assertIn("st == SR_CD_OK ? 0 : ERR_DELETE_NO_DATA", code,
                      "any non-OK seam status must fail the guest call")

    def test_do_erase_routes_through_the_contained_seam(self):
        code = _savedata_fn("static uint32_t do_erase", "/* LIST (11)")
        self.assertIn("sr_cd_root_open(ms_root()", code,
                      "the trusted root must be bound before anything is destroyed")
        self.assertIn("sr_cd_delete_leaf(&root, rel, fileName)", code,
                      "ERASE must go through the contained leaf-delete entry point")
        self.assertIn("save_rel(", code,
                      "ERASE must pass a ROOT-RELATIVE path, never an absolute pathname")
        self.assertIn("st == SR_CD_OK ? 0 : ERR_RW_NO_DATA", code,
                      "any non-OK seam status must fail the guest call")

    def test_posix_backend_is_descriptor_relative_end_to_end(self):
        posix = _seam_backend("SR_CD_BACKEND_POSIX_AT")
        for primitive in ("openat(", "fdopendir(", "unlinkat(", "O_DIRECTORY", "O_NOFOLLOW",
                          "AT_REMOVEDIR", "AT_SYMLINK_NOFOLLOW"):
            self.assertIn(primitive, posix,
                          f"the POSIX backend must anchor with {primitive}")
        # Nothing destructive may be named by pathname. Word boundaries keep
        # fdopendir/unlinkat/AT_REMOVEDIR from tripping their by-name cousins.
        _assert_absent(self, ("unlink", "rmdir", "remove", "opendir", "lstat", "stat"),
                       posix, "the POSIX backend")

    def test_posix_backend_never_probes_type_before_deleting(self):
        """The check/use race is removed by ORDER, not by a better check.

        The old code asked stat() what the leaf was and then unlinked the name;
        an actor who changed the answer in between won. The repair does not use
        a better probe -- it stops probing. unlinkat() without AT_REMOVEDIR
        cannot remove a directory, so the kernel enforces the file/directory
        distinction at the moment of deletion. The single fstatat in the backend
        runs only inside the failure handler, after the kernel has ALREADY
        refused, and can therefore never select a victim.

        (Textual position proves nothing here -- the handler is defined above
        its callers. What is asserted is that the probe lives in the handler and
        that every use of the handler directly follows a refused unlinkat.)"""
        posix = _seam_backend("SR_CD_BACKEND_POSIX_AT")
        self.assertEqual(posix.count("fstatat("), 1,
                         "exactly one fstatat may exist, and only for diagnosis")
        self.assertEqual(posix.count("S_ISDIR"), 1,
                         "no destructive step may be gated on a directory-type probe")

        handler_start = posix.index("static inline sr_cd_status sr_cd__at_unlink_fail")
        handler_end = posix.index("\n}\n", handler_start)
        handler = posix[handler_start:handler_end]
        self.assertIn("fstatat(", handler, "the only fstatat must be the post-refusal diagnosis")
        self.assertIn("S_ISDIR", handler, "the only type test must be the post-refusal diagnosis")
        self.assertNotIn("unlinkat(", handler,
                         "the diagnosis handler must not itself delete anything")

        after = posix[handler_end:]
        calls = [m.start() for m in re.finditer(r"sr_cd__at_unlink_fail\(", after)]
        self.assertEqual(len(calls), 2,
                         "both destructive entry points must diagnose the same way")
        for at in calls:
            window = after[max(0, at - 240):at]
            self.assertIn("unlinkat(", window,
                          "the diagnosis must directly follow the deletion the kernel refused")
            # ...and that unlinkat's result must be tested, so the diagnosis can
            # only be reached on the branch where the kernel already refused.
            self.assertIsNotNone(
                re.search(r"unlinkat\([^;]*\)\s*(!=|==)\s*0", window),
                "the diagnosis must run only on the failure branch of the deletion")

    def test_seam_has_no_unsafe_fallback_for_an_unsupported_host(self):
        header = SEAM_H.read_text(encoding="utf-8")
        fallback = _seam_backend("SR_CD_BACKEND_NONE")
        self.assertIn("SR_CD_UNSUPPORTED_HOST", fallback)
        _assert_absent(self, ("unlink", "unlinkat", "rmdir", "remove", "openat",
                              "DeleteFileA", "DeleteFileW", "RemoveDirectoryA"),
                       fallback, "the unsupported backend")
        # Losing containment must be a decision, not an accident.
        self.assertIn("SR_CD_ALLOW_UNSUPPORTED_HOST", header,
                      "an unsupported host must be opted into explicitly")
        self.assertIn("#error", header,
                      "a build with no backend must stop rather than silently degrade")

    def test_seam_is_not_windows_shaped(self):
        """The generic half of the seam must name no host primitive: a future
        desktop/mobile/handheld backend has to be addable without touching it."""
        generic = _seam_generic()
        _assert_absent(self, ("CreateFileW", "openat", "unlinkat", "fdopendir", "unlink",
                              "SetFileInformationByHandle", "GetFinalPathNameByHandleW"),
                       generic, "the generic contract")
        for entry in ("sr_cd_root_open", "sr_cd_root_close", "sr_cd_delete_leaf",
                      "sr_cd_delete_dir_shallow"):
            self.assertIn(entry, generic, f"{entry} must be declared host-neutrally")

    def test_save_rel_validates_the_joined_component(self):
        """game and save are validated separately elsewhere, but it is the
        CONCATENATION that reaches the host: "NU" + "L" is two safe components
        and one device alias."""
        code = _savedata_fn("static int save_rel", "static int sdlog")
        self.assertIn("sr_vfs_is_safe_component(leaf", code,
                      "the joined save-directory name must be validated as one component")
        self.assertIn('"PSP/SAVEDATA/%s"', code,
                      "save_rel must produce a root-relative path")
        self.assertNotIn("ms_root()", code,
                         "save_rel must not embed the host root in the relative path")



if __name__ == "__main__":
    unittest.main()
