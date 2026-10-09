// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Prints the native reader's verdict for image paths read from stdin, one path per line.
 * tools/test_pgf_validate_parity.py builds this against src/rt/pgf_public.c and compares
 * every field with the Python mirror in tools/nk_core/fonts.py. It has no other use.
 *
 * Output per path: "ERROR" when the file cannot be read, otherwise
 *   refusal ok revision header_size first last glyph_count char_map_count
 *   nominal_h nominal_v has_latin has_kana has_hangul
 * with refusal the PgfRefusal index (0 = accepted). Fields are zero for a refusal. */

#include "pgf_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    char line[4096];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t length = strlen(line);
        FILE *stream;
        long size;
        unsigned char *bytes;
        size_t received;
        PgfVerdict verdict;
        int accepted;

        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
            line[--length] = '\0';
        }
        stream = fopen(line, "rb");
        if (stream == NULL) {
            printf("ERROR\n");
            continue;
        }
        if (fseek(stream, 0L, SEEK_END) != 0 || (size = ftell(stream)) < 0 || fseek(stream, 0L, SEEK_SET) != 0) {
            fclose(stream);
            printf("ERROR\n");
            continue;
        }
        bytes = (unsigned char *)malloc(size > 0 ? (size_t)size : 1u);
        if (bytes == NULL) {
            fclose(stream);
            printf("ERROR\n");
            continue;
        }
        received = size > 0 ? fread(bytes, 1u, (size_t)size, stream) : 0u;
        fclose(stream);
        memset(&verdict, 0, sizeof(verdict));
        accepted = pgf_validate_memory(bytes, received, &verdict);
        if (accepted) {
            printf("%d 1 %u %u %u %u %u %u %d %d %u %u %u\n",
                   (int)verdict.refusal, (unsigned)verdict.revision, (unsigned)verdict.header_size,
                   (unsigned)verdict.first_glyph, (unsigned)verdict.last_glyph,
                   (unsigned)verdict.glyph_count, (unsigned)verdict.char_map_count,
                   (int)verdict.nominal_h, (int)verdict.nominal_v,
                   (unsigned)verdict.has_latin, (unsigned)verdict.has_kana, (unsigned)verdict.has_hangul);
        } else {
            printf("%d 0 0 0 0 0 0 0 0 0 0 0 0\n", (int)verdict.refusal);
        }
        free(bytes);
    }
    return 0;
}
