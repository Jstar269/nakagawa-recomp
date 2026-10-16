// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Read-only walk of the system-font registry subtrees (oracle case registry-font, evidence id
 * PSP-REGISTRY-002).
 *
 * A PSP title's own font library reads /DATA/FONT (path_name, num_fonts, PROPERTY/INFO<n>) and
 * /SYSPROFILE/RESOLUTION. This walk records every key in those two subtrees (name, type, size,
 * value and enumeration order) without ever writing the registry. It is shared by the probe
 * (fixtures/psp_oracle/probe.c, CASE=registry-font) and a host harness
 * (fixtures/psp_oracle/registry_walk_host.c) that runs the same code against a fake registry.
 *
 * Why the rules below exist (measured on PSP-3000 6.6.1, oracle case PSP-REGISTRY-001):
 *  - sceRegOpenCategory on a category that does not exist CREATES it, persistently, even with
 *    mode 1, and stores its name cut to 26 bytes. Such a cut name could not be reopened.
 *  - sceRegGetKeyInfo with a name that does not exist returns 0x8008271D and creates nothing.
 *  - sceRegGetKeysNum with a forged handle returns 0x8008272E and leaves the count untouched.
 *
 * Safety contract (every rule is checked by the host harness tests in tools/test_psp_oracle.py):
 *  - The walk calls only sceRegOpenRegistry, sceRegCloseRegistry, sceRegOpenCategory,
 *    sceRegCloseCategory, sceRegGetKeysNum, sceRegGetKeys, sceRegGetKeyInfo and
 *    sceRegGetKeyValue. No call sets, creates, removes or flushes anything.
 *  - No category is opened by a literal path. The root is never opened: it is enumerated through
 *    the registry handle itself. If the registry handle does not enumerate, nothing is opened.
 *  - The registry handle's listing is taken as the root only when it lists CONFIG and
 *    sceRegGetKeyInfo reports CONFIG as a directory: /CONFIG is the one root category measured
 *    to exist (PSP-REGISTRY-001). Otherwise no category below the listing is opened.
 *  - A category is opened only after it was listed by sceRegGetKeys of its already-open parent,
 *    sceRegGetKeyInfo on that parent reported it as a directory, and its name is 1 to 25 bytes
 *    of [A-Za-z0-9_.-] (so it round-trips through the 26-byte stored-name limit and cannot be
 *    a path). Its path is the parent's path plus that listed name, so every opened category
 *    existed when it was listed.
 *  - Only the target routes are entered: /DATA, then /DATA/FONT and everything below it, and
 *    /SYSPROFILE, then /SYSPROFILE/RESOLUTION and everything below it. Values are read only
 *    inside /DATA/FONT and /SYSPROFILE/RESOLUTION; every other key records name, type and size.
 *  - sceRegGetKeyInfo and sceRegGetKeyValue only ever receive names the same handle listed.
 *  - Every loop is bounded (keys per category, depth, categories opened, value size), and every
 *    buffer is static: the probe starts on a small stack.
 *
 * The includer provides REGHANDLE, SceSize, REG_KEYNAME_SIZE, REG_TYPE_DIR, SYSTEM_REGISTRY,
 * struct RegParam and the eight calls above (pspreg.h on the PSP; the fake on the host). */

#ifndef NAKAGAWA_REGISTRY_WALK_H
#define NAKAGAWA_REGISTRY_WALK_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define REGWALK_TEST_ID "PSP-REGISTRY-002"
/* Stored names are cut to 26 bytes; a name of 26 bytes or more may be a cut copy. */
#define REGWALK_NAME_MAX 26u
#define REGWALK_KEYS_MAX 64u
/* The root is depth 0; /DATA/FONT/PROPERTY/INFO<n> is depth 4. */
#define REGWALK_DEPTH_MAX 6u
#define REGWALK_OPEN_MAX 96u
#define REGWALK_VALUE_MAX 1024u
#define REGWALK_PATH_MAX 192u
#define REGWALK_LINE_MAX 4096u
#define REGWALK_OPEN_MODE 1
#define REGWALK_ERROR 0xffffffffu
/* Each opened category (and the root) yields at most REGWALK_KEYS_MAX key steps plus one pop. */
#define REGWALK_ITERATIONS_MAX ((REGWALK_OPEN_MAX + 1u) * (REGWALK_KEYS_MAX + 1u))

_Static_assert(1u + REGWALK_DEPTH_MAX * REGWALK_NAME_MAX < REGWALK_PATH_MAX,
               "a path of REGWALK_DEPTH_MAX reopenable names must fit REGWALK_PATH_MAX");
_Static_assert(512u + REGWALK_PATH_MAX + 2u * REG_KEYNAME_SIZE + 2u * REGWALK_VALUE_MAX <
                   REGWALK_LINE_MAX,
               "a key record with its value must fit one line");

/* out3 of a font-key record: what the walk did with the listed name. */
enum {
    REGWALK_KEY_METADATA = 0,     /* leaf outside the value subtrees: name, type, size only */
    REGWALK_KEY_VALUE = 1,        /* leaf inside a value subtree: value read, out4 = result */
    REGWALK_KEY_OPENED = 2,       /* directory on a target route: opened, its category follows */
    REGWALK_KEY_OFF_ROUTE = 3,    /* directory off the target routes: never opened */
    REGWALK_KEY_NAME_REFUSED = 4, /* directory whose name may not round-trip: never opened */
    REGWALK_KEY_DEPTH_LIMIT = 5,  /* directory below REGWALK_DEPTH_MAX: never opened */
    REGWALK_KEY_OPEN_LIMIT = 6,   /* REGWALK_OPEN_MAX categories already opened: never opened */
    REGWALK_KEY_VALUE_SIZE = 7,   /* leaf in a value subtree, size 0 or above REGWALK_VALUE_MAX */
    REGWALK_KEY_INFO_FAILED = 8,  /* sceRegGetKeyInfo failed: type unknown, nothing opened */
    REGWALK_KEY_NAME_UNSAFE = 9,  /* listed slot not a safe NUL-terminated name: never queried */
    REGWALK_KEY_ROOT_UNVERIFIED = 10, /* root directory on a route, but the listing did not show
                                       * CONFIG as a directory: not known to be the root */
};

/* The root category measured to exist; its presence identifies the registry handle's listing. */
#define REGWALK_ROOT_ANCHOR "CONFIG"

typedef struct RegWalk {
    void (*emit)(void *ctx, const char *line, size_t length);
    void (*step)(void *ctx, const char *step);
    void *ctx;
    uint32_t categories;      /* font-category records */
    uint32_t keys;            /* font-key records */
    uint32_t records;         /* every record emitted */
    uint32_t open_attempts;   /* sceRegOpenCategory calls */
    uint32_t opened;          /* sceRegOpenCategory calls that succeeded */
    uint32_t refused;         /* directories refused by the name, depth, open or root rule */
    uint32_t root_enumerated; /* 1 when the registry handle listed something */
    uint32_t root_verified;   /* 1 when that listing showed REGWALK_ROOT_ANCHOR as a directory */
} RegWalk;

typedef struct RegWalkLevel {
    REGHANDLE handle;
    uint32_t count;
    uint32_t next;
    int route;      /* index into s_regwalk_routes, -1 at the root */
    int in_subtree; /* 1 inside /DATA/FONT or /SYSPROFILE/RESOLUTION: values are read */
    char path[REGWALK_PATH_MAX];
    char names[REGWALK_KEYS_MAX * REG_KEYNAME_SIZE];
} RegWalkLevel;

/* Target routes: a directory is entered only on a route; below a route's last name everything
 * is entered and every value is read. */
#define REGWALK_ROUTE_LENGTH 2u
static const char *const s_regwalk_routes[][REGWALK_ROUTE_LENGTH] = {
    {"DATA", "FONT"},
    {"SYSPROFILE", "RESOLUTION"},
};
#define REGWALK_ROUTES (sizeof(s_regwalk_routes) / sizeof(s_regwalk_routes[0]))

static RegWalkLevel s_regwalk_levels[REGWALK_DEPTH_MAX + 1u];
static char s_regwalk_line[REGWALK_LINE_MAX];
static uint8_t s_regwalk_value[REGWALK_VALUE_MAX];
static char s_regwalk_step[REGWALK_PATH_MAX + 16u];

/* A listed name is usable only when it is NUL-terminated inside its REG_KEYNAME_SIZE slot and
 * made of [A-Za-z0-9_.-]: no '/', no space, nothing a path or a record could misread. */
static size_t regwalk_safe_name_length(const char *slot) {
    const char *end = (const char *)memchr(slot, '\0', REG_KEYNAME_SIZE);
    if (end == NULL || end == slot) return 0u;
    for (const char *c = slot; c < end; c++) {
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
              (*c >= '0' && *c <= '9') || *c == '_' || *c == '-' || *c == '.')) return 0u;
    }
    return (size_t)(end - slot);
}

/* A directory may be opened only when its name round-trips: shorter than the stored-name limit,
 * and not one of the project's own stray test categories. */
static int regwalk_name_reopenable(const char *name, size_t length) {
    if (length == 0u || length >= REGWALK_NAME_MAX) return 0;
    if (strncmp(name, "__NAKAGAWA", 10) == 0) return 0;
    return 1;
}

/* Join a parent path and a listed safe name. The root's path is "/". */
static int regwalk_join(char *out, const char *parent, const char *name, size_t name_length) {
    size_t parent_length = strlen(parent);
    int root = parent_length == 1u && parent[0] == '/';
    size_t total = (root ? 1u : parent_length + 1u) + name_length;
    if (total >= REGWALK_PATH_MAX) return 0;
    if (root) {
        out[0] = '/';
        memcpy(out + 1, name, name_length);
    } else {
        memcpy(out, parent, parent_length);
        out[parent_length] = '/';
        memcpy(out + parent_length + 1u, name, name_length);
    }
    out[total] = '\0';
    return 1;
}

static void regwalk_emit(RegWalk *w, int used) {
    if (used <= 0 || (size_t)used + 1u >= REGWALK_LINE_MAX) return;
    s_regwalk_line[used++] = '\n';
    s_regwalk_line[used] = '\0';
    w->emit(w->ctx, s_regwalk_line, (size_t)used);
    w->records++;
}

static int regwalk_append(int used, const char *text) {
    if (used <= 0) return used;
    size_t length = strlen(text);
    if ((size_t)used + length >= REGWALK_LINE_MAX) return -1;
    memcpy(s_regwalk_line + used, text, length + 1u);
    return used + (int)length;
}

static int regwalk_append_hex(int used, const char *field, const uint8_t *bytes, size_t count) {
    static const char digits[] = "0123456789abcdef";
    used = regwalk_append(used, field);
    if (used <= 0 || (size_t)used + 2u * count >= REGWALK_LINE_MAX) return -1;
    for (size_t i = 0; i < count; i++) {
        s_regwalk_line[used++] = digits[bytes[i] >> 4];
        s_regwalk_line[used++] = digits[bytes[i] & 0x0fu];
    }
    s_regwalk_line[used] = '\0';
    return used;
}

static void regwalk_step(RegWalk *w, const char *prefix, const char *path) {
    int used = snprintf(s_regwalk_step, sizeof(s_regwalk_step), "%s%s", prefix, path);
    if (used > 0 && (size_t)used < sizeof(s_regwalk_step)) w->step(w->ctx, s_regwalk_step);
}

/* font-category-NNNN: result = sceRegOpenCategory result (0 for the root, which is never
 * opened), out0 = sceRegGetKeysNum result, out1 = key count, out2 = sceRegGetKeys result
 * (0xffffffff when not called), out3 = depth. */
static void regwalk_emit_category(RegWalk *w, int ok, uint32_t open_rc, uint32_t count_rc,
                                  uint32_t count, uint32_t keys_rc, uint32_t depth,
                                  const char *path) {
    int used = snprintf(s_regwalk_line, REGWALK_LINE_MAX,
        "NAKAGAWA_PSP_TEST schema=1 test_id=" REGWALK_TEST_ID " case_id=font-category-%04u "
        "status=%s result=0x%08x out0=0x%08x out1=0x%08x out2=0x%08x out3=0x%08x detail=",
        (unsigned int)w->categories, ok ? "PASS" : "FAIL", (unsigned int)open_rc,
        (unsigned int)count_rc, (unsigned int)count, (unsigned int)keys_rc,
        (unsigned int)depth);
    if (used <= 0 || (size_t)used >= REGWALK_LINE_MAX) return;
    used = regwalk_append(used, path);
    regwalk_emit(w, used);
    w->categories++;
}

/* font-key-NNNN: result = sceRegGetKeyInfo result (0xffffffff when not called), out0 = type,
 * out1 = size, out2 = index in the parent's sceRegGetKeys order, out3 = REGWALK_KEY_*, out4 =
 * sceRegGetKeyValue result (0xffffffff when not called), detail = path (the parent's path for
 * an unsafe name, whose whole REG_KEYNAME_SIZE listing slot then follows as name_hex),
 * value_hex when a value was read. */
static void regwalk_emit_key(RegWalk *w, uint32_t info_rc, uint32_t type, uint32_t size,
                             uint32_t index, uint32_t disposition, uint32_t value_rc,
                             const char *detail, const uint8_t *name_bytes, size_t name_count,
                             const uint8_t *value, size_t value_count) {
    int ok = info_rc == 0u && disposition != REGWALK_KEY_NAME_UNSAFE;
    int used = snprintf(s_regwalk_line, REGWALK_LINE_MAX,
        "NAKAGAWA_PSP_TEST schema=1 test_id=" REGWALK_TEST_ID " case_id=font-key-%04u "
        "status=%s result=0x%08x out0=0x%08x out1=0x%08x out2=0x%08x out3=0x%08x "
        "out4=0x%08x detail=",
        (unsigned int)w->keys, ok ? "PASS" : "FAIL", (unsigned int)info_rc,
        (unsigned int)type, (unsigned int)size, (unsigned int)index,
        (unsigned int)disposition, (unsigned int)value_rc);
    if (used <= 0 || (size_t)used >= REGWALK_LINE_MAX) return;
    used = regwalk_append(used, detail);
    if (name_bytes != NULL) used = regwalk_append_hex(used, " name_hex=", name_bytes, name_count);
    if (value != NULL) used = regwalk_append_hex(used, " value_hex=", value, value_count);
    regwalk_emit(w, used);
    w->keys++;
}

/* Count and list one open category (or the root, through the registry handle). Returns 1 when
 * the listing is usable: both calls succeeded and the count is within REGWALK_KEYS_MAX. */
static int regwalk_enumerate(RegWalk *w, RegWalkLevel *level, uint32_t depth, uint32_t open_rc) {
    int count = -1;
    if (depth == 0u) w->step(w->ctx, "root-count");
    int count_rc = sceRegGetKeysNum(level->handle, &count);
    uint32_t keys_rc = REGWALK_ERROR;
    int ok = 0;
    level->count = 0u;
    level->next = 0u;
    if (count_rc == 0 && count >= 0 && (uint32_t)count <= REGWALK_KEYS_MAX) {
        memset(level->names, 0, sizeof(level->names));
        if (count == 0) {
            ok = 1;
        } else {
            if (depth == 0u) w->step(w->ctx, "root-keys");
            keys_rc = (uint32_t)sceRegGetKeys(level->handle, level->names, count);
            ok = keys_rc == 0u;
        }
        if (ok) level->count = (uint32_t)count;
    }
    regwalk_emit_category(w, ok, open_rc, (uint32_t)count_rc,
                          count < 0 ? REGWALK_ERROR : (uint32_t)count, keys_rc, depth,
                          level->path);
    return ok;
}

/* Whether a listed directory at child depth (depth + 1) is on a target route. */
static int regwalk_route_for(const RegWalkLevel *parent, uint32_t depth, const char *name,
                             int *route, int *in_subtree) {
    if (parent->in_subtree) {
        *route = parent->route;
        *in_subtree = 1;
        return 1;
    }
    if (depth >= REGWALK_ROUTE_LENGTH) return 0;
    for (size_t r = 0; r < REGWALK_ROUTES; r++) {
        if (parent->route >= 0 && (size_t)parent->route != r) continue;
        if (strcmp(s_regwalk_routes[r][depth], name) == 0) {
            *route = (int)r;
            *in_subtree = depth + 1u == REGWALK_ROUTE_LENGTH;
            return 1;
        }
    }
    return 0;
}

static void regwalk_walk(RegWalk *w, REGHANDLE registry) {
    RegWalkLevel *root = &s_regwalk_levels[0];
    root->handle = registry;
    root->route = -1;
    root->in_subtree = 0;
    root->path[0] = '/';
    root->path[1] = '\0';
    w->root_enumerated = (uint32_t)regwalk_enumerate(w, root, 0u, 0u);
    w->root_verified = 0u;
    for (uint32_t i = 0; i < root->count; i++) {
        const char *slot = root->names + (size_t)i * REG_KEYNAME_SIZE;
        if (regwalk_safe_name_length(slot) == 0u || strcmp(slot, REGWALK_ROOT_ANCHOR) != 0) continue;
        REGHANDLE anchor = 0;
        unsigned int anchor_type = 0;
        SceSize anchor_size = 0;
        w->step(w->ctx, "root-verify");
        w->root_verified = sceRegGetKeyInfo(registry, slot, &anchor, &anchor_type,
                                            &anchor_size) == 0 &&
                           anchor_type == (unsigned int)REG_TYPE_DIR;
        break;
    }
    uint32_t depth = 0u;
    char detail[REGWALK_PATH_MAX];
    for (uint32_t iteration = 0; iteration < REGWALK_ITERATIONS_MAX; iteration++) {
        RegWalkLevel *level = &s_regwalk_levels[depth];
        if (level->next >= level->count) {
            if (depth == 0u) break;
            (void)sceRegCloseCategory(level->handle);
            depth--;
            continue;
        }
        const uint32_t index = level->next++;
        const char *slot = level->names + (size_t)index * REG_KEYNAME_SIZE;
        const size_t name_length = regwalk_safe_name_length(slot);
        if (name_length == 0u || !regwalk_join(detail, level->path, slot, name_length)) {
            regwalk_emit_key(w, REGWALK_ERROR, REGWALK_ERROR, REGWALK_ERROR, index,
                             REGWALK_KEY_NAME_UNSAFE, REGWALK_ERROR, level->path,
                             (const uint8_t *)slot, (size_t)REG_KEYNAME_SIZE, NULL, 0u);
            continue;
        }
        if (depth == 0u) regwalk_step(w, "root-info:", detail);
        REGHANDLE key = 0;
        unsigned int type = 0;
        SceSize size = 0;
        const int info_rc = sceRegGetKeyInfo(level->handle, slot, &key, &type, &size);
        if (info_rc != 0) {
            regwalk_emit_key(w, (uint32_t)info_rc, REGWALK_ERROR, REGWALK_ERROR, index,
                             REGWALK_KEY_INFO_FAILED, REGWALK_ERROR, detail, NULL, 0u, NULL, 0u);
            continue;
        }
        if (type != (unsigned int)REG_TYPE_DIR) {
            if (!level->in_subtree) {
                regwalk_emit_key(w, 0u, (uint32_t)type, (uint32_t)size, index,
                                 REGWALK_KEY_METADATA, REGWALK_ERROR, detail, NULL, 0u, NULL, 0u);
            } else if (size == 0u || (uint32_t)size > REGWALK_VALUE_MAX) {
                regwalk_emit_key(w, 0u, (uint32_t)type, (uint32_t)size, index,
                                 REGWALK_KEY_VALUE_SIZE, REGWALK_ERROR, detail, NULL, 0u, NULL, 0u);
            } else {
                memset(s_regwalk_value, 0, sizeof(s_regwalk_value));
                const int value_rc = sceRegGetKeyValue(level->handle, key, s_regwalk_value, size);
                regwalk_emit_key(w, 0u, (uint32_t)type, (uint32_t)size, index,
                                 REGWALK_KEY_VALUE, (uint32_t)value_rc, detail, NULL, 0u,
                                 value_rc == 0 ? s_regwalk_value : NULL,
                                 value_rc == 0 ? (size_t)size : 0u);
            }
            continue;
        }
        int child_route = -1;
        int child_subtree = 0;
        uint32_t disposition = REGWALK_KEY_OPENED;
        if (!regwalk_route_for(level, depth, slot, &child_route, &child_subtree)) {
            disposition = REGWALK_KEY_OFF_ROUTE;
        } else if (depth == 0u && !w->root_verified) {
            disposition = REGWALK_KEY_ROOT_UNVERIFIED;
        } else if (!regwalk_name_reopenable(slot, name_length)) {
            disposition = REGWALK_KEY_NAME_REFUSED;
        } else if (depth + 1u > REGWALK_DEPTH_MAX) {
            disposition = REGWALK_KEY_DEPTH_LIMIT;
        } else if (w->open_attempts >= REGWALK_OPEN_MAX) {
            disposition = REGWALK_KEY_OPEN_LIMIT;
        }
        regwalk_emit_key(w, 0u, (uint32_t)type, (uint32_t)size, index, disposition,
                         REGWALK_ERROR, detail, NULL, 0u, NULL, 0u);
        if (disposition != REGWALK_KEY_OPENED) {
            if (disposition != REGWALK_KEY_OFF_ROUTE) w->refused++;
            continue;
        }
        RegWalkLevel *child = &s_regwalk_levels[depth + 1u];
        memcpy(child->path, detail, sizeof(child->path));
        child->route = child_route;
        child->in_subtree = child_subtree;
        child->count = 0u;
        child->next = 0u;
        regwalk_step(w, "open:", child->path);
        REGHANDLE handle = 0;
        w->open_attempts++;
        const int open_rc = sceRegOpenCategory(registry, child->path, REGWALK_OPEN_MODE, &handle);
        if (open_rc < 0) {
            regwalk_emit_category(w, 0, (uint32_t)open_rc, REGWALK_ERROR, REGWALK_ERROR,
                                  REGWALK_ERROR, depth + 1u, child->path);
            continue;
        }
        w->opened++;
        child->handle = handle;
        if (regwalk_enumerate(w, child, depth + 1u, (uint32_t)open_rc)) {
            depth++;
        } else {
            (void)sceRegCloseCategory(handle);
        }
    }
    /* Only reachable with the iteration bound exhausted, which the bound's own arithmetic
     * excludes; close whatever is still open rather than leak handles. */
    while (depth > 0u) {
        (void)sceRegCloseCategory(s_regwalk_levels[depth].handle);
        depth--;
    }
}

/* Open the system registry read-only, walk it, close it. Records: font-registry-open, the
 * walk's font-category/font-key records in enumeration order, then font-registry-done
 * (result = sceRegCloseRegistry result, out0 categories, out1 keys, out2 records before it,
 * out3 categories opened, out4 directories refused, out5 1 when the registry handle listed
 * something, out6 1 when that listing showed CONFIG as a directory). */
static void regwalk_run(RegWalk *w) {
    static struct RegParam param;
    memset(&param, 0, sizeof(param));
    param.regtype = 1u;
    memcpy(param.name, SYSTEM_REGISTRY, sizeof(SYSTEM_REGISTRY));
    param.namelen = sizeof(SYSTEM_REGISTRY) - 1u;
    param.unk2 = 1u;
    param.unk3 = 1u;
    REGHANDLE registry = 0;
    w->step(w->ctx, "open-registry");
    const int open_rc = sceRegOpenRegistry(&param, REGWALK_OPEN_MODE, &registry);
    int used = snprintf(s_regwalk_line, REGWALK_LINE_MAX,
        "NAKAGAWA_PSP_TEST schema=1 test_id=" REGWALK_TEST_ID " case_id=font-registry-open "
        "status=%s result=0x%08x out0=0x%08x out1=0x%08x",
        open_rc == 0 ? "PASS" : "FAIL", (unsigned int)open_rc,
        (unsigned int)REGWALK_OPEN_MODE, (unsigned int)param.regtype);
    regwalk_emit(w, used);
    uint32_t close_rc = REGWALK_ERROR;
    if (open_rc == 0) {
        regwalk_walk(w, registry);
        w->step(w->ctx, "close-registry");
        close_rc = (uint32_t)sceRegCloseRegistry(registry);
    }
    used = snprintf(s_regwalk_line, REGWALK_LINE_MAX,
        "NAKAGAWA_PSP_TEST schema=1 test_id=" REGWALK_TEST_ID " case_id=font-registry-done "
        "status=PASS result=0x%08x out0=0x%08x out1=0x%08x out2=0x%08x out3=0x%08x "
        "out4=0x%08x out5=0x%08x out6=0x%08x",
        (unsigned int)close_rc, (unsigned int)w->categories, (unsigned int)w->keys,
        (unsigned int)w->records, (unsigned int)w->opened, (unsigned int)w->refused,
        (unsigned int)w->root_enumerated, (unsigned int)w->root_verified);
    regwalk_emit(w, used);
}

#endif /* NAKAGAWA_REGISTRY_WALK_H */
