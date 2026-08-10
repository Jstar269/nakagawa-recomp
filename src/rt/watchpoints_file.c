// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
//
// watchpoints_file.c — bounded parser for the derived `watchpoints.json`
// runtime artifact (issue #188). See watchpoints_file.h for the contract.
//
// This is a deliberately small, strict JSON reader for exactly the shapes the
// dashboard writer can produce — it is NOT a general JSON parser. It bounds
// every input (file size, entry count, label bytes), decodes only the minimal
// JSON string escapes the writer may emit, and rejects the whole file on any
// semantic violation (out-of-range, zero span, oversize span, bad label,
// duplicates, unknown keys, wrong version/format).

#include "watchpoints_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *sr_wp_skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Parse a JSON string token at *pp (advancing past it). Decodes the minimal
 * escapes; non-ASCII \u escapes are rejected because they are outside the
 * canonical label charset. Returns 0 and fills `out` (bounded by out_cap) on
 * success, -1 on error. */
static int sr_wp_parse_string(const char **pp, char *out, size_t out_cap) {
    const char *s = *pp;
    if (*s != '"') return -1;
    s++;
    size_t n = 0;
    while (*s != '\0' && *s != '"') {
        char c = *s++;
        if (c == '\\') {
            if (*s == '\0') return -1;
            char esc = *s++;
            switch (esc) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    /* \uXXXX: only ASCII is in the canonical charset. */
                    unsigned code = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = s[i];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= (unsigned)(h - 'A' + 10);
                        else return -1;
                    }
                    if (code > 0x7f) return -1; /* outside canonical charset */
                    c = (char)code;
                    s += 4;
                    break;
                }
                default:
                    return -1;
            }
        }
        if (n + 1 >= out_cap) return -1;
        out[n++] = c;
    }
    if (*s != '"') return -1;
    out[n] = '\0';
    *pp = s + 1;
    return 0;
}

/* Parse an unsigned JSON integer at *pp (advancing past it). JSON numbers only
 * (no 0x, no sign, no fractions) bounded to UINT32_MAX. */
static int sr_wp_parse_uint(const char **pp, uint32_t *out) {
    const char *s = *pp;
    if (*s < '0' || *s > '9') return -1;
    uint64_t value = 0;
    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (uint64_t)(*s - '0');
        if (value > UINT32_MAX) return -1;
        s++;
    }
    *out = (uint32_t)value;
    *pp = s;
    return 0;
}

static int sr_wp_valid_label(const char *label) {
    if (label[0] == '\0') return 0;
    for (const char *p = label; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ' ' ||
              c == '-')) {
            return 0;
        }
    }
    return 1;
}

static void sr_wp_error(char *errbuf, size_t errbuf_size, const char *msg) {
    if (errbuf && errbuf_size > 0) {
        snprintf(errbuf, errbuf_size, "%s", msg);
    }
}

/* Parse one watchpoint object: { "start": N, "end": N, "label": "..." } with
 * no unknown keys. Returns 0 on success, -1 on error. */
static int sr_wp_parse_watchpoint(const char **pp, SrWatchpointEntry *out) {
    const char *s = sr_wp_skip_ws(*pp);
    if (*s != '{') return -1;
    s++;
    uint32_t start = 0, end = 0;
    char label[SR_WATCHPOINT_LABEL_BUF];
    label[0] = '\0';
    int have_start = 0, have_end = 0, have_label = 0;
    for (;;) {
        s = sr_wp_skip_ws(s);
        if (*s == '}') {
            s++;
            break;
        }
        if (*s != '"') return -1;
        char key[16];
        if (sr_wp_parse_string(&s, key, sizeof(key)) != 0) return -1;
        s = sr_wp_skip_ws(s);
        if (*s != ':') return -1;
        s = sr_wp_skip_ws(s + 1);
        if (strcmp(key, "start") == 0) {
            if (have_start || sr_wp_parse_uint(&s, &start) != 0) return -1;
            have_start = 1;
        } else if (strcmp(key, "end") == 0) {
            if (have_end || sr_wp_parse_uint(&s, &end) != 0) return -1;
            have_end = 1;
        } else if (strcmp(key, "label") == 0) {
            if (have_label || sr_wp_parse_string(&s, label, sizeof(label)) != 0) return -1;
            have_label = 1;
        } else {
            return -1; /* unknown key */
        }
        s = sr_wp_skip_ws(s);
        if (*s == ',') {
            s++;
            continue;
        }
        if (*s == '}') {
            s++;
            break;
        }
        return -1;
    }
    if (!have_start || !have_end || !have_label) return -1;
    if (start >= end) return -1;                       /* zero span / reversed */
    if ((uint64_t)end - (uint64_t)start > SR_WATCHPOINT_MAX_SPAN) return -1;
    if (!sr_wp_valid_label(label)) return -1;
    out->start = start;
    out->end = end;
    memcpy(out->label, label, sizeof(label));
    *pp = s;
    return 0;
}

static int sr_wp_has_duplicate(const SrWatchpointEntry *entries, int count,
                               uint32_t start, uint32_t end) {
    for (int i = 0; i < count; i++) {
        if (entries[i].start == start && entries[i].end == end) return 1;
    }
    return 0;
}

/* Parse a watchpoint array [ ... ] at *pp into out. */
static int sr_wp_parse_array(const char **pp, SrWatchpointEntry *out, int out_cap,
                             char *errbuf, size_t errbuf_size) {
    const char *s = sr_wp_skip_ws(*pp);
    if (*s != '[') {
        sr_wp_error(errbuf, errbuf_size, "expected watchpoint array");
        return -1;
    }
    s++;
    int count = 0;
    for (;;) {
        s = sr_wp_skip_ws(s);
        if (*s == ']') {
            s++;
            break;
        }
        if (count >= out_cap) {
            sr_wp_error(errbuf, errbuf_size, "too many watchpoints (exceeds runtime capacity)");
            return -1;
        }
        SrWatchpointEntry entry;
        if (sr_wp_parse_watchpoint(&s, &entry) != 0) {
            sr_wp_error(errbuf, errbuf_size, "malformed watchpoint object");
            return -1;
        }
        if (sr_wp_has_duplicate(out, count, entry.start, entry.end)) {
            sr_wp_error(errbuf, errbuf_size, "duplicate watchpoint range");
            return -1;
        }
        out[count++] = entry;
        s = sr_wp_skip_ws(s);
        if (*s == ',') {
            s++;
            continue;
        }
        if (*s == ']') {
            s++;
            break;
        }
        sr_wp_error(errbuf, errbuf_size, "malformed watchpoint array");
        return -1;
    }
    *pp = s;
    return count;
}

int sr_parse_watchpoints_buffer(const char *json, SrWatchpointEntry *out, int out_cap,
                                char *errbuf, size_t errbuf_size) {
    if (!json || !out || out_cap <= 0) {
        sr_wp_error(errbuf, errbuf_size, "invalid parser arguments");
        return -1;
    }
    const char *s = sr_wp_skip_ws(json);
    if (*s == '[') {
        /* Legacy bare array (pre-#188 writer output). */
        int n = sr_wp_parse_array(&s, out, out_cap, errbuf, errbuf_size);
        if (n >= 0) {
            const char *tail = sr_wp_skip_ws(s);
            if (*tail != '\0') {
                sr_wp_error(errbuf, errbuf_size, "trailing data after watchpoint array");
                return -1;
            }
        }
        return n;
    }
    if (*s != '{') {
        sr_wp_error(errbuf, errbuf_size, "file is neither an envelope nor a watchpoint array");
        return -1;
    }
    /* Envelope: walk key/value pairs; require format + version; read watchpoints. */
    s++;
    int saw_format = 0, saw_version = 0, saw_watchpoints = 0;
    int result = -1;
    for (;;) {
        s = sr_wp_skip_ws(s);
        if (*s == '}') {
            s++;
            break;
        }
        if (*s != '"') {
            sr_wp_error(errbuf, errbuf_size, "malformed envelope");
            goto done;
        }
        char key[32];
        if (sr_wp_parse_string(&s, key, sizeof(key)) != 0) {
            sr_wp_error(errbuf, errbuf_size, "malformed envelope key");
            goto done;
        }
        s = sr_wp_skip_ws(s);
        if (*s != ':') {
            sr_wp_error(errbuf, errbuf_size, "malformed envelope");
            goto done;
        }
        s = sr_wp_skip_ws(s + 1);
        if (strcmp(key, "format") == 0) {
            char fmt[32];
            if (sr_wp_parse_string(&s, fmt, sizeof(fmt)) != 0 ||
                strcmp(fmt, SR_WATCHPOINTS_FILE_FORMAT) != 0) {
                sr_wp_error(errbuf, errbuf_size, "unexpected watchpoints file format");
                goto done;
            }
            saw_format = 1;
        } else if (strcmp(key, "version") == 0) {
            uint32_t version = 0;
            if (sr_wp_parse_uint(&s, &version) != 0 || version != SR_WATCHPOINTS_FILE_VERSION) {
                sr_wp_error(errbuf, errbuf_size, "unsupported watchpoints file version");
                goto done;
            }
            saw_version = 1;
        } else if (strcmp(key, "watchpoints") == 0) {
            int n = sr_wp_parse_array(&s, out, out_cap, errbuf, errbuf_size);
            if (n < 0) goto done;
            result = n;
            saw_watchpoints = 1;
        } else {
            /* profileId / source / writtenAt / contentHash: skip any scalar. */
            if (*s == '"') {
                char ignored[128]; /* contentHash is 64 hex chars + NUL */
                if (sr_wp_parse_string(&s, ignored, sizeof(ignored)) != 0) {
                    sr_wp_error(errbuf, errbuf_size, "malformed envelope field");
                    goto done;
                }
            } else if (*s == 'n') {
                if (strncmp(s, "null", 4) != 0) {
                    sr_wp_error(errbuf, errbuf_size, "malformed envelope field");
                    goto done;
                }
                s += 4;
            } else {
                sr_wp_error(errbuf, errbuf_size, "malformed envelope field");
                goto done;
            }
        }
        s = sr_wp_skip_ws(s);
        if (*s == ',') {
            s++;
            continue;
        }
        if (*s == '}') {
            s++;
            break;
        }
        sr_wp_error(errbuf, errbuf_size, "malformed envelope");
        goto done;
    }
    if (!saw_format || !saw_version || !saw_watchpoints) {
        sr_wp_error(errbuf, errbuf_size, "envelope missing format, version or watchpoints");
        result = -1;
        goto done;
    }
    {
        const char *tail = sr_wp_skip_ws(s);
        if (*tail != '\0') {
            sr_wp_error(errbuf, errbuf_size, "trailing data after envelope");
            result = -1;
            goto done;
        }
    }
done:
    return result;
}

int sr_parse_watchpoints_file(const char *path, SrWatchpointEntry *out, int out_cap,
                              char *errbuf, size_t errbuf_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        sr_wp_error(errbuf, errbuf_size, "watchpoints.json is not readable");
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        sr_wp_error(errbuf, errbuf_size, "cannot seek watchpoints.json");
        return -1;
    }
    long sz = ftell(f);
    if (sz < 0 || (unsigned long)sz > SR_WATCHPOINTS_FILE_MAX_BYTES) {
        fclose(f);
        sr_wp_error(errbuf, errbuf_size, "watchpoints.json exceeds the size bound");
        return -1;
    }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        sr_wp_error(errbuf, errbuf_size, "out of memory reading watchpoints.json");
        return -1;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    if (rd != (size_t)sz) {
        free(buf);
        fclose(f);
        sr_wp_error(errbuf, errbuf_size, "short read on watchpoints.json");
        return -1;
    }
    buf[sz] = '\0';
    int rc = sr_parse_watchpoints_buffer(buf, out, out_cap, errbuf, errbuf_size);
    free(buf);
    fclose(f);
    return rc;
}
