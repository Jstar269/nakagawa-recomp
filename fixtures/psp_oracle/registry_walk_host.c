// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Host harness for registry_walk.h: runs the probe's font-registry walk against a fake registry
 * and logs every registry call, so tools/test_psp_oracle.py can check the walk's safety rules
 * without a PSP.
 *
 * The fake follows the behaviour measured on PSP-3000 6.6.1 (PSP-REGISTRY-001):
 * sceRegOpenCategory of a missing category CREATES it with its name cut to 26 bytes (the
 * harness logs CREATED, which the tests treat as a failure), sceRegGetKeyInfo of a missing name
 * returns 0x8008271D, and a handle the fake does not know returns 0x8008272E. Whether the
 * registry handle itself lists the root is the unmeasured question the probe asks the console,
 * so it is a scenario option here.
 *
 * Only the eight read calls are defined. A walk that called any other sceReg function would not
 * compile (implicit declarations are errors under -Werror).
 *
 * Input (stdin), one directive per line:
 *   option root-lists 0|1          the registry handle lists the root (default 0)
 *   option open-fails <path>       sceRegOpenCategory of <path> returns 0x80082710
 *   option info-fails <path>       sceRegGetKeyInfo of <path> returns 0x80082711
 *   option value-fails <path>      sceRegGetKeyValue of <path> returns 0x80082712
 *   node <type> <parent> <name-hex> [<value-hex>]
 * <type> is a decimal key type (1 = directory), <parent> an existing directory path ("/" for the
 * root, names joined by '/'), <name-hex> the exact name bytes (up to 27; 27 bytes means no NUL
 * in the listed slot; "-" is empty), <value-hex> the value bytes ("-" is empty). Output
 * (stdout): CALL, LISTED, INFO, EMIT and STEP lines, then a SUMMARY line. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int REGHANDLE;
typedef unsigned int SceSize;
#define SYSTEM_REGISTRY "/system"
#define REG_KEYNAME_SIZE 27
enum { REG_TYPE_DIR = 1, REG_TYPE_INT = 2, REG_TYPE_STR = 3, REG_TYPE_BIN = 4 };
struct RegParam {
    unsigned int regtype;
    char name[256];
    unsigned int namelen;
    unsigned int unk2;
    unsigned int unk3;
};

int sceRegOpenRegistry(struct RegParam *reg, int mode, REGHANDLE *h);
int sceRegCloseRegistry(REGHANDLE h);
int sceRegOpenCategory(REGHANDLE h, const char *name, int mode, REGHANDLE *hd);
int sceRegCloseCategory(REGHANDLE hd);
int sceRegGetKeysNum(REGHANDLE hd, int *num);
int sceRegGetKeys(REGHANDLE hd, char *buf, int num);
int sceRegGetKeyInfo(REGHANDLE hd, const char *name, REGHANDLE *hk, unsigned int *type,
                     SceSize *size);
int sceRegGetKeyValue(REGHANDLE hd, REGHANDLE hk, void *buf, SceSize size);

#include "registry_walk.h"

#define FAKE_NODES 4096
#define FAKE_VALUE 2048
#define FAKE_SLOTS 64
#define FAKE_PATHS 16
#define FAKE_NOT_FOUND ((int)0x8008271D)
#define FAKE_BAD_HANDLE ((int)0x8008272E)
#define FAKE_REGISTRY_HANDLE 0x52470001u
#define FAKE_CATEGORY_TAG 0x43410000u
#define FAKE_KEY_TAG 0x4b000000u

typedef struct FakeNode {
    int parent;
    unsigned char name[REG_KEYNAME_SIZE];
    size_t name_bytes;
    unsigned int type;
    unsigned char value[FAKE_VALUE];
    size_t value_bytes;
} FakeNode;

static FakeNode s_nodes[FAKE_NODES];
static int s_node_count;
static int s_root_lists;
static int s_slots[FAKE_SLOTS]; /* node index + 1 while a category handle is open */
static int s_registry_open;
static int s_created;
static char s_open_fails[FAKE_PATHS][REGWALK_PATH_MAX];
static char s_info_fails[FAKE_PATHS][REGWALK_PATH_MAX];
static char s_value_fails[FAKE_PATHS][REGWALK_PATH_MAX];
static int s_open_fail_count;
static int s_info_fail_count;
static int s_value_fail_count;

static size_t fake_name_length(const FakeNode *n) {
    const unsigned char *end = (const unsigned char *)memchr(n->name, '\0', n->name_bytes);
    return end == NULL ? n->name_bytes : (size_t)(end - n->name);
}

static void fake_path(int node, char *out, size_t capacity) {
    int chain[REGWALK_DEPTH_MAX * 4];
    int depth = 0;
    for (int n = node; n > 0 && depth < (int)(sizeof(chain) / sizeof(chain[0])); n = s_nodes[n].parent) {
        chain[depth++] = n;
    }
    size_t used = 0;
    out[0] = '\0';
    if (depth == 0) {
        snprintf(out, capacity, "/");
        return;
    }
    for (int i = depth - 1; i >= 0; i--) {
        const FakeNode *n = &s_nodes[chain[i]];
        int wrote = snprintf(out + used, capacity - used, "/%.*s",
                             (int)fake_name_length(n), n->name);
        if (wrote < 0 || (size_t)wrote >= capacity - used) return;
        used += (size_t)wrote;
    }
}

static int fake_child(int parent, const char *name, size_t length) {
    for (int i = 1; i < s_node_count; i++) {
        if (s_nodes[i].parent != parent) continue;
        size_t stored = fake_name_length(&s_nodes[i]);
        if (stored == length && memcmp(s_nodes[i].name, name, length) == 0) return i;
    }
    return -1;
}

static int fake_children(int parent) {
    int count = 0;
    for (int i = 1; i < s_node_count; i++) {
        if (s_nodes[i].parent == parent) count++;
    }
    return count;
}

/* Resolve "/A/B" to a node. With create set, a missing component is created with its name cut
 * to 26 bytes, as the console does for sceRegOpenCategory. */
static int fake_resolve(const char *path, int create) {
    int node = 0;
    const char *p = path;
    if (*p == '/') p++;
    while (*p != '\0') {
        const char *end = strchr(p, '/');
        size_t length = end == NULL ? strlen(p) : (size_t)(end - p);
        int child = fake_child(node, p, length);
        if (child < 0) {
            if (!create || s_node_count >= FAKE_NODES) return -1;
            child = s_node_count++;
            memset(&s_nodes[child], 0, sizeof(s_nodes[child]));
            s_nodes[child].parent = node;
            s_nodes[child].type = REG_TYPE_DIR;
            s_nodes[child].name_bytes = length > 26u ? 26u : length;
            memcpy(s_nodes[child].name, p, s_nodes[child].name_bytes);
            s_created++;
            printf("CREATED %.*s\n", (int)length, p);
        }
        node = child;
        if (end == NULL) break;
        p = end + 1;
    }
    return node;
}

static int fake_listed(char lists[][REGWALK_PATH_MAX], int count, const char *path) {
    for (int i = 0; i < count; i++) {
        if (strcmp(lists[i], path) == 0) return 1;
    }
    return 0;
}

/* The directory a handle names: 0 for the registry handle when it lists the root. */
static int fake_handle_node(REGHANDLE handle) {
    if (handle == FAKE_REGISTRY_HANDLE && s_registry_open) return s_root_lists ? 0 : -1;
    if ((handle & 0xffff0000u) != FAKE_CATEGORY_TAG) return -1;
    unsigned int slot = handle & 0xffffu;
    if (slot >= FAKE_SLOTS || s_slots[slot] == 0) return -1;
    return s_slots[slot] - 1;
}

int sceRegOpenRegistry(struct RegParam *reg, int mode, REGHANDLE *h) {
    printf("CALL sceRegOpenRegistry mode=%d regtype=%u name=%s\n", mode, reg->regtype, reg->name);
    s_registry_open++;
    *h = FAKE_REGISTRY_HANDLE;
    return 0;
}

int sceRegCloseRegistry(REGHANDLE h) {
    printf("CALL sceRegCloseRegistry handle=0x%08x\n", h);
    if (h != FAKE_REGISTRY_HANDLE || s_registry_open == 0) return FAKE_BAD_HANDLE;
    s_registry_open--;
    return 0;
}

int sceRegOpenCategory(REGHANDLE h, const char *name, int mode, REGHANDLE *hd) {
    printf("CALL sceRegOpenCategory path=%s mode=%d\n", name, mode);
    if (h != FAKE_REGISTRY_HANDLE || s_registry_open == 0) return FAKE_BAD_HANDLE;
    if (fake_listed(s_open_fails, s_open_fail_count, name)) return (int)0x80082710;
    int node = fake_resolve(name, 1);
    if (node <= 0 || s_nodes[node].type != REG_TYPE_DIR) {
        printf("OPEN-NOT-A-CATEGORY %s\n", name);
        return FAKE_NOT_FOUND;
    }
    for (unsigned int slot = 0; slot < FAKE_SLOTS; slot++) {
        if (s_slots[slot] == 0) {
            s_slots[slot] = node + 1;
            *hd = FAKE_CATEGORY_TAG | slot;
            return 0;
        }
    }
    return (int)0x80082713;
}

int sceRegCloseCategory(REGHANDLE hd) {
    printf("CALL sceRegCloseCategory handle=0x%08x\n", hd);
    if ((hd & 0xffff0000u) != FAKE_CATEGORY_TAG) return FAKE_BAD_HANDLE;
    unsigned int slot = hd & 0xffffu;
    if (slot >= FAKE_SLOTS || s_slots[slot] == 0) return FAKE_BAD_HANDLE;
    s_slots[slot] = 0;
    return 0;
}

int sceRegGetKeysNum(REGHANDLE hd, int *num) {
    int node = fake_handle_node(hd);
    char path[REGWALK_PATH_MAX];
    if (node >= 0) fake_path(node, path, sizeof(path));
    printf("CALL sceRegGetKeysNum handle=0x%08x category=%s\n", hd, node >= 0 ? path : "-");
    if (node < 0) return FAKE_BAD_HANDLE;
    *num = fake_children(node);
    return 0;
}

int sceRegGetKeys(REGHANDLE hd, char *buf, int num) {
    int node = fake_handle_node(hd);
    char path[REGWALK_PATH_MAX];
    if (node >= 0) fake_path(node, path, sizeof(path));
    printf("CALL sceRegGetKeys handle=0x%08x category=%s num=%d\n", hd, node >= 0 ? path : "-", num);
    if (node < 0) return FAKE_BAD_HANDLE;
    int written = 0;
    for (int i = 1; i < s_node_count && written < num; i++) {
        if (s_nodes[i].parent != node) continue;
        char *slot = buf + (size_t)written * REG_KEYNAME_SIZE;
        memset(slot, 0, REG_KEYNAME_SIZE);
        memcpy(slot, s_nodes[i].name, s_nodes[i].name_bytes);
        printf("LISTED category=%s name_hex=", path);
        for (size_t b = 0; b < s_nodes[i].name_bytes; b++) printf("%02x", s_nodes[i].name[b]);
        printf("\n");
        written++;
    }
    return 0;
}

int sceRegGetKeyInfo(REGHANDLE hd, const char *name, REGHANDLE *hk, unsigned int *type,
                     SceSize *size) {
    int node = fake_handle_node(hd);
    char path[REGWALK_PATH_MAX];
    if (node >= 0) fake_path(node, path, sizeof(path));
    printf("CALL sceRegGetKeyInfo handle=0x%08x category=%s name=%s\n", hd,
           node >= 0 ? path : "-", name);
    if (node < 0) return FAKE_BAD_HANDLE;
    int key = fake_child(node, name, strlen(name));
    if (key < 0) return FAKE_NOT_FOUND;
    char key_path[REGWALK_PATH_MAX];
    fake_path(key, key_path, sizeof(key_path));
    if (fake_listed(s_info_fails, s_info_fail_count, key_path)) return (int)0x80082711;
    *hk = FAKE_KEY_TAG | (unsigned int)key;
    *type = s_nodes[key].type;
    *size = s_nodes[key].type == REG_TYPE_DIR ? 0u : (SceSize)s_nodes[key].value_bytes;
    printf("INFO path=%s type=%u size=%u\n", key_path, *type, *size);
    return 0;
}

int sceRegGetKeyValue(REGHANDLE hd, REGHANDLE hk, void *buf, SceSize size) {
    int node = fake_handle_node(hd);
    int key = (int)(hk & 0x00ffffffu);
    char key_path[REGWALK_PATH_MAX] = "-";
    if ((hk & 0xff000000u) == FAKE_KEY_TAG && key > 0 && key < s_node_count) {
        fake_path(key, key_path, sizeof(key_path));
    }
    printf("CALL sceRegGetKeyValue handle=0x%08x key=%s size=%u\n", hd, key_path, size);
    if (node < 0 || (hk & 0xff000000u) != FAKE_KEY_TAG || key <= 0 || key >= s_node_count ||
        s_nodes[key].parent != node) return FAKE_BAD_HANDLE;
    if (fake_listed(s_value_fails, s_value_fail_count, key_path)) return (int)0x80082712;
    size_t copy = size < s_nodes[key].value_bytes ? size : s_nodes[key].value_bytes;
    memcpy(buf, s_nodes[key].value, copy);
    return 0;
}

static void harness_emit(void *ctx, const char *line, size_t length) {
    (void)ctx;
    printf("EMIT %.*s", (int)length, line);
}

static void harness_step(void *ctx, const char *step) {
    (void)ctx;
    printf("STEP %s\n", step);
}

static int hex_bytes(const char *hex, unsigned char *out, size_t capacity, size_t *count) {
    if (strcmp(hex, "-") == 0) {
        *count = 0u;
        return 1;
    }
    size_t length = strlen(hex);
    if (length % 2u != 0u || length / 2u > capacity) return 0;
    for (size_t i = 0; i < length / 2u; i++) {
        unsigned int byte = 0;
        if (sscanf(hex + 2u * i, "%2x", &byte) != 1) return 0;
        out[i] = (unsigned char)byte;
    }
    *count = length / 2u;
    return 1;
}

static int add_path_option(char lists[][REGWALK_PATH_MAX], int *count, const char *path) {
    if (*count >= FAKE_PATHS || strlen(path) >= REGWALK_PATH_MAX) return 0;
    snprintf(lists[*count], REGWALK_PATH_MAX, "%s", path);
    (*count)++;
    return 1;
}

static int load_scenario(FILE *input) {
    static char line[8192];
    memset(&s_nodes[0], 0, sizeof(s_nodes[0]));
    s_nodes[0].type = REG_TYPE_DIR;
    s_node_count = 1;
    while (fgets(line, sizeof(line), input) != NULL) {
        char word[32], a[REGWALK_PATH_MAX], b[REGWALK_PATH_MAX], c[64], d[4200];
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;
        int fields = sscanf(line, "%31s %191s %191s %63s %4199s", word, a, b, c, d);
        if (strcmp(word, "option") == 0 && fields == 3) {
            if (strcmp(a, "root-lists") == 0) s_root_lists = atoi(b);
            else if (strcmp(a, "open-fails") == 0) {
                if (!add_path_option(s_open_fails, &s_open_fail_count, b)) return 0;
            } else if (strcmp(a, "info-fails") == 0) {
                if (!add_path_option(s_info_fails, &s_info_fail_count, b)) return 0;
            } else if (strcmp(a, "value-fails") == 0) {
                if (!add_path_option(s_value_fails, &s_value_fail_count, b)) return 0;
            } else return 0;
        } else if (strcmp(word, "node") == 0 && (fields == 4 || fields == 5)) {
            if (s_node_count >= FAKE_NODES) return 0;
            int parent = fake_resolve(b, 0);
            if (parent < 0 || s_nodes[parent].type != REG_TYPE_DIR) return 0;
            FakeNode *n = &s_nodes[s_node_count];
            memset(n, 0, sizeof(*n));
            n->parent = parent;
            n->type = (unsigned int)atoi(a);
            if (!hex_bytes(c, n->name, sizeof(n->name), &n->name_bytes)) return 0;
            if (fields == 5 && !hex_bytes(d, n->value, sizeof(n->value), &n->value_bytes)) {
                return 0;
            }
            s_node_count++;
        } else {
            return 0;
        }
    }
    return 1;
}

int main(void) {
    if (!load_scenario(stdin)) {
        fprintf(stderr, "registry_walk_host: malformed scenario\n");
        return 2;
    }
    const int nodes_before = s_node_count;
    static RegWalk walk;
    memset(&walk, 0, sizeof(walk));
    walk.emit = harness_emit;
    walk.step = harness_step;
    regwalk_run(&walk);
    int open_categories = 0;
    for (unsigned int slot = 0; slot < FAKE_SLOTS; slot++) {
        if (s_slots[slot] != 0) open_categories++;
    }
    printf("SUMMARY created=%d nodes_before=%d nodes_after=%d open_categories=%d "
           "open_registries=%d\n",
           s_created, nodes_before, s_node_count, open_categories, s_registry_open);
    return 0;
}
