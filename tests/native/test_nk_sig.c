// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* nk_ed25519_verify against the RFC 8032 section 7.1 Ed25519 test vectors
 * (public key, message, signature; the secret keys are not needed to verify),
 * plus the rejections a signed update notice relies on. */

#include "nk_sig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *pub_hex;
    const char *msg_hex;
    const char *sig_hex;
} SigVector;

static const SigVector k_vectors[] = {
    {"TEST 1",
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
        "",
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
    {"TEST 2",
        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
        "72",
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
    {"TEST 3",
        "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
        "af82",
        "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
        "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
    {"TEST 1024",
        "278117fc144c72340f67d0f2316e8386ceffbf2b2428c9c51fef7c597f1d426e",
        "08b8b2b733424243760fe426a4b54908632110a66c2f6591eabd3345e3e4eb98"
        "fa6e264bf09efe12ee50f8f54e9f77b1e355f6c50544e23fb1433ddf73be84d8"
        "79de7c0046dc4996d9e773f4bc9efe5738829adb26c81b37c93a1b270b20329d"
        "658675fc6ea534e0810a4432826bf58c941efb65d57a338bbd2e26640f89ffbc"
        "1a858efcb8550ee3a5e1998bd177e93a7363c344fe6b199ee5d02e82d522c4fe"
        "ba15452f80288a821a579116ec6dad2b3b310da903401aa62100ab5d1a36553e"
        "06203b33890cc9b832f79ef80560ccb9a39ce767967ed628c6ad573cb116dbef"
        "efd75499da96bd68a8a97b928a8bbc103b6621fcde2beca1231d206be6cd9ec7"
        "aff6f6c94fcd7204ed3455c68c83f4a41da4af2b74ef5c53f1d8ac70bdcb7ed1"
        "85ce81bd84359d44254d95629e9855a94a7c1958d1f8ada5d0532ed8a5aa3fb2"
        "d17ba70eb6248e594e1a2297acbbb39d502f1a8c6eb6f1ce22b3de1a1f40cc24"
        "554119a831a9aad6079cad88425de6bde1a9187ebb6092cf67bf2b13fd65f270"
        "88d78b7e883c8759d2c4f5c65adb7553878ad575f9fad878e80a0c9ba63bcbcc"
        "2732e69485bbc9c90bfbd62481d9089beccf80cfe2df16a2cf65bd92dd597b07"
        "07e0917af48bbb75fed413d238f5555a7a569d80c3414a8d0859dc65a46128ba"
        "b27af87a71314f318c782b23ebfe808b82b0ce26401d2e22f04d83d1255dc51a"
        "ddd3b75a2b1ae0784504df543af8969be3ea7082ff7fc9888c144da2af58429e"
        "c96031dbcad3dad9af0dcbaaaf268cb8fcffead94f3c7ca495e056a9b47acdb7"
        "51fb73e666c6c655ade8297297d07ad1ba5e43f1bca32301651339e22904cc8c"
        "42f58c30c04aafdb038dda0847dd988dcda6f3bfd15c4b4c4525004aa06eeff8"
        "ca61783aacec57fb3d1f92b0fe2fd1a85f6724517b65e614ad6808d6f6ee34df"
        "f7310fdc82aebfd904b01e1dc54b2927094b2db68d6f903b68401adebf5a7e08"
        "d78ff4ef5d63653a65040cf9bfd4aca7984a74d37145986780fc0b16ac451649"
        "de6188a7dbdf191f64b5fc5e2ab47b57f7f7276cd419c17a3ca8e1b939ae49e4"
        "88acba6b965610b5480109c8b17b80e1b7b750dfc7598d5d5011fd2dcc5600a3"
        "2ef5b52a1ecc820e308aa342721aac0943bf6686b64b2579376504ccc493d97e"
        "6aed3fb0f9cd71a43dd497f01f17c0e2cb3797aa2a2f256656168e6c496afc5f"
        "b93246f6b1116398a346f1a641f3b041e989f7914f90cc2c7fff357876e506b5"
        "0d334ba77c225bc307ba537152f3f1610e4eafe595f6d9d90d11faa933a15ef1"
        "369546868a7f3a45a96768d40fd9d03412c091c6315cf4fde7cb68606937380d"
        "b2eaaa707b4c4185c32eddcdd306705e4dc1ffc872eeee475a64dfac86aba41c"
        "0618983f8741c5ef68d3a101e8a3b8cac60c905c15fc910840b94c00a0b9d0",
        "0aab4c900501b3e24d7cdf4663326a3a87df5e4843b2cbdb67cbf6e460fec350"
        "aa5371b1508f9f4528ecea23c436d94b5e8fcd4f681e30a6ac00a9704a188a03"},
    {"TEST SHA(abc)",
        "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
        "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b589"
        "09351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704"},
};

/* The Ed25519 group order L, little-endian. */
static const uint8_t k_order_l[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
};

static int g_failures;

static void check(int ok, const char *what, const char *name) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s (%s)\n", what, name);
        ++g_failures;
    }
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Decodes hex into a new buffer; *len gets the byte count. NULL on bad input. */
static uint8_t *decode_hex(const char *hex, size_t *len) {
    size_t n = strlen(hex);
    size_t i;
    uint8_t *out;
    if (n % 2u != 0u) {
        return NULL;
    }
    out = (uint8_t *)malloc(n / 2u + 1u);
    if (out == NULL) {
        return NULL;
    }
    for (i = 0; i < n / 2u; ++i) {
        int hi = hex_nibble(hex[2u * i]);
        int lo = hex_nibble(hex[2u * i + 1u]);
        if (hi < 0 || lo < 0) {
            free(out);
            return NULL;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    *len = n / 2u;
    return out;
}

/* S' = S + L in the signature's second half: the same point equation holds,
 * so only the S < L canonicality check can reject it. */
static void add_order_to_s(uint8_t sig[64]) {
    unsigned carry = 0u;
    size_t i;
    for (i = 0; i < 32u; ++i) {
        unsigned sum = (unsigned)sig[32u + i] + k_order_l[i] + carry;
        sig[32u + i] = (uint8_t)sum;
        carry = sum >> 8;
    }
}

/* ---- minisign ------------------------------------------------------------------------
 *
 * Real minisign output, so the format is checked against the tool the owner signs
 * releases with, not only against this project's reading of it. TEST-ONLY: the key
 * was made for this test with `minisign -G -W` (minisign 0.12, the newest release,
 * Windows x86_64 build) and its secret half was deleted after signing; it signs
 * nothing else and is not the update-signing key. Both signatures cover the
 * 105-byte checksum file below, once in the default prehashed form and once with
 * `-l` (legacy). minisign on Windows ends its lines in CRLF. */

static const char k_minisign_test_pub[] =
    "untrusted comment: minisign public key D0A699A2C54C0C9E\r\n"
    "RWSeDEzFopmm0LdwefFMjtRkQtZckksdhfycG9vMHzmHiu5pl16/wA81\r\n";

static const char k_minisign_test_message[] =
    "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"
    "  nakagawa-recomp-0.0.2-windows-x64.zip\r\n";

static const char k_minisign_test_prehashed[] =
    "untrusted comment: TEST-ONLY signature for tests/native\r\n"
    "RUSeDEzFopmm0IiX23jdEjzmTHOAw6AfJC98fPtQU+kDZ64h1gF5D9wc+Pn85CUmfAegosjtlzFrlH9svmOykGUu"
    "uxM9Zs9aRQY=\r\n"
    "trusted comment: nakagawa-recomp 0.0.2 windows-x64 zip checksum, TEST-ONLY key\r\n"
    "SsnFMq8GKumiLOR/M6CeTgj5Ctxz+fXvZozm4UZZU7+lTV/lF41uuuegIOGzMOGScgx0k1gM/1tHvzYZNnwLAw==\r\n";

static const char k_minisign_test_legacy[] =
    "untrusted comment: TEST-ONLY legacy signature for tests/native\r\n"
    "RWSeDEzFopmm0Lr40CootHEj/wZzyjl/S153q4mVrlBHYguSVxMUVu1BVZSzi8y/mERJ+AEzBDfOZ9vUB1crwxYI"
    "aVeE4ov7YAQ=\r\n"
    "trusted comment: nakagawa-recomp 0.0.2 windows-x64 zip checksum, TEST-ONLY key, legacy\r\n"
    "oFNkHnhMFNeC9K/K+VFnkDUA8jP2J8siZok/+wu68GftlsWqlJd914riAow1cApDFgkuAT/9sNXaY6Owrn9BCA==\r\n";

static NkMinisignResult minisign_check(const NkMinisignKey *key, const char *sig,
                                       const char *msg, size_t msg_len) {
    return nk_minisign_verify(key, sig, strlen(sig), (const uint8_t *)msg, msg_len, NULL, NULL);
}

/* sig with the first occurrence of from replaced by to (same length). */
static char *edited(const char *sig, const char *from, const char *to) {
    size_t n = strlen(sig);
    char *copy = (char *)malloc(n + 1u);
    char *at;
    if (copy == NULL) return NULL;
    memcpy(copy, sig, n + 1u);
    at = strstr(copy, from);
    if (at == NULL || strlen(from) != strlen(to)) {
        free(copy);
        return NULL;
    }
    memcpy(at, to, strlen(to));
    return copy;
}

static void test_minisign(void) {
    static const char *const sigs[] = { k_minisign_test_prehashed, k_minisign_test_legacy };
    static const char *const names[] = { "minisign prehashed (ED)", "minisign legacy (Ed)" };
    const size_t msg_len = sizeof(k_minisign_test_message) - 1u;
    NkMinisignKey key;
    NkMinisignKey other;
    const char *comment = NULL;
    size_t comment_len = 0;
    char lf[512];

    /* The key: the whole .pub file, its base64 line alone, with LF line ends. */
    check(nk_minisign_parse_key(k_minisign_test_pub, strlen(k_minisign_test_pub), &key),
          "minisign .pub file parses", "minisign key");
    check(key.key_id[0] == 0x9eu && key.key_id[7] == 0xd0u,
          "key ID read in file order (shown reversed as D0A699A2C54C0C9E)", "minisign key");
    check(nk_minisign_parse_key("RWSeDEzFopmm0LdwefFMjtRkQtZckksdhfycG9vMHzmHiu5pl16/wA81", 56u,
                                &other) &&
              memcmp(&other, &key, sizeof(key)) == 0,
          "the bare base64 line parses to the same key", "minisign key");
    {
        static const char *const bad_keys[] = {
            "",
            "untrusted comment: minisign public key D0A699A2C54C0C9E\n",
            "RWSeDEzFopmm0LdwefFMjtRkQtZckksdhfycG9vMHzmHiu5pl16/wA8",     /* short */
            "RWSeDEzFopmm0LdwefFMjtRkQtZckksdhfycG9vMHzmHiu5pl16/wA81A",   /* long */
            "RWSeDEzFopmm0LdwefFMjtRkQtZckksdhfycG9vMHzmHiu5pl16/wA8!",    /* not base64 */
            "RUSeDEzFopmm0LdwefFMjtRkQtZckksdhfycG9vMHzmHiu5pl16/wA81",    /* "ED", not a key */
            "RWSeDEzFopmm0LdwefFMjtRkQtZckksdhfycG9vMHzmHiu5pl16/wA81\nmore",
        };
        for (size_t i = 0; i < sizeof(bad_keys) / sizeof(bad_keys[0]); ++i) {
            check(!nk_minisign_parse_key(bad_keys[i], strlen(bad_keys[i]), &other),
                  "a malformed public key is refused", bad_keys[i]);
        }
    }

    for (size_t i = 0; i < 2u; ++i) {
        const char *sig = sigs[i];
        char *bad;
        check(nk_minisign_verify(&key, sig, strlen(sig), (const uint8_t *)k_minisign_test_message,
                                 msg_len, &comment, &comment_len) == NK_MINISIGN_VALID,
              "minisign's own signature verifies", names[i]);
        check(comment != NULL && comment_len >= 49u &&
                  strncmp(comment, "nakagawa-recomp 0.0.2 windows-x64 zip checksum, TEST-ONLY key",
                          49u) == 0 &&
                  comment[comment_len] == '\r',
              "the trusted comment is returned without its prefix or line end", names[i]);
        /* The same file with LF line ends (minisign elsewhere writes LF). */
        {
            size_t used = 0;
            for (const char *c = sig; *c && used + 1u < sizeof(lf); ++c) {
                if (*c != '\r') lf[used++] = *c;
            }
            lf[used] = '\0';
            check(minisign_check(&key, lf, k_minisign_test_message, msg_len) == NK_MINISIGN_VALID,
                  "LF line ends verify too", names[i]);
            /* and without the final line end */
            lf[used - 1u] = '\0';
            check(minisign_check(&key, lf, k_minisign_test_message, msg_len) == NK_MINISIGN_VALID,
                  "no final line end verifies too", names[i]);
        }
        /* The message: one changed byte, one byte short, one byte more. */
        {
            char changed[sizeof(k_minisign_test_message)];
            memcpy(changed, k_minisign_test_message, sizeof(changed));
            changed[0] = '3';
            check(minisign_check(&key, sig, changed, msg_len) == NK_MINISIGN_BAD_SIGNATURE,
                  "a changed checksum file is refused", names[i]);
            check(minisign_check(&key, sig, k_minisign_test_message, msg_len - 1u) ==
                      NK_MINISIGN_BAD_SIGNATURE,
                  "a truncated checksum file is refused", names[i]);
        }
        /* The trusted comment, changed by one letter: only the global signature
           covers it. */
        bad = edited(sig, "windows-x64 zip", "windows-x86 zip");
        check(bad != NULL && minisign_check(&key, bad, k_minisign_test_message, msg_len) ==
                                 NK_MINISIGN_BAD_COMMENT,
              "a changed trusted comment is refused", names[i]);
        free(bad);
        /* The untrusted comment is not signed and may say anything. */
        bad = edited(sig, "TEST-ONLY", "ANYTHING!");
        check(bad != NULL && minisign_check(&key, bad, k_minisign_test_message, msg_len) ==
                                 NK_MINISIGN_VALID,
              "the untrusted comment is not checked", names[i]);
        free(bad);
        /* A signature that names another key ID. */
        bad = edited(sig, "SeDEzFopmm0", "SeDEzFopmm1");
        check(bad != NULL && minisign_check(&key, bad, k_minisign_test_message, msg_len) ==
                                 NK_MINISIGN_UNKNOWN_KEY,
              "another key ID is refused before any check", names[i]);
        free(bad);
        /* The right key ID on another public key. */
        other = key;
        other.public_key[5] ^= 0x01u;
        check(minisign_check(&other, sig, k_minisign_test_message, msg_len) != NK_MINISIGN_VALID,
              "another public key with the same ID is refused", names[i]);
        /* Malformed files: cut short, garbled base64, missing lines, wrong prefixes. */
        {
            /* Every cut that takes more than the final line end. */
            size_t n = strlen(sig);
            for (size_t cut = 0; cut + 2u < n; cut += 7u) {
                NkMinisignResult r = nk_minisign_verify(
                    &key, sig, cut, (const uint8_t *)k_minisign_test_message, msg_len, NULL, NULL);
                if (r == NK_MINISIGN_VALID) {
                    check(0, "a signature cut short is refused", names[i]);
                    break;
                }
            }
        }
        bad = edited(sig, "untrusted comment:", "untrusted_comment:");
        check(bad != NULL && minisign_check(&key, bad, k_minisign_test_message, msg_len) ==
                                 NK_MINISIGN_MALFORMED,
              "a wrong first-line prefix is refused", names[i]);
        free(bad);
        bad = edited(sig, "trusted comment: nakagawa", "trusted_comment: nakagawa");
        check(bad != NULL && minisign_check(&key, bad, k_minisign_test_message, msg_len) ==
                                 NK_MINISIGN_MALFORMED,
              "a wrong trusted-comment prefix is refused", names[i]);
        free(bad);
        bad = edited(sig, "Fopmm0", "Fop*m0");
        check(bad != NULL && minisign_check(&key, bad, k_minisign_test_message, msg_len) ==
                                 NK_MINISIGN_MALFORMED,
              "garbled signature base64 is refused", names[i]);
        free(bad);
        bad = edited(sig, "==\r\n", "A=\r\n");
        check(bad != NULL && minisign_check(&key, bad, k_minisign_test_message, msg_len) ==
                                 NK_MINISIGN_MALFORMED,
              "global signature base64 without its padding is refused", names[i]);
        free(bad);
        {
            /* An extra line after the global signature. */
            size_t n = strlen(sig);
            char *longer = (char *)malloc(n + 8u);
            if (longer != NULL) {
                memcpy(longer, sig, n);
                memcpy(longer + n, "extra\r\n", 8u);
                check(minisign_check(&key, longer, k_minisign_test_message, msg_len) ==
                          NK_MINISIGN_MALFORMED,
                      "text after the global signature is refused", names[i]);
                free(longer);
            }
        }
    }
    /* An algorithm minisign does not define: "Ex". */
    {
        char *bad = edited(k_minisign_test_prehashed, "RUSeDEz", "RXSeDEz");
        check(bad != NULL && minisign_check(&key, bad, k_minisign_test_message, msg_len) ==
                                 NK_MINISIGN_MALFORMED,
              "an unknown signature algorithm is refused", "minisign");
        free(bad);
    }
    /* A prehashed signature relabelled as legacy, and the other way round. */
    {
        char *bad = edited(k_minisign_test_prehashed, "RUSeDEz", "RWSeDEz");
        check(bad != NULL && minisign_check(&key, bad, k_minisign_test_message, msg_len) ==
                                 NK_MINISIGN_BAD_SIGNATURE,
              "a prehashed signature read as legacy is refused", "minisign");
        free(bad);
        bad = edited(k_minisign_test_legacy, "RWSeDEz", "RUSeDEz");
        check(bad != NULL && minisign_check(&key, bad, k_minisign_test_message, msg_len) ==
                                 NK_MINISIGN_BAD_SIGNATURE,
              "a legacy signature read as prehashed is refused", "minisign");
        free(bad);
    }
    check(nk_minisign_verify(NULL, k_minisign_test_prehashed, strlen(k_minisign_test_prehashed),
                             (const uint8_t *)k_minisign_test_message, msg_len, NULL, NULL) ==
              NK_MINISIGN_MALFORMED,
          "no key is refused", "minisign");
    check(nk_minisign_verify(&key, NULL, 0u, (const uint8_t *)k_minisign_test_message, msg_len,
                             NULL, NULL) == NK_MINISIGN_MALFORMED,
          "no signature is refused", "minisign");
    printf("nk_sig selftest: minisign signatures OK\n");
}

int main(void) {
    const size_t count = sizeof(k_vectors) / sizeof(k_vectors[0]);
    size_t v;
    size_t pub_len = 0;
    size_t sig_len = 0;
    size_t msg_len = 0;
    uint8_t *other_pub = NULL;

    for (v = 0; v < count; ++v) {
        const SigVector *t = &k_vectors[v];
        uint8_t *pub = decode_hex(t->pub_hex, &pub_len);
        uint8_t *sig = decode_hex(t->sig_hex, &sig_len);
        uint8_t *msg = decode_hex(t->msg_hex, &msg_len);
        uint8_t bad_sig[64];
        uint8_t bad_pub[32];
        if (pub == NULL || sig == NULL || msg == NULL || pub_len != 32u || sig_len != 64u) {
            check(0, "vector decodes", t->name);
            free(pub);
            free(sig);
            free(msg);
            continue;
        }
        check(nk_ed25519_verify(sig, pub, msg, msg_len), "valid signature accepted", t->name);
        if (msg_len == 0u) {
            check(nk_ed25519_verify(sig, pub, NULL, 0u), "NULL empty message accepted", t->name);
        } else {
            uint8_t *flip = (uint8_t *)malloc(msg_len);
            if (flip != NULL) {
                memcpy(flip, msg, msg_len);
                flip[msg_len - 1u] ^= 0x01u;
                check(!nk_ed25519_verify(sig, pub, flip, msg_len), "flipped message bit rejected",
                      t->name);
                free(flip);
            }
            check(!nk_ed25519_verify(sig, pub, msg, msg_len - 1u), "truncated message rejected",
                  t->name);
            check(!nk_ed25519_verify(sig, pub, msg, 0u), "zero-length message rejected", t->name);
            check(!nk_ed25519_verify(sig, pub, NULL, msg_len), "NULL message with length rejected",
                  t->name);
        }
        memcpy(bad_sig, sig, 64u);
        bad_sig[0] ^= 0x01u;   /* R */
        check(!nk_ed25519_verify(bad_sig, pub, msg, msg_len), "flipped R bit rejected", t->name);
        memcpy(bad_sig, sig, 64u);
        bad_sig[40] ^= 0x01u;  /* S */
        check(!nk_ed25519_verify(bad_sig, pub, msg, msg_len), "flipped S bit rejected", t->name);
        memcpy(bad_sig, sig, 64u);
        add_order_to_s(bad_sig);
        check(!nk_ed25519_verify(bad_sig, pub, msg, msg_len), "non-canonical S (S + L) rejected",
              t->name);
        memcpy(bad_pub, pub, 32u);
        bad_pub[0] ^= 0x01u;
        check(!nk_ed25519_verify(sig, bad_pub, msg, msg_len), "flipped key bit rejected", t->name);
        if (other_pub != NULL) {
            check(!nk_ed25519_verify(sig, other_pub, msg, msg_len), "another vector's key rejected",
                  t->name);
        }
        check(!nk_ed25519_verify(NULL, pub, msg, msg_len), "NULL signature rejected", t->name);
        check(!nk_ed25519_verify(sig, NULL, msg, msg_len), "NULL key rejected", t->name);
        free(other_pub);
        other_pub = pub;
        free(sig);
        free(msg);
    }
    free(other_pub);
    test_minisign();

    if (g_failures != 0) {
        fprintf(stderr, "nk_sig selftest: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("nk_sig selftest: %u RFC 8032 vectors OK\n", (unsigned)count);
    return 0;
}
