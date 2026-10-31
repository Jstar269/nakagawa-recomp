// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* The system registry (sceReg) and the console's OpenPSID. Extracted from hle.c on 2026-10-31
 * (#307). */

#include "hle_internal.h"

/* ---- sceReg: virtual system registry with a per-user overlay ----
 * Sources:
 *  - MEASURED on PSP-3000 6.6.1, 2026-10-09, read-only enumeration (oracle case PSP-REGISTRY-001):
 *    the names, types, sizes and sceRegGetKeys order of the /CONFIG tree in s_sreg_tree; directory
 *    entries report size 0; empty strings report size 1 and longer strings their length, so a
 *    string key's size follows its value; sceRegGetKeyInfo on an unknown key returns 0x8008271D;
 *    sceRegGetKeysNum on a forged handle returns 0x8008272E and leaves the count untouched; 256
 *    further sceRegOpenRegistry calls succeeded while one handle was open; and a mode-1
 *    sceRegOpenCategory of a missing category returned 0 and the category then appeared in its
 *    parent with its 36-byte name cut to 26 bytes (two such entries were present, so an earlier
 *    run's category had persisted). Opening the 26-byte name afterwards failed; that is not
 *    modeled. Values were read only for six non-personal keys (see systemparam_int_value and
 *    s_sreg_measured_ints).
 *  - PSPSDK Registry Kernel Library (pspdev.github.io/pspsdk group__Reg, src/registry/pspreg.h and
 *    the sceReg.S import stubs): the signatures, REGHANDLE, the key types DIR=1, INT=2 ("Key is an
 *    integer (4 bytes)"), STR=3 and BIN=4, REG_KEYNAME_SIZE 27, sceRegCreateKey's type ("cannot be
 *    a directory type") and size ("Size of the allocated value space"), and the category open mode
 *    ("can be 1 or 2, probably read or read/write"). struct RegParam is 272 bytes: regtype u32 at
 *    +0, name[256] at +4 (documented as seemingly unused), namelen u32 at +260, unk2 u32 at +264,
 *    unk3 u32 at +268. The Rust libc psp bindings name the first argument of sceRegRemoveCategory
 *    the registry handle (not a category handle); it takes a category name.
 *  - Public homebrew title behaviour (Adrenaline, cef/recovery/utils.c): reading or writing a
 *    setting opens the registry and the category with mode 2; a write tries sceRegSetKeyValue first
 *    and, when the key is missing, calls sceRegCreateKey and sets the value again; both paths call
 *    sceRegFlushCategory and sceRegFlushRegistry before closing.
 *
 * UNMEASURED: the oracle never writes the registry, so every write semantic below is unmeasured.
 * Apart from the two measured codes (not-found 0x8008271D and bad handle 0x8008272E, applied to
 * every not-found and invalid-handle case by analogy with the one call each was measured on), the
 * error codes are errno-class (0x8001xxxx, newlib numbering) project choices, as is the check order.
 *
 * Model (project choices where the sources are silent; each one is unmeasured):
 *  - Defaults: the measured tree, except the user's own saved network profiles
 *    (/CONFIG/NETWORK/INFRASTRUCTURE/<n>), which a neutral console does not have. No value is
 *    copied from the console except the six measured non-personal ones. The integer keys
 *    sceUtilityGetSystemParamInt shares take their values from systemparam_int_value(), and the
 *    getter reads the registry back through sreg_systemparam_int(), so the two surfaces agree after
 *    a game writes a setting. Every other key holds a neutral placeholder (INT 0, an empty string,
 *    or zero bytes); personal keys (owner_name, passwords, network and account keys) never hold a
 *    real value. Reading a placeholder logs once, naming the key. RegParam regtype and name are
 *    not checked; every open serves the one system registry.
 *  - An open of a missing category creates it in either mode (measured for mode 1), cutting each
 *    new name to 26 bytes; lookups compare whole names, so a longer name never matches a stored
 *    one. sceRegCreateKey cuts names the same way (inferred: categories are DIR keys in the same
 *    name table).
 *  - Writes need a mode-2 category handle (sceRegSetKeyValue, sceRegCreateKey) or a mode-2 registry
 *    handle (sceRegRemoveCategory); a mode-1 handle is refused with EACCES. A registry handle's mode
 *    does not limit the categories opened under it. Default categories cannot be removed (EPERM),
 *    and a category that still has entries is refused (ENOTEMPTY). sceRegRemoveKey stays
 *    unregistered because no public header documents its arguments.
 *  - A string key's size is its stored value: sceRegSetKeyValue replaces it with the bytes given.
 *    INT writes are exactly 4 bytes; BIN keys keep their size and a shorter write zero-fills the rest.
 *  - Writes change the live registry at once. sceRegFlushCategory and sceRegFlushRegistry persist
 *    the whole overlay (every difference from the defaults) to one schema-versioned JSON file that
 *    is replaced atomically; whether firmware persists only the flushed category, or commits on
 *    close without a flush, is unmeasured. The file is SR_SYSTEM_REGISTRY when set, else
 *    <per-user data dir>/registry/system.json; nothing is written anywhere resembling a console. A
 *    missing file means no changes. An unreadable or invalid file is reported once and ignored as a
 *    whole (the defaults are served), and the next flush moves it aside to <file>.corrupt.
 *  - /DATA/FONT is generated, not measured as a tree: the guest's own font library asks for it (it
 *    opens the category, reads path_name and num_fonts, then every INFO<n> category in index order;
 *    it was seen to stop at the first INFO<n> it could not open and to leave every later font with an
 *    empty file name, so an absent index hides the indices after it from that library). The index
 *    count and the numeric codes of each index are measured on a PSP-3000 (2026-10-15,
 *    flash0_font.c); the key names and types, the path_name string and the unit of the size values
 *    are not. The library also asks for /SYSPROFILE/RESOLUTION/horizontal, served from the
 *    2026-10-20 measurement (s_sreg_tree, s_sreg_measured_ints). The subtree is read-only
 *    (SREG_NODE_READONLY): a write is EACCES, an open does not create entries in it, it is never
 *    persisted, and an overlay that names it is refused whole.
 *  - Project bounds, not console limits: 512 registry handles (the console served at least 257)
 *    and 512 category handles, 1024 nodes, category paths and names read up to 255 bytes, STR/BIN
 *    values up to 4096 bytes, an overlay file up to 1 MiB, and names of printable ASCII
 *    without '/'. */
#define SREG_REGPARAM_BYTES 272u
#define SREG_TAG_REG 0x52470000u /* 'RG' registry handle; low 16 bits = slot + 1 */
#define SREG_TAG_CAT 0x52430000u /* 'RC' category handle; low 16 bits = slot + 1 */
#define SREG_TAG_KEY 0x524b0000u /* 'RK' key handle; low 16 bits = the entry's node id */
#define SREG_ERR_ILLEGAL_ADDR 0x80000103u /* SCE_KERNEL_ERROR_ILLEGAL_ADDR: bad guest pointer or span */
#define SREG_ERR_BAD_HANDLE 0x8008272eu   /* measured: sceRegGetKeysNum on a forged handle */
#define SREG_ERR_PERM 0x80010001u         /* EPERM: a default category cannot be removed */
#define SREG_ERR_NOMEM 0x8001000cu        /* ENOMEM: handle slots exhausted */
#define SREG_ERR_ACCES 0x8001000du        /* EACCES: a write through a mode-1 (read-only) handle */
#define SREG_ERR_FTYPE 0x8001004fu        /* EFTYPE: value use of a DIR entry; INT write not 4 bytes */
#define SREG_ERR_NOTEMPTY 0x8001005au     /* ENOTEMPTY: the category still has entries */
#define SREG_ERR_NAMETOOLONG 0x8001005bu  /* ENAMETOOLONG: no NUL in the first 256 bytes of a guest name */
SregRegSlot s_sreg_reg[SREG_REG_SLOTS];
SregCatSlot s_sreg_cat[SREG_CAT_SLOTS];

static int sreg_has_entries(int cat) {
    for (uint32_t i = 0; i < SREG_MAX_NODES; i++)
        if (s_sreg_nodes[i].id && s_sreg_nodes[i].parent == cat) return 1;
    return 0;
}

/* Read a NUL-terminated guest name of at most cap - 1 bytes into out. Returns 0, ILLEGAL_ADDR when
 * the pointer is NULL or a byte before the terminator is unreadable, or NAMETOOLONG when the first
 * cap bytes hold no terminator. */
static uint32_t sreg_guest_name(uint32_t addr, char *out, uint32_t cap) {
    out[0] = '\0';
    if (!addr) return SREG_ERR_ILLEGAL_ADDR;
    for (uint32_t i = 0; i < cap; i++) {
        uint32_t cur = 0;
        if (!sr_size_add_ok(addr, i, &cur) || !sr_guest_span_readable(cur, 1u)) {
            out[0] = '\0';
            return SREG_ERR_ILLEGAL_ADDR;
        }
        out[i] = (char)MEM_R8(cur);
        if (!out[i]) return 0;
    }
    out[0] = '\0';
    return SREG_ERR_NAMETOOLONG;
}

static int sreg_slot_index(uint32_t h, uint32_t tag, uint32_t slots) {
    uint32_t n = h & 0xffffu;
    if ((h & 0xffff0000u) != tag || n == 0u || n > slots) return -1;
    return (int)(n - 1u);
}

static int sreg_reg_slot(uint32_t h) {
    int slot = sreg_slot_index(h, SREG_TAG_REG, SREG_REG_SLOTS);
    return slot >= 0 && s_sreg_reg[slot].open ? slot : -1;
}

/* An open category handle whose category still exists: removing a category leaves its handles
 * stale, and they are refused as bad handles (closing one still succeeds). */
static int sreg_cat_slot(uint32_t h) {
    int slot = sreg_slot_index(h, SREG_TAG_CAT, SREG_CAT_SLOTS), node;
    if (slot < 0 || !s_sreg_cat[slot].open) return -1;
    node = s_sreg_cat[slot].node;
    if (node < 0 || node >= (int)SREG_MAX_NODES || s_sreg_nodes[node].id != s_sreg_cat[slot].node_id)
        return -1;
    return slot;
}

/* The entry a key handle names, provided it belongs to the category. */
static int sreg_key_by_handle(uint32_t hk, int cat) {
    uint32_t id = hk & 0xffffu;
    if ((hk & 0xffff0000u) != SREG_TAG_KEY || id == 0u) return -1;
    for (uint32_t i = 0; i < SREG_MAX_NODES; i++)
        if (s_sreg_nodes[i].id == id) return s_sreg_nodes[i].parent == cat ? (int)i : -1;
    return -1;
}

/* Look up the entry named by a guest string in an open category. A name longer than 26 bytes can
 * never match a stored one, so it is not found. */
static uint32_t sreg_lookup_entry(int slot, uint32_t name_addr, const char *caller, int *node_out) {
    char key[SREG_PATH_BYTES];
    uint32_t rc = sreg_guest_name(name_addr, key, SREG_PATH_BYTES);
    int node;
    if (rc) return rc;
    node = strlen(key) < SREG_NAME_BYTES ? sreg_child(s_sreg_cat[slot].node, key) : -1;
    if (node < 0) {
        char path[SREG_PATH_BYTES];
        if (!sreg_node_path(s_sreg_cat[slot].node, path, sizeof(path))) path[0] = '\0';
        fprintf(stderr, "%s: unknown key \"%s\" in category \"%s\" (virtual system registry fails closed)\n",
                caller, key, path);
        return SREG_ERR_NOT_FOUND;
    }
    if ((s_sreg_nodes[node].flags & SREG_NODE_READONLY) && getenv("SR_FONTLOG"))
        fprintf(stderr, "%s: key \"%s\" type %u size %u\n", caller, s_sreg_nodes[node].name,
                s_sreg_nodes[node].type, s_sreg_nodes[node].size);
    *node_out = node;
    return 0;
}

static uint32_t sreg_refuse_read_only(const char *caller) {
    fprintf(stderr, "%s: refused: the handle was opened with mode 1 (read only)\n", caller);
    return SREG_ERR_ACCES;
}

/* A write to the generated font subtree (SREG_NODE_READONLY) is refused whatever the handle's mode.
 * EACCES is a project choice: the console's answer is unmeasured (the oracle never wrote). */
static uint32_t sreg_refuse_generated(const char *caller, const char *what) {
    fprintf(stderr, "%s: refused: \"%s\" is generated from the served fonts and is read-only\n", caller, what);
    return SREG_ERR_ACCES;
}

/* Copy a value key into a guest buffer of buf_size bytes; a buffer smaller than the value is
 * refused, and a string value includes whatever NUL it was stored with. */
static uint32_t sreg_copy_out(int node, uint32_t buf, uint32_t buf_size, const char *caller) {
    SregNode *n = &s_sreg_nodes[node];
    if (n->type == SREG_TYPE_DIR) return SREG_ERR_FTYPE;
    if (buf_size < n->size) return SREG_ERR_INVAL;
    if (!buf || !sr_guest_span_writable(buf, n->size)) return SREG_ERR_ILLEGAL_ADDR;
    sreg_note_placeholder_read(node, caller);
    for (uint32_t i = 0; i < n->size; i++) MEM_W8(buf + i, n->value[i]);
    if ((n->flags & SREG_NODE_READONLY) && getenv("SR_FONTLOG")) {
        char path[SREG_PATH_BYTES];
        if (!sreg_node_path(n->parent, path, sizeof(path))) path[0] = '\0';
        if (n->type == SREG_TYPE_INT && n->size == 4u)
            fprintf(stderr, "%s: read %s/%s = %u\n", caller, path, n->name, sreg_get_le32(n->value));
        else
            fprintf(stderr, "%s: read %s/%s (%u bytes)\n", caller, path, n->name, n->size);
    }
    return 0;
}

/* sceRegOpenRegistry(struct RegParam *reg, int mode, REGHANDLE *h): mode 1 (read) or 2 (read/write). */
static uint32_t h_RegOpenRegistry(CpuState *s) {
    uint32_t reg = A0, mode = A1, out = A2;
    int slot = -1;
    if (!reg || !sr_guest_span_readable(reg, SREG_REGPARAM_BYTES)) return SREG_ERR_ILLEGAL_ADDR;
    if (!out || !sr_guest_span_writable(out, 4u)) return SREG_ERR_ILLEGAL_ADDR;
    if (mode != 1u && mode != 2u) {
        fprintf(stderr, "sceRegOpenRegistry: mode %u unsupported (1 = read, 2 = read/write)\n", mode);
        return SREG_ERR_INVAL;
    }
    for (uint32_t i = 0; i < SREG_REG_SLOTS && slot < 0; i++)
        if (!s_sreg_reg[i].open) slot = (int)i;
    if (slot < 0) return SREG_ERR_NOMEM;
    sreg_ensure_loaded();
    s_sreg_reg[slot].open = 1;
    s_sreg_reg[slot].mode = (uint8_t)mode;
    MEM_W32(out, SREG_TAG_REG | (uint32_t)(slot + 1));
    return 0;
}

static uint32_t h_RegCloseRegistry(CpuState *s) {
    int slot = sreg_reg_slot(A0);
    if (slot < 0) return SREG_ERR_BAD_HANDLE;
    s_sreg_reg[slot].open = 0;
    return 0;
}

/* sceRegFlushRegistry(REGHANDLE h) */
static uint32_t h_RegFlushRegistry(CpuState *s) {
    if (sreg_reg_slot(A0) < 0) return SREG_ERR_BAD_HANDLE;
    return sreg_persist("sceRegFlushRegistry");
}

/* sceRegOpenCategory(REGHANDLE h, const char *name, int mode, REGHANDLE *hd). An open of a missing
 * category creates it in either mode (measured for mode 1; see the model above); mode 2 also
 * allows writes through the handle. */
static uint32_t h_RegOpenCategory(CpuState *s) {
    uint32_t reg = A0, name = A1, mode = A2, out = A3, err = 0, rc;
    char path[SREG_PATH_BYTES];
    int slot = -1, node, created = 0;
    if (sreg_reg_slot(reg) < 0) return SREG_ERR_BAD_HANDLE;
    if (!out || !sr_guest_span_writable(out, 4u)) return SREG_ERR_ILLEGAL_ADDR;
    rc = sreg_guest_name(name, path, SREG_PATH_BYTES);
    if (rc) return rc;
    if (mode != 1u && mode != 2u) return SREG_ERR_INVAL;
    for (uint32_t i = 0; i < SREG_CAT_SLOTS && slot < 0; i++)
        if (!s_sreg_cat[i].open) slot = (int)i;
    if (slot < 0) return SREG_ERR_NOMEM;
    node = sreg_walk(path, 1, 0u, &err, &created);
    if (node < 0) {
        fprintf(stderr, "sceRegOpenCategory: refused category path \"%s\" (0x%08x)\n", path, err);
        return err;
    }
    if (created) {
        s_sreg_dirty = 1;
        fprintf(stderr, "sceRegOpenCategory: created category \"%s\" (an open of a missing category "
                        "creates it, as measured on a PSP-3000)\n", path);
    }
    if ((s_sreg_nodes[node].flags & SREG_NODE_READONLY) && getenv("SR_FONTLOG"))
        fprintf(stderr, "sceRegOpenCategory: opened \"%s\" (mode %u, read-only font subtree)\n", path, mode);
    s_sreg_cat[slot].open = 1;
    s_sreg_cat[slot].mode = (uint8_t)mode;
    s_sreg_cat[slot].node = node;
    s_sreg_cat[slot].node_id = s_sreg_nodes[node].id;
    MEM_W32(out, SREG_TAG_CAT | (uint32_t)(slot + 1));
    return 0;
}

static uint32_t h_RegCloseCategory(CpuState *s) {
    int slot = sreg_slot_index(A0, SREG_TAG_CAT, SREG_CAT_SLOTS);
    if (slot < 0 || !s_sreg_cat[slot].open) return SREG_ERR_BAD_HANDLE;
    s_sreg_cat[slot].open = 0;
    return 0;
}

/* sceRegFlushCategory(REGHANDLE hd) */
static uint32_t h_RegFlushCategory(CpuState *s) {
    if (sreg_cat_slot(A0) < 0) return SREG_ERR_BAD_HANDLE;
    return sreg_persist("sceRegFlushCategory");
}

/* sceRegRemoveCategory(REGHANDLE h, const char *name): h is the registry handle and name a
 * category path. Only an empty category a game created can be removed. */
static uint32_t h_RegRemoveCategory(CpuState *s) {
    uint32_t err = 0, rc;
    char path[SREG_PATH_BYTES];
    int slot = sreg_reg_slot(A0), node;
    if (slot < 0) return SREG_ERR_BAD_HANDLE;
    if (s_sreg_reg[slot].mode != 2u) return sreg_refuse_read_only("sceRegRemoveCategory");
    rc = sreg_guest_name(A1, path, SREG_PATH_BYTES);
    if (rc) return rc;
    node = sreg_walk(path, 0, 0u, &err, NULL);
    if (node < 0) {
        if (err == SREG_ERR_NOT_FOUND)
            fprintf(stderr, "sceRegRemoveCategory: unknown category \"%s\"\n", path);
        return err;
    }
    if (s_sreg_nodes[node].flags & SREG_NODE_READONLY) return sreg_refuse_generated("sceRegRemoveCategory", path);
    if (s_sreg_nodes[node].flags & SREG_NODE_DEFAULT) {
        fprintf(stderr, "sceRegRemoveCategory: refused: \"%s\" is a modeled system category\n", path);
        return SREG_ERR_PERM;
    }
    if (sreg_has_entries(node)) return SREG_ERR_NOTEMPTY;
    sreg_free_node(node);
    s_sreg_dirty = 1;
    return 0;
}

/* sceRegGetKeyInfo(REGHANDLE hd, const char *name, REGHANDLE *hk, unsigned int *type, SceSize *size).
 * The fifth argument (t0/r8 under MIPS EABI) is the size pointer, read through stack_arg. */
static uint32_t h_RegGetKeyInfo(CpuState *s) {
    uint32_t hk_out = A2, type_out = A3, size_out = stack_arg(s, 0), rc;
    int slot = sreg_cat_slot(A0), node = -1;
    if (slot < 0) return SREG_ERR_BAD_HANDLE;
    if (!hk_out || !sr_guest_span_writable(hk_out, 4u) || !type_out ||
        !sr_guest_span_writable(type_out, 4u) || !size_out || !sr_guest_span_writable(size_out, 4u))
        return SREG_ERR_ILLEGAL_ADDR;
    rc = sreg_lookup_entry(slot, A1, "sceRegGetKeyInfo", &node);
    if (rc) return rc;
    MEM_W32(hk_out, SREG_TAG_KEY | s_sreg_nodes[node].id);
    MEM_W32(type_out, s_sreg_nodes[node].type);
    MEM_W32(size_out, s_sreg_nodes[node].size);
    return 0;
}

/* sceRegGetKeyInfoByName(REGHANDLE hd, const char *name, unsigned int *type, SceSize *size) */
static uint32_t h_RegGetKeyInfoByName(CpuState *s) {
    uint32_t type_out = A2, size_out = A3, rc;
    int slot = sreg_cat_slot(A0), node = -1;
    if (slot < 0) return SREG_ERR_BAD_HANDLE;
    if (!type_out || !sr_guest_span_writable(type_out, 4u) || !size_out ||
        !sr_guest_span_writable(size_out, 4u))
        return SREG_ERR_ILLEGAL_ADDR;
    rc = sreg_lookup_entry(slot, A1, "sceRegGetKeyInfoByName", &node);
    if (rc) return rc;
    MEM_W32(type_out, s_sreg_nodes[node].type);
    MEM_W32(size_out, s_sreg_nodes[node].size);
    return 0;
}

/* sceRegGetKeyValue(REGHANDLE hd, REGHANDLE hk, void *buf, SceSize size) */
static uint32_t h_RegGetKeyValue(CpuState *s) {
    int slot = sreg_cat_slot(A0), node;
    if (slot < 0) return SREG_ERR_BAD_HANDLE;
    /* A key handle of another category, or of an entry removed since, is stale. */
    node = sreg_key_by_handle(A1, s_sreg_cat[slot].node);
    if (node < 0) return SREG_ERR_BAD_HANDLE;
    return sreg_copy_out(node, A2, A3, "sceRegGetKeyValue");
}

/* sceRegGetKeyValueByName(REGHANDLE hd, const char *name, void *buf, SceSize size) */
static uint32_t h_RegGetKeyValueByName(CpuState *s) {
    uint32_t rc;
    int slot = sreg_cat_slot(A0), node = -1;
    if (slot < 0) return SREG_ERR_BAD_HANDLE;
    rc = sreg_lookup_entry(slot, A1, "sceRegGetKeyValueByName", &node);
    if (rc) return rc;
    return sreg_copy_out(node, A2, A3, "sceRegGetKeyValueByName");
}

/* sceRegSetKeyValue(REGHANDLE hd, const char *name, const void *buf, SceSize size): the key must
 * exist (a missing one is not found; titles then call sceRegCreateKey). A string takes exactly the
 * bytes given and its size becomes size (measured: a string key's size follows its value). */
static uint32_t h_RegSetKeyValue(CpuState *s) {
    uint32_t buf = A2, size = A3, rc;
    int slot = sreg_cat_slot(A0), node = -1, changed = 0;
    SregNode *n;
    if (slot < 0) return SREG_ERR_BAD_HANDLE;
    if (s_sreg_cat[slot].mode != 2u) return sreg_refuse_read_only("sceRegSetKeyValue");
    rc = sreg_lookup_entry(slot, A1, "sceRegSetKeyValue", &node);
    if (rc) return rc;
    n = &s_sreg_nodes[node];
    if (n->flags & SREG_NODE_READONLY) return sreg_refuse_generated("sceRegSetKeyValue", n->name);
    if (n->type == SREG_TYPE_DIR) return SREG_ERR_FTYPE;
    if (size == 0u) return SREG_ERR_INVAL;
    if (n->type == SREG_TYPE_INT && size != 4u) return SREG_ERR_FTYPE;
    if (size > (n->type == SREG_TYPE_STR ? SREG_VALUE_MAX : n->size)) return SREG_ERR_NOSPC;
    if (!buf || !sr_guest_span_readable(buf, size)) return SREG_ERR_ILLEGAL_ADDR;
    if (n->type == SREG_TYPE_STR && size != n->size) {
        uint8_t *resized = (uint8_t *)realloc(n->value, size);
        if (!resized) return SREG_ERR_NOSPC;
        if (size > n->size) memset(resized + n->size, 0, size - n->size);
        n->value = resized;
        n->size = size;
        changed = 1;
    }
    for (uint32_t i = 0; i < n->size; i++) {
        uint8_t b = i < size ? MEM_R8(buf + i) : 0u;
        if (n->value[i] != b) {
            n->value[i] = b;
            changed = 1;
        }
    }
    n->flags &= (uint8_t)~SREG_NODE_PLACEHOLDER;
    if (changed) s_sreg_dirty = 1;
    if (hle_log_on())
        fprintf(stderr, "sceRegSetKeyValue: \"%s\" <- %u bytes%s\n", n->name, size,
                changed ? "" : " (unchanged)");
    return 0;
}

/* sceRegCreateKey(REGHANDLE hd, const char *name, int type, SceSize size): an INT, STR or BIN key
 * with size bytes of zero-filled value space. A name over 26 bytes is cut to 26, as the console
 * cuts a new category's name (categories are DIR keys in the same name table; inferred). */
static uint32_t h_RegCreateKey(CpuState *s) {
    uint32_t type = A2, size = A3, rc;
    char key[SREG_PATH_BYTES];
    int slot = sreg_cat_slot(A0), cat;
    if (slot < 0) return SREG_ERR_BAD_HANDLE;
    if (s_sreg_cat[slot].mode != 2u) return sreg_refuse_read_only("sceRegCreateKey");
    rc = sreg_guest_name(A1, key, SREG_PATH_BYTES);
    if (rc) return rc;
    key[SREG_NAME_BYTES - 1u] = '\0';
    if (!sreg_name_ok(key)) return SREG_ERR_INVAL;
    if (type != SREG_TYPE_INT && type != SREG_TYPE_STR && type != SREG_TYPE_BIN) return SREG_ERR_INVAL;
    if (size == 0u || (type == SREG_TYPE_INT && size != 4u)) return SREG_ERR_INVAL;
    if (size > SREG_VALUE_MAX) return SREG_ERR_NOSPC;
    cat = s_sreg_cat[slot].node;
    if (s_sreg_nodes[cat].flags & SREG_NODE_READONLY) return sreg_refuse_generated("sceRegCreateKey", key);
    if (sreg_child(cat, key) >= 0) return SREG_ERR_EXIST;
    if (sreg_add(cat, key, type, size, 0u) < 0) return SREG_ERR_NOSPC;
    s_sreg_dirty = 1;
    if (hle_log_on()) fprintf(stderr, "sceRegCreateKey: \"%s\" type %u size %u\n", key, type, size);
    return 0;
}

/* sceRegGetKeysNum(REGHANDLE hd, int *num): the category's keys plus its child categories. */
static uint32_t h_RegGetKeysNum(CpuState *s) {
    static int entries[SREG_MAX_NODES];
    int slot = sreg_cat_slot(A0);
    if (slot < 0) return SREG_ERR_BAD_HANDLE;
    if (!A1 || !sr_guest_span_writable(A1, 4u)) return SREG_ERR_ILLEGAL_ADDR;
    MEM_W32(A1, sreg_ordered(s_sreg_cat[slot].node, entries));
    return 0;
}

/* sceRegGetKeys(REGHANDLE hd, char *buf, int num): the first num entry names in creation order,
 * each NUL-padded to REG_KEYNAME_SIZE (27) bytes. A num below the entry count returns that many
 * names (truncation is a project choice); a negative num is EINVAL. */
static uint32_t h_RegGetKeys(CpuState *s) {
    static int entries[SREG_MAX_NODES];
    uint32_t buf = A1, num = A2, count;
    int slot = sreg_cat_slot(A0);
    if (slot < 0) return SREG_ERR_BAD_HANDLE;
    if ((int32_t)num < 0) return SREG_ERR_INVAL;
    count = sreg_ordered(s_sreg_cat[slot].node, entries);
    if (num < count) count = num;
    if (count == 0u) return 0;
    /* count <= SREG_MAX_NODES, so the byte count cannot overflow. */
    if (!buf || !sr_guest_span_writable(buf, count * SREG_NAME_BYTES)) return SREG_ERR_ILLEGAL_ADDR;
    for (uint32_t i = 0; i < count; i++) {
        const char *name = s_sreg_nodes[entries[i]].name;
        size_t len = strlen(name);
        for (uint32_t j = 0; j < SREG_NAME_BYTES; j++)
            MEM_W8(buf + i * SREG_NAME_BYTES + j, j < len ? (uint8_t)name[j] : 0u);
    }
    return 0;
}

/* sceOpenPSIDGetOpenPSID(SceOpenPSID *psid): the console profile's 16-byte OpenPSID. As with the
 * MAC address, NULL or an unwritable span answers ILLEGAL_ADDR and writes nothing (UNMEASURED). */
static uint32_t h_OpenPSIDGetOpenPSID(CpuState *s) {
    if (!A0 || !sr_guest_span_writable(A0, 16u)) return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (!console_profile_identity())
        return hle_unmodelled_stop("sceOpenPSIDGetOpenPSID", "has no OpenPSID: the host's random "
                                   "source is unavailable");
    for (int i = 0; i < 16; i++) MEM_W8(A0 + (uint32_t)i, s_console.psid[i]);
    return 0;
}

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

/* From hle_register_console_profile_handlers(). */
void hle_register_console_profile_system(void) {
    sr_hle_register(0xc69bebce, "sceOpenPSIDGetOpenPSID", h_OpenPSIDGetOpenPSID);
}

/* From hle_register_sysreg_handlers(). */
void hle_register_sysreg_system(void) {
    sr_hle_register(0x92e41280, "sceRegOpenRegistry", h_RegOpenRegistry);
    sr_hle_register(0xfa8a5739, "sceRegCloseRegistry", h_RegCloseRegistry);
    sr_hle_register(0x39461b4d, "sceRegFlushRegistry", h_RegFlushRegistry);
    sr_hle_register(0x1d8a762e, "sceRegOpenCategory", h_RegOpenCategory);
    sr_hle_register(0x0cae832b, "sceRegCloseCategory", h_RegCloseCategory);
    sr_hle_register(0x0d69bf40, "sceRegFlushCategory", h_RegFlushCategory);
    sr_hle_register(0x4ca16893, "sceRegRemoveCategory", h_RegRemoveCategory);
    sr_hle_register(0xd4475aa8, "sceRegGetKeyInfo", h_RegGetKeyInfo);
    sr_hle_register(0xc5768d02, "sceRegGetKeyInfoByName", h_RegGetKeyInfoByName);
    sr_hle_register(0x28a8e98a, "sceRegGetKeyValue", h_RegGetKeyValue);
    sr_hle_register(0x30be0259, "sceRegGetKeyValueByName", h_RegGetKeyValueByName);
    sr_hle_register(0x17768e14, "sceRegSetKeyValue", h_RegSetKeyValue);
    sr_hle_register(0x57641a81, "sceRegCreateKey", h_RegCreateKey);
    sr_hle_register(0x2c0db9dd, "sceRegGetKeysNum", h_RegGetKeysNum);
    sr_hle_register(0x2d211135, "sceRegGetKeys", h_RegGetKeys);
}
