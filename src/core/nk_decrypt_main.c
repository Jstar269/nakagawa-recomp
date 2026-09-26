/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * nk_decrypt -- command-line entry for the built-in PSP decryption
 * boundary (issue #295).  The Python tooling (tools/nk_core) and the
 * player integration call this binary; it never embeds key material:
 * keys come exclusively from a user-supplied local key file and a
 * missing entry fails closed naming the exact entry required.
 *
 * Subcommands:
 *   kat    --vector {aes-ecb|aes-cbc|aes-cmac|sha1}
 *          Print the published known-answer vector as JSON (exit 0).
 *   probe  --in FILE [--key-file FILE]
 *          Report which key entries the container needs (exit 0).
 *   decrypt --key-file FILE --in FILE --out FILE
 *          Run the full production boundary; the output file is written
 *          only on success.  Failures print one of:
 *          MISSING_KEY_ENTRY / CONTAINER_MALFORMED / INTEGRITY /
 *          KEYFILE_INVALID / DECOMPRESS_FAILED / INPUT_UNREADABLE /
 *          UNSUPPORTED_CONTAINER and exit non-zero.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nk_psp_aes.h"
#include "nk_psp_container.h"
#include "nk_psp_crypto.h"
#include "nk_psp_keystore.h"
#include "nk_psp_sha1.h"

/* --------------------------- helpers --------------------------------- */

static void to_hex(const u8 *bytes, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0Fu];
    }
    out[n * 2] = '\0';
}

static int read_file(const char *path, u8 **data, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long len;
    u8 *buf;
    size_t got;
    if (f == NULL) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    len = ftell(f);
    if (len <= 0 || len > (long)(512u * 1024u * 1024u)) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    buf = (u8 *)malloc((size_t)len);
    if (buf == NULL) { fclose(f); return -1; }
    got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) { free(buf); return -1; }
    *data = buf;
    *size = (size_t)len;
    return 0;
}

static int write_file(const char *path, const u8 *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    size_t put;
    if (f == NULL) return -1;
    put = fwrite(data, 1, size, f);
    if (fclose(f) != 0) return -1;
    return put == size ? 0 : -1;
}

static int file_present(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return 0;
    fclose(f);
    return 1;
}

/* --------------------------- diagnostics ------------------------------ */

static int print_diagnostic(int rc, const NkPspCtx *ctx)
{
    const char *entry = (ctx != NULL && ctx->missing_entry[0] != '\0')
                            ? ctx->missing_entry
                            : NULL;
    const char *msg = (ctx != NULL && ctx->message[0] != '\0')
                          ? ctx->message
                          : NULL;
    switch (rc) {
    case NK_PSP_ERR_MISSING_KEY:
        printf("MISSING_KEY_ENTRY %s\n", entry != NULL ? entry : "(unknown)");
        printf("this executable needs key entry %s\n",
               entry != NULL ? entry : "(unknown)");
        if (msg != NULL) printf("%s\n", msg);
        return 3;
    case NK_PSP_ERR_FORMAT:
        printf("CONTAINER_MALFORMED\n");
        if (msg != NULL) printf("%s\n", msg);
        return 4;
    case NK_PSP_ERR_INTEGRITY:
        printf("INTEGRITY_CHECK_FAILED\n");
        if (msg != NULL) printf("%s\n", msg);
        return 5;
    case NK_PSP_ERR_KEYFILE:
        printf("KEYFILE_INVALID\n");
        if (msg != NULL) printf("%s\n", msg);
        return 6;
    case NK_PSP_ERR_UNSUPPORTED:
        printf("UNSUPPORTED_CONTAINER\n");
        if (msg != NULL) printf("%s\n", msg);
        return 7;
    case NK_PSP_ERR_DECOMPRESS:
        printf("DECOMPRESS_FAILED\n");
        if (msg != NULL) printf("%s\n", msg);
        return 8;
    case NK_PSP_ERR_INPUT:
        printf("INPUT_UNREADABLE\n");
        if (msg != NULL) printf("%s\n", msg);
        return 1;
    default:
        printf("INTERNAL_ERROR\n");
        if (msg != NULL) printf("%s\n", msg);
        return 9;
    }
}

/* ------------------------------ kat ----------------------------------- */

/*
 * Published known-answer vectors: FIPS 197 appendix C.1 (AES-128),
 * RFC 4493 section 4 (AES-CMAC), RFC 3174 (SHA-1).  These are standard
 * public test vectors used to prove the primitives; they are not key
 * material for any protected content.
 */
static const u8 kat_aes_plain[16] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
static const u8 kat_aes_k[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
static const u8 kat_cmac_k[16] = {
    0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
    0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
static const u8 kat_cmac_msg[16] = {
    0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
    0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a};

static int cmd_kat(const char *vector)
{
    char hex[129];
    if (vector == NULL) {
        fprintf(stderr, "kat: --vector is required\n");
        return 2;
    }
    if (strcmp(vector, "aes-ecb") == 0) {
        AES_ctx ctx;
        u8 cipher[16];
        u8 back[16];
        AES_set_key(&ctx, kat_aes_k, 128);
        AES_encrypt(&ctx, kat_aes_plain, cipher);
        AES_decrypt(&ctx, cipher, back);
        to_hex(cipher, 16, hex);
        printf("{\"cipher\":\"%s\",", hex);
        to_hex(back, 16, hex);
        printf("\"roundtrip\":\"%s\"}\n", hex);
        return 0;
    }
    if (strcmp(vector, "aes-cbc") == 0) {
        AES_ctx ctx;
        u8 cipher[16];
        u8 back[16];
        AES_set_key(&ctx, kat_aes_k, 128);
        AES_cbc_encrypt(&ctx, kat_aes_plain, cipher, 16);
        AES_cbc_decrypt(&ctx, cipher, back, 16);
        to_hex(cipher, 16, hex);
        printf("{\"cipher\":\"%s\",", hex);
        to_hex(back, 16, hex);
        printf("\"roundtrip\":\"%s\"}\n", hex);
        return 0;
    }
    if (strcmp(vector, "aes-cmac") == 0) {
        AES_ctx ctx;
        u8 msg[16];
        u8 mac[16];
        AES_set_key(&ctx, kat_cmac_k, 128);
        /* RFC 4493 section 4, example 1 (empty message). */
        AES_CMAC(&ctx, msg, 0, mac);
        to_hex(mac, 16, hex);
        printf("{\"mac\":\"%s\",", hex);
        /* RFC 4493 section 4, example 2 (one full block). */
        memcpy(msg, kat_cmac_msg, 16);
        AES_CMAC(&ctx, msg, 16, mac);
        to_hex(mac, 16, hex);
        printf("\"mac_msg16\":\"%s\"}\n", hex);
        return 0;
    }
    if (strcmp(vector, "sha1") == 0) {
        SHA_CTX sha;
        u8 digest[20];
        BYTE msg[3];
        msg[0] = (BYTE)'a';
        msg[1] = (BYTE)'b';
        msg[2] = (BYTE)'c';
        SHAInit(&sha);
        SHAUpdate(&sha, msg, 3);
        SHAFinal(digest, &sha);
        to_hex(digest, 20, hex);
        printf("{\"digest\":\"%s\"}\n", hex);
        return 0;
    }
    fprintf(stderr, "kat: unknown vector '%s'\n", vector);
    return 2;
}

/* ----------------------------- probe ---------------------------------- */

static const char *kind_name(NkContainerKind kind)
{
    switch (kind) {
    case NK_CTR_PLAIN_ELF: return "elf";
    case NK_CTR_PSP: return "psp";
    case NK_CTR_SCE_WRAPPER: return "sce-wrapper";
    case NK_CTR_PBP: return "pbp";
    case NK_CTR_GZIP: return "gzip";
    default: return "unknown";
    }
}

static int cmd_probe(const char *in_path, const char *key_path)
{
    u8 *data = NULL;
    size_t size = 0;
    NkPspCtx ctx;
    NkContainerInfo info;
    NkKeystore *ks = NULL;
    char entries[8][80];
    int n;
    int i;
    int rc;

    if (in_path == NULL) {
        fprintf(stderr, "probe: --in is required\n");
        return 2;
    }
    if (read_file(in_path, &data, &size) != 0) {
        printf("INPUT_UNREADABLE cannot read input file %s\n", in_path);
        return 1;
    }
    nk_psp_ctx_init(&ctx, NULL);
    rc = nk_container_probe(&ctx, data, size, &info);
    if (rc != NK_PSP_OK) {
        free(data);
        return print_diagnostic(rc, &ctx);
    }

    /* An optional key file enriches the listing (keyvault slot). */
    if (key_path != NULL && file_present(key_path)) {
        char kerr[256];
        ks = nk_keystore_create();
        if (ks == NULL ||
            nk_keystore_load_file(ks, key_path, kerr, sizeof(kerr)) !=
                NK_PSP_OK) {
            printf("KEYFILE_INVALID %s\n", kerr);
            nk_keystore_free(ks);
            free(data);
            return 6;
        }
    }

    printf("container %s\n", kind_name(info.kind));
    n = nk_container_key_entries(&info, ks, entries, 8);
    for (i = 0; i < n; i++) {
        printf("%s\n", entries[i]);
    }
    if (n == 0) {
        printf("(no key entries needed)\n");
    }
    nk_keystore_free(ks);
    free(data);
    return 0;
}

/* ---------------------------- decrypt ---------------------------------- */

static int cmd_decrypt(const char *key_path, const char *in_path,
                       const char *out_path)
{
    u8 *data = NULL;
    size_t size = 0;
    u8 *plain = NULL;
    size_t plain_size = 0;
    NkKeystore *ks = NULL;
    NkPspCtx ctx;
    int rc;

    if (in_path == NULL || out_path == NULL) {
        fprintf(stderr, "decrypt: --in and --out are required\n");
        return 2;
    }
    if (read_file(in_path, &data, &size) != 0) {
        printf("INPUT_UNREADABLE cannot read input file %s\n", in_path);
        return 1;
    }

    ks = nk_keystore_create();
    if (ks == NULL) {
        free(data);
        printf("INTERNAL_ERROR out of memory\n");
        return 9;
    }
    if (key_path == NULL) {
        printf("note: no key file requested; continuing with an empty "
               "key store\n");
    } else if (!file_present(key_path)) {
        printf("note: no key file at %s; continuing with an empty key "
               "store\n", key_path);
    } else {
        char kerr[256];
        rc = nk_keystore_load_file(ks, key_path, kerr, sizeof(kerr));
        if (rc != NK_PSP_OK) {
            printf("KEYFILE_INVALID %s\n", kerr);
            nk_keystore_free(ks);
            free(data);
            return 6;
        }
    }

    nk_psp_ctx_init(&ctx, ks);
    rc = nk_container_decrypt(&ctx, data, size, &plain, &plain_size);
    if (rc != NK_PSP_OK) {
        int code = print_diagnostic(rc, &ctx);
        nk_keystore_free(ks);
        free(data);
        return code;
    }

    /* Success: only now is the output file created. */
    if (write_file(out_path, plain, plain_size) != 0) {
        printf("OUTPUT_UNWRITABLE cannot write %s\n", out_path);
        free(plain);
        nk_keystore_free(ks);
        free(data);
        return 1;
    }
    printf("DECRYPT_OK %u bytes\n", (unsigned)plain_size);
    free(plain);
    nk_keystore_free(ks);
    free(data);
    return 0;
}

/* ------------------------------ main ----------------------------------- */

static void usage(void)
{
    fprintf(stderr,
            "usage: nk_decrypt kat --vector {aes-ecb|aes-cbc|aes-cmac|sha1}\n"
            "       nk_decrypt probe --in FILE [--key-file FILE]\n"
            "       nk_decrypt decrypt --key-file FILE --in FILE --out FILE\n");
}

int main(int argc, char **argv)
{
    const char *cmd;
    const char *vector = NULL;
    const char *in_path = NULL;
    const char *out_path = NULL;
    const char *key_path = NULL;
    int i;

    if (argc < 2) {
        usage();
        return 2;
    }
    cmd = argv[1];
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--vector") == 0 && i + 1 < argc) {
            vector = argv[++i];
        } else if (strcmp(argv[i], "--in") == 0 && i + 1 < argc) {
            in_path = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--key-file") == 0 && i + 1 < argc) {
            key_path = argv[++i];
        } else {
            fprintf(stderr, "nk_decrypt: unknown argument '%s'\n", argv[i]);
            usage();
            return 2;
        }
    }

    if (strcmp(cmd, "kat") == 0) return cmd_kat(vector);
    if (strcmp(cmd, "probe") == 0) return cmd_probe(in_path, key_path);
    if (strcmp(cmd, "decrypt") == 0)
        return cmd_decrypt(key_path, in_path, out_path);

    fprintf(stderr, "nk_decrypt: unknown command '%s'\n", cmd);
    usage();
    return 2;
}
