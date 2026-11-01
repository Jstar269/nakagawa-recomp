// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* Fonts: sceFont (libfont) over parsed PGF fonts. Extracted from hle.c on 2026-10-31 (#307). */

#include "hle_internal.h"

/* sceLibFont. The PSP firmware fonts (flash0 font PGFs) are not on the game ISO, so we load the
 * user-supplied PGF fonts from an explicit SR_FONTDIR or from the executable's sibling `font`
 * directory and rasterise glyphs with the PGF reader (src/rt/pgf.c). A missing configured font is
 * reported as a path/configuration error; there is no current-working-directory retry or
 * synthetic fallback that could hide a bad root. */
static PGF *s_pgf_slot[NK_FONT_SLOT_COUNT];
/* Where each slot's font came from: "user-imported", "project", "unreadable" or "none". */
static const char *s_pgf_slot_source[NK_FONT_SLOT_COUNT];
/* A Latin registry index owns its own opened PGF handle even when it falls back to another
 * same-size index or the project font. This keeps sceFontOpen and flash0: on one resolver. */
static PGF *s_pgf_latin_index[NK_FONT_LATIN_INDEX_COUNT];
static Flash0Sources s_pgf_sources;
static atomic_int s_pgf_state;

static PGF *font_open_host_path(const char *path, const char *label, const char *source) {
#ifdef _WIN32
    /* Host paths are UTF-8 (the asset-index convention); Windows opens them wide. */
    wchar_t *wide = NULL;
    PGF *font = NULL;
    if (sr_utf8_to_wide_alloc(path, &wide)) {
        font = pgf_open_w(wide);
        if (!font)
            fprintf(stderr, "font_load: %s %s font could not be opened (%ls)\n", source, label, wide);
        free(wide);
    } else {
        fprintf(stderr, "font_load: %s %s font path is not convertible\n", source, label);
    }
    return font;
#else
    PGF *font = pgf_open(path);
    if (!font)
        fprintf(stderr, "font_load: %s %s font could not be opened (%s)\n", source, label, path);
    return font;
#endif
}

static void font_load(void) {
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&s_pgf_state, &expected, 1,
                                                  memory_order_acq_rel, memory_order_acquire)) {
        while (atomic_load_explicit(&s_pgf_state, memory_order_acquire) == 1) { }
        return;
    }
    /* SR_FONTDIR is read once here, and a bad value is named once; the result is the project
     * root the flash0: device also serves from, so both font paths read the same directories. */
    wchar_t *root = NULL;
    wchar_t *configured = NULL;
    int configured_present = 0;
    int env_ok = sr_wide_env_alloc(L"SR_FONTDIR", &configured, &configured_present);
    if (!env_ok) {
        fprintf(stderr, "font_load: SR_FONTDIR could not be read\n");
    } else if (configured_present) {
        if (!configured[0] || !sr_wide_configured_root_wide_alloc(configured, &root))
            fprintf(stderr, "font_load: SR_FONTDIR is configured but is not a valid absolute path\n");
    } else if (!sr_wide_module_font_root(&root)) {
        fprintf(stderr, "font_load: no SR_FONTDIR and executable font root could not be resolved\n");
    }
    free(configured);
    Flash0Sources sources;
    memset(&sources, 0, sizeof(sources));
    flash0_user_data_root(sources.user_data_dir, sizeof(sources.user_data_dir));
    sources.project_dir[0] = '\0';
    char *root_utf8 = NULL;
    if (root && sr_wide_to_utf8_alloc(root, &root_utf8))
        flash0_copy_root(sources.project_dir, sizeof(sources.project_dir), root_utf8);
    free(root_utf8);
    free(root);
    s_pgf_sources = sources;
    /* Japanese, the legacy unindexed Latin alias, and Korean keep the slot resolver. The
     * registry's sixteen Latin files resolve separately below, through the same indexed route
     * the flash0: device uses. */
    for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
        char path[SR_FLASH0_PATH_MAX + 64];
        const char *from = "none";
        PGF *pgf = NULL;
        if (sr_flash0_font_resolve_path(&s_pgf_sources, (NkFontSlot)slot, path, sizeof(path), &from)) {
            pgf = font_open_host_path(path, sr_flash0_font_slot_label((NkFontSlot)slot), from);
            if (!pgf) from = "unreadable";
        } else {
            fprintf(stderr,
                    "font_load: no %s font in the user-imported cache or the project font directory\n",
                    sr_flash0_font_slot_label((NkFontSlot)slot));
        }
        s_pgf_slot[slot] = pgf;
        s_pgf_slot_source[slot] = from;
    }
    for (int index = 0; index < NK_FONT_LATIN_INDEX_COUNT; index++) {
        char path[SR_FLASH0_PATH_MAX + 64];
        char label[64];
        const char *from = "none";
        int served_index = -3;
        PGF *pgf = NULL;
        snprintf(label, sizeof(label), "Latin registry index %d", index + 1);
        if (sr_flash0_font_resolve_latin_index_path(&s_pgf_sources, index, path, sizeof(path),
                                                    &from, &served_index)) {
            pgf = font_open_host_path(path, label, from);
            if (!pgf) from = "unreadable";
        } else {
            fprintf(stderr, "font_load: no source for Latin registry index %d (%s)\n",
                    index + 1, nk_font_latin_index_filename(index));
        }
        s_pgf_latin_index[index] = pgf;
    }
    if (getenv("SR_FONTLOG"))
        fprintf(stderr, "font_load: japanese=%s legacy-latin=%s korean=%s\n",
                s_pgf_slot_source[NK_FONT_SLOT_JAPANESE], s_pgf_slot_source[NK_FONT_SLOT_LATIN],
                s_pgf_slot_source[NK_FONT_SLOT_KOREAN]);
    atomic_store_explicit(&s_pgf_state, 2, memory_order_release);
}
#ifdef SR_HLE_THREAD_SELFTEST
/* Test seam: drop the loaded fonts so the next font_load resolves them again, and name the
 * source a slot came from ("user-imported", "project", "unreadable" or "none"). */
void sr_font_test_reset(void) {
    for (int slot = 0; slot < NK_FONT_SLOT_COUNT; slot++) {
        if (s_pgf_slot[slot]) pgf_close(s_pgf_slot[slot]);
        s_pgf_slot[slot] = NULL;
        s_pgf_slot_source[slot] = NULL;
    }
    for (int index = 0; index < NK_FONT_LATIN_INDEX_COUNT; index++) {
        if (s_pgf_latin_index[index]) pgf_close(s_pgf_latin_index[index]);
        s_pgf_latin_index[index] = NULL;
    }
    memset(&s_pgf_sources, 0, sizeof(s_pgf_sources));
    atomic_store_explicit(&s_pgf_state, 0, memory_order_release);
}
const char *sr_font_test_slot_source(int slot) {
    font_load();
    if (slot < 0 || slot >= NK_FONT_SLOT_COUNT || !s_pgf_slot_source[slot]) return "none";
    return s_pgf_slot_source[slot];
}
#endif

/* sceFont library layer (PPSSPP model): the FontLibrary and Font handles are real structs in
 * game-owned memory, carved from the caller's allocator so the game's Fpl accounting stays
 * consistent. The internal bookkeeping lives host-side (mirrors PPSSPP's FontLib/LoadedFont);
 * the guest struct just carries a magic + the identity fields, and the handle IS the guest ptr. */
#define SR_FONTLIB_MAGIC 0x464C4942u   /* 'FLIB' */
#define SR_FONT_MAGIC    0x464F4E54u   /* 'FONT' */
#define SR_FONT_SIZE     0x20u
#define SR_FONT_ERROR_OOM          0x80460001u
#define SR_FONT_ERROR_INVALID_LIB  0x80460002u
#define SR_FONT_ERROR_INVALID_ARG  0x80460003u
#define SR_FONT_ERROR_TOO_MANY     0x80460009u
#define SR_FONT_ERROR_INVALID_DATA 0x8046000au
/* A new library's fallback character: 0x005F ('_'), pspautotests tests/font/altcharcode.expected
 * "Initial value: 005f" (rev 2ed7c9c5, recorded on a PSP). */
#define SR_FONT_DEFAULT_ALT_CHAR   0x005fu
/* A new library's resolution: 128 x 128 (pspautotests tests/font/newlib.expected and
 * resolution.expected at f573fe8c, recorded on a PSP). */
#define SR_FONT_DEFAULT_RESOLUTION 128.0f
/* sceFontNewLib serves at most 9 fonts: a larger count, a negative one included (the field is
 * read unsigned), gets a 9-font library, and 0 is accepted (pspautotests tests/font/newlib.expected
 * at f573fe8c: -1, 9, 10 and 100 allocate the same total, 0 allocates the least). */
#define SR_FONT_MAX_FONTS          9u
/* An open font answers at most this many sceFontOpen calls before it is closed; the next is
 * TOO_MANY (pspautotests tests/font/open.expected at f573fe8c: four opens of index 0 in a
 * 4-font library, then 0x80460009). Whether the limit is the library's font count or a
 * per-font reference limit is not separated by that test; PPSSPP 7fab97c0 uses 4 per font. */
#define SR_FONT_MAX_OPENS          4u

/* sceFont uses the same measured index map and served-header values as /DATA/FONT. */

typedef struct {
    int used;
    uint32_t handle;      /* guest pointer to the FontLibrary struct */
    uint32_t allocFn, freeFn, userData;
    int numFonts;
    int openCount;
    int pending;
    uint32_t params[11];  /* complete FontNewLibParams (0x2c), retained for callbacks */
    uint32_t allocSize;
    uint16_t altCharCode; /* the fallback character for a missing glyph (sceFontSetAltCharacterCode) */
    float hRes, vRes;     /* sceFontSetResolution; a new library has 128 x 128 */
} SrFontLib;
typedef struct {
    int used;
    int closing;
    unsigned refs;
    uint32_t handle;      /* guest pointer to the Font struct */
    uint32_t lib;         /* owning FontLibrary handle */
    const PGF *pgf;       /* PGF backend this font resolves against */
    PGF *ownedPgf;        /* non-NULL for sceFontOpenUserMemory */
    int index;
    unsigned opens;       /* sceFontOpen calls this handle answers, not yet closed (at most 4) */
    int has_registry_info;
    SrFontRegistryInfo registry_info;
} SrFont;
static SrFontLib s_fontlibs[32];
static SrFont s_fonts[64];

static int fontlib_snapshot(uint32_t handle, SrFontLib *out) {
    int found = 0;
    if (!handle) return 0;
    hle_lock();
    for (size_t i = 0; i < sizeof(s_fontlibs) / sizeof(s_fontlibs[0]); i++)
        if (s_fontlibs[i].used == 1 && s_fontlibs[i].handle == handle) {
            if (out) *out = s_fontlibs[i];
            found = 1; break;
        }
    hle_unlock();
    return found;
}
static const PGF *font_acquire(uint32_t handle, SrFont **record) {
    const PGF *pgf = NULL;
    if (record) *record = NULL;
    if (!handle) return NULL;
    hle_lock();
    for (size_t i = 0; i < sizeof(s_fonts) / sizeof(s_fonts[0]); i++)
        if (s_fonts[i].used == 1 && !s_fonts[i].closing && s_fonts[i].handle == handle) {
            s_fonts[i].refs++;
            pgf = s_fonts[i].pgf;
            if (record) *record = &s_fonts[i];
            break;
        }
    hle_unlock();
    return pgf;
}
static void font_release(SrFont *f) {
    PGF *destroy = NULL;
    if (!f) return;
    hle_lock();
    if (f->refs) f->refs--;
    if (f->closing && f->refs == 0 && f->ownedPgf) {
        destroy = f->ownedPgf; f->ownedPgf = NULL; f->pgf = NULL;
    }
    if (f->closing && f->refs == 0 && !f->ownedPgf)
        memset(f, 0, sizeof(*f));
    hle_unlock();
    if (destroy) pgf_close(destroy);
}
/* Resolve the PGF for a character through its opened handle. Handle identity is preserved: a
 * missing glyph is reported by pgf.c rather than silently switching to another registry font. */
static const PGF *font_pgf_for(uint32_t handle, int cc, SrFont **record) {
    const PGF *p = font_acquire(handle, record);
    (void)cc;
    return p; /* An opened handle is permanently bound to its PGF; fallback stays inside pgf.c. */
}

/* Allocate size bytes of guest memory for a font handle. When the lib was created with a real
 * allocFunc, call it (PSP ABI) so the block comes from the game's Fpl pool; otherwise fall back
 * to the private bump heap. */
static uint32_t font_call_alloc(CpuState *s, uint32_t allocFn, uint32_t userData, uint32_t size) {
    if (allocFn) return ge_call_guest_rv(s, allocFn, userData, size, 0);
    return 0;
}
/* A guest allocator result is usable only when its whole span is guest-writable and it overlaps
 * no live or reserved library or font object; a duplicate or overlapping return would alias one
 * handle's state onto another's. Rejected blocks are not passed to freeFunc: an out-of-range
 * block is not the guest's, and an overlapping one belongs to a live object. Host policy: what
 * the PSP's libfont does with such a block is UNMEASURED, so this fails closed. Holds hle_lock. */
static int font_alloc_span_free_locked(uint32_t addr, uint32_t size) {
    if (!addr || !sr_guest_span_writable(addr, size)) return 0;
    uint64_t lo = addr, hi = (uint64_t)addr + size;
    for (size_t i = 0; i < sizeof(s_fontlibs) / sizeof(s_fontlibs[0]); i++) {
        const SrFontLib *L = &s_fontlibs[i];
        if (L->used && L->handle && lo < (uint64_t)L->handle + L->allocSize && L->handle < hi)
            return 0;
    }
    for (size_t i = 0; i < sizeof(s_fonts) / sizeof(s_fonts[0]); i++) {
        const SrFont *f = &s_fonts[i];
        if (f->used && f->handle && lo < (uint64_t)f->handle + SR_FONT_SIZE && f->handle < hi)
            return 0;
    }
    return 1;
}
static void font_zero(uint32_t addr, uint32_t size) {
    if (!addr) return;
    for (uint32_t i = 0; i < size; i += 4) MEM_W32(addr + i, 0u);
}

static uint32_t font_lib_alloc_size(int numFonts) {
    /* PPSSPP's native FontLib layout: header + per-open slots + per-user-font slots +
     * internal-font style records. Keep arithmetic checked before calling guest code. */
    uint64_t n = (uint32_t)numFonts;
    uint64_t size = 0x4cull + n * 0x4cull + n * 0x230ull + 3ull * 0xa8ull;
    return size <= 0x7fffffffull ? (uint32_t)size : 0;
}

static SrFontRegistryStatus font_registry_info_for_index(uint32_t index, SrFontRegistryInfo *info) {
    font_load();
    return sr_flash0_font_registry_info(&s_pgf_sources, index, info);
}

static const PGF *font_pgf_for_index(uint32_t index) {
    NkFontSlot slot;
    font_load();
    if (!sr_flash0_font_registry_slot(index, &slot) || (unsigned)slot >= NK_FONT_SLOT_COUNT)
        return NULL;
    if (slot == NK_FONT_SLOT_LATIN) {
        if (index == 0u || index > NK_FONT_LATIN_INDEX_COUNT) return NULL;
        return s_pgf_latin_index[index - 1u];
    }
    return s_pgf_slot[slot];
}

/* One 0xA8-byte PGFFontStyle at `st` from a registry index's values: the shape FontInfo embeds
 * at +0x5c and sceFontGetFontList writes one of per font. */
static void font_write_style(uint32_t st, const SrFontRegistryInfo *info);

static void font_overlay_style(uint32_t fi, const SrFontRegistryInfo *info, int userMemory) {
    if (!fi) return;
    /* PGFFontStyle begins at +0x5c. For user-memory PGFs, intrinsic metrics remain useful but
     * registry identity is deliberately unknown. */
    if (!info || userMemory) {
        for (uint32_t i = 0; i < 0x90; i++) MEM_W8(fi + 0x70u + i, 0);
        return;
    }
    font_write_style(fi + 0x5cu, info);
}

static void font_write_style(uint32_t st, const SrFontRegistryInfo *info) {
    const uint32_t fi = st - 0x5cu; /* the offsets below are FontInfo's */
    float f[4] = {
        (float)info->h_size / 64.0f, (float)info->v_size / 64.0f,
        (float)info->h_resolution / 64.0f, (float)info->v_resolution / 64.0f,
    };
    for (int i = 0; i < 4; i++) {
        uint32_t w;
        memcpy(&w, &f[i], sizeof(w));
        MEM_W32(fi + 0x5cu + (uint32_t)i * 4u, w);
    }
    MEM_W32(fi + 0x6c, info->weight);
    MEM_W16(fi + 0x70, (uint16_t)info->family_code);
    MEM_W16(fi + 0x72, (uint16_t)info->style);
    MEM_W16(fi + 0x74, (uint16_t)info->sub_style);
    MEM_W16(fi + 0x76, (uint16_t)info->language_code);
    MEM_W16(fi + 0x78, (uint16_t)info->region_code);
    MEM_W16(fi + 0x7a, (uint16_t)info->country_code);
    for (uint32_t i = 0; i < 64; i++) MEM_W8(fi + 0x7c + i, 0);
    for (uint32_t i = 0; info->font_name[i] && i < 63; i++)
        MEM_W8(fi + 0x7c + i, (uint8_t)info->font_name[i]);
    for (uint32_t i = 0; i < 64; i++) MEM_W8(fi + 0xbc + i, 0);
    const char *file_name = info->file_name ? info->file_name : "";
    for (uint32_t i = 0; file_name[i] && i < 63; i++)
        MEM_W8(fi + 0xbc + i, (uint8_t)file_name[i]);
    MEM_W32(fi + 0xfc, info->extra_attributes);
    MEM_W32(fi + 0x100, info->expire_date);
}

static void font_write_info(uint32_t addr, const PGF *pgf,
                            const SrFontRegistryInfo *info, int userMemory) {
    for (uint32_t i = 0; i < 0x108u; i++) MEM_W8(addr + i, 0u);
    pgf_get_font_info(pgf, addr);
    font_overlay_style(addr, info, userMemory);
}

static uint32_t h_FontNewLib(CpuState *s) {
    /* sceFontNewLib(a0=FontNewLibParams*, a1=u32* errorCode). PPSSPP param layout:
     * +0 userData, +4 numFonts, +8 cacheData, +0xc allocFunc, +0x10 freeFunc, ... */
    uint32_t params = A0, errPtr = A1;
    uint32_t userData = 0, allocFn = 0, freeFn = 0, requestedFonts = 0;
    uint32_t paramsCopy[11] = {0};
    if (params && !sr_guest_span_readable(params, (uint32_t)sizeof(paramsCopy))) {
        /* The whole 0x2c-byte FontNewLibParams block is retained, so all of it must be guest
         * memory before any word is read or the allocator is called. */
        if (errPtr) MEM_W32(errPtr, SR_FONT_ERROR_INVALID_ARG);
        return 0;
    }
    if (params) {
        for (int i = 0; i < 11; i++) paramsCopy[i] = MEM_R32(params + (uint32_t)i * 4);
        userData = MEM_R32(params + 0x00);
        requestedFonts = MEM_R32(params + 0x04);
        allocFn  = MEM_R32(params + 0x0c);
        freeFn   = MEM_R32(params + 0x10);
    }
    if (!params || !allocFn || !freeFn) {
        if (errPtr) MEM_W32(errPtr, SR_FONT_ERROR_INVALID_ARG);
        return 0;
    }
    /* Measured clamp, not a refusal: see SR_FONT_MAX_FONTS. */
    int numFonts = (int)(requestedFonts > SR_FONT_MAX_FONTS ? SR_FONT_MAX_FONTS : requestedFonts);
    uint32_t allocSize = font_lib_alloc_size(numFonts);
    if (!allocSize) { if (errPtr) MEM_W32(errPtr, SR_FONT_ERROR_OOM); return 0; }
    font_load();
    int slot = -1;
    hle_lock();
    for (size_t i = 0; i < sizeof(s_fontlibs) / sizeof(s_fontlibs[0]); i++)
        if (!s_fontlibs[i].used) { s_fontlibs[i].used = -1; slot = (int)i; break; }
    hle_unlock();
    if (slot < 0) { if (errPtr) MEM_W32(errPtr, SR_FONT_ERROR_TOO_MANY); return 0; }
    uint32_t lib = font_call_alloc(s, allocFn, userData, allocSize);
    /* Validate the allocator's block and reserve its span before writing to it or publishing. */
    hle_lock();
    int lib_ok = font_alloc_span_free_locked(lib, allocSize);
    if (lib_ok) { s_fontlibs[slot].handle = lib; s_fontlibs[slot].allocSize = allocSize; }
    else memset(&s_fontlibs[slot], 0, sizeof(s_fontlibs[slot]));
    hle_unlock();
    if (!lib_ok) lib = 0;
    if (lib) {
        font_zero(lib, allocSize);
        MEM_W32(lib + 0x00, SR_FONTLIB_MAGIC);
        MEM_W32(lib + 0x04, (uint32_t)numFonts);
        MEM_W32(lib + 0x08, userData);
        MEM_W32(lib + 0x0c, allocFn);
        MEM_W32(lib + 0x10, freeFn);
        hle_lock();
        s_fontlibs[slot].used = 1;
        s_fontlibs[slot].allocFn = allocFn; s_fontlibs[slot].freeFn = freeFn;
        s_fontlibs[slot].userData = userData; s_fontlibs[slot].numFonts = numFonts;
        s_fontlibs[slot].openCount = 0;
        s_fontlibs[slot].altCharCode = SR_FONT_DEFAULT_ALT_CHAR;
        s_fontlibs[slot].hRes = SR_FONT_DEFAULT_RESOLUTION;
        s_fontlibs[slot].vRes = SR_FONT_DEFAULT_RESOLUTION;
        memcpy(s_fontlibs[slot].params, paramsCopy, sizeof(paramsCopy));
        hle_unlock();
    }
    if (errPtr) MEM_W32(errPtr, lib ? 0 : SR_FONT_ERROR_OOM);
    if (getenv("SR_FONTLOG"))
        fprintf(stderr, "h_FontNewLib: params=0x%08x numFonts=%d alloc=0x%08x free=0x%08x -> lib=0x%08x\n",
                params, numFonts, allocFn, freeFn, lib);
    return lib;
}
/* Shared Font-handle constructor for Open / OpenUserMemory. */
static uint32_t font_open_common(CpuState *s, uint32_t lib, uint32_t index, uint32_t mode,
                                 const PGF *pgf, PGF *ownedPgf,
                                 const SrFontRegistryInfo *registry_info,
                                 uint32_t errPtr) {
    SrFontLib L;
    if (!fontlib_snapshot(lib, &L)) {
        if (errPtr) MEM_W32(errPtr, SR_FONT_ERROR_INVALID_LIB);
        if (ownedPgf) pgf_close(ownedPgf);
        return 0;
    }
    if (!pgf) {
        if (errPtr) MEM_W32(errPtr, SR_FONT_ERROR_INVALID_DATA);
        if (ownedPgf) pgf_close(ownedPgf);
        return 0;
    }
    /* A system font this library already has open answers the same handle again, with no new
     * allocation, until it has answered SR_FONT_MAX_OPENS opens (pspautotests tests/font/open
     * at f573fe8c: a second open of index 0 returns the first one's handle, which stays usable
     * after one of the two is closed, and a further open allocates nothing). User fonts are new
     * fonts every time. */
    if (!ownedPgf && registry_info) {
        uint32_t reopened = 0;
        int full = 0;
        hle_lock();
        for (size_t i = 0; i < sizeof(s_fonts) / sizeof(s_fonts[0]); i++) {
            SrFont *f = &s_fonts[i];
            if (f->used == 1 && !f->closing && f->lib == lib && !f->ownedPgf &&
                f->has_registry_info && f->index == (int)index) {
                if (f->opens >= SR_FONT_MAX_OPENS) full = 1;
                else { f->opens++; reopened = f->handle; }
                break;
            }
        }
        hle_unlock();
        if (reopened || full) {
            if (errPtr) MEM_W32(errPtr, reopened ? 0u : SR_FONT_ERROR_TOO_MANY);
            return reopened;
        }
    }
    int slot = -1, libslot = -1;
    hle_lock();
    for (size_t i = 0; i < sizeof(s_fontlibs) / sizeof(s_fontlibs[0]); i++)
        if (s_fontlibs[i].used == 1 && s_fontlibs[i].handle == lib &&
            s_fontlibs[i].openCount + s_fontlibs[i].pending < s_fontlibs[i].numFonts) { libslot = (int)i; break; }
    for (size_t i = 0; i < sizeof(s_fonts) / sizeof(s_fonts[0]); i++) {
        if (!s_fonts[i].used && !s_fonts[i].closing && s_fonts[i].refs == 0 && !s_fonts[i].ownedPgf) {
            s_fonts[i].used = -1; slot = (int)i; break;
        }
    }
    if (slot >= 0 && libslot >= 0) s_fontlibs[libslot].pending++;
    else if (slot >= 0) memset(&s_fonts[slot], 0, sizeof(s_fonts[slot]));
    hle_unlock();
    if (slot < 0 || libslot < 0) {
        if (errPtr) MEM_W32(errPtr, SR_FONT_ERROR_TOO_MANY);
        if (ownedPgf) pgf_close(ownedPgf);
        return 0;
    }
    uint32_t allocFn = L.allocFn, userData = L.userData;
    uint32_t fh = font_call_alloc(s, allocFn, userData, SR_FONT_SIZE);
    /* Validate the allocator's block and reserve its span before writing to it or publishing. */
    hle_lock();
    if (font_alloc_span_free_locked(fh, SR_FONT_SIZE)) s_fonts[slot].handle = fh;
    else fh = 0;
    hle_unlock();
    if (fh) {
        font_zero(fh, SR_FONT_SIZE);
        MEM_W32(fh + 0x00, SR_FONT_MAGIC);
        MEM_W32(fh + 0x04, lib);
        MEM_W32(fh + 0x08, index);
        MEM_W32(fh + 0x0c, (pgf && pgf == s_pgf_slot[NK_FONT_SLOT_JAPANESE]) ? 1u : 0u);
        MEM_W32(fh + 0x10, mode);
    }
    hle_lock();
    if (fh) {
        s_fonts[slot].used = 1; s_fonts[slot].closing = 0; s_fonts[slot].refs = 0;
        s_fonts[slot].handle = fh; s_fonts[slot].lib = lib;
        s_fonts[slot].pgf = pgf; s_fonts[slot].ownedPgf = ownedPgf;
        s_fonts[slot].index = (int)index;
        s_fonts[slot].opens = 1u;
        s_fonts[slot].has_registry_info = registry_info != NULL;
        if (registry_info) s_fonts[slot].registry_info = *registry_info;
        else memset(&s_fonts[slot].registry_info, 0, sizeof(s_fonts[slot].registry_info));
        s_fontlibs[libslot].pending--; s_fontlibs[libslot].openCount++;
        MEM_W32(lib + 0x14, (uint32_t)s_fontlibs[libslot].openCount);
    } else {
        memset(&s_fonts[slot], 0, sizeof(s_fonts[slot]));
        if (s_fontlibs[libslot].used == 1 && s_fontlibs[libslot].pending > 0)
            s_fontlibs[libslot].pending--;
    }
    hle_unlock();
    if (!fh && ownedPgf) pgf_close(ownedPgf);
    if (errPtr) MEM_W32(errPtr, fh ? 0 : SR_FONT_ERROR_OOM);
    return fh;
}
static uint32_t h_FontOpen(CpuState *s) {
    /* sceFontOpen(a0=lib, a1=index, a2=mode, a3=u32* errorCode). A NULL library answers
     * INVALID_LIB, and an index past the system font list (-1 and 50 measured) INVALID_ARG
     * (pspautotests tests/font/open.expected at f573fe8c, recorded on a PSP). */
    if (!fontlib_snapshot(A0, NULL)) {
        if (A3) MEM_W32(A3, SR_FONT_ERROR_INVALID_LIB);
        return 0;
    }
    if (A1 >= SR_FONT_REGISTRY_COUNT) {
        if (A3) MEM_W32(A3, SR_FONT_ERROR_INVALID_ARG);
        return 0;
    }
    SrFontRegistryInfo info;
    SrFontRegistryStatus status = font_registry_info_for_index(A1, &info);
    const PGF *pgf = status == SR_FONT_REGISTRY_PRESENT ? font_pgf_for_index(A1) : NULL;
    if (!pgf || !info.file_name) {
        if (A3) MEM_W32(A3, SR_FONT_ERROR_INVALID_DATA);
        return 0;
    }
    uint32_t fh = font_open_common(s, A0, A1, A2, pgf, NULL, &info, A3);
    if (getenv("SR_FONTLOG"))
        fprintf(stderr, "h_FontOpen: lib=0x%08x index=%u mode=%u -> font=0x%08x\n", A0, A1, A2, fh);
    return fh;
}
/* Reads via the same VFS as sceIoOpen, defined with the descriptor implementation below. */
static uint32_t font_read_user_file(uint32_t guest_path, uint8_t **bytes, uint32_t *size);
static uint32_t h_FontOpenUserFile(CpuState *s) {
    if (!A3 || !sr_guest_span_writable(A3, 4u)) return SR_FONT_ERROR_INVALID_ARG;
    if (!A1) { MEM_W32(A3, SR_FONT_ERROR_INVALID_ARG); return 0; }
    SrFontLib lib;
    if (!fontlib_snapshot(A0, &lib)) { MEM_W32(A3, SR_FONT_ERROR_INVALID_LIB); return 0; }
    /* Mode 0 uses guest file callbacks, which are not implemented here. */
    if (A2 != 1u) { MEM_W32(A3, 0x80010086u); return 0; }
    uint8_t *bytes = NULL;
    uint32_t size = 0, error = font_read_user_file(A1, &bytes, &size);
    if (error) { MEM_W32(A3, error); return 0; }
    PGF *owned = pgf_open_memory(bytes, size);
    free(bytes);
    return font_open_common(s, A0, 0, A2, owned, owned, NULL, A3);
}
static uint32_t h_FontOpenUserMemory(CpuState *s) {
    /* sceFontOpenUserMemory(a0=lib, a1=memAddr, a2=memLen, a3=u32* errorCode). */
    font_load();
    PGF *owned = NULL;
    if (A1 && A2 >= 16 && A2 <= 16u * 1024u * 1024u && sr_guest_span_readable(A1, A2)) {
        uint8_t *copy = (uint8_t *)malloc(A2);
        if (copy) {
            for (uint32_t i = 0; i < A2; i++) copy[i] = MEM_R8(A1 + i);
            owned = pgf_open_memory(copy, A2);
            free(copy);
        }
    }
    uint32_t fh = font_open_common(s, A0, 0, 4, owned, owned, NULL, A3);
    if (getenv("SR_FONTLOG"))
        fprintf(stderr, "h_FontOpenUserMemory: lib=0x%08x mem=0x%08x len=%u -> font=0x%08x\n", A0, A1, A2, fh);
    return fh;
}
static uint32_t h_FontClose(CpuState *s) {
    /* sceFontClose(a0=font): retire the native font record. The guest struct's pool memory is
     * retained for the library's lifetime (the game frees the whole Fpl pool at teardown); we do
     * not replay the guest freeFunc here to avoid dispatching guest code from a Close reachable
     * on the save-icon render path. */
    PGF *destroy = NULL; uint32_t lib = 0; int found = 0; SrFontLib owner; memset(&owner, 0, sizeof(owner));
    hle_lock();
    for (size_t i = 0; i < sizeof(s_fonts) / sizeof(s_fonts[0]); i++) {
        SrFont *f = &s_fonts[i];
        if (f->used == 1 && f->handle == A0 && f->opens > 1u) {
            /* One of several opens of the same font: the handle stays open for the rest. */
            f->opens--;
            hle_unlock();
            return 0;
        }
        if (f->used == 1 && f->handle == A0) {
            found = 1; lib = f->lib; f->used = 0; f->closing = 1;
            if (f->refs == 0 && f->ownedPgf) { destroy = f->ownedPgf; f->ownedPgf = NULL; f->pgf = NULL; }
            if (f->refs == 0) memset(f, 0, sizeof(*f));
            for (size_t j = 0; j < sizeof(s_fontlibs) / sizeof(s_fontlibs[0]); j++)
                if (s_fontlibs[j].used == 1 && s_fontlibs[j].handle == lib && s_fontlibs[j].openCount > 0) {
                    owner = s_fontlibs[j];
                    s_fontlibs[j].openCount--; MEM_W32(lib + 0x14, (uint32_t)s_fontlibs[j].openCount); break;
                }
            break;
        }
    }
    hle_unlock();
    if (destroy) pgf_close(destroy);
    if (found && owner.freeFn)
        (void)ge_call_guest_rv(s, owner.freeFn, owner.userData, A0, 0);
    return found ? 0 : SR_FONT_ERROR_INVALID_ARG;
}
static uint32_t h_FontDoneLib(CpuState *s) {
    /* sceFontDoneLib(a0=lib): retire the library and any fonts still open under it. */
    PGF *destroy[64]; uint32_t handles[64]; size_t ndestroy = 0, nhandles = 0;
    int found = 0; SrFontLib owner; memset(&owner, 0, sizeof(owner));
    hle_lock();
    for (size_t j = 0; j < sizeof(s_fontlibs) / sizeof(s_fontlibs[0]); j++) {
        if (s_fontlibs[j].used == 1 && s_fontlibs[j].handle == A0) {
            if (s_fontlibs[j].pending) { hle_unlock(); return SR_FONT_ERROR_INVALID_ARG; }
            found = 1; owner = s_fontlibs[j]; memset(&s_fontlibs[j], 0, sizeof(s_fontlibs[j])); break;
        }
    }
    if (found) for (size_t i = 0; i < sizeof(s_fonts) / sizeof(s_fonts[0]); i++) {
        SrFont *f = &s_fonts[i];
        if (f->used == 1 && f->lib == A0) {
            handles[nhandles++] = f->handle;
            f->used = 0; f->closing = 1;
            if (f->refs == 0 && f->ownedPgf) {
                destroy[ndestroy++] = f->ownedPgf; f->ownedPgf = NULL; f->pgf = NULL;
            }
            if (f->refs == 0) memset(f, 0, sizeof(*f));
        }
    }
    hle_unlock();
    for (size_t i = 0; i < ndestroy; i++) pgf_close(destroy[i]);
    if (found && owner.freeFn) {
        for (size_t i = 0; i < nhandles; i++)
            (void)ge_call_guest_rv(s, owner.freeFn, owner.userData, handles[i], 0);
        (void)ge_call_guest_rv(s, owner.freeFn, owner.userData, A0, 0);
    }
    return found ? 0 : SR_FONT_ERROR_INVALID_LIB;
}
static uint32_t h_FontGetFontInfo(CpuState *s) {
    font_load();
    SrFont *f = NULL; const PGF *p = font_acquire(A0, &f);
    /* FontInfo is 0x108 bytes; the whole output span must be guest memory before any write. */
    if (!p || !A1 || !sr_guest_span_writable(A1, 0x108u)) {
        font_release(f);
        return SR_FONT_ERROR_INVALID_ARG;
    }
    font_write_info(A1, p, f && f->has_registry_info ? &f->registry_info : NULL,
                    f ? (f->ownedPgf != NULL) : 1);
    font_release(f);
    return 0;
}
#ifdef SR_HLE_THREAD_SELFTEST
uint32_t sr_font_test_info_for_index(uint32_t index, uint32_t info_addr) {
    SrFontRegistryInfo info;
    if (font_registry_info_for_index(index, &info) != SR_FONT_REGISTRY_PRESENT)
        return SR_FONT_ERROR_INVALID_DATA;
    const PGF *pgf = font_pgf_for_index(index);
    if (!info_addr || !pgf) return SR_FONT_ERROR_INVALID_DATA;
    font_write_info(info_addr, pgf, &info, 0);
    return 0u;
}
#endif
/* The fallback character of the library that owns font record `f` (the default with none). */
static int font_alt_char_for(const SrFont *f) {
    SrFontLib L;
    if (f && fontlib_snapshot(f->lib, &L)) return (int)L.altCharCode;
    return (int)SR_FONT_DEFAULT_ALT_CHAR;
}
/* sceFontSetAltCharacterCode(lib, charCode): set the character a missing glyph falls back to.
 * Measured on a PSP by pspautotests tests/font/altcharcode.expected (rev 2ed7c9c5): a live library
 * returns 0 and stores the code as 16 bits (0 -> 0000, -1 -> ffff, 0xFFFF -> ffff, 0x10000 ->
 * 0000), a NULL library returns 0x80460002; sceFontGetCharInfo for the missing character 0xFFFF
 * then reports a bitmap height of 0 with fallback 0x0000 or 0xFFFF and 2 (the '_' glyph's) with
 * 0x005F. The capture's DoneLib cell writes freed memory and answers 0, which is not modelled:
 * a library handle that is not live stops the run by name. The fallback is kept host-side; the
 * field the PSP's own library struct holds it in is not part of this runtime's library layout. */
static uint32_t h_FontSetAltCharacterCode(CpuState *s) {
    if (!A0) return SR_FONT_ERROR_INVALID_LIB;
    int found = 0;
    hle_lock();
    for (size_t i = 0; i < sizeof(s_fontlibs) / sizeof(s_fontlibs[0]); i++)
        if (s_fontlibs[i].used == 1 && s_fontlibs[i].handle == A0) {
            s_fontlibs[i].altCharCode = (uint16_t)A1;
            found = 1;
            break;
        }
    hle_unlock();
    if (!found)
        return hle_unmodelled_stop("sceFontSetAltCharacterCode",
                                   "on library 0x%08x, which is not live, is not measured", A0);
    return 0;
}
uint32_t h_FontGetCharInfo(CpuState *s) {
    /* CharInfo is 0x3c bytes; the whole output span must be guest memory before any write. */
    uint32_t ci = A2; if (!ci || !sr_guest_span_writable(ci, 0x3cu)) return SR_FONT_ERROR_INVALID_ARG;
    uint32_t cc = A1 & 0xffff;
    SrFont *f = NULL; const PGF *p = font_pgf_for(A0, (int)cc, &f);
    if (p) { pgf_get_char_info(p, (int)cc, font_alt_char_for(f), ci); font_release(f); return 0; }
    font_release(f);
    return SR_FONT_ERROR_INVALID_ARG;
}


uint32_t h_FontGetCharGlyphImage(CpuState *s) {
    uint32_t cc = A1 & 0xffff, gi = A2;
    if (!gi) return SR_FONT_ERROR_INVALID_ARG;
    SrFont *f = NULL; const PGF *p = font_pgf_for(A0, (int)cc, &f);
    if (p) {
        (void)pgf_draw_glyph(p, (int)cc, font_alt_char_for(f), gi);
        font_release(f); return 0;
    }
    font_release(f);
    return SR_FONT_ERROR_INVALID_ARG;
}

/* Some libfont revisions expose an internal glyph-table-id entry point even though the public
 * sceLibFont NID table only names the character-code form.  Keep the implementation available
 * to the module loader without inventing a firmware NID. */
uint32_t h_FontGetGlyphImageById(CpuState *s) {
    if (!A2) return SR_FONT_ERROR_INVALID_ARG;
    SrFont *f = NULL; const PGF *p = font_acquire(A0, &f);
    if (!p) { font_release(f); return SR_FONT_ERROR_INVALID_ARG; }
    int drawn = pgf_draw_glyph_by_id(p, (int)A1, A2);
    font_release(f);
    return drawn ? 0 : SR_FONT_ERROR_INVALID_ARG;
}
/* sceFontSetResolution(lib, float hRes, float vRes): the library's resolution, which
 * sceFontFindOptimumFont uses for a style that names none. Measured on a PSP (pspautotests
 * tests/font/resolution.expected at f573fe8c): 1 and infinity are stored and answer 0; -1, 0,
 * -1/128 and -infinity answer INVALID_ARG and change nothing; a NULL library answers
 * INVALID_LIB. The test sets both values alike, so a pair with one bad value is refused whole
 * here, and NaN, which it does not try, is refused with the non-positive values (UNMEASURED).
 * The floats arrive in $f12 and $f13 (MIPS EABI). A library that is not live stops by name, as
 * the other library calls do. */
static uint32_t h_FontSetResolution(CpuState *s) {
    float h = s->f[12], v = s->f[13];
    if (!A0) return SR_FONT_ERROR_INVALID_LIB;
    if (!fontlib_snapshot(A0, NULL))
        return hle_unmodelled_stop("sceFontSetResolution",
                                   "on library 0x%08x, which is not live, is not measured", A0);
    if (!(h > 0.0f) || !(v > 0.0f)) return SR_FONT_ERROR_INVALID_ARG;
    hle_lock();
    for (size_t i = 0; i < sizeof(s_fontlibs) / sizeof(s_fontlibs[0]); i++)
        if (s_fontlibs[i].used == 1 && s_fontlibs[i].handle == A0) {
            s_fontlibs[i].hRes = h;
            s_fontlibs[i].vRes = v;
            break;
        }
    hle_unlock();
    return 0;
}

/* One served system font's style, as sceFontFindOptimumFont compares it. */
typedef struct {
    float h, v, h_res, v_res;
    uint16_t family, style, language, country;
    const char *name, *file_name;
} SrFontMatchStyle;

/* The requested PGFFontStyle (0xA8 bytes): five floats, then u16 family, style, sub-style,
 * language, region and country, a 64-byte name and a 64-byte file name. */
typedef struct {
    float h, v, h_res, v_res;
    uint16_t family, style, language, country;
    char name[64], file_name[64];
} SrFontStyleRequest;

/* sceFontFindOptimumFont's choice among `count` served fonts, in registry index order: the
 * first font nearest the requested height when one is given, else the last font every given
 * field matches, else index 0. A field that is zero (or an empty string) is not compared; any
 * given field that differs excludes the font. The vertical distance compares the font's
 * vertical size against the requested HEIGHT (fontH), not fontV, which is what reproduces the
 * PSP's "V match" answers. Algorithm from PPSSPP 7fab97c0 Core/HLE/sceFont.cpp
 * (sceFontFindOptimumFont, Font::MatchesStyle; GPL-2.0-or-later, derived-translated); every
 * answer of pspautotests tests/font/optimum.expected at f573fe8c, recorded on a PSP, follows
 * from it. `index_of[i]` is the registry index of fonts[i]. */
static int font_find_optimum_index(const SrFontMatchStyle *fonts, const int *index_of, int count,
                                   const SrFontStyleRequest *req, float lib_h_res,
                                   float lib_v_res) {
    float h_res = req->h_res > 0.0f ? req->h_res : lib_h_res;
    float v_res = req->v_res > 0.0f ? req->v_res : lib_v_res;
    int optimum = -1, nearest = -1;
    float nearest_dist = INFINITY;
    int given = req->family || req->style || req->language || req->country || req->name[0] ||
                req->file_name[0];
    for (int i = 0; i < count; i++) {
        const SrFontMatchStyle *f = &fonts[i];
        if ((req->family && req->family != f->family) ||
            (req->style && req->style != f->style) ||
            (req->language && req->language != f->language) ||
            (req->country && req->country != f->country) ||
            (req->name[0] && strcmp(req->name, f->name ? f->name : "") != 0) ||
            (req->file_name[0] && strcmp(req->file_name, f->file_name ? f->file_name : "") != 0))
            continue;
        if (req->h > 0.0f) {
            float d = fabsf(f->h_res * f->h - h_res * req->h);
            if (d < nearest_dist) { nearest_dist = d; nearest = index_of[i]; }
        }
        if (req->v > 0.0f) {
            float d = fabsf(f->v_res * f->v - v_res * req->h);
            if (d < nearest_dist) { nearest_dist = d; nearest = index_of[i]; }
        }
        if (given) optimum = index_of[i];
    }
    if (nearest >= 0) optimum = nearest;
    return optimum >= 0 ? optimum : 0;
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Test-build-only: font_find_optimum_index over a synthetic inventory (sizes: h, v, h_res,
 * v_res; codes: family, style, language, country) and request. */
int sr_font_test_find_optimum(const float sizes[][4], const uint16_t codes[][4],
                              const char *const names[], const char *const files[], int count,
                              const float req_sizes[4], const uint16_t req_codes[4],
                              const char *req_name, const char *req_file, float lib_h_res,
                              float lib_v_res) {
    SrFontMatchStyle fonts[SR_FONT_REGISTRY_COUNT];
    int index_of[SR_FONT_REGISTRY_COUNT];
    SrFontStyleRequest req;
    if (count < 0 || count > (int)SR_FONT_REGISTRY_COUNT) return -1;
    memset(&req, 0, sizeof(req));
    for (int i = 0; i < count; i++) {
        fonts[i] = (SrFontMatchStyle){ sizes[i][0], sizes[i][1], sizes[i][2], sizes[i][3],
                                       codes[i][0], codes[i][1], codes[i][2], codes[i][3],
                                       names[i], files[i] };
        index_of[i] = i;
    }
    req.h = req_sizes[0]; req.v = req_sizes[1]; req.h_res = req_sizes[2]; req.v_res = req_sizes[3];
    req.family = req_codes[0]; req.style = req_codes[1];
    req.language = req_codes[2]; req.country = req_codes[3];
    snprintf(req.name, sizeof(req.name), "%s", req_name ? req_name : "");
    snprintf(req.file_name, sizeof(req.file_name), "%s", req_file ? req_file : "");
    return font_find_optimum_index(fonts, index_of, count, &req, lib_h_res, lib_v_res);
}
#endif

/* sceFontFindOptimumFont(lib, const SceFontStyle *style, u32 *errorCode): the index of the
 * served system font that best fits the style (font_find_optimum_index), with *errorCode 0.
 * Measured on a PSP (pspautotests tests/font/optimum.expected at f573fe8c): a NULL library and
 * a NULL style both answer 0 with *errorCode INVALID_LIB (0x80460002). Only fonts this runtime
 * serves are candidates; on a console all 18 are. Not measured, so the run stops by name: a
 * library that is not live, an unreadable style and an unwritable error pointer (a NULL one
 * crashes the PSP). */
static uint32_t h_FontFindOptimumFont(CpuState *s) {
    const uint32_t style_size = 0xa8u;
    if (!A2 || !sr_guest_span_writable(A2, 4u))
        return hle_unmodelled_stop("sceFontFindOptimumFont",
                                   "with an unwritable error pointer 0x%08x is not measured", A2);
    if (!A0 || !A1) { MEM_W32(A2, SR_FONT_ERROR_INVALID_LIB); return 0; }
    SrFontLib lib;
    if (!fontlib_snapshot(A0, &lib))
        return hle_unmodelled_stop("sceFontFindOptimumFont",
                                   "on library 0x%08x, which is not live, is not measured", A0);
    if (!sr_guest_span_readable(A1, style_size))
        return hle_unmodelled_stop("sceFontFindOptimumFont",
                                   "with an unreadable style 0x%08x is not measured", A1);
    SrFontStyleRequest req;
    memset(&req, 0, sizeof(req));
    uint32_t w[4];
    for (int i = 0; i < 4; i++) w[i] = MEM_R32(A1 + (uint32_t)i * 4u);
    memcpy(&req.h, &w[0], 4); memcpy(&req.v, &w[1], 4);
    memcpy(&req.h_res, &w[2], 4); memcpy(&req.v_res, &w[3], 4);
    req.family = MEM_R16(A1 + 0x14u);
    req.style = MEM_R16(A1 + 0x16u);
    req.language = MEM_R16(A1 + 0x1au);
    req.country = MEM_R16(A1 + 0x1eu);
    for (uint32_t i = 0; i < 63u; i++) {
        req.name[i] = (char)MEM_R8(A1 + 0x20u + i);
        req.file_name[i] = (char)MEM_R8(A1 + 0x60u + i);
    }
    SrFontRegistryInfo info[SR_FONT_REGISTRY_COUNT];
    SrFontMatchStyle fonts[SR_FONT_REGISTRY_COUNT];
    int index_of[SR_FONT_REGISTRY_COUNT], count = 0;
    for (uint32_t i = 0; i < SR_FONT_REGISTRY_COUNT; i++) {
        if (font_registry_info_for_index(i, &info[i]) != SR_FONT_REGISTRY_PRESENT) continue;
        const SrFontRegistryInfo *r = &info[i];
        fonts[count] = (SrFontMatchStyle){
            (float)r->h_size / 64.0f, (float)r->v_size / 64.0f,
            (float)r->h_resolution / 64.0f, (float)r->v_resolution / 64.0f,
            (uint16_t)r->family_code, (uint16_t)r->style, (uint16_t)r->language_code,
            (uint16_t)r->country_code, r->font_name, r->file_name };
        index_of[count++] = (int)i;
    }
    int index = font_find_optimum_index(fonts, index_of, count, &req, lib.hRes, lib.vRes);
    MEM_W32(A2, 0u);
    return (uint32_t)index;
}
/* ---- sceFontGetNumFontList / sceFontGetFontList: the system font list ----------------------
 * Measured on a PSP by pspautotests tests/font/fontlist.expected (rev 2ed7c9c5, the console's
 * own libfont.prx, a library made with numFonts 4):
 *   - GetNumFontList(lib, &err) returns 18 and sets err 0; a NULL library returns 0 and sets
 *     err 0x80460002.
 *   - GetFontList(lib, styles, num) returns 0 and writes one 0xA8-byte style per font: with num 1
 *     exactly one (the word after it is left alone), the first being the Japanese font (index 0:
 *     10.125 x 10.125 at 128 x 128, family 1, style 103, language 1, country 1). num -1 and 0
 *     return 0 and write nothing. A NULL library returns 0x80460002; NULL styles with a live
 *     library return 0x80460003.
 * 18 is SR_FONT_REGISTRY_COUNT, the measured index map /DATA/FONT and sceFontOpen already use,
 * and each style comes from the same per-index registry values (font_write_style). A count
 * above 1 writes min(num, 18) styles in index order; only the first of them is shown by the
 * capture. Not measured, so the run stops by name: a library handle that is not live (the
 * capture's DoneLib cell reads freed memory and answers like an empty library), a NULL error
 * pointer (the capture notes it crashes), an unwritable style array, and an index whose font is
 * not served (the console has all 18; a missing user font would leave a hole the PSP never
 * shows). */
static uint32_t h_FontGetNumFontList(CpuState *s) {
    if (!A1 || !sr_guest_span_writable(A1, 4u))
        return hle_unmodelled_stop("sceFontGetNumFontList",
                                   "with an unwritable error pointer 0x%08x is not measured", A1);
    if (!A0) { MEM_W32(A1, SR_FONT_ERROR_INVALID_LIB); return 0; }
    if (!fontlib_snapshot(A0, NULL))
        return hle_unmodelled_stop("sceFontGetNumFontList",
                                   "on library 0x%08x, which is not live, is not measured", A0);
    MEM_W32(A1, 0u);
    return SR_FONT_REGISTRY_COUNT;
}
static uint32_t h_FontGetFontList(CpuState *s) {
    const uint32_t style_size = 0xa8u;
    if (!A0) return SR_FONT_ERROR_INVALID_LIB;
    if (!fontlib_snapshot(A0, NULL))
        return hle_unmodelled_stop("sceFontGetFontList",
                                   "on library 0x%08x, which is not live, is not measured", A0);
    if (!A1) return SR_FONT_ERROR_INVALID_ARG;
    int32_t num = (int32_t)A2;
    if (num <= 0) return 0;
    uint32_t count = (uint32_t)num < SR_FONT_REGISTRY_COUNT ? (uint32_t)num : SR_FONT_REGISTRY_COUNT;
    if (!sr_guest_span_writable(A1, count * style_size))
        return hle_unmodelled_stop("sceFontGetFontList",
                                   "with an unwritable style array 0x%08x is not measured", A1);
    SrFontRegistryInfo info[SR_FONT_REGISTRY_COUNT];
    for (uint32_t i = 0; i < count; i++) {
        if (font_registry_info_for_index(i, &info[i]) != SR_FONT_REGISTRY_PRESENT)
            return hle_unmodelled_stop("sceFontGetFontList",
                                       "with font index %u not served is not measured", i);
    }
    for (uint32_t i = 0; i < count; i++) font_write_style(A1 + i * style_size, &info[i]);
    return 0;
}
/* Read an entire user font through the descriptor's actual backing, including ISO and archives. */
static uint32_t font_read_user_file(uint32_t guest_path, uint8_t **bytes, uint32_t *size) {
    char path[512];
    if (io_guest_path(guest_path, path, sizeof(path)) != 0u) return SR_FONT_ERROR_INVALID_ARG;
    uint32_t fd = h_io_open_path(path, 1u, -1);
    if ((int32_t)fd < 0) return 0x80460005u; /* handler open failed */
    Fd *f = &s_fds[fd];
    uint32_t error = 0u, n = f->size;
    uint8_t *data = NULL;
    if (f->kind != FD_KIND_FILE || n < 16u || n > 16u * 1024u * 1024u) error = SR_FONT_ERROR_INVALID_DATA;
    else if (!(data = (uint8_t *)malloc(n))) error = SR_FONT_ERROR_OOM;
    else if (f->archive_vfs) {
        SrArchiveFile file = { f->archive_mount, f->archive_entry, f->size, -1 };
        size_t got = 0;
        if (sr_archive_vfs_read(f->archive_vfs, &file, 0, data, n, &got) != NK_OK || got != n)
            error = 0x80460005u;
    } else if (f->host) {
        if (fseek(f->host, 0, SEEK_SET) != 0 || fread(data, 1, n, f->host) != n) error = 0x80460005u;
    } else if (iso_read(f->lba, 0, data, n) != (int)n) error = 0x80460005u;
    hle_fd_release(f, 0);
    if (error) { free(data); return error; }
    *bytes = data; *size = n;
    return 0;
}

void hle_register_font_handlers(void) {
    sr_hle_register(0x67f17ed7, "sceFontNewLib", h_FontNewLib);
    sr_hle_register(0xa834319d, "sceFontOpen", h_FontOpen);
    sr_hle_register(0x574b6fbc, "sceFontDoneLib", h_FontDoneLib);
    sr_hle_register(0x0da7535e, "sceFontGetFontInfo", h_FontGetFontInfo);
    sr_hle_register(0xdcc80c2f, "sceFontGetCharInfo", h_FontGetCharInfo);
    sr_hle_register(0x980f4895, "sceFontGetCharGlyphImage", h_FontGetCharGlyphImage);
    sr_hle_register(0x099ef33c, "sceFontFindOptimumFont", h_FontFindOptimumFont);
    sr_hle_register(0x48293280, "sceFontSetResolution", h_FontSetResolution);
    sr_hle_register(0x3aea8cb6, "sceFontClose", h_FontClose);
    sr_hle_register(0xbb8e7fe6, "sceFontOpenUserMemory", h_FontOpenUserMemory);
    sr_hle_register(0x57fcb733, "sceFontOpenUserFile", h_FontOpenUserFile);
}

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

/* From hle_register_launch_sweep_blocker_handlers(). */
void hle_register_launch_sweep_blocker_font(void) {
    sr_hle_register(0x27f6e642, "sceFontGetNumFontList", h_FontGetNumFontList);
    sr_hle_register(0xbc75d85b, "sceFontGetFontList", h_FontGetFontList);
    sr_hle_register(0xee232411, "sceFontSetAltCharacterCode", h_FontSetAltCharacterCode);
}
