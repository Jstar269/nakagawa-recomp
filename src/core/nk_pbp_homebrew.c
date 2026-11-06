/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Homebrew in an EBOOT.PBP (issue #2143): see nk_pbp_homebrew.h. The disc
 * image is written from ECMA-119 and the PARAM.SFO layout; no third-party
 * code is used.
 */

#include "nk_pbp_homebrew.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nk_iso.h"
#include "nk_platform.h"

#define HB_PBP_HEADER_SIZE 0x28u
#define HB_SECTOR 2048u
#define HB_CHUNK (64u * 1024u)
#define HB_MAX_ICON_BYTES (1024u * 1024u)
#define HB_SFO_SIZE 240u
#define HB_SFO_TITLE_MAX 127u
/* The sectors before the program: system area (0-15), primary volume
   descriptor (16), terminator (17), root (18), PSP_GAME (19), SYSDIR (20) and
   PARAM.SFO (21). */
#define HB_PROGRAM_LBA 22u
#define HB_PLACEHOLDER_DISC_ID "NKHB00000"

static uint32_t hb_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t hb_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void hb_put_le16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void hb_put_le32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static int hb_read_at(FILE *f, uint64_t offset, void *buf, size_t len)
{
    if (nk_fseek64(f, (int64_t)offset, SEEK_SET) != 0) return -1;
    return fread(buf, 1, len, f) == len ? 0 : -1;
}

/* The PARAM.SFO the image carries: CATEGORY, DISC_ID and TITLE, 240 bytes. */
static void hb_build_sfo(const char *title, uint8_t sfo[HB_SFO_SIZE])
{
    static const char keys[] = "CATEGORY\0DISC_ID\0TITLE";
    size_t length = 0;
    char clean[HB_SFO_TITLE_MAX + 1];
    memset(sfo, 0, HB_SFO_SIZE);
    for (; title != NULL && title[length] != '\0' && length < HB_SFO_TITLE_MAX; length++) {
        unsigned char c = (unsigned char)title[length];
        clean[length] = c < 0x20u || c == 0x7Fu ? ' ' : (char)c;
    }
    clean[length] = '\0';
    if (length == 0) {
        memcpy(clean, "Homebrew", 9);
        length = 8;
    }
    memcpy(sfo, "\0PSF", 4);
    hb_put_le32(sfo + 4, 0x00000101u);
    hb_put_le32(sfo + 8, 68u);  /* key table */
    hb_put_le32(sfo + 12, 92u); /* data table */
    hb_put_le32(sfo + 16, 3u);
    /* (key offset, format 0x0204 UTF-8, length, maximum length, data offset) */
    {
        const struct { uint16_t key; uint32_t len, max, data; } rows[3] = {
            { 0u, 3u, 4u, 0u },
            { 9u, (uint32_t)strlen(HB_PLACEHOLDER_DISC_ID) + 1u, 16u, 4u },
            { 17u, (uint32_t)length + 1u, 128u, 20u },
        };
        for (int i = 0; i < 3; i++) {
            uint8_t *row = sfo + 20 + i * 16;
            hb_put_le16(row, rows[i].key);
            hb_put_le16(row + 2, 0x0204u);
            hb_put_le32(row + 4, rows[i].len);
            hb_put_le32(row + 8, rows[i].max);
            hb_put_le32(row + 12, rows[i].data);
        }
    }
    memcpy(sfo + 68, keys, sizeof(keys));
    memcpy(sfo + 92, "UG", 2);
    memcpy(sfo + 92 + 4, HB_PLACEHOLDER_DISC_ID, strlen(HB_PLACEHOLDER_DISC_ID));
    memcpy(sfo + 92 + 20, clean, length);
}

typedef struct {
    uint32_t offsets[8];
    uint64_t size;
} HbPackage;

static int hb_read_package(NkPspCtx *ctx, FILE *f, HbPackage *pkg)
{
    uint8_t header[HB_PBP_HEADER_SIZE];
    int64_t end;
    if (nk_fseek64(f, 0, SEEK_END) != 0 || (end = nk_ftell64(f)) < 0) {
        return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL, "the file's size could not be read");
    }
    pkg->size = (uint64_t)end;
    if (hb_read_at(f, 0, header, sizeof(header)) != 0 || memcmp(header, "\0PBP", 4) != 0) {
        return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL, "the file is not a PBP package");
    }
    for (int i = 0; i < 8; i++) {
        pkg->offsets[i] = hb_le32(header + 8 + i * 4);
        if (pkg->offsets[i] < HB_PBP_HEADER_SIZE || (uint64_t)pkg->offsets[i] > pkg->size ||
            (i > 0 && pkg->offsets[i] < pkg->offsets[i - 1])) {
            return nk_psp_fail(ctx, NK_PSP_ERR_FORMAT, NULL,
                               "the PBP package's section table is malformed");
        }
    }
    return NK_PSP_OK;
}

/* Whether the module declares kernel mode: SceModuleInfo's attribute word
   carries 0x1000. The record is found the way a PRX loader finds it, through
   the first program header's p_paddr; a module that declares none is user
   mode. Imports only custom firmware provides are refused later, by the import
   table check code generation runs on every program. */
static int hb_module_is_kernel_mode(FILE *f, uint64_t program_offset, uint32_t program_size,
                                    bool *kernel)
{
    uint8_t elf[0x34];
    uint8_t phdr[32];
    uint8_t attr[2];
    uint32_t phoff, paddr, poffset;
    uint16_t phnum, phentsize;
    uint64_t record;
    *kernel = false;
    if (hb_read_at(f, program_offset, elf, sizeof(elf)) != 0) return -1;
    phoff = hb_le32(elf + 28);
    phentsize = hb_le16(elf + 42);
    phnum = hb_le16(elf + 44);
    if (phnum == 0 || phentsize < sizeof(phdr) || (uint64_t)phoff + sizeof(phdr) > program_size)
        return 0;
    if (hb_read_at(f, program_offset + phoff, phdr, sizeof(phdr)) != 0) return -1;
    poffset = hb_le32(phdr + 4);
    paddr = hb_le32(phdr + 12);
    if (paddr == 0u) return 0;
    record = (uint64_t)poffset + (paddr & 0x7FFFFFFFu);
    if (record + sizeof(attr) > program_size) return 0;
    if (hb_read_at(f, program_offset + record, attr, sizeof(attr)) != 0) return -1;
    *kernel = (hb_le16(attr) & 0x1000u) != 0;
    return 0;
}

static void hb_u64(NkSha256 *sha, uint64_t value)
{
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++) bytes[i] = (uint8_t)(value >> (8 * i));
    nk_sha256_update(sha, bytes, sizeof(bytes));
}

/* The key nk_iso_inspect gives the finished image: the digest over the domain
   tag (with its terminating NUL, as nk_iso.c hashes it), then PARAM.SFO and
   EBOOT.BIN, each after its length as a little-endian 64-bit integer. */
static int hb_key(NkPspCtx *ctx, FILE *f, uint64_t program_offset, uint32_t program_size,
                  const uint8_t sfo[HB_SFO_SIZE], char key[10])
{
    static const char domain[] = "nakagawa-non-retail-disc-key-v1";
    NkSha256 sha;
    uint8_t digest[32];
    uint8_t *buffer = (uint8_t *)malloc(HB_CHUNK);
    uint32_t done = 0;
    if (buffer == NULL) return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL, "out of memory");
    nk_sha256_init(&sha);
    nk_sha256_update(&sha, (const uint8_t *)domain, sizeof(domain));
    hb_u64(&sha, HB_SFO_SIZE);
    nk_sha256_update(&sha, sfo, HB_SFO_SIZE);
    hb_u64(&sha, program_size);
    while (done < program_size) {
        uint32_t count = program_size - done < HB_CHUNK ? program_size - done : HB_CHUNK;
        if (hb_read_at(f, program_offset + done, buffer, count) != 0) {
            free(buffer);
            return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                               "the package's program could not be read");
        }
        nk_sha256_update(&sha, buffer, count);
        done += count;
    }
    free(buffer);
    nk_sha256_finish(&sha, digest);
    nk_non_retail_disc_key(digest, key);
    return NK_PSP_OK;
}

int nk_pbp_homebrew_inspect_file(NkPspCtx *ctx, const char *pbp_path, const NkPbpInfo *info,
                                 NkPbpHomebrew *out)
{
    FILE *f;
    HbPackage pkg;
    uint8_t head[0x34];
    uint8_t sfo[HB_SFO_SIZE];
    uint8_t png[8];
    uint32_t program_size;
    uint64_t program_offset;
    bool kernel = false;
    int rc;

    if (ctx == NULL || pbp_path == NULL || info == NULL || out == NULL) return NK_PSP_ERR_INTERNAL;
    memset(out, 0, sizeof(*out));
    snprintf(out->title, sizeof(out->title), "%s", info->title);
    f = nk_fopen_utf8(pbp_path, "rb");
    if (f == NULL) {
        return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL, "the file could not be opened");
    }
    rc = hb_read_package(ctx, f, &pkg);
    if (rc != NK_PSP_OK) {
        fclose(f);
        return rc;
    }
    program_offset = pkg.offsets[6];
    program_size = pkg.offsets[7] - pkg.offsets[6];
    out->program_size = program_size;
    if (program_size > NK_PBP_HOMEBREW_MAX_PROGRAM_BYTES) {
        out->kind = NK_PBP_HB_TOO_LARGE;
        fclose(f);
        return nk_psp_fail(ctx, NK_PSP_ERR_UNSUPPORTED, NULL,
                           "the package's program is larger than Nakagawa reads");
    }
    if (program_size >= sizeof(head)) {
        if (hb_read_at(f, program_offset, head, sizeof(head)) != 0) {
            fclose(f);
            return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                               "the package's program could not be read");
        }
    } else {
        memset(head, 0, sizeof(head));
        if (program_size > 0 && hb_read_at(f, program_offset, head, program_size) != 0) {
            fclose(f);
            return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                               "the package's program could not be read");
        }
    }
    if (program_size >= 4 && memcmp(head, "~PSP", 4) == 0) {
        out->kind = NK_PBP_HB_ENCRYPTED;
        fclose(f);
        return nk_psp_fail(ctx, NK_PSP_ERR_UNSUPPORTED, NULL,
                           "the package's program is encrypted");
    }
    if (program_size < sizeof(head) || memcmp(head, "\x7f" "ELF", 4) != 0 || head[4] != 1 ||
        head[5] != 1 || hb_le16(head + 18) != 8u ||
        (hb_le16(head + 16) != 2u && hb_le16(head + 16) != 0xFFA0u)) {
        out->kind = NK_PBP_HB_NOT_A_PROGRAM;
        fclose(f);
        return nk_psp_fail(ctx, NK_PSP_ERR_UNSUPPORTED, NULL,
                           "the package's program is not a PSP program");
    }
    if (hb_module_is_kernel_mode(f, program_offset, program_size, &kernel) != 0) {
        fclose(f);
        return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                           "the package's program could not be read");
    }
    if (kernel) {
        out->kind = NK_PBP_HB_KERNEL_MODE;
        fclose(f);
        return nk_psp_fail(ctx, NK_PSP_ERR_UNSUPPORTED, NULL,
                           "the package's program is kernel-mode homebrew");
    }
    {
        uint32_t icon = pkg.offsets[2] - pkg.offsets[1];
        if (icon >= sizeof(png) && icon <= HB_MAX_ICON_BYTES &&
            hb_read_at(f, pkg.offsets[1], png, sizeof(png)) == 0 &&
            memcmp(png, "\x89PNG\r\n\x1a\n", 8) == 0) {
            out->icon_size = icon;
        }
    }
    hb_build_sfo(info->title, sfo);
    rc = hb_key(ctx, f, program_offset, program_size, sfo, out->key);
    fclose(f);
    out->kind = NK_PBP_HB_PROGRAM;
    return rc;
}

/* ECMA-119 9.1 directory record, both-endian extent and size. */
static size_t hb_dir_record(uint8_t *record, const uint8_t *name, size_t name_len, uint32_t lba,
                            uint32_t size, bool is_dir)
{
    size_t length = 33 + name_len + ((name_len & 1u) == 0 ? 1u : 0u);
    memset(record, 0, length);
    record[0] = (uint8_t)length;
    for (int i = 0; i < 4; i++) {
        record[2 + i] = (uint8_t)(lba >> (8 * i));
        record[9 - i] = (uint8_t)(lba >> (8 * i));
        record[10 + i] = (uint8_t)(size >> (8 * i));
        record[17 - i] = (uint8_t)(size >> (8 * i));
    }
    record[25] = is_dir ? 0x02 : 0x00;
    record[28] = 1;
    record[31] = 1;
    record[32] = (uint8_t)name_len;
    memcpy(record + 33, name, name_len);
    return length;
}

static size_t hb_dir_start(uint8_t *sector, uint32_t self, uint32_t parent)
{
    static const uint8_t dot[1] = { 0 };
    static const uint8_t dotdot[1] = { 1 };
    size_t used = hb_dir_record(sector, dot, 1, self, HB_SECTOR, true);
    return used + hb_dir_record(sector + used, dotdot, 1, parent, HB_SECTOR, true);
}

static uint32_t hb_sectors(uint32_t bytes)
{
    return (bytes + HB_SECTOR - 1u) / HB_SECTOR;
}

/* The image's first HB_PROGRAM_LBA sectors. */
static void hb_build_head(uint8_t *head, const uint8_t sfo[HB_SFO_SIZE], uint32_t program_size,
                          uint32_t icon_size, uint32_t total_sectors)
{
    uint32_t icon_lba = HB_PROGRAM_LBA + hb_sectors(program_size);
    uint8_t *pvd = head + 16u * HB_SECTOR;
    uint8_t *root = head + 18u * HB_SECTOR;
    uint8_t *game = head + 19u * HB_SECTOR;
    uint8_t *sysdir = head + 20u * HB_SECTOR;
    size_t used;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    memset(pvd + 8, ' ', 64); /* system and volume identifiers */
    memcpy(pvd + 40, "NAKAGAWA_HOMEBREW", 17);
    hb_put_le32(pvd + 80, total_sectors);
    for (int i = 0; i < 4; i++) pvd[87 - i] = (uint8_t)(total_sectors >> (8 * i));
    hb_put_le16(pvd + 120, 1u);
    pvd[123] = 1;
    hb_put_le16(pvd + 124, 1u);
    pvd[127] = 1;
    hb_put_le16(pvd + 128, HB_SECTOR);
    pvd[131] = (uint8_t)(HB_SECTOR & 0xFFu);
    pvd[130] = (uint8_t)(HB_SECTOR >> 8);
    {
        static const uint8_t dot[1] = { 0 };
        hb_dir_record(pvd + 156, dot, 1, 18u, HB_SECTOR, true);
    }
    pvd[881] = 1;
    {
        uint8_t *terminator = head + 17u * HB_SECTOR;
        terminator[0] = 255;
        memcpy(terminator + 1, "CD001", 5);
        terminator[6] = 1;
    }
    used = hb_dir_start(root, 18u, 18u);
    hb_dir_record(root + used, (const uint8_t *)"PSP_GAME", 8, 19u, HB_SECTOR, true);
    used = hb_dir_start(game, 19u, 18u);
    if (icon_size > 0) {
        used += hb_dir_record(game + used, (const uint8_t *)"ICON0.PNG", 9, icon_lba, icon_size,
                              false);
    }
    used += hb_dir_record(game + used, (const uint8_t *)"PARAM.SFO", 9, 21u, HB_SFO_SIZE, false);
    hb_dir_record(game + used, (const uint8_t *)"SYSDIR", 6, 20u, HB_SECTOR, true);
    used = hb_dir_start(sysdir, 20u, 19u);
    hb_dir_record(sysdir + used, (const uint8_t *)"EBOOT.BIN", 9, HB_PROGRAM_LBA, program_size,
                  false);
    memcpy(head + 21u * HB_SECTOR, sfo, HB_SFO_SIZE);
}

static int hb_write_padded(FILE *out, size_t written_in_sector)
{
    static const uint8_t zeros[HB_SECTOR] = { 0 };
    size_t pad = (HB_SECTOR - written_in_sector % HB_SECTOR) % HB_SECTOR;
    return pad == 0 || fwrite(zeros, 1, pad, out) == pad ? 0 : -1;
}

/* Copy count bytes of the package to out, padded to a sector. */
static int hb_copy_section(NkPspCtx *ctx, FILE *in, uint64_t offset, uint32_t count, FILE *out,
                           const NkNpumdimgCallbacks *callbacks, uint64_t *done, uint64_t total)
{
    uint8_t *buffer = (uint8_t *)malloc(HB_CHUNK);
    uint32_t copied = 0;
    if (buffer == NULL) return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL, "out of memory");
    while (copied < count) {
        uint32_t step = count - copied < HB_CHUNK ? count - copied : HB_CHUNK;
        if (callbacks != NULL && callbacks->is_cancelled != NULL &&
            callbacks->is_cancelled(callbacks->user)) {
            free(buffer);
            return nk_psp_fail(ctx, NK_PSP_ERR_CANCELLED, NULL,
                               "the conversion was cancelled; the partial disc image was "
                               "removed");
        }
        if (hb_read_at(in, offset + copied, buffer, step) != 0) {
            free(buffer);
            return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                               "the package's program could not be read");
        }
        if (fwrite(buffer, 1, step, out) != step) {
            free(buffer);
            return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                               "the disc image could not be written (is the disk full?)");
        }
        copied += step;
        *done += step;
        if (callbacks != NULL && callbacks->on_progress != NULL)
            callbacks->on_progress(callbacks->user, *done, total);
    }
    free(buffer);
    if (hb_write_padded(out, count) != 0) {
        return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                           "the disc image could not be written (is the disk full?)");
    }
    return NK_PSP_OK;
}

int nk_pbp_homebrew_convert_file(NkPspCtx *ctx, const char *pbp_path, const NkPbpInfo *info,
                                 const char *iso_path, const NkNpumdimgCallbacks *callbacks,
                                 NkPbpHomebrew *out)
{
    NkPbpHomebrew local;
    NkPbpHomebrew *hb = out != NULL ? out : &local;
    HbPackage pkg;
    FILE *in = NULL;
    FILE *img = NULL;
    uint8_t *head = NULL;
    uint8_t sfo[HB_SFO_SIZE];
    char *temp = NULL;
    size_t temp_len;
    uint32_t total_sectors;
    uint64_t done = 0, total;
    NkIsoMetadata *meta = NULL;
    int rc;

    if (ctx == NULL || pbp_path == NULL || info == NULL || iso_path == NULL)
        return NK_PSP_ERR_INTERNAL;
    rc = nk_pbp_homebrew_inspect_file(ctx, pbp_path, info, hb);
    if (rc != NK_PSP_OK) return rc;
    in = nk_fopen_utf8(pbp_path, "rb");
    if (in == NULL) return nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL, "the file could not be opened");
    rc = hb_read_package(ctx, in, &pkg);
    if (rc != NK_PSP_OK) {
        fclose(in);
        return rc;
    }
    hb_build_sfo(info->title, sfo);
    total_sectors = HB_PROGRAM_LBA + hb_sectors(hb->program_size) + hb_sectors(hb->icon_size);
    total = (uint64_t)hb->program_size + hb->icon_size;
    head = (uint8_t *)calloc(HB_PROGRAM_LBA, HB_SECTOR);
    temp_len = strlen(iso_path) + sizeof(".converting");
    temp = (char *)malloc(temp_len);
    meta = (NkIsoMetadata *)calloc(1, sizeof(*meta));
    if (head == NULL || temp == NULL || meta == NULL) {
        rc = nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL, "out of memory");
        goto done;
    }
    snprintf(temp, temp_len, "%s.converting", iso_path);
    (void)nk_remove_utf8(temp);
    hb_build_head(head, sfo, hb->program_size, hb->icon_size, total_sectors);
    img = nk_fopen_utf8(temp, "wb");
    if (img == NULL) {
        rc = nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL, "the disc image could not be created");
        goto done;
    }
    if (fwrite(head, 1, (size_t)HB_PROGRAM_LBA * HB_SECTOR, img) !=
        (size_t)HB_PROGRAM_LBA * HB_SECTOR) {
        rc = nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                         "the disc image could not be written (is the disk full?)");
        goto done;
    }
    rc = hb_copy_section(ctx, in, pkg.offsets[6], hb->program_size, img, callbacks, &done, total);
    if (rc == NK_PSP_OK && hb->icon_size > 0) {
        rc = hb_copy_section(ctx, in, pkg.offsets[1], hb->icon_size, img, callbacks, &done,
                             total);
    }
    if (rc != NK_PSP_OK) goto done;
    if (!nk_platform_sync_file(img) || fclose(img) != 0) {
        img = NULL;
        rc = nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                         "the disc image could not be written (is the disk full?)");
        goto done;
    }
    img = NULL;
    /* The finished image must be the game the inspection named. */
    if (nk_iso_inspect(temp, meta) != NK_OK || !meta->non_retail ||
        strcmp(meta->disc_id, hb->key) != 0) {
        rc = nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL,
                         "the disc image written for this package was not read back as "
                         "the same game");
        goto done;
    }
    if (nk_rename_utf8(temp, iso_path) != 0) {
        rc = nk_psp_fail(ctx, NK_PSP_ERR_INPUT, NULL,
                         "the disc image could not be moved into place");
        goto done;
    }

done:
    if (img != NULL) fclose(img);
    if (rc != NK_PSP_OK && temp != NULL) (void)nk_remove_utf8(temp);
    free(meta);
    free(temp);
    free(head);
    fclose(in);
    return rc;
}
