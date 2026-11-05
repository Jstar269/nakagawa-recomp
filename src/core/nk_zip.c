/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Store-only ZIP reader and writer for save backups (#1883); nk_zip.h says what
   is written and what is refused. Written from PKWARE's APPNOTE.TXT (6.3.x):
   section 4.3.7 (local header), 4.3.12 (central directory header), 4.3.16
   (end of central directory record), 4.3.14-4.3.15 (the ZIP64 records),
   4.4.4 (general purpose flags) and 4.5.3 (the ZIP64 extra field). */

#include "nk_zip.h"

#ifndef NK_ZIP_NO_HOST_IO
#include "nk_platform.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZIP_SIG_LOCAL 0x04034b50u
#define ZIP_SIG_CENTRAL 0x02014b50u
#define ZIP_SIG_END 0x06054b50u
#define ZIP_SIG_ZIP64_LOCATOR 0x07064b50u
#define ZIP_LOCAL_SIZE 30u
#define ZIP_CENTRAL_SIZE 46u
#define ZIP_END_SIZE 22u
#define ZIP_ZIP64_LOCATOR_SIZE 20u
#define ZIP_MAX_COMMENT 65535u
#define ZIP_FLAG_ENCRYPTED 0x0001u
#define ZIP_FLAG_DESCRIPTOR 0x0008u
#define ZIP_FLAG_STRONG_ENCRYPTION 0x0040u
#define ZIP_FLAG_UTF8 0x0800u
#define ZIP_FLAG_MASKED_HEADERS 0x2000u
#define ZIP_EXTRA_ZIP64 0x0001u
#define ZIP_VERSION_STORED 10u   /* 1.0: stored entries and folders */
#define ZIP_VERSION_MADE_BY 20u  /* 2.0, MS-DOS attributes */
#define ZIP_DOS_FOLDER 0x10u
#define ZIP_CHUNK 65536u
/* An archive's data, headers and central directory: bigger than this cannot
   be one the caps allow. */
#define ZIP_MAX_ARCHIVE_BYTES \
    (NK_ZIP_MAX_TOTAL_BYTES + (uint64_t)NK_ZIP_MAX_CENTRAL_BYTES + \
     (uint64_t)NK_ZIP_MAX_ENTRIES * (ZIP_LOCAL_SIZE + NK_ZIP_MAX_NAME_BYTES + 65535u) + \
     ZIP_END_SIZE + ZIP_MAX_COMMENT)

/* ---- Status words ---- */

static const struct {
    const char *code;
    const char *message;
} k_zip_status[NK_ZIP_STATUS_COUNT] = {
    {"ZIP_OK", "The archive is whole."},
    {"ZIP_ARGUMENT", "A file or folder name was missing."},
    {"ZIP_IO", "A file could not be read or written."},
    {"ZIP_NO_MEMORY", "There was not enough memory to read the archive."},
    {"ZIP_NOT_ZIP", "It is not a ZIP file, or its end is missing."},
    {"ZIP_END_RECORD", "Its end record is damaged or appears more than once."},
    {"ZIP_TRUNCATED", "It is cut short: part of the file is missing."},
    {"ZIP_MULTI_DISK", "It is split across several files, which Nakagawa does not read."},
    {"ZIP_ZIP64", "It uses ZIP64, the format for very large archives, which Nakagawa does "
                  "not read."},
    {"ZIP_CENTRAL_DIRECTORY", "Its list of files is damaged or not where it should be."},
    {"ZIP_TOO_MANY_ENTRIES", "It holds more than 4096 files and folders."},
    {"ZIP_TOO_LARGE", "Its files add up to more than 512 MiB."},
    {"ZIP_OFFSET", "A file in it points past the end of its part of the archive."},
    {"ZIP_OVERLAP", "Two files in it share the same bytes."},
    {"ZIP_METHOD", "A file in it is compressed. Nakagawa backups store files without "
                   "compression."},
    {"ZIP_ENCRYPTED", "A file in it is encrypted."},
    {"ZIP_DATA_DESCRIPTOR", "A file in it gives its size after its data, which Nakagawa "
                            "does not read."},
    {"ZIP_FLAGS", "A file in it uses a ZIP option Nakagawa does not read."},
    {"ZIP_SIZE_MISMATCH", "A file in it has two different sizes, or a folder in it holds "
                          "data."},
    {"ZIP_LOCAL_MISMATCH", "A file's own header disagrees with the archive's list of files."},
    {"ZIP_CRC", "A file in it is damaged: its checksum does not match."},
    {"ZIP_NAME_EMPTY", "A file in it has no name."},
    {"ZIP_NAME_TOO_LONG", "A file name in it is too long."},
    {"ZIP_NAME_NUL", "A file name in it holds a NUL byte."},
    {"ZIP_NAME_ABSOLUTE", "A file name in it starts at the top of a drive (\"/\")."},
    {"ZIP_NAME_BACKSLASH", "A file name in it uses a backslash; ZIP names use \"/\"."},
    {"ZIP_NAME_DRIVE", "A file name in it names a drive (\"C:\")."},
    {"ZIP_NAME_PARENT", "A file name in it climbs out of its folder (\"..\")."},
    {"ZIP_NAME_DOT", "A file name in it has a \".\" folder."},
    {"ZIP_NAME_EMPTY_SEGMENT", "A file name in it has an empty folder name (\"//\")."},
    {"ZIP_NAME_CHARACTER", "A file name in it has a character a Windows folder can't hold."},
    {"ZIP_NAME_RESERVED", "A file name in it is one Windows keeps for devices, such as CON "
                          "or NUL."},
    {"ZIP_NAME_ENCODING", "A file name in it is not valid UTF-8 text."},
    {"ZIP_NAME_TOO_DEEP", "A file in it is more than 32 folders deep."},
    {"ZIP_NAME_DUPLICATE", "Two files in it have the same name (upper and lower case are "
                           "the same on Windows)."},
    {"ZIP_NAME_CONFLICT", "A file in it has the name of a folder in it."},
    {"ZIP_EXISTS", "A file it would write is there already."},
};

const char *nk_zip_status_code(NkZipStatus status) {
    if ((unsigned)status >= (unsigned)NK_ZIP_STATUS_COUNT) return "ZIP_UNKNOWN";
    return k_zip_status[status].code;
}

const char *nk_zip_status_message(NkZipStatus status) {
    if ((unsigned)status >= (unsigned)NK_ZIP_STATUS_COUNT) return "The archive was refused.";
    return k_zip_status[status].message;
}

/* Records the status and, when given, the entry's name in printable bytes. */
static NkZipStatus zip_fail(NkZipError *err, NkZipStatus status, const char *name,
                            size_t name_size) {
    if (err) {
        size_t n = 0;
        err->status = status;
        if (name) {
            for (size_t i = 0; i < name_size && n + 1 < sizeof(err->entry); ++i) {
                unsigned char c = (unsigned char)name[i];
                err->entry[n++] = (char)(c < 0x20u || c == 0x7Fu ? '?' : c);
            }
        }
        err->entry[n] = '\0';
    }
    return status;
}

static NkZipStatus zip_ok(NkZipError *err) {
    if (err) {
        err->status = NK_ZIP_OK;
        err->entry[0] = '\0';
    }
    return NK_ZIP_OK;
}

/* ---- CRC-32 ---- */

static void zip_crc_table(uint32_t table[256]) {
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        table[i] = c;
    }
}

static uint32_t zip_crc_update(const uint32_t table[256], uint32_t crc, const uint8_t *data,
                               size_t size) {
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

uint32_t nk_zip_crc32(uint32_t crc, const void *data, size_t size) {
    uint32_t table[256];
    if (!data || size == 0) return crc;
    zip_crc_table(table);
    return zip_crc_update(table, crc, (const uint8_t *)data, size);
}

/* ---- Little-endian fields ---- */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

#ifndef NK_ZIP_NO_HOST_IO
static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}
#endif

/* ---- Names ---- */

static bool zip_utf8_valid(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i];
        size_t len;
        uint32_t cp;
        if (c < 0x80u) {
            ++i;
            continue;
        }
        if (c >= 0xC2u && c <= 0xDFu) {
            len = 2;
            cp = c & 0x1Fu;
        } else if (c >= 0xE0u && c <= 0xEFu) {
            len = 3;
            cp = c & 0x0Fu;
        } else if (c >= 0xF0u && c <= 0xF4u) {
            len = 4;
            cp = c & 0x07u;
        } else {
            return false;
        }
        if (n - i < len) return false;
        for (size_t k = 1; k < len; ++k) {
            unsigned b = s[i + k];
            if ((b & 0xC0u) != 0x80u) return false;
            cp = (cp << 6) | (b & 0x3Fu);
        }
        if (len == 3 && (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu))) return false;
        if (len == 4 && (cp < 0x10000u || cp > 0x10FFFFu)) return false;
        i += len;
    }
    return true;
}

static char zip_upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* A Windows device name, alone or before an extension ("NUL", "con.txt"). */
static bool zip_reserved_device(const char *seg, size_t n) {
    static const char *const names[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
    char stem[8];
    size_t len = 0;
    while (len < n && seg[len] != '.') ++len;
    if (len < 3 || len > 7) return false;
    for (size_t i = 0; i < len; ++i) stem[i] = zip_upper(seg[i]);
    stem[len] = '\0';
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcmp(stem, names[i]) == 0) return true;
    }
    return len == 4 && (memcmp(stem, "COM", 3) == 0 || memcmp(stem, "LPT", 3) == 0) &&
           stem[3] >= '1' && stem[3] <= '9';
}

NkZipStatus nk_zip_check_name(const char *name, size_t size, bool *is_folder) {
    bool folder;
    size_t end;
    size_t start = 0;
    size_t depth = 0;
    if (is_folder) *is_folder = false;
    if (!name || size == 0) return NK_ZIP_ERR_NAME_EMPTY;
    if (size > NK_ZIP_MAX_NAME_BYTES) return NK_ZIP_ERR_NAME_TOO_LONG;
    if (memchr(name, '\0', size)) return NK_ZIP_ERR_NAME_NUL;
    if (name[0] == '/') return NK_ZIP_ERR_NAME_ABSOLUTE;
    if (memchr(name, '\\', size)) return NK_ZIP_ERR_NAME_BACKSLASH;
    if (size >= 2 && name[1] == ':' &&
        ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z'))) {
        return NK_ZIP_ERR_NAME_DRIVE;
    }
    if (!zip_utf8_valid((const unsigned char *)name, size)) return NK_ZIP_ERR_NAME_ENCODING;
    folder = name[size - 1] == '/';
    end = folder ? size - 1 : size;
    for (size_t i = 0; i <= end; ++i) {
        const char *seg;
        size_t len;
        if (i < end && name[i] != '/') continue;
        seg = name + start;
        len = i - start;
        if (len == 0) return NK_ZIP_ERR_NAME_EMPTY_SEGMENT;
        if (len == 2 && seg[0] == '.' && seg[1] == '.') return NK_ZIP_ERR_NAME_PARENT;
        if (len == 1 && seg[0] == '.') return NK_ZIP_ERR_NAME_DOT;
        for (size_t k = 0; k < len; ++k) {
            unsigned char c = (unsigned char)seg[k];
            if (c < 0x20u || c == '<' || c == '>' || c == ':' || c == '"' || c == '|' ||
                c == '?' || c == '*') {
                return NK_ZIP_ERR_NAME_CHARACTER;
            }
        }
        /* Windows drops a trailing dot or space, so "a." would land on "a". */
        if (seg[len - 1] == '.' || seg[len - 1] == ' ') return NK_ZIP_ERR_NAME_CHARACTER;
        if (zip_reserved_device(seg, len)) return NK_ZIP_ERR_NAME_RESERVED;
        if (++depth > NK_ZIP_MAX_DEPTH) return NK_ZIP_ERR_NAME_TOO_DEEP;
        start = i + 1;
    }
    if (is_folder) *is_folder = folder;
    return NK_ZIP_OK;
}

/* Two names that land on the same Windows path, or a file named like a folder
   another entry is in. A name is compared without a folder's trailing '/', with
   ASCII case folded and '/' sorted first, so everything inside "a/" follows
   "a" directly. */
typedef struct {
    const char *name;
    size_t size;   /* without a folder's trailing '/' */
    bool folder;
} ZipNameRef;

static int zip_fold(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c == '/') return 1;
    return c;
}

static int zip_name_compare(const void *a, const void *b) {
    const ZipNameRef *x = (const ZipNameRef *)a;
    const ZipNameRef *y = (const ZipNameRef *)b;
    size_t n = x->size < y->size ? x->size : y->size;
    for (size_t i = 0; i < n; ++i) {
        int d = zip_fold((unsigned char)x->name[i]) - zip_fold((unsigned char)y->name[i]);
        if (d) return d;
    }
    return (x->size > y->size) - (x->size < y->size);
}

static bool zip_folded_prefix(const ZipNameRef *a, const ZipNameRef *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (zip_fold((unsigned char)a->name[i]) != zip_fold((unsigned char)b->name[i])) {
            return false;
        }
    }
    return true;
}

static NkZipStatus zip_names_distinct(ZipNameRef *refs, size_t count, NkZipError *err) {
    if (count < 2) return NK_ZIP_OK;
    qsort(refs, count, sizeof(refs[0]), zip_name_compare);
    for (size_t k = 0; k + 1 < count; ++k) {
        const ZipNameRef *a = &refs[k];
        const ZipNameRef *b = &refs[k + 1];
        if (a->size == b->size && zip_folded_prefix(a, b, a->size)) {
            return zip_fail(err, NK_ZIP_ERR_NAME_DUPLICATE, b->name, b->size);
        }
        if (!a->folder && b->size > a->size && b->name[a->size] == '/' &&
            zip_folded_prefix(a, b, a->size)) {
            return zip_fail(err, NK_ZIP_ERR_NAME_CONFLICT, a->name, a->size);
        }
    }
    return NK_ZIP_OK;
}

/* ---- Reader ---- */

typedef struct {
    char *name;            /* NUL-terminated copy */
    uint16_t name_size;
    uint16_t flags;
    uint32_t crc;
    uint32_t size;
    uint32_t local_offset;
    uint64_t data_offset;
    bool folder;
} ZipEntry;

struct NkZipReader {
    const uint8_t *mem;
    uint64_t size;
#ifndef NK_ZIP_NO_HOST_IO
    FILE *file;
#endif
    ZipEntry *entries;
    size_t slots;          /* entries allocated */
    size_t count;          /* entries checked */
    uint64_t total;
    uint8_t *chunk;
    uint32_t crc_table[256];
};

static NkZipStatus zip_read_at(NkZipReader *r, uint64_t offset, void *buf, size_t size) {
    if (offset > r->size || (uint64_t)size > r->size - offset) return NK_ZIP_ERR_TRUNCATED;
    if (size == 0) return NK_ZIP_OK;
    if (r->mem) {
        memcpy(buf, r->mem + (size_t)offset, size);
        return NK_ZIP_OK;
    }
#ifndef NK_ZIP_NO_HOST_IO
    if (r->file && nk_fseek64(r->file, (int64_t)offset, SEEK_SET) == 0 &&
        fread(buf, 1, size, r->file) == size) {
        return NK_ZIP_OK;
    }
#endif
    return NK_ZIP_ERR_IO;
}

/* The extra field's blocks: false when one runs past the field. A ZIP64 block
   sets *zip64. A tail shorter than a block header is padding and is ignored. */
static bool zip_extra_scan(const uint8_t *extra, size_t size, bool *zip64) {
    size_t at = 0;
    *zip64 = false;
    while (size - at >= 4u) {
        uint16_t id = rd16(extra + at);
        uint16_t len = rd16(extra + at + 2);
        if ((size_t)len > size - at - 4u) return false;
        if (id == ZIP_EXTRA_ZIP64) *zip64 = true;
        at += 4u + len;
    }
    return true;
}

/* The flag and method rules, the same for the central and the local header. */
static NkZipStatus zip_check_flags(uint16_t flags, uint16_t method) {
    if (flags & (ZIP_FLAG_ENCRYPTED | ZIP_FLAG_STRONG_ENCRYPTION | ZIP_FLAG_MASKED_HEADERS)) {
        return NK_ZIP_ERR_ENCRYPTED;
    }
    if (flags & ZIP_FLAG_DESCRIPTOR) return NK_ZIP_ERR_DATA_DESCRIPTOR;
    if (method != 0u) return NK_ZIP_ERR_METHOD;
    if (flags & (uint16_t)~ZIP_FLAG_UTF8) return NK_ZIP_ERR_FLAGS;
    return NK_ZIP_OK;
}

/* Finds the one end record: the signature whose comment length reaches the
   end of the file exactly. */
static NkZipStatus zip_find_end(NkZipReader *r, uint64_t *end_offset, uint8_t record[ZIP_END_SIZE]) {
    uint64_t tail_size = r->size < (uint64_t)(ZIP_END_SIZE + ZIP_MAX_COMMENT)
                             ? r->size : (uint64_t)(ZIP_END_SIZE + ZIP_MAX_COMMENT);
    uint64_t tail_offset = r->size - tail_size;
    uint8_t *tail = (uint8_t *)malloc((size_t)tail_size);
    NkZipStatus status;
    bool found = false;
    bool signature_seen = false;
    size_t at = 0;
    if (!tail) return NK_ZIP_ERR_NO_MEMORY;
    status = zip_read_at(r, tail_offset, tail, (size_t)tail_size);
    if (status != NK_ZIP_OK) {
        free(tail);
        return status;
    }
    /* A signature in the last 21 bytes is an end record cut short: damaged,
       not "not a ZIP". */
    for (size_t pos = tail_size >= 4u ? (size_t)tail_size - 4u : 0u; tail_size >= 4u; --pos) {
        if (rd32(tail + pos) == ZIP_SIG_END) {
            signature_seen = true;
            if ((uint64_t)pos + ZIP_END_SIZE <= tail_size &&
                (uint64_t)pos + ZIP_END_SIZE + rd16(tail + pos + 20) == tail_size) {
                if (found) {
                    free(tail);
                    return NK_ZIP_ERR_END_RECORD;
                }
                found = true;
                at = pos;
            }
        }
        if (pos == 0) break;
    }
    if (found) {
        memcpy(record, tail + at, ZIP_END_SIZE);
        *end_offset = tail_offset + at;
    }
    free(tail);
    if (!found) return signature_seen ? NK_ZIP_ERR_END_RECORD : NK_ZIP_ERR_NOT_ZIP;
    return NK_ZIP_OK;
}

static int zip_entry_offset_compare(const void *a, const void *b) {
    const ZipEntry *const *x = (const ZipEntry *const *)a;
    const ZipEntry *const *y = (const ZipEntry *const *)b;
    return ((*x)->local_offset > (*y)->local_offset) - ((*x)->local_offset < (*y)->local_offset);
}

static NkZipStatus zip_reader_parse(NkZipReader *r, NkZipError *err) {
    uint8_t end[ZIP_END_SIZE];
    uint64_t end_offset = 0;
    uint16_t disk, central_disk, disk_entries, total_entries;
    uint32_t central_size, central_offset;
    uint8_t *central = NULL;
    ZipNameRef *refs = NULL;
    ZipEntry **by_offset = NULL;
    NkZipStatus status;
    size_t at = 0;

    if (r->size < ZIP_END_SIZE) return zip_fail(err, NK_ZIP_ERR_NOT_ZIP, NULL, 0);
    if (r->size > ZIP_MAX_ARCHIVE_BYTES) return zip_fail(err, NK_ZIP_ERR_TOO_LARGE, NULL, 0);
    status = zip_find_end(r, &end_offset, end);
    if (status != NK_ZIP_OK) return zip_fail(err, status, NULL, 0);
    disk = rd16(end + 4);
    central_disk = rd16(end + 6);
    disk_entries = rd16(end + 8);
    total_entries = rd16(end + 10);
    central_size = rd32(end + 12);
    central_offset = rd32(end + 16);
    /* APPNOTE 4.4.1.4: these values say "look in the ZIP64 record". */
    if (disk == 0xFFFFu || central_disk == 0xFFFFu || disk_entries == 0xFFFFu ||
        total_entries == 0xFFFFu || central_size == 0xFFFFFFFFu ||
        central_offset == 0xFFFFFFFFu) {
        return zip_fail(err, NK_ZIP_ERR_ZIP64, NULL, 0);
    }
    if (disk != 0u || central_disk != 0u || disk_entries != total_entries) {
        return zip_fail(err, NK_ZIP_ERR_MULTI_DISK, NULL, 0);
    }
    if (total_entries > NK_ZIP_MAX_ENTRIES) {
        return zip_fail(err, NK_ZIP_ERR_TOO_MANY_ENTRIES, NULL, 0);
    }
    if ((uint64_t)central_offset + central_size != end_offset) {
        /* A ZIP64 end record and locator sit between the two. */
        uint8_t sig[4];
        if (end_offset >= ZIP_ZIP64_LOCATOR_SIZE &&
            zip_read_at(r, end_offset - ZIP_ZIP64_LOCATOR_SIZE, sig, sizeof(sig)) == NK_ZIP_OK &&
            rd32(sig) == ZIP_SIG_ZIP64_LOCATOR) {
            return zip_fail(err, NK_ZIP_ERR_ZIP64, NULL, 0);
        }
        return zip_fail(err, NK_ZIP_ERR_CENTRAL_DIRECTORY, NULL, 0);
    }
    if (central_size > NK_ZIP_MAX_CENTRAL_BYTES ||
        (uint64_t)central_size < (uint64_t)total_entries * ZIP_CENTRAL_SIZE) {
        return zip_fail(err, NK_ZIP_ERR_CENTRAL_DIRECTORY, NULL, 0);
    }
    central = (uint8_t *)malloc((size_t)central_size + 1u);
    r->entries = (ZipEntry *)calloc(total_entries ? total_entries : 1u, sizeof(ZipEntry));
    refs = (ZipNameRef *)calloc(total_entries ? total_entries : 1u, sizeof(ZipNameRef));
    by_offset = (ZipEntry **)calloc(total_entries ? total_entries : 1u, sizeof(ZipEntry *));
    if (r->entries) r->slots = total_entries;
    if (!central || !r->entries || !refs || !by_offset) {
        status = zip_fail(err, NK_ZIP_ERR_NO_MEMORY, NULL, 0);
        goto done;
    }
    status = zip_read_at(r, central_offset, central, central_size);
    if (status != NK_ZIP_OK) {
        status = zip_fail(err, status, NULL, 0);
        goto done;
    }

    /* The central directory, entry by entry. */
    for (size_t i = 0; i < total_entries; ++i) {
        const uint8_t *h;
        uint16_t flags, method, name_size, extra_size, comment_size, start_disk;
        uint32_t crc, packed, size, local;
        size_t record;
        bool zip64 = false;
        bool folder = false;
        ZipEntry *e = &r->entries[i];
        if ((size_t)central_size - at < ZIP_CENTRAL_SIZE) {
            status = zip_fail(err, NK_ZIP_ERR_CENTRAL_DIRECTORY, NULL, 0);
            goto done;
        }
        h = central + at;
        if (rd32(h) != ZIP_SIG_CENTRAL) {
            status = zip_fail(err, NK_ZIP_ERR_CENTRAL_DIRECTORY, NULL, 0);
            goto done;
        }
        flags = rd16(h + 8);
        method = rd16(h + 10);
        crc = rd32(h + 16);
        packed = rd32(h + 20);
        size = rd32(h + 24);
        name_size = rd16(h + 28);
        extra_size = rd16(h + 30);
        comment_size = rd16(h + 32);
        start_disk = rd16(h + 34);
        local = rd32(h + 42);
        record = (size_t)ZIP_CENTRAL_SIZE + name_size + extra_size + comment_size;
        if ((size_t)central_size - at < record) {
            status = zip_fail(err, NK_ZIP_ERR_CENTRAL_DIRECTORY, NULL, 0);
            goto done;
        }
        {
            const char *name = (const char *)(h + ZIP_CENTRAL_SIZE);
            NkZipStatus named = nk_zip_check_name(name, name_size, &folder);
            if (named != NK_ZIP_OK) {
                status = zip_fail(err, named, name, name_size);
                goto done;
            }
            e->name = (char *)malloc((size_t)name_size + 1u);
            if (!e->name) {
                status = zip_fail(err, NK_ZIP_ERR_NO_MEMORY, NULL, 0);
                goto done;
            }
            memcpy(e->name, name, name_size);
            e->name[name_size] = '\0';
            e->name_size = name_size;
        }
        if (!zip_extra_scan(h + ZIP_CENTRAL_SIZE + name_size, extra_size, &zip64)) {
            status = zip_fail(err, NK_ZIP_ERR_CENTRAL_DIRECTORY, e->name, e->name_size);
            goto done;
        }
        if (zip64 || packed == 0xFFFFFFFFu || size == 0xFFFFFFFFu || local == 0xFFFFFFFFu ||
            start_disk == 0xFFFFu) {
            status = zip_fail(err, NK_ZIP_ERR_ZIP64, e->name, e->name_size);
            goto done;
        }
        if (start_disk != 0u) {
            status = zip_fail(err, NK_ZIP_ERR_MULTI_DISK, e->name, e->name_size);
            goto done;
        }
        status = zip_check_flags(flags, method);
        if (status != NK_ZIP_OK) {
            status = zip_fail(err, status, e->name, e->name_size);
            goto done;
        }
        if (packed != size || (folder && size != 0u)) {
            status = zip_fail(err, NK_ZIP_ERR_SIZE_MISMATCH, e->name, e->name_size);
            goto done;
        }
        if (folder && crc != 0u) {
            status = zip_fail(err, NK_ZIP_ERR_CRC, e->name, e->name_size);
            goto done;
        }
        r->total += size;
        if (r->total > NK_ZIP_MAX_TOTAL_BYTES) {
            status = zip_fail(err, NK_ZIP_ERR_TOO_LARGE, e->name, e->name_size);
            goto done;
        }
        e->flags = flags;
        e->crc = crc;
        e->size = size;
        e->local_offset = local;
        e->folder = folder;
        refs[i].name = e->name;
        refs[i].size = folder ? (size_t)name_size - 1u : name_size;
        refs[i].folder = folder;
        r->count = i + 1;
        at += record;
    }
    if (at != central_size) {
        status = zip_fail(err, NK_ZIP_ERR_CENTRAL_DIRECTORY, NULL, 0);
        goto done;
    }
    status = zip_names_distinct(refs, r->count, err);
    if (status != NK_ZIP_OK) goto done;

    /* Each local header against its central entry. */
    for (size_t i = 0; i < r->count; ++i) {
        ZipEntry *e = &r->entries[i];
        uint8_t h[ZIP_LOCAL_SIZE];
        uint8_t *tail;
        uint16_t name_size, extra_size;
        uint64_t header_end;
        bool zip64 = false;
        if ((uint64_t)e->local_offset + ZIP_LOCAL_SIZE > central_offset) {
            status = zip_fail(err, NK_ZIP_ERR_OFFSET, e->name, e->name_size);
            goto done;
        }
        status = zip_read_at(r, e->local_offset, h, sizeof(h));
        if (status != NK_ZIP_OK) {
            status = zip_fail(err, status, e->name, e->name_size);
            goto done;
        }
        if (rd32(h) != ZIP_SIG_LOCAL) {
            status = zip_fail(err, NK_ZIP_ERR_LOCAL_MISMATCH, e->name, e->name_size);
            goto done;
        }
        name_size = rd16(h + 26);
        extra_size = rd16(h + 28);
        header_end = (uint64_t)e->local_offset + ZIP_LOCAL_SIZE + name_size + extra_size;
        if (header_end > central_offset) {
            status = zip_fail(err, NK_ZIP_ERR_OFFSET, e->name, e->name_size);
            goto done;
        }
        {
            NkZipStatus local_flags = zip_check_flags(rd16(h + 6), rd16(h + 8));
            if (local_flags != NK_ZIP_OK) {
                status = zip_fail(err, local_flags, e->name, e->name_size);
                goto done;
            }
        }
        if (rd16(h + 6) != e->flags || rd32(h + 14) != e->crc || rd32(h + 18) != e->size ||
            rd32(h + 22) != e->size || name_size != e->name_size) {
            status = zip_fail(err, NK_ZIP_ERR_LOCAL_MISMATCH, e->name, e->name_size);
            goto done;
        }
        tail = (uint8_t *)malloc((size_t)name_size + extra_size + 1u);
        if (!tail) {
            status = zip_fail(err, NK_ZIP_ERR_NO_MEMORY, NULL, 0);
            goto done;
        }
        status = zip_read_at(r, (uint64_t)e->local_offset + ZIP_LOCAL_SIZE, tail,
                             (size_t)name_size + extra_size);
        if (status == NK_ZIP_OK && memcmp(tail, e->name, name_size) != 0) {
            status = NK_ZIP_ERR_LOCAL_MISMATCH;
        }
        if (status == NK_ZIP_OK && !zip_extra_scan(tail + name_size, extra_size, &zip64)) {
            status = NK_ZIP_ERR_LOCAL_MISMATCH;
        }
        if (status == NK_ZIP_OK && zip64) status = NK_ZIP_ERR_ZIP64;
        free(tail);
        if (status != NK_ZIP_OK) {
            status = zip_fail(err, status, e->name, e->name_size);
            goto done;
        }
        e->data_offset = header_end;
        if (header_end + e->size > central_offset) {
            status = zip_fail(err, NK_ZIP_ERR_OFFSET, e->name, e->name_size);
            goto done;
        }
        by_offset[i] = e;
    }
    /* No two entries share bytes: each one's header and data end before the
       next one's header starts. */
    if (r->count > 1) {
        qsort(by_offset, r->count, sizeof(by_offset[0]), zip_entry_offset_compare);
        for (size_t i = 0; i + 1 < r->count; ++i) {
            if (by_offset[i]->data_offset + by_offset[i]->size > by_offset[i + 1]->local_offset) {
                status = zip_fail(err, NK_ZIP_ERR_OVERLAP, by_offset[i + 1]->name,
                                  by_offset[i + 1]->name_size);
                goto done;
            }
        }
    }
    status = zip_ok(err);
done:
    free(central);
    free(refs);
    free(by_offset);
    return status;
}

static NkZipStatus zip_reader_start(NkZipReader *r, NkZipReader **out, NkZipError *err) {
    NkZipStatus status;
    zip_crc_table(r->crc_table);
    r->chunk = (uint8_t *)malloc(ZIP_CHUNK);
    if (!r->chunk) {
        nk_zip_reader_close(r);
        return zip_fail(err, NK_ZIP_ERR_NO_MEMORY, NULL, 0);
    }
    status = zip_reader_parse(r, err);
    if (status != NK_ZIP_OK) {
        nk_zip_reader_close(r);
        return status;
    }
    *out = r;
    return NK_ZIP_OK;
}

NkZipStatus nk_zip_reader_open_memory(NkZipReader **out, const void *data, size_t size,
                                      NkZipError *err) {
    NkZipReader *r;
    if (!out) return zip_fail(err, NK_ZIP_ERR_ARGUMENT, NULL, 0);
    *out = NULL;
    if (!data && size) return zip_fail(err, NK_ZIP_ERR_ARGUMENT, NULL, 0);
    if (size < ZIP_END_SIZE) return zip_fail(err, NK_ZIP_ERR_NOT_ZIP, NULL, 0);
    r = (NkZipReader *)calloc(1, sizeof(*r));
    if (!r) return zip_fail(err, NK_ZIP_ERR_NO_MEMORY, NULL, 0);
    r->mem = (const uint8_t *)data;
    r->size = size;
    return zip_reader_start(r, out, err);
}

size_t nk_zip_reader_count(const NkZipReader *reader) { return reader ? reader->count : 0; }

uint64_t nk_zip_reader_total_bytes(const NkZipReader *reader) {
    return reader ? reader->total : 0;
}

bool nk_zip_reader_entry(const NkZipReader *reader, size_t index, NkZipEntryInfo *out) {
    if (!reader || !out || index >= reader->count) return false;
    out->name = reader->entries[index].name;
    out->size = reader->entries[index].size;
    out->crc32 = reader->entries[index].crc;
    out->is_folder = reader->entries[index].folder;
    return true;
}

/* Streams one entry's data through `sink` (when given) and checks its CRC-32. */
typedef bool (*ZipSink)(void *ctx, const uint8_t *data, size_t size);

static NkZipStatus zip_stream_entry(NkZipReader *r, const ZipEntry *e, ZipSink sink, void *ctx,
                                    NkZipError *err) {
    uint64_t left = e->size;
    uint64_t at = e->data_offset;
    uint32_t crc = 0;
    while (left > 0) {
        size_t n = left > ZIP_CHUNK ? ZIP_CHUNK : (size_t)left;
        NkZipStatus status = zip_read_at(r, at, r->chunk, n);
        if (status != NK_ZIP_OK) return zip_fail(err, status, e->name, e->name_size);
        crc = zip_crc_update(r->crc_table, crc, r->chunk, n);
        if (sink && !sink(ctx, r->chunk, n)) {
            return zip_fail(err, NK_ZIP_ERR_IO, e->name, e->name_size);
        }
        at += n;
        left -= n;
    }
    if (crc != e->crc) return zip_fail(err, NK_ZIP_ERR_CRC, e->name, e->name_size);
    return NK_ZIP_OK;
}

NkZipStatus nk_zip_reader_verify(NkZipReader *reader, NkZipError *err) {
    if (!reader) return zip_fail(err, NK_ZIP_ERR_ARGUMENT, NULL, 0);
    for (size_t i = 0; i < reader->count; ++i) {
        NkZipStatus status = zip_stream_entry(reader, &reader->entries[i], NULL, NULL, err);
        if (status != NK_ZIP_OK) return status;
    }
    return zip_ok(err);
}

typedef struct {
    uint8_t *out;
    size_t at;
} ZipBufferSink;

static bool zip_buffer_sink(void *ctx, const uint8_t *data, size_t size) {
    ZipBufferSink *sink = (ZipBufferSink *)ctx;
    memcpy(sink->out + sink->at, data, size);
    sink->at += size;
    return true;
}

NkZipStatus nk_zip_reader_read(NkZipReader *reader, size_t index, void *buf, size_t cap,
                               NkZipError *err) {
    ZipBufferSink sink;
    NkZipStatus status;
    if (!reader || index >= reader->count || (!buf && reader->entries[index].size)) {
        return zip_fail(err, NK_ZIP_ERR_ARGUMENT, NULL, 0);
    }
    if (cap < reader->entries[index].size) return zip_fail(err, NK_ZIP_ERR_ARGUMENT, NULL, 0);
    sink.out = (uint8_t *)buf;
    sink.at = 0;
    status = zip_stream_entry(reader, &reader->entries[index], zip_buffer_sink, &sink, err);
    return status == NK_ZIP_OK ? zip_ok(err) : status;
}

void nk_zip_reader_close(NkZipReader *reader) {
    if (!reader) return;
    /* Every slot, so a name copied for an entry that was then refused goes too. */
    for (size_t i = 0; reader->entries && i < reader->slots; ++i) free(reader->entries[i].name);
#ifndef NK_ZIP_NO_HOST_IO
    if (reader->file) fclose(reader->file);
#endif
    free(reader->entries);
    free(reader->chunk);
    free(reader);
}

#ifndef NK_ZIP_NO_HOST_IO

/* ---- Reader: files and extraction ---- */

NkZipStatus nk_zip_reader_open_file(NkZipReader **out, const char *path, NkZipError *err) {
    NkZipReader *r;
    int64_t size;
    if (!out) return zip_fail(err, NK_ZIP_ERR_ARGUMENT, NULL, 0);
    *out = NULL;
    if (!path || !path[0]) return zip_fail(err, NK_ZIP_ERR_ARGUMENT, NULL, 0);
    r = (NkZipReader *)calloc(1, sizeof(*r));
    if (!r) return zip_fail(err, NK_ZIP_ERR_NO_MEMORY, NULL, 0);
    r->file = nk_fopen_utf8(path, "rb");
    if (!r->file || nk_fseek64(r->file, 0, SEEK_END) != 0 || (size = nk_ftell64(r->file)) < 0) {
        nk_zip_reader_close(r);
        return zip_fail(err, NK_ZIP_ERR_IO, NULL, 0);
    }
    r->size = (uint64_t)size;
    if (r->size < ZIP_END_SIZE) {
        nk_zip_reader_close(r);
        return zip_fail(err, NK_ZIP_ERR_NOT_ZIP, NULL, 0);
    }
    return zip_reader_start(r, out, err);
}

static bool zip_file_sink(void *ctx, const uint8_t *data, size_t size) {
    return fwrite(data, 1, size, (FILE *)ctx) == size;
}

/* folder + the entry's name, '/' turned into the host separator, without a
   folder entry's trailing one. */
static bool zip_host_path(const char *folder, const ZipEntry *e, char *out, size_t cap) {
    char sep = nk_platform_path_separator();
    size_t name_size = e->folder ? (size_t)e->name_size - 1u : e->name_size;
    int n = snprintf(out, cap, "%s%c%.*s", folder, sep, (int)name_size, e->name);
    if (n <= 0 || (size_t)n >= cap) return false;
    for (char *p = out + strlen(folder) + 1; *p; ++p) {
        if (*p == '/') *p = sep;
    }
    return true;
}

NkZipStatus nk_zip_reader_extract(NkZipReader *reader, const char *folder, NkZipError *err) {
    char path[NK_ZIP_MAX_NAME_BYTES + 4096];
    NkZipStatus status;
    if (!reader || !folder || !folder[0]) return zip_fail(err, NK_ZIP_ERR_ARGUMENT, NULL, 0);
    if (!nk_platform_dir_exists(folder)) return zip_fail(err, NK_ZIP_ERR_IO, NULL, 0);
    /* Every checksum before the first byte is written. */
    status = nk_zip_reader_verify(reader, err);
    if (status != NK_ZIP_OK) return status;
    for (size_t i = 0; i < reader->count; ++i) {
        const ZipEntry *e = &reader->entries[i];
        FILE *f;
        char *cut;
        if (!zip_host_path(folder, e, path, sizeof(path))) {
            return zip_fail(err, NK_ZIP_ERR_NAME_TOO_LONG, e->name, e->name_size);
        }
        if (e->folder) {
            if (!nk_platform_mkdir_p(path)) return zip_fail(err, NK_ZIP_ERR_IO, e->name, e->name_size);
            continue;
        }
        cut = strrchr(path, nk_platform_path_separator());
        if (cut && cut > path + strlen(folder)) {
            *cut = '\0';
            bool made = nk_platform_mkdir_p(path);
            *cut = nk_platform_path_separator();
            if (!made) return zip_fail(err, NK_ZIP_ERR_IO, e->name, e->name_size);
        }
        if (!nk_platform_path_missing(path)) {
            return zip_fail(err, NK_ZIP_ERR_EXISTS, e->name, e->name_size);
        }
        f = nk_fopen_utf8(path, "wb");
        if (!f) return zip_fail(err, NK_ZIP_ERR_IO, e->name, e->name_size);
        status = zip_stream_entry(reader, e, zip_file_sink, f, err);
        if (fclose(f) != 0 && status == NK_ZIP_OK) {
            status = zip_fail(err, NK_ZIP_ERR_IO, e->name, e->name_size);
        }
        if (status != NK_ZIP_OK) return status;
    }
    return zip_ok(err);
}

/* ---- Writer ---- */

typedef struct {
    char *name;
    uint16_t name_size;
    uint16_t flags;
    uint32_t crc;
    uint32_t size;
    uint32_t offset;
    bool folder;
} ZipWriterEntry;

struct NkZipWriter {
    FILE *file;
    char *path;
    uint16_t dos_date;
    uint16_t dos_time;
    ZipWriterEntry *entries;
    size_t count;
    size_t capacity;
    uint64_t total;
    uint64_t offset;
    NkZipStatus failed;      /* the first failure; every later call returns it */
    char failed_entry[160];
    uint8_t *chunk;
    uint32_t crc_table[256];
};

static NkZipStatus zip_writer_fail(NkZipWriter *w, NkZipError *err, NkZipStatus status,
                                   const char *name, size_t name_size) {
    NkZipError local;
    zip_fail(&local, status, name, name_size);
    if (w && w->failed == NK_ZIP_OK) {
        w->failed = status;
        memcpy(w->failed_entry, local.entry, sizeof(w->failed_entry));
    }
    if (err) *err = local;
    return status;
}

NkZipStatus nk_zip_writer_open(NkZipWriter **out, const char *path, uint16_t dos_date,
                               uint16_t dos_time, NkZipError *err) {
    NkZipWriter *w;
    size_t path_size;
    if (!out) return zip_fail(err, NK_ZIP_ERR_ARGUMENT, NULL, 0);
    *out = NULL;
    if (!path || !path[0]) return zip_fail(err, NK_ZIP_ERR_ARGUMENT, NULL, 0);
    w = (NkZipWriter *)calloc(1, sizeof(*w));
    path_size = strlen(path) + 1u;
    if (!w || !(w->path = (char *)malloc(path_size)) || !(w->chunk = (uint8_t *)malloc(ZIP_CHUNK))) {
        if (w) {
            free(w->path);
            free(w);
        }
        return zip_fail(err, NK_ZIP_ERR_NO_MEMORY, NULL, 0);
    }
    memcpy(w->path, path, path_size);
    w->file = nk_fopen_utf8(path, "wb");
    if (!w->file) {
        free(w->chunk);
        free(w->path);
        free(w);
        return zip_fail(err, NK_ZIP_ERR_IO, NULL, 0);
    }
    w->dos_date = dos_date;
    w->dos_time = dos_time;
    zip_crc_table(w->crc_table);
    *out = w;
    return zip_ok(err);
}

static bool zip_name_ascii(const char *name, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        if ((unsigned char)name[i] >= 0x80u) return false;
    }
    return true;
}

/* A new entry after the name and count checks, with its local header written
   (CRC and size as given; add_file fixes them up afterwards). */
static NkZipStatus zip_writer_begin_entry(NkZipWriter *w, const char *name, size_t name_size,
                                          bool folder, uint32_t crc, uint32_t size,
                                          ZipWriterEntry **out, NkZipError *err) {
    ZipWriterEntry *e;
    uint8_t h[ZIP_LOCAL_SIZE];
    bool is_folder = false;
    NkZipStatus status;
    if (w->failed != NK_ZIP_OK) {
        if (err) {
            err->status = w->failed;
            memcpy(err->entry, w->failed_entry, sizeof(err->entry));
        }
        return w->failed;
    }
    status = nk_zip_check_name(name, name_size, &is_folder);
    if (status == NK_ZIP_OK && is_folder != folder) status = NK_ZIP_ERR_ARGUMENT;
    if (status != NK_ZIP_OK) return zip_writer_fail(w, err, status, name, name_size);
    if (w->count >= NK_ZIP_MAX_ENTRIES) {
        return zip_writer_fail(w, err, NK_ZIP_ERR_TOO_MANY_ENTRIES, name, name_size);
    }
    if (w->offset + ZIP_LOCAL_SIZE + name_size + (uint64_t)size > 0xFFFFFFFFull) {
        return zip_writer_fail(w, err, NK_ZIP_ERR_TOO_LARGE, name, name_size);
    }
    if (w->count == w->capacity) {
        size_t capacity = w->capacity ? w->capacity * 2u : 64u;
        ZipWriterEntry *grown =
            (ZipWriterEntry *)realloc(w->entries, capacity * sizeof(ZipWriterEntry));
        if (!grown) return zip_writer_fail(w, err, NK_ZIP_ERR_NO_MEMORY, name, name_size);
        w->entries = grown;
        w->capacity = capacity;
    }
    e = &w->entries[w->count];
    memset(e, 0, sizeof(*e));
    e->name = (char *)malloc(name_size + 1u);
    if (!e->name) return zip_writer_fail(w, err, NK_ZIP_ERR_NO_MEMORY, name, name_size);
    memcpy(e->name, name, name_size);
    e->name[name_size] = '\0';
    e->name_size = (uint16_t)name_size;
    e->flags = zip_name_ascii(name, name_size) ? 0u : ZIP_FLAG_UTF8;
    e->crc = crc;
    e->size = size;
    e->offset = (uint32_t)w->offset;
    e->folder = folder;
    w->count++;
    wr32(h, ZIP_SIG_LOCAL);
    wr16(h + 4, ZIP_VERSION_STORED);
    wr16(h + 6, e->flags);
    wr16(h + 8, 0u);
    wr16(h + 10, w->dos_time);
    wr16(h + 12, w->dos_date);
    wr32(h + 14, crc);
    wr32(h + 18, size);
    wr32(h + 22, size);
    wr16(h + 26, (uint16_t)name_size);
    wr16(h + 28, 0u);
    if (fwrite(h, 1, sizeof(h), w->file) != sizeof(h) ||
        fwrite(name, 1, name_size, w->file) != name_size) {
        return zip_writer_fail(w, err, NK_ZIP_ERR_IO, name, name_size);
    }
    w->offset += ZIP_LOCAL_SIZE + name_size;
    *out = e;
    return NK_ZIP_OK;
}

NkZipStatus nk_zip_writer_add_folder(NkZipWriter *writer, const char *name, NkZipError *err) {
    char folder[NK_ZIP_MAX_NAME_BYTES + 2];
    size_t size;
    ZipWriterEntry *e;
    NkZipStatus status;
    if (!writer || !name) return zip_fail(err, NK_ZIP_ERR_ARGUMENT, NULL, 0);
    size = strlen(name);
    if (size > NK_ZIP_MAX_NAME_BYTES) {
        return zip_writer_fail(writer, err, NK_ZIP_ERR_NAME_TOO_LONG, name, size);
    }
    memcpy(folder, name, size);
    if (size == 0 || folder[size - 1] != '/') folder[size++] = '/';
    status = zip_writer_begin_entry(writer, folder, size, true, 0u, 0u, &e, err);
    return status == NK_ZIP_OK ? zip_ok(err) : status;
}

NkZipStatus nk_zip_writer_add_bytes(NkZipWriter *writer, const char *name, const void *data,
                                    size_t size, NkZipError *err) {
    ZipWriterEntry *e;
    NkZipStatus status;
    size_t name_size;
    if (!writer || !name || (!data && size)) return zip_fail(err, NK_ZIP_ERR_ARGUMENT, NULL, 0);
    name_size = strlen(name);
    if ((uint64_t)size > 0xFFFFFFFEull || writer->total + size > NK_ZIP_MAX_TOTAL_BYTES) {
        return zip_writer_fail(writer, err, NK_ZIP_ERR_TOO_LARGE, name, name_size);
    }
    status = zip_writer_begin_entry(writer, name, name_size, false,
                                    size ? zip_crc_update(writer->crc_table, 0u,
                                                          (const uint8_t *)data, size)
                                         : 0u,
                                    (uint32_t)size, &e, err);
    if (status != NK_ZIP_OK) return status;
    if (size && fwrite(data, 1, size, writer->file) != size) {
        return zip_writer_fail(writer, err, NK_ZIP_ERR_IO, name, name_size);
    }
    writer->offset += size;
    writer->total += size;
    return zip_ok(err);
}

NkZipStatus nk_zip_writer_add_file(NkZipWriter *writer, const char *name, const char *host_path,
                                   NkZipError *err) {
    ZipWriterEntry *e;
    NkZipStatus status;
    FILE *in;
    size_t name_size;
    uint64_t size = 0;
    uint32_t crc = 0;
    uint8_t fix[12];
    if (!writer || !name || !host_path || !host_path[0]) {
        return zip_fail(err, NK_ZIP_ERR_ARGUMENT, NULL, 0);
    }
    name_size = strlen(name);
    status = zip_writer_begin_entry(writer, name, name_size, false, 0u, 0u, &e, err);
    if (status != NK_ZIP_OK) return status;
    in = nk_fopen_utf8(host_path, "rb");
    if (!in) return zip_writer_fail(writer, err, NK_ZIP_ERR_IO, name, name_size);
    for (;;) {
        size_t got = fread(writer->chunk, 1, ZIP_CHUNK, in);
        if (got == 0) break;
        size += got;
        if (size > 0xFFFFFFFEull || writer->total + size > NK_ZIP_MAX_TOTAL_BYTES ||
            writer->offset + size > 0xFFFFFFFFull) {
            fclose(in);
            return zip_writer_fail(writer, err, NK_ZIP_ERR_TOO_LARGE, name, name_size);
        }
        crc = zip_crc_update(writer->crc_table, crc, writer->chunk, got);
        if (fwrite(writer->chunk, 1, got, writer->file) != got) {
            fclose(in);
            return zip_writer_fail(writer, err, NK_ZIP_ERR_IO, name, name_size);
        }
    }
    if (ferror(in)) {
        fclose(in);
        return zip_writer_fail(writer, err, NK_ZIP_ERR_IO, name, name_size);
    }
    fclose(in);
    /* The CRC and sizes go back into the local header (offset 14). */
    wr32(fix, crc);
    wr32(fix + 4, (uint32_t)size);
    wr32(fix + 8, (uint32_t)size);
    if (nk_fseek64(writer->file, (int64_t)e->offset + 14, SEEK_SET) != 0 ||
        fwrite(fix, 1, sizeof(fix), writer->file) != sizeof(fix) ||
        nk_fseek64(writer->file, 0, SEEK_END) != 0) {
        return zip_writer_fail(writer, err, NK_ZIP_ERR_IO, name, name_size);
    }
    e->crc = crc;
    e->size = (uint32_t)size;
    writer->offset += size;
    writer->total += size;
    return zip_ok(err);
}

static void zip_writer_free(NkZipWriter *w) {
    for (size_t i = 0; i < w->count; ++i) free(w->entries[i].name);
    free(w->entries);
    free(w->chunk);
    free(w->path);
    free(w);
}

void nk_zip_writer_discard(NkZipWriter *writer) {
    if (!writer) return;
    if (writer->file) fclose(writer->file);
    (void)nk_remove_utf8(writer->path);
    zip_writer_free(writer);
}

NkZipStatus nk_zip_writer_finish(NkZipWriter *writer, NkZipError *err) {
    ZipNameRef *refs;
    uint64_t central_offset;
    uint8_t h[ZIP_CENTRAL_SIZE];
    NkZipStatus status = NK_ZIP_OK;
    NkZipError local;
    if (!writer) return zip_fail(err, NK_ZIP_ERR_ARGUMENT, NULL, 0);
    if (writer->failed != NK_ZIP_OK) {
        if (err) {
            err->status = writer->failed;
            memcpy(err->entry, writer->failed_entry, sizeof(err->entry));
        }
        status = writer->failed;
        nk_zip_writer_discard(writer);
        return status;
    }
    /* What the reader would refuse is not written. */
    refs = (ZipNameRef *)calloc(writer->count ? writer->count : 1u, sizeof(ZipNameRef));
    if (!refs) {
        nk_zip_writer_discard(writer);
        return zip_fail(err, NK_ZIP_ERR_NO_MEMORY, NULL, 0);
    }
    for (size_t i = 0; i < writer->count; ++i) {
        refs[i].name = writer->entries[i].name;
        refs[i].size = writer->entries[i].folder ? writer->entries[i].name_size - 1u
                                                 : writer->entries[i].name_size;
        refs[i].folder = writer->entries[i].folder;
    }
    status = zip_names_distinct(refs, writer->count, &local);
    free(refs);
    if (status != NK_ZIP_OK) {
        if (err) *err = local;
        nk_zip_writer_discard(writer);
        return status;
    }
    central_offset = writer->offset;
    for (size_t i = 0; i < writer->count && status == NK_ZIP_OK; ++i) {
        const ZipWriterEntry *e = &writer->entries[i];
        wr32(h, ZIP_SIG_CENTRAL);
        wr16(h + 4, ZIP_VERSION_MADE_BY);
        wr16(h + 6, ZIP_VERSION_STORED);
        wr16(h + 8, e->flags);
        wr16(h + 10, 0u);
        wr16(h + 12, writer->dos_time);
        wr16(h + 14, writer->dos_date);
        wr32(h + 16, e->crc);
        wr32(h + 20, e->size);
        wr32(h + 24, e->size);
        wr16(h + 28, e->name_size);
        wr16(h + 30, 0u);
        wr16(h + 32, 0u);
        wr16(h + 34, 0u);
        wr16(h + 36, 0u);
        wr32(h + 38, e->folder ? ZIP_DOS_FOLDER : 0u);
        wr32(h + 42, e->offset);
        if (fwrite(h, 1, sizeof(h), writer->file) != sizeof(h) ||
            fwrite(e->name, 1, e->name_size, writer->file) != e->name_size) {
            status = zip_fail(err, NK_ZIP_ERR_IO, e->name, e->name_size);
        }
        writer->offset += ZIP_CENTRAL_SIZE + e->name_size;
    }
    if (status == NK_ZIP_OK && writer->offset > 0xFFFFFFFFull) {
        status = zip_fail(err, NK_ZIP_ERR_TOO_LARGE, NULL, 0);
    }
    if (status == NK_ZIP_OK) {
        uint8_t end[ZIP_END_SIZE];
        wr32(end, ZIP_SIG_END);
        wr16(end + 4, 0u);
        wr16(end + 6, 0u);
        wr16(end + 8, (uint16_t)writer->count);
        wr16(end + 10, (uint16_t)writer->count);
        wr32(end + 12, (uint32_t)(writer->offset - central_offset));
        wr32(end + 16, (uint32_t)central_offset);
        wr16(end + 20, 0u);
        if (fwrite(end, 1, sizeof(end), writer->file) != sizeof(end)) {
            status = zip_fail(err, NK_ZIP_ERR_IO, NULL, 0);
        }
    }
    if (status == NK_ZIP_OK) {
        int closed = fclose(writer->file);
        writer->file = NULL;
        if (closed != 0) status = zip_fail(err, NK_ZIP_ERR_IO, NULL, 0);
    }
    if (status != NK_ZIP_OK) {
        nk_zip_writer_discard(writer);
        return status;
    }
    zip_writer_free(writer);
    return zip_ok(err);
}

/* ---- Writing a folder ---- */

typedef struct {
    char *name;
    bool folder;
} ZipListed;

typedef struct {
    ZipListed *items;
    size_t count;
    size_t capacity;
    bool failed;
} ZipListing;

static bool zip_list_add(const char *name, bool is_dir, void *ctx) {
    ZipListing *list = (ZipListing *)ctx;
    size_t size = strlen(name) + 1u;
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2u : 16u;
        ZipListed *grown = (ZipListed *)realloc(list->items, capacity * sizeof(ZipListed));
        if (!grown) {
            list->failed = true;
            return false;
        }
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count].name = (char *)malloc(size);
    if (!list->items[list->count].name) {
        list->failed = true;
        return false;
    }
    memcpy(list->items[list->count].name, name, size);
    list->items[list->count].folder = is_dir;
    list->count++;
    return true;
}

static int zip_listed_compare(const void *a, const void *b) {
    return strcmp(((const ZipListed *)a)->name, ((const ZipListed *)b)->name);
}

static void zip_listing_free(ZipListing *list) {
    for (size_t i = 0; i < list->count; ++i) free(list->items[i].name);
    free(list->items);
}

static NkZipStatus zip_write_tree(NkZipWriter *w, const char *host, const char *relative,
                                  unsigned depth, size_t *entries, uint64_t *bytes,
                                  NkZipError *err) {
    ZipListing list;
    NkZipStatus status = NK_ZIP_OK;
    char sep = nk_platform_path_separator();
    memset(&list, 0, sizeof(list));
    if (depth > NK_ZIP_MAX_DEPTH) {
        return zip_writer_fail(w, err, NK_ZIP_ERR_NAME_TOO_DEEP, relative, strlen(relative));
    }
    if (!nk_platform_list_dir(host, zip_list_add, &list) || list.failed) {
        zip_listing_free(&list);
        return zip_writer_fail(w, err, list.failed ? NK_ZIP_ERR_NO_MEMORY : NK_ZIP_ERR_IO,
                               relative, strlen(relative));
    }
    qsort(list.items, list.count, sizeof(list.items[0]), zip_listed_compare);
    for (size_t i = 0; i < list.count && status == NK_ZIP_OK; ++i) {
        char name[NK_ZIP_MAX_NAME_BYTES + 2];
        char path[NK_ZIP_MAX_NAME_BYTES + 4096];
        int n = snprintf(name, sizeof(name), "%s%s%s", relative, relative[0] ? "/" : "",
                         list.items[i].name);
        int m = snprintf(path, sizeof(path), "%s%c%s", host, sep, list.items[i].name);
        if (n <= 0 || (size_t)n > NK_ZIP_MAX_NAME_BYTES || m <= 0 || (size_t)m >= sizeof(path)) {
            status = zip_writer_fail(w, err, NK_ZIP_ERR_NAME_TOO_LONG, list.items[i].name,
                                     strlen(list.items[i].name));
            break;
        }
        if (list.items[i].folder) {
            status = nk_zip_writer_add_folder(w, name, err);
            if (status == NK_ZIP_OK) {
                ++*entries;
                status = zip_write_tree(w, path, name, depth + 1u, entries, bytes, err);
            }
        } else {
            uint64_t before = w->total;
            status = nk_zip_writer_add_file(w, name, path, err);
            if (status == NK_ZIP_OK) {
                ++*entries;
                *bytes += w->total - before;
            }
        }
    }
    zip_listing_free(&list);
    return status;
}

NkZipStatus nk_zip_write_folder(const char *folder, const char *zip_path, uint16_t dos_date,
                                uint16_t dos_time, size_t *out_entries, uint64_t *out_bytes,
                                NkZipError *err) {
    NkZipWriter *w = NULL;
    NkZipStatus status;
    size_t entries = 0;
    uint64_t bytes = 0;
    if (out_entries) *out_entries = 0;
    if (out_bytes) *out_bytes = 0;
    if (!folder || !folder[0] || !zip_path || !zip_path[0]) {
        return zip_fail(err, NK_ZIP_ERR_ARGUMENT, NULL, 0);
    }
    if (!nk_platform_dir_exists(folder)) return zip_fail(err, NK_ZIP_ERR_IO, NULL, 0);
    status = nk_zip_writer_open(&w, zip_path, dos_date, dos_time, err);
    if (status != NK_ZIP_OK) return status;
    status = zip_write_tree(w, folder, "", 1u, &entries, &bytes, err);
    if (status != NK_ZIP_OK) {
        nk_zip_writer_discard(w);
        return status;
    }
    status = nk_zip_writer_finish(w, err);
    if (status != NK_ZIP_OK) return status;
    if (out_entries) *out_entries = entries;
    if (out_bytes) *out_bytes = bytes;
    return NK_ZIP_OK;
}

#endif /* NK_ZIP_NO_HOST_IO */
