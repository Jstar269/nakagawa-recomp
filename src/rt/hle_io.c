// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* File I/O: sceIo (IoFileMgrForUser) over the game image, the memory stick and host data.
 * Extracted from hle.c on 2026-10-31 (#307). */

#include "hle_internal.h"

typedef struct HleDevctlRefusal {
    struct HleDevctlRefusal *next;
    uint32_t command;
    char device[];
} HleDevctlRefusal;
static HleDevctlRefusal *s_hle_devctl_refusals;
#ifdef SR_HLE_THREAD_SELFTEST
static atomic_uint s_hle_devctl_refusal_log_count;
#endif

static int hle_devctl_refusal_first(const char *device, uint32_t command) {
    size_t device_len = strlen(device);
    if (device_len > SIZE_MAX - sizeof(HleDevctlRefusal) - 1u) return 1;
    hle_lock();
    for (HleDevctlRefusal *entry = s_hle_devctl_refusals; entry; entry = entry->next) {
        if (entry->command == command && strcmp(entry->device, device) == 0) {
            hle_unlock();
            return 0;
        }
    }
    HleDevctlRefusal *entry = (HleDevctlRefusal *)malloc(
        sizeof(*entry) + device_len + 1u);
    if (!entry) {
        hle_unlock();
        return 1;
    }
    entry->command = command;
    memcpy(entry->device, device, device_len + 1u);
    entry->next = s_hle_devctl_refusals;
    s_hle_devctl_refusals = entry;
    hle_unlock();
    return 1;
}

static void hle_mark_dynamic_unsupported(HleEntry *entry, uint32_t error) {
    if (!entry) return;
    atomic_store_explicit(&entry->unimplemented_error, error, memory_order_release);
    if (!atomic_exchange_explicit(&entry->unsupported_state, 1u, memory_order_acq_rel))
        hle_register_unsupported_summary();
}

static void hle_note_devctl_refusal(const char *device, uint32_t command,
                                   uint32_t error) {
    if (hle_devctl_refusal_first(device, command)) {
        fprintf(stderr,
                "HLE: sceIoDevctl refused device '%s' command 0x%08x -> 0x%08x; in the works (#341)\n",
                device, command, error);
#ifdef SR_HLE_THREAD_SELFTEST
        atomic_fetch_add_explicit(&s_hle_devctl_refusal_log_count, 1u,
                                  memory_order_relaxed);
#endif
    }
    hle_mark_dynamic_unsupported(hle_find(0x54f5fb11u), error);
}

/* The three helpers below name no host primitive: the unified Memory Stick
 * root resolution, the byte-path join and the directory-root resolution are
 * the same work on every host, so they are compiled once and shared. */
char *sr_utf8_join_alloc(const char *left, const char *right, char separator) {
    if (!left || !right) return NULL;
    size_t a = strlen(left), b = strlen(right);
    int need_separator = a > 0 && b > 0 && left[a - 1] != '/' && left[a - 1] != '\\';
    size_t extra = b;
    if (extra > SIZE_MAX - (size_t)need_separator - 1u) return NULL;
    extra += (size_t)need_separator + 1u;
    if (a > SIZE_MAX - extra) return NULL;
    char *joined = (char *)malloc(a + extra);
    if (!joined) return NULL;
    memcpy(joined, left, a);
    size_t at = a;
    if (need_separator) joined[at++] = separator;
    memcpy(joined + at, right, b + 1u);
    return joined;
}

static char *host_dir_path_alloc(const char *guest) {
    if (!guest) return NULL;
    const char *root = sr_ms0_root();
    if (!sr_ensure_directory_utf8(root)) return NULL;
    size_t root_len = strlen(root), guest_len = strlen(guest);
    if (root_len > SIZE_MAX - 2u || guest_len > SIZE_MAX - root_len - 2u) return NULL;
    size_t cap = root_len + guest_len + 2u;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    if (!sr_ms0_resolve(root, guest, out, cap, SR_HOST_PATH_SEP)) {
        free(out);
        return NULL;
    }
    return out;
}

static void hle_dirfd_release_descriptor(uint32_t fd) {
    if (fd >= sizeof(s_fds) / sizeof(s_fds[0])) return;
    hle_dirfd_clear(&s_dirfds[fd]);
    if (s_fds[fd].used && s_fds[fd].kind == FD_KIND_DIRECTORY)
        hle_fd_release(&s_fds[fd], 0);
}

static int hle_fd_is_directory(uint32_t fd) {
    return fd < (uint32_t)(sizeof(s_fds) / sizeof(s_fds[0])) &&
           s_fds[fd].used && s_fds[fd].kind == FD_KIND_DIRECTORY &&
           s_dirfds[fd].used;
}

static int hle_fd_is_std(uint32_t fd) {
    return fd < (uint32_t)(sizeof(s_fds) / sizeof(s_fds[0])) &&
           s_fds[fd].used && s_fds[fd].kind == FD_KIND_STD;
}

/* The UMD block device. A read-only descriptor for "umd0:", "umd1:" or "umd:" addresses the
 * disc image in 2048-byte sectors (PSP-UMDRAW-001 captured cells, docs/HARDWARE_ORACLE.md):
 * sceIoLseek and sceIoLseek32 return the new position in sectors, SEEK_END is relative to the
 * last sector, and sceIoRead takes and returns a count of sectors and advances the position by
 * it. A position below zero or past the last sector is refused with 0x8001b003 and leaves the
 * position where it was; a zero count is refused with 0x80010016. Two cases stay at a named
 * boundary because no capture covers them: a read that would cross the last sector, and every
 * data operation while no disc image is open. The console wrote a whole request even past the
 * end of a smaller buffer; the runtime refuses a destination that is not writable guest memory. */
#define IO_UMD_SECTOR_BYTES 2048u

/* The asynchronous request queue serves the UMD block device while a disc image is open: one
 * captured sceIoReadAsync of a raw UMD descriptor returned 0, its immediate sceIoPollAsync 1
 * (still pending) and its sceIoWaitAsync 0 with the sector count as the result
 * (PSP-UMDRAW-001). A queued read or seek completes through the same sector-unit device calls
 * as the synchronous ones, so its result and refusals are theirs. Every other raw descriptor,
 * and the UMD with no image, keeps its named boundary. */
static uint32_t hle_io_raw_async_fd_boundary(uint32_t fd, IoRawDeviceBoundary operation,
                                             const char *api) {
    if (hle_fd_is_raw_umd(fd)) return 0u;
    return hle_io_raw_fd_boundary(fd, operation, api);
}

static uint32_t hle_io_raw_umd_read(uint32_t fd, uint32_t dst, uint32_t sectors) {
    Fd *f = &s_fds[fd];
    if (sectors == 0u) return 0x80010016u;
    if (sectors > UINT32_MAX / IO_UMD_SECTOR_BYTES ||
        !sr_guest_span_writable(dst, sectors * IO_UMD_SECTOR_BYTES))
        return 0x80010016u;
    if (sectors > f->size - f->off)
        return hle_io_raw_device_boundary(IO_RAW_BOUNDARY_READ_PAST_END,
                                          "sceIoRead past the last disc sector",
                                          f->guest_path, fd);
    const uint32_t bytes = sectors * IO_UMD_SECTOR_BYTES;
    uint8_t tmp[4096];
    for (uint32_t done = 0; done < bytes;) {
        uint32_t n = bytes - done;
        if (n > sizeof(tmp)) n = (uint32_t)sizeof(tmp);
        if (iso_read(f->off, done, tmp, n) != (int)n) return 0x80010005u;
        for (uint32_t k = 0; k < n; k++) MEM_W8(dst + done + k, tmp[k]);
        done += n;
    }
    if (getenv("SR_IOLOG"))
        fprintf(stderr, "Read vbl=%u fd=%u sector=%u sectors=%u dst=0x%08x\n",
                s_vcount_fwd, fd, f->off, sectors, dst);
    f->off += sectors;
    return sectors;
}

#ifdef _WIN32
static int data_loose_merge_dir(const char *key, SrVfsDirList *list, int *found_out) {
    if (found_out) *found_out = 0;
    if (!s_data_root_utf8 || !list) return -1;
    char *path = data_loose_path_alloc(s_data_root_utf8, key ? key : "");
    if (!path) return -1;
    wchar_t *wide = NULL;
    wchar_t *pattern = NULL;
    if (!sr_wide_path_alloc(path, &wide) || !sr_wide_join_alloc(wide, L"*", &pattern)) {
        free(wide);
        free(pattern);
        free(path);
        return -1;
    }
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &attributes)) {
        free(pattern);
        free(wide);
        free(path);
        return 0;
    }
    if (!(attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        free(pattern);
        free(wide);
        free(path);
        return -1;
    }
    if (found_out) *found_out = 1;
    WIN32_FIND_DATAW child;
    HANDLE handle = FindFirstFileW(pattern, &child);
    free(pattern);
    free(wide);
    free(path);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? 0 : -1;
    }
    int result = 0;
    for (;;) {
        if (!(child.cFileName[0] == L'.' &&
              (child.cFileName[1] == L'\0' ||
               (child.cFileName[1] == L'.' && child.cFileName[2] == L'\0')))) {
            char *name = NULL;
            if (!sr_wide_to_utf8_alloc(child.cFileName, &name)) {
                result = -1;
            } else {
                int variant = -1;
                if (!sr_archive_variant_from_name(name, &variant)) {
                    if (child.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                        result = -1;
                    } else {
                        int is_dir = (child.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                        uint64_t size = is_dir ? 0u : (((uint64_t)child.nFileSizeHigh << 32) |
                                                         child.nFileSizeLow);
                        if (!is_dir && size > UINT32_MAX) {
                            list->skipped++;
                        } else if (!sr_vfs_dirlist_merge(list, name, is_dir, size)) {
                            result = -1;
                        }
                    }
                }
                free(name);
            }
        }
        if (result != 0) break;
        if (!FindNextFileW(handle, &child)) {
            if (GetLastError() != ERROR_NO_MORE_FILES) result = -1;
            break;
        }
    }
    FindClose(handle);
    return result;
}
#else /* !_WIN32: the same merge over opendir/readdir -------------------------- */
static int data_loose_merge_dir(const char *key, SrVfsDirList *list, int *found_out) {
    if (found_out) *found_out = 0;
    if (!s_data_root_utf8 || !list) return -1;
    char *path = data_loose_path_alloc(s_data_root_utf8, key ? key : "");
    if (!path) return -1;
    struct stat st, link;
    int is_link = lstat(path, &link) == 0 && S_ISLNK(link.st_mode);
    if (stat(path, &st) != 0) {
        free(path);
        return 0;
    }
    if (!S_ISDIR(st.st_mode) || is_link) {
        free(path);
        return -1;
    }
    if (found_out) *found_out = 1;
    DIR *dir = opendir(path);
    if (!dir) {
        int missing = errno == ENOENT;
        free(path);
        return missing ? 0 : -1;
    }
    int result = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            if (errno != 0) result = -1;
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        int variant = -1;
        if (!sr_archive_variant_from_name(entry->d_name, &variant)) {
            char *child = sr_utf8_join_alloc(path, entry->d_name, '/');
            if (!child) {
                result = -1;
            } else {
                struct stat cst;
                if (lstat(child, &cst) != 0 || S_ISLNK(cst.st_mode) ||
                    !(S_ISDIR(cst.st_mode) || S_ISREG(cst.st_mode))) {
                    result = -1;
                } else if (S_ISDIR(cst.st_mode)) {
                    if (!sr_vfs_dirlist_merge(list, entry->d_name, 1, 0u)) result = -1;
                } else if ((uint64_t)cst.st_size > UINT32_MAX) {
                    list->skipped++;
                } else if (!sr_vfs_dirlist_merge(list, entry->d_name, 0,
                                                 (uint64_t)cst.st_size)) {
                    result = -1;
                }
                free(child);
            }
        }
        if (result != 0) break;
    }
    closedir(dir);
    free(path);
    return result;
}
#endif /* _WIN32 */

static uint32_t h_IoOpen(CpuState *s) {
    char path[512];
    uint32_t rc = io_guest_path(A0, path, sizeof(path));
    if (rc != 0u) return rc;
    return h_io_open_path(path, A1, -1);
}

uint32_t h_IoWrite(CpuState *s) {
    /* a0=fd, a1=src, a2=count. Returns bytes written. */
    uint32_t fd = A0, src = A1, count = A2;
    if (fd >= (uint32_t)(sizeof(s_fds) / sizeof(s_fds[0])) || !s_fds[fd].used)
        return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    Fd *f = &s_fds[fd];
    if (f->async.state == IO_ASYNC_PENDING) return SCE_ERROR_KERNEL_ASYNC_BUSY;
    if (hle_fd_is_std(fd)) {                          /* std streams: dump to stderr */
        uint8_t buf[1024]; uint32_t n = count < sizeof(buf) ? count : sizeof(buf) - 1;
        for (uint32_t k = 0; k < n; k++) buf[k] = (uint8_t)MEM_R8(src + k);
        buf[n] = 0;
        if (hle_log_on()) {
            fprintf(stderr, "SCETYPEWRITE[%u]: %s\n", fd, (char*)buf);
            fflush(stderr);
        }
        return count;
    }
    uint32_t raw_error = hle_io_raw_fd_boundary(fd, IO_RAW_BOUNDARY_WRITE, "sceIoWrite");
    if (raw_error != 0u) return raw_error;
    if (f->kind != FD_KIND_FILE) return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    if (f->flash0) return sr_flash0_font_refuse_write(f->guest_path, "write through a font descriptor");
    if (!f->host) return 0x80010013;                 /* read-only (ISO) fd: not writable */
    if (count > UINT32_MAX - f->off) {
        /* A seek may leave the position anywhere in 32 bits; a write that would carry it
         * past them stops at the same boundary as such a seek (#1380). */
        static atomic_flag reported = ATOMIC_FLAG_INIT;
        if (!atomic_flag_test_and_set_explicit(&reported, memory_order_acq_rel))
            fprintf(stderr,
                    "SEMANTIC_BOUNDARY: IO_WRITE_OUT_OF_RANGE: sceIoWrite beyond the "
                    "descriptor's 32-bit position is not supported yet (0x80010086)\n");
        return 0x80010086u;
    }
    if (io_host_seek(f->host, f->off) != 0) return 0x80010005u;  /* host I/O error */
    uint8_t tmp[4096]; uint32_t done = 0;
    while (done < count) {
        uint32_t n = count - done; if (n > sizeof(tmp)) n = sizeof(tmp);
        for (uint32_t k = 0; k < n; k++) tmp[k] = (uint8_t)MEM_R8(src + done + k);
        done += (uint32_t)fwrite(tmp, 1, n, f->host);
        if (done < count) break;
    }
    fflush(f->host);
    f->off += done; if (f->off > f->size) f->size = f->off;
    return done;
}
uint32_t h_IoRead(CpuState *s) {
    /* a0=fd, a1=dst, a2=count. Returns bytes read. */
    uint32_t fd = A0, dst = A1, count = A2;
    if (fd < 3 && s_fds[fd].used && s_fds[fd].kind == FD_KIND_STD)
        return 0x80010009; /* baseline behavior preserved for standard streams */
    if (hle_fd_is_raw_umd(fd)) {
        if (s_fds[fd].async.state == IO_ASYNC_PENDING) return SCE_ERROR_KERNEL_ASYNC_BUSY;
        return hle_io_raw_umd_read(fd, dst, count);
    }
    uint32_t raw_error = hle_io_raw_fd_boundary(fd, IO_RAW_BOUNDARY_READ, "sceIoRead");
    if (raw_error != 0u) return raw_error;
    if (!hle_fd_is_file(fd)) return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    Fd *f = &s_fds[fd];
    if (f->async.state == IO_ASYNC_PENDING) return SCE_ERROR_KERNEL_ASYNC_BUSY;
    uint64_t perf_started = sr_perf_now_ns();
    if (f->enhancement_data) {
        if (f->off >= f->size || count == 0u) return 0u;
        if (count > f->size - f->off) count = f->size - f->off;
        if (!sr_guest_span_writable(dst, count)) return 0x80010016u;
        for (uint32_t i = 0; i < count; i++) MEM_W8(dst + i, f->enhancement_data[f->off + i]);
        f->off += count;
        return count;
    }
    if (f->archive_vfs) {
        if (dst && !sr_guest_span_writable(dst, count)) return 0x80010016u;
        if (f->off >= f->size || count == 0u) return 0u;
        if (count > f->size - f->off) count = f->size - f->off;
        uint8_t tmp[4096];
        uint32_t done = 0;
        SrArchiveFile archive_file = {
            f->archive_mount, f->archive_entry, f->size, -1
        };
        while (done < count) {
            uint32_t amount = count - done;
            if (amount > sizeof(tmp)) amount = (uint32_t)sizeof(tmp);
            size_t got = 0;
            NkResult result = sr_archive_vfs_read(
                f->archive_vfs, &archive_file, f->off + done, tmp, amount, &got);
            if (result != NK_OK || got == 0u || got > amount) {
                fprintf(stderr, "host_data: XB member read failed for fd=%u (%d)\n",
                        fd, (int)result);
                return 0x80010005u;
            }
            for (uint32_t i = 0; i < got; i++) MEM_W8(dst + done + i, tmp[i]);
            done += (uint32_t)got;
        }
        f->off += done;
        if (perf_started)
            sr_perf_storage_read(SR_PERF_STORAGE_VFS, done, perf_started, done == count);
        return done;
    }
    if (f->pgd) {
        /* Decrypt-on-read: f->off/f->size are the logical (decrypted) view; the
         * ciphertext is read from f->host at physical block offsets by pgd.c. */
        if (f->off >= f->size) return 0;
        if (f->off + count > f->size) count = f->size - f->off;
        uint32_t bs = sr_pgd_block_size(f->pgd), done = 0;
        while (done < count) {
            uint32_t L = f->off + done, bidx = L / bs, boff = L % bs;
            const uint8_t *blk = sr_pgd_block(f->pgd, f->host, bidx);
            if (!blk) break;
            uint32_t blen = sr_pgd_block_len(f->pgd, bidx);
            if (boff >= blen) break;
            uint32_t n = blen - boff; if (n > count - done) n = count - done;
            for (uint32_t k = 0; k < n; k++) MEM_W8(dst + done + k, blk[boff + k]);
            done += n;
        }
        f->off += done;
        if (perf_started) sr_perf_storage_read(SR_PERF_STORAGE_VFS, done, perf_started, done == count);
        return done;
    }
    if (f->off >= f->size) return 0u;
    if (count > f->size - f->off) count = f->size - f->off;
    /* Read in chunks straight into guest memory, from the host file or the ISO. */
    uint8_t tmp[4096];
    uint32_t done = 0;
    if (f->host && io_host_seek(f->host, f->off) != 0) return 0x80010005u;
    while (done < count) {
        uint32_t n = count - done; if (n > sizeof(tmp)) n = sizeof(tmp);
        if (f->host) { size_t got = fread(tmp, 1, n, f->host); if (got < n) n = (uint32_t)got; }
        else {
            int got = iso_read(f->lba, f->off + done, tmp, n);
            n = got > 0 ? (uint32_t)got : 0;
        }
        for (uint32_t k = 0; k < n; k++) MEM_W8(dst + done + k, tmp[k]);
        done += n;
        if (n == 0) break;
    }
    if (f->host && perf_started)
        sr_perf_storage_read(SR_PERF_STORAGE_VFS, done, perf_started, done == count);
    f->off += done;
    if (getenv("SR_IOLOG")) {
        static int n = 0;
        if (n++ < 4000)
            fprintf(stderr, "Read vbl=%u fd=%u off=%u size=%u dst=0x%08x -> %u\n",
                    s_vcount_fwd, fd, f->off - done, count, dst, done);
    }
    return done;
}
uint32_t h_IoLseek32(CpuState *s) {
    /* a0=fd, a1=offset, a2=whence. Returns new position (32-bit). */
    uint32_t fd = A0; int32_t off = (int32_t)A1; uint32_t whence = A2;
    if (fd < 3 && s_fds[fd].used && s_fds[fd].kind == FD_KIND_STD)
        return 0x80010009; /* baseline behavior preserved for standard streams */
    if (hle_fd_is_raw_umd(fd)) {
        if (s_fds[fd].async.state == IO_ASYNC_PENDING) return SCE_ERROR_KERNEL_ASYNC_BUSY;
        uint32_t position = 0;
        uint32_t rc = hle_io_raw_umd_seek(&s_fds[fd], off, whence, &position);
        return rc != 0u ? rc : position;
    }
    uint32_t raw_error = hle_io_raw_fd_boundary(fd, IO_RAW_BOUNDARY_LSEEK32,
                                               "sceIoLseek32");
    if (raw_error != 0u) return raw_error;
    if (!hle_fd_is_file(fd)) return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    if (whence >= 3u) return SCE_ERROR_KERNEL_INVALID_ARGUMENT;
    Fd *f = &s_fds[fd];
    if (f->async.state == IO_ASYNC_PENDING) return SCE_ERROR_KERNEL_ASYNC_BUSY;
    uint32_t position = 0;
    uint32_t refused = io_seek_destination(f, off, whence, "sceIoLseek32", &position);
    if (refused != 0u) return refused;
    f->off = position;
    return f->off;
}
static uint32_t h_IoLseek(CpuState *s) {
    /* a0=fd, a2/a3=aligned 64-bit offset (a1 is skipped), t0=whence.
     * Returns the 64-bit position in v0:v1. */
    uint32_t fd = A0;
    if (fd < 3 && s_fds[fd].used && s_fds[fd].kind == FD_KIND_STD) {
        s->r[3] = 0;
        return 0x80010009; /* baseline behavior preserved for standard streams */
    }
    if (hle_fd_is_raw_umd(fd)) {
        if (s_fds[fd].async.state == IO_ASYNC_PENDING) {
            s->r[3] = 0xffffffffu;
            return SCE_ERROR_KERNEL_ASYNC_BUSY;
        }
        uint32_t position = 0;
        uint32_t rc = hle_io_raw_umd_seek(&s_fds[fd], (int64_t)(((uint64_t)A3 << 32) | A2),
                                          stack_arg(s, 0), &position);
        s->r[3] = rc != 0u ? 0xffffffffu : 0u;
        return rc != 0u ? rc : position;
    }
    uint32_t raw_error = hle_io_raw_fd_boundary(fd, IO_RAW_BOUNDARY_LSEEK64,
                                               "sceIoLseek");
    if (raw_error != 0u) { s->r[3] = 0xffffffffu; return raw_error; }
    if (!hle_fd_is_file(fd)) { s->r[3] = 0xFFFFFFFF; return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR; }
    int64_t off = (int64_t)(((uint64_t)A3 << 32) | A2);
    uint32_t whence = stack_arg(s, 0);
    if (s_fds[fd].async.state == IO_ASYNC_PENDING) {
        s->r[3] = 0xFFFFFFFF;
        return SCE_ERROR_KERNEL_ASYNC_BUSY;
    }
    if (whence >= 3u) { s->r[3] = 0xFFFFFFFF; return SCE_ERROR_KERNEL_INVALID_ARGUMENT; }
    Fd *f = &s_fds[fd];
    uint32_t position = 0;
    uint32_t refused = io_seek_destination(f, off, whence, "sceIoLseek", &position);
    if (refused != 0u) { s->r[3] = 0xFFFFFFFF; return refused; }
    f->off = position;
    s->r[3] = 0;
    return position;
}
static uint32_t h_IoIoctl(CpuState *s) {
    /* a0=fd, a1=cmd, a2=indata, a3=inlen, t0=outdata, t1=outlen */
    return h_io_ioctl_args(A0, A1, A2, A3, stack_arg(s, 0), stack_arg(s, 1));
}
static uint32_t h_IoClose(CpuState *s) {
    uint32_t fd = A0;
    if (fd >= (uint32_t)(sizeof(s_fds) / sizeof(s_fds[0])) || !s_fds[fd].used)
        return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    if (s_fds[fd].kind == FD_KIND_DIRECTORY)
        return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    if (s_fds[fd].async.state == IO_ASYNC_PENDING)
        return SCE_ERROR_KERNEL_ASYNC_BUSY;
    hle_fd_release(&s_fds[fd], s_fds[fd].kind == FD_KIND_STD);
    s_closed_res[fd] = 0;
    return 0;
}

/* Classify the initial FindFirstFileW result for an existing, contained host
 * directory.  An empty directory reports ERROR_FILE_NOT_FOUND for the `*`
 * pattern and is still an existing directory; every other initial failure is
 * an incomplete listing and must stay fail-closed. */
#ifdef _WIN32
static uint32_t vfs_overlay_initial_find_error(unsigned long error, int *found) {
    if (found) *found = 0;
    if (error == ERROR_FILE_NOT_FOUND) {
        if (found) *found = 1;
        return 0;
    }
    return 0x80010005u; /* SCE_KERNEL_ERROR_ERRNO_IO */
}
#else
/* The POSIX overlay listing. The directory is reached through the contained seam's
 * descriptor walk from the bound Memory Stick root, the walk every POSIX ms0: open
 * already takes: each component is opened with O_NOFOLLOW, so a link on the way is
 * refused where the Win32 arm refuses a directory resolving outside the root. The
 * listing reads that same descriptor, so no path is resolved twice. */
static uint32_t vfs_overlay_merge_dir_posix(const char *guest_path, SrVfsDirList *list,
                                            int *found) {
    char trimmed[SR_CD_REL_MAX + 16u], rel[SR_CD_REL_MAX];
    const char *tail = NULL;
    size_t n = strlen(guest_path);
    if (n >= sizeof(trimmed)) return 0;
    memcpy(trimmed, guest_path, n + 1u);
    /* A trailing separator names the same directory, as the Win32 host path does. */
    while (n > 0u && (trimmed[n - 1u] == '/' || trimmed[n - 1u] == '\\')) trimmed[--n] = '\0';
    if (sr_ms0_classify(trimmed, &tail) != SR_MS0_OWNED || !tail) return 0;
    while (*tail == '/' || *tail == '\\') tail++;
    if (*tail == '\0') rel[0] = '\0'; /* the root itself */
    else if (ms0_posix_guest_relpath(trimmed, rel, sizeof(rel)) != SR_CD_OK) return 0;
    sr_cd_root root;
    if (ms0_posix_root_open(&root) != SR_CD_OK) return 0;
    int dir_fd = -1;
    sr_cd_status st = sr_cd__at_walk(root.fd, rel, &dir_fd);
    sr_cd_root_close(&root);
    if (st == SR_CD_NOT_FOUND) return 0;
    if (st != SR_CD_OK) {
        fprintf(stderr, "sceIoDopen: refusing '%s': no directory reachable without a link "
                        "inside the configured Memory Stick root (%s)\n",
                guest_path, sr_cd_status_name(st));
        fflush(stderr);
        return 0x80010014u; /* SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND, as on Win32 */
    }
    DIR *dir = fdopendir(dir_fd);
    if (!dir) {
        close(dir_fd);
        return 0x80010005u; /* SCE_KERNEL_ERROR_ERRNO_IO */
    }
    *found = 1;
    uint32_t rc = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            if (errno != 0) {
                fprintf(stderr, "sceIoDopen: host enumeration of '%s' ended early (errno=%d); "
                                "refusing a short listing\n", guest_path, errno);
                rc = 0x80010005u;
            }
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat child;
        if (fstatat(dirfd(dir), entry->d_name, &child, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) continue; /* removed since readdir named it */
            fprintf(stderr, "sceIoDopen: host child of '%s' cannot be classified (errno=%d); "
                            "refusing a short listing\n", guest_path, errno);
            rc = 0x80010005u;
            break;
        }
        int is_dir = S_ISDIR(child.st_mode);
        if (!is_dir && (uint64_t)child.st_size > UINT32_MAX) {
            fprintf(stderr, "sceIoDopen: host file '%s' exceeds the guest size limit; "
                            "skipping one entry\n", entry->d_name);
            list->skipped++;
        } else if (!sr_vfs_dirlist_merge(list, entry->d_name, is_dir,
                                         is_dir ? 0u : (uint64_t)child.st_size)) {
            rc = 0x80010008u; /* SCE_KERNEL_ERROR_NO_MEMORY */
            break;
        }
    }
    closedir(dir);
    return rc;
}
#endif

/* Merge the writable host overlay's children for `guest_path` into `list`.
 *
 * `*found` reports whether the overlay actually has this directory; the return
 * value is 0 on success or a PSP error when the overlay has the directory but
 * it cannot be enumerated safely.  Containment stays fail-closed AND loud: a
 * host directory whose final path resolves outside the unified Memory Stick
 * root's canonical root is refused here and the refusal is the caller's
 * answer, so a planted junction can never be quietly papered over by the
 * index leg.  A missing unified-root directory that still exists as a
 * hierarchical tree under legacy SR_FSDIR is reported as a refused migration
 * (found stays 0) rather than silently listed from the old root.
 *
 * A single child the guest namespace cannot represent (an overlong name, or a
 * size the guest's 32-bit stat field cannot carry) is skipped and counted, not
 * escalated: one malformed entry must not destroy an otherwise-valid
 * directory.  A truncated enumeration IS escalated, because a short listing
 * the guest believes is complete is a correctness failure. */
static uint32_t vfs_overlay_merge_dir(const char *guest_path, SrVfsDirList *list, int *found) {
    *found = 0;
    char *hp = host_dir_path_alloc(guest_path);
    if (!hp) return 0;
    wchar_t *root = NULL;
    wchar_t *pattern = NULL;
    uint32_t rc = 0;
    if (sr_wide_path_alloc(hp, &root) && sr_wide_join_alloc(root, L"*", &pattern)) {
#ifdef _WIN32
        /* Canonical containment root is the unified Memory Stick root (issue
         * #334), not the legacy flat SR_FSDIR. SR_FSDIR remains only as the
         * legacy hierarchical-dir migration source checked below. */
        wchar_t canonical_ms[MAX_PATH * 2];
        const char *ms_root = sr_ms0_root();
        int ms_ok = sr_vfs_canonical_root(ms_root, canonical_ms,
                                          sizeof(canonical_ms) / sizeof(wchar_t));

        HANDLE h_dir = CreateFileW(root, FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (h_dir != INVALID_HANDLE_VALUE) {
            int contained = ms_ok && sr_vfs_handle_is_contained(h_dir, canonical_ms);
            CloseHandle(h_dir);
            if (!contained) {
                fprintf(stderr,
                        "sceIoDopen: refusing '%s': host directory resolves outside the "
                        "configured Memory Stick root\n", guest_path);
                fflush(stderr);
                rc = 0x80010014u; /* SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND */
            } else {
                WIN32_FIND_DATAW fd;
                HANDLE h_find = FindFirstFileW(pattern, &fd);
                if (h_find != INVALID_HANDLE_VALUE) {
                    *found = 1;
                    BOOL more = TRUE;
                    while (more && rc == 0) {
                        int dot = fd.cFileName[0] == L'.' &&
                                  (fd.cFileName[1] == L'\0' ||
                                   (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'));
                        if (!dot) {
                            int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                            char *child = NULL;
                            if (!sr_wide_to_utf8_alloc(fd.cFileName, &child)) {
                                fprintf(stderr,
                                        "sceIoDopen: host child name is not convertible in '%s'; "
                                        "skipping one entry\n", guest_path);
                                list->skipped++;
                            } else if (!is_dir && fd.nFileSizeHigh != 0u) {
                                fprintf(stderr,
                                        "sceIoDopen: host file '%s' exceeds the guest size limit; "
                                        "skipping one entry\n", child);
                                list->skipped++;
                                free(child);
                            } else {
                                if (!sr_vfs_dirlist_merge(list, child, is_dir,
                                                          is_dir ? 0u : (uint64_t)fd.nFileSizeLow))
                                    rc = 0x80010008u; /* SCE_KERNEL_ERROR_NO_MEMORY */
                                free(child);
                            }
                        }
                        if (rc == 0) more = FindNextFileW(h_find, &fd);
                    }
                    DWORD last_err = GetLastError();
                    FindClose(h_find);
                    if (rc == 0 && !more && last_err != ERROR_NO_MORE_FILES) {
                        fprintf(stderr,
                                "sceIoDopen: host enumeration of '%s' ended early (error=%lu); "
                                "refusing a short listing\n", guest_path, last_err);
                        rc = 0x80010005u;
                    }
                } else {
                    DWORD first_error = GetLastError();
                    rc = vfs_overlay_initial_find_error(first_error, found);
                    if (rc != 0) {
                        fprintf(stderr,
                                "sceIoDopen: initial host enumeration of '%s' failed "
                                "(error=%lu); refusing a short listing\n",
                                guest_path, (unsigned long)first_error);
                    }
                }
            }
        } else {
            DWORD open_err = GetLastError();
            if (open_err == ERROR_FILE_NOT_FOUND || open_err == ERROR_PATH_NOT_FOUND) {
                /* Unified-root directory missing: if the same relative path
                 * still exists as a hierarchical tree under legacy SR_FSDIR,
                 * log the refused migration once per attempt. *found stays 0
                 * so the guest sees not-found, never a silent dual-root list. */
                char *configured_fs = NULL;
                int configured_fs_present = 0;
                sr_utf8_env_alloc(L"SR_FSDIR", &configured_fs, &configured_fs_present);
                const char *fs_dir = configured_fs_present && configured_fs[0]
                                         ? configured_fs
                                         : "fs";
                const char *rel = NULL;
                if (sr_ms0_classify(guest_path, &rel) == SR_MS0_OWNED && rel) {
                    while (*rel == '/' || *rel == '\\') rel++;
                    if (*rel) {
                        size_t fs_len = strlen(fs_dir), rel_len = strlen(rel);
                        if (fs_len <= SIZE_MAX - rel_len - 2u) {
                            size_t lcap = fs_len + rel_len + 2u;
                            char *legacy = (char *)malloc(lcap);
                            if (legacy &&
                                sr_vfs_host_dir_path(fs_dir, rel, legacy, lcap, '\\') > 0) {
                                wchar_t *wlegacy = NULL;
                                if (sr_wide_path_alloc(legacy, &wlegacy)) {
                                    DWORD lattrs = GetFileAttributesW(wlegacy);
                                    if (lattrs != INVALID_FILE_ATTRIBUTES &&
                                        (lattrs & FILE_ATTRIBUTE_DIRECTORY)) {
                                        fprintf(stderr,
                                                "sceIoDopen: '%s' exists under legacy SR_FSDIR "
                                                "but not the unified Memory Stick root; "
                                                "automatic migration is refused\n",
                                                guest_path);
                                        fflush(stderr);
                                    }
                                    free(wlegacy);
                                }
                            }
                            free(legacy);
                        }
                    }
                }
                free(configured_fs);
            }
        }
#else
        rc = vfs_overlay_merge_dir_posix(guest_path, list, found);
#endif
    }
    free(pattern);
    free(root);
    free(hp);
    return rc;
}

static uint32_t h_io_dopen_path(const char *path) {
    if (!path) return 0x80010016u;
    if (getenv("SR_IOLOG")) fprintf(stderr, "HLE_IoDopen: path='%s'\n", path);
    if (io_path_equals_ci(path, "disc0:")) return 0x80010016u;
    IoRawDevicePath raw_device = io_raw_device_path(path);
    if (raw_device != IO_RAW_DEVICE_NONE) {
        if ((raw_device == IO_RAW_DEVICE_UMD0 && io_path_equals_ci(path, "umd0:")) ||
            (raw_device == IO_RAW_DEVICE_UMD1 && io_path_equals_ci(path, "umd1:")) ||
            (raw_device == IO_RAW_DEVICE_UMD_ALIAS && io_path_equals_ci(path, "umd:")))
            return 0x80020325u;
        return hle_io_raw_device_boundary(IO_RAW_BOUNDARY_DOPEN,
                                          "sceIoDopen", path, UINT32_MAX);
    }
    int fd = hle_fd_allocate();
    if (fd < 0) return SCE_ERROR_KERNEL_TOO_MANY_OPEN_FILES;
    uint32_t i = (uint32_t)fd;
    DirFd *d = &s_dirfds[i];
    memset(d, 0, sizeof(*d));
    d->used = 1; d->index = 0;
    memset(&s_fds[i], 0, sizeof(s_fds[i]));
    s_fds[i].used = 1;
    s_fds[i].kind = FD_KIND_DIRECTORY;
    s_closed_res[i] = 0;
    s_closed_async_state[i] = IO_ASYNC_IDLE;
    d->path = sr_asset_index_strdup(path);
    if (!d->path) { hle_dirfd_release_descriptor(i); return 0x80010014u; }
    if (sr_flash0_font_is_device_path(path)) {
                /* flash0: listings are materialized here like the merged namespace (backend 1),
                 * so Dread and Dclose need no device-specific code. */
                Flash0Sources sources;
                flash0_font_sources(&sources);
                sr_vfs_dirlist_init(&d->list);
                uint32_t font_dir_rc = sr_flash0_font_list_dir(path, &sources, &d->list);
                if (font_dir_rc != 0u) {
                    hle_dirfd_release_descriptor(i);
                    return font_dir_rc;
                }
                d->backend = 1;
                return i;
    }
            int device_path = io_path_is_disc_device(path);
            int iso_first_result = -1;
            IsoDirEntry iso_first = {0};
            if (device_path) {
                iso_first_result = iso_list(path, 0, &iso_first);
                if (iso_first_result < 0) {
                    uint32_t iso_lba = 0, iso_size = 0;
                    /* A file on the ISO must not be shadowed by the extracted
                     * directory index merely because its directory probe failed. */
                    if (iso_lookup(path, &iso_lba, &iso_size) == 0) {
                        hle_dirfd_release_descriptor(i);
                        return 0x80010014u; /* SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND */
                    }
                }
            }
            /* One guest namespace, three possible sources, materialized here
             * so a listing the guest walks cannot change mid-walk.  An ISO
             * directory is the source sceIoOpen resolves first; the writable
             * overlay and then the extracted-data index fill in names the ISO
             * does not provide.  A missing ISO directory still reaches the
             * same overlay/index fallback below. */
            sr_vfs_dirlist_init(&d->list);

            if (iso_first_result >= 0) {
                d->list.exists = 1;
                for (uint32_t iso_index = 0;; iso_index++) {
                    IsoDirEntry entry;
                    int iso_result;
                    if (iso_index == 0u) {
                        entry = iso_first;
                        iso_result = iso_first_result;
                    } else {
                        iso_result = iso_list(path, iso_index, &entry);
                    }
                    if (iso_result == 0) break;
                    if (iso_result < 0) {
                        hle_dirfd_release_descriptor(i);
                        return 0x80010005u; /* SCE_KERNEL_ERROR_ERRNO_IO */
                    }
                    if (!sr_vfs_dirlist_merge_lba(
                            &d->list, entry.name, entry.is_dir,
                            entry.is_dir ? 0u : (uint64_t)entry.size,
                            entry.lba)) {
                        hle_dirfd_release_descriptor(i);
                        return 0x80010008u; /* SCE_KERNEL_ERROR_NO_MEMORY */
                    }
                    if (iso_index == UINT32_MAX) {
                        hle_dirfd_release_descriptor(i);
                        return 0x80010005u; /* SCE_KERNEL_ERROR_ERRNO_IO */
                    }
                }
            }

            int overlay_found = 0;
            uint32_t overlay_rc = vfs_overlay_merge_dir(path, &d->list, &overlay_found);
            if (overlay_rc != 0) {
                hle_dirfd_release_descriptor(i);
                return overlay_rc;
            }
            if (overlay_found) d->list.exists = 1;

            int wanted_variant = -2;
            char *norm_key = data_normalize_guest_key(path, &wanted_variant);
            if (norm_key) {
                if (s_data_archive_mode && data_archive_guest_path_allowed(path)) {
                    int loose_found = 0;
                    if (data_loose_merge_dir(norm_key, &d->list, &loose_found) != 0) {
                        free(norm_key);
                        hle_dirfd_release_descriptor(i);
                        return 0x80010005u;
                    }
                    if (loose_found) d->list.exists = 1;
                    if (sr_asset_index_list_dir(&s_data_index, norm_key,
                                                wanted_variant, &d->list) < 0) {
                        free(norm_key);
                        hle_dirfd_release_descriptor(i);
                        return 0x80010008u;
                    }
                    if (sr_archive_vfs_list_dir(&s_archive_vfs, norm_key,
                                                wanted_variant, &d->list) < 0) {
                        free(norm_key);
                        hle_dirfd_release_descriptor(i);
                        return 0x80010008u;
                    }
                } else if (atomic_load_explicit(&s_data_state, memory_order_acquire) ==
                           SR_DATA_STATE_READY) {
                    if (sr_asset_index_list_dir(&s_data_index, norm_key,
                                                wanted_variant, &d->list) < 0) {
                        free(norm_key);
                        hle_dirfd_release_descriptor(i);
                        return 0x80010008u;
                    }
                }
                free(norm_key);
            }

            /* ABSENT and EMPTY are different answers. Only a directory no
             * source has is a not-found; a directory that exists with no
             * representable children opens and reads back zero entries. */
            if (!d->list.exists) {
                hle_dirfd_release_descriptor(i);
                return 0x80010014u; /* SCE_KERNEL_ERROR_ERRNO_FILE_NOT_FOUND */
            }
            if (d->list.skipped != 0u) {
                fprintf(stderr,
                        "sceIoDopen: '%s' listing omits %zu entry/entries the guest "
                        "namespace cannot represent\n", path, d->list.skipped);
            }
            sr_vfs_dirlist_sort(&d->list);
            d->backend = 1;
            return i;
    return SCE_ERROR_KERNEL_TOO_MANY_OPEN_FILES;
}

static uint32_t h_IoDopen(CpuState *s) {
    char path[512];
    uint32_t rc = io_guest_path(A0, path, sizeof(path));
    return rc != 0u ? rc : h_io_dopen_path(path);
}

static uint32_t h_IoDread(CpuState *s) {
    uint32_t fd = A0, de = A1;
    uint32_t raw_error = hle_io_raw_fd_boundary(fd, IO_RAW_BOUNDARY_DREAD,
                                                "sceIoDread");
    if (raw_error != 0u) return raw_error;
    if (!hle_fd_is_directory(fd)) return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    DirFd *d = &s_dirfds[fd];
    if (!de) return 0x80010009u; /* preserve baseline behavior for null pointer */
    IsoDirEntry e;
    if (d->backend == 0) {
        int r = iso_list(d->path, d->index, &e);
        if (r <= 0) return r < 0 ? 0x80010005u : 0;
        d->index++;
    } else {
        /* The listing was materialized and bounded at Dopen: every name fits
         * e.name and every size fits the guest's 32-bit field, so this walk
         * has no failure mode of its own and simply runs out of entries. */
        if (d->index >= d->list.count) return 0;
        const SrVfsDirEntry *ve = &d->list.entries[d->index++];
        memset(&e, 0, sizeof(e));
        memcpy(e.name, ve->name, strlen(ve->name) + 1u);
        e.is_dir = ve->is_dir;
        e.size = (uint32_t)ve->size;
        e.lba = ve->lba;
    }
    for (uint32_t i = 0; i < 0x15cu; i++) MEM_W8(de + i, 0);
    MEM_W32(de + 0x00, (e.is_dir ? 0x1000u : 0x2000u) | 0x0124u);
    MEM_W32(de + 0x04, e.is_dir ? 0x0010u : 0x0020u);
    MEM_W32(de + 0x08, e.size); MEM_W32(de + 0x0c, 0);
    MEM_W32(de + 0x40, e.lba);
    for (uint32_t i = 0; i < sizeof(e.name) && e.name[i]; i++) MEM_W8(de + 0x58 + i, (uint8_t)e.name[i]);
    return 1;
}

static uint32_t h_IoDclose(CpuState *s) {
    uint32_t fd = A0;
    uint32_t raw_error = hle_io_raw_fd_boundary(fd, IO_RAW_BOUNDARY_DCLOSE,
                                                "sceIoDclose");
    if (raw_error != 0u) return raw_error;
    if (!hle_fd_is_directory(fd)) return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    hle_dirfd_release_descriptor(fd);
    return 0;
}
/* PSPSDK's SceDevctlCmd contains a pointer to SceDevInf for SCE_PR_GETDEV
 * (0x02425818); its five words are maxClusters, freeClusters, maxSectors,
 * sectorSize, and sectorCount. The host root is the unified Memory Stick
 * root (issue #334) shared with ordinary sceIo* ms0: operations and savedata. */
#ifdef _WIN32
int io_memstick_capacity(uint32_t info[5]) {
    char *host_root = host_dir_path_alloc("");
    wchar_t *host_root_wide = NULL;
    int path_ok = host_root && sr_wide_path_alloc(host_root, &host_root_wide);
    free(host_root);
    if (!path_ok) {
        free(host_root_wide);
        return 0;
    }

    DWORD sectors_per_cluster = 0, bytes_per_sector = 0;
    DWORD free_clusters = 0, total_clusters = 0;
    BOOL queried = GetDiskFreeSpaceW(host_root_wide, &sectors_per_cluster,
                                     &bytes_per_sector, &free_clusters,
                                     &total_clusters);
    free(host_root_wide);
    if (!queried || !sectors_per_cluster || !bytes_per_sector || !total_clusters ||
        sectors_per_cluster > INT32_MAX || bytes_per_sector > INT32_MAX)
        return 0;

    uint64_t cluster_bytes = (uint64_t)sectors_per_cluster * bytes_per_sector;
    if (!cluster_bytes) return 0;
    /* PSP-visible arithmetic is commonly 32-bit. Limit the reported volume to
     * the largest byte capacity those consumers can represent without wrap. */
    uint64_t max_clusters = total_clusters;
    uint64_t max_psp_clusters = UINT32_MAX / cluster_bytes;
    if (max_clusters > max_psp_clusters) max_clusters = max_psp_clusters;
    uint64_t available_clusters = free_clusters;
    if (available_clusters > max_clusters) available_clusters = max_clusters;
    uint64_t max_sectors = max_clusters * sectors_per_cluster;
    if (max_sectors > UINT32_MAX) return 0;

    info[0] = (uint32_t)max_clusters;
    info[1] = (uint32_t)available_clusters;
    info[2] = (uint32_t)max_sectors;
    info[3] = bytes_per_sector;
    info[4] = sectors_per_cluster;
    return 1;
}
#else /* !_WIN32: the same PSP-visible volume numbers from statvfs() ----------- */
int io_memstick_capacity(uint32_t info[5]) {
    char *host_root = host_dir_path_alloc("");
    struct statvfs vfs;
    int queried = host_root && statvfs(host_root, &vfs) == 0;
    free(host_root);
    if (!queried) return 0;
    /* statvfs reports fragment size; POSIX.1-2008 exposes no sector size, so
     * the PSP per-sector field carries the host fragment and the cluster
     * count is expressed in those same fragments. The PSP-visible clamp
     * below is the one that matters: a 32-bit consumer must not wrap. */
    uint32_t bytes_per_sector = vfs.f_frsize ? (uint32_t)vfs.f_frsize : (uint32_t)vfs.f_bsize;
    uint32_t sectors_per_cluster = 1u;
    if (!bytes_per_sector) return 0;
    uint64_t max_clusters = vfs.f_blocks;
    uint64_t max_psp_clusters = UINT32_MAX / (uint64_t)bytes_per_sector;
    if (max_clusters > max_psp_clusters) max_clusters = max_psp_clusters;
    uint64_t available_clusters = vfs.f_bavail < vfs.f_bfree ? vfs.f_bavail : vfs.f_bfree;
    if (available_clusters > max_clusters) available_clusters = max_clusters;
    uint64_t max_sectors = max_clusters * sectors_per_cluster;
    if (max_sectors > UINT32_MAX) return 0;
    info[0] = (uint32_t)max_clusters;
    info[1] = (uint32_t)available_clusters;
    info[2] = (uint32_t)max_sectors;
    info[3] = bytes_per_sector;
    info[4] = sectors_per_cluster;
    return 1;
}
#endif /* _WIN32 */

static uint32_t h_IoDevctl(CpuState *s) {
    enum {
        IO_DEVCTL_GET_CAPACITY = 0x02425818u,
        IO_DEVCTL_CHECK_INSERTED = 0x02025806u,
        IO_DEVCTL_CHECK_READY = 0x02025801u,
        IO_DEVCTL_CHECK_MEDIUM = 0x02425823u,
        IO_DEVCTL_REGISTER_CALLBACK = 0x02415821u,
        IO_DEVCTL_UNREGISTER_CALLBACK = 0x02415822u,
        IO_DEVCTL_MSCM_REGISTER_CALLBACK = 0x02015804u,
        IO_DEVCTL_MSCM_UNREGISTER_CALLBACK = 0x02015805u,
        IO_DEVCTL_ERROR_UNSUPPORTED = 0x80010086u,
        IO_DEVCTL_ERROR_INVALID_ARGUMENT = 0x80010016u,
        IO_DEVCTL_ERROR_IO = 0x80010005u
    };
    char device[128];
    uint32_t outdata = s->r[8], outlen = s->r[9];
    if (!A0 || !guest_cstr(A0, device, (int)sizeof(device)))
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (io_raw_device_path(device) != IO_RAW_DEVICE_NONE)
        return hle_io_raw_device_boundary(IO_RAW_BOUNDARY_DEVCTL,
                                          "sceIoDevctl", device, UINT32_MAX);
    int is_memstick = strcmp(device, "ms0:") == 0 ||
                      strcmp(device, "fatms0:") == 0 ||
                      strcmp(device, "mscmhc0:") == 0 ||
                      strcmp(device, "memstick:") == 0;
    if (!is_memstick) {
        hle_note_devctl_refusal(device, A1, IO_DEVCTL_ERROR_UNSUPPORTED);
        return IO_DEVCTL_ERROR_UNSUPPORTED;
    }

    switch (A1) {
        case IO_DEVCTL_GET_CAPACITY: {
            if (A3 < 4u || A3 > INT32_MAX) return IO_DEVCTL_ERROR_INVALID_ARGUMENT;
            if (!A2 || !sr_guest_span_readable(A2, 4u))
                return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
            uint32_t info_addr = MEM_R32(A2);
            if (!info_addr || !sr_guest_span_writable(info_addr, 20u))
                return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
            uint32_t info[5];
            if (!io_memstick_capacity(info)) return IO_DEVCTL_ERROR_IO;
            for (uint32_t i = 0; i < 5u; i++) MEM_W32(info_addr + i * 4u, info[i]);
            return 0;
        }
        /* 0x02425823 is the fatms0: medium check games make before saving or
         * loading ("is a Memory Stick inserted?"). PPSSPP's sceIo.cpp answers it
         * with 1 while its Memory Stick is inserted, which this runtime's always
         * is; refusing it told games there was no Memory Stick (#341). */
        case IO_DEVCTL_CHECK_INSERTED:
        case IO_DEVCTL_CHECK_MEDIUM:
        case IO_DEVCTL_CHECK_READY: {
            if (outlen < 4u || outlen > INT32_MAX)
                return IO_DEVCTL_ERROR_INVALID_ARGUMENT;
            if (!outdata || !sr_guest_span_writable(outdata, 4u))
                return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
            MEM_W32(outdata, A1 == IO_DEVCTL_CHECK_READY ? 4u : 1u);
            return 0;
        }
        case IO_DEVCTL_REGISTER_CALLBACK:
        case IO_DEVCTL_UNREGISTER_CALLBACK:
        case IO_DEVCTL_MSCM_REGISTER_CALLBACK:
        case IO_DEVCTL_MSCM_UNREGISTER_CALLBACK: {
            if (A3 < 4u || A3 > INT32_MAX) return IO_DEVCTL_ERROR_INVALID_ARGUMENT;
            if (!A2 || !sr_guest_span_readable(A2, 4u))
                return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
            uint32_t callback_uid = MEM_R32(A2);
            if (A1 == IO_DEVCTL_UNREGISTER_CALLBACK ||
                A1 == IO_DEVCTL_MSCM_UNREGISTER_CALLBACK)
                return memstick_callback_remove_first(callback_uid)
                           ? 0u : IO_DEVCTL_ERROR_INVALID_ARGUMENT;
            if (sr_callback_find_in_table(callback_uid) < 0 ||
                s_memstick_callback_count >= MEMSTICK_CALLBACK_MAX)
                return IO_DEVCTL_ERROR_INVALID_ARGUMENT;
            s_memstick_callbacks[s_memstick_callback_count++] = callback_uid;
            /* The runtime's Memory Stick is always inserted and assigned (the
             * inserted query above answers 1), so registering queues one
             * notification with argument 1 (inserted). sr_callback_notify only
             * marks it pending and wakes the owner; the body runs on the owning
             * thread at its next callback point. No later insert or eject event
             * exists in this runtime. */
            return sr_callback_notify(callback_uid, 1u);
        }
        default:
            hle_note_devctl_refusal(device, A1, IO_DEVCTL_ERROR_UNSUPPORTED);
            return IO_DEVCTL_ERROR_UNSUPPORTED;
    }
}

/* Asynchronous file requests wait in per-descriptor state. The runtime has no
 * host I/O worker, so one due request is completed at each later HLE import;
 * WaitAsync completes its target immediately because the guest explicitly
 * requested a wait. The deterministic two-import delay is synthetic policy,
 * not a measured PSP scheduling interval. */
static uint32_t io_async_submit(uint32_t fd, IoAsyncOp op, uint32_t a1,
                                uint32_t a2, uint32_t a3,
                                uint32_t stack0, uint32_t stack1) {
    const int raw_umd_queued = hle_fd_is_raw_umd(fd) &&
        (op == IO_ASYNC_READ || op == IO_ASYNC_LSEEK32 || op == IO_ASYNC_LSEEK64 ||
         op == IO_ASYNC_CLOSE);
    if (hle_fd_is_raw_device(fd) && !raw_umd_queued) {
        switch (op) {
        case IO_ASYNC_READ:
            return hle_io_raw_fd_boundary(fd, IO_RAW_BOUNDARY_READ_ASYNC,
                                          "sceIoReadAsync");
        case IO_ASYNC_WRITE:
            return hle_io_raw_fd_boundary(fd, IO_RAW_BOUNDARY_WRITE_ASYNC,
                                          "sceIoWriteAsync");
        case IO_ASYNC_LSEEK32:
            return hle_io_raw_fd_boundary(fd, IO_RAW_BOUNDARY_LSEEK32_ASYNC,
                                          "sceIoLseek32Async");
        case IO_ASYNC_LSEEK64:
            return hle_io_raw_fd_boundary(fd, IO_RAW_BOUNDARY_LSEEK64_ASYNC,
                                          "sceIoLseekAsync");
        case IO_ASYNC_IOCTL:
            return hle_io_raw_fd_boundary(fd, IO_RAW_BOUNDARY_IOCTL_ASYNC,
                                          "sceIoIoctlAsync");
        case IO_ASYNC_CLOSE:
            return hle_io_raw_fd_boundary(fd, IO_RAW_BOUNDARY_CLOSE_ASYNC,
                                          "sceIoCloseAsync");
        default:
            return hle_io_raw_device_boundary(IO_RAW_BOUNDARY_ASYNC_STATUS,
                                              "asynchronous I/O", NULL, fd);
        }
    }
    if (fd >= sizeof(s_fds) / sizeof(s_fds[0]) || !s_fds[fd].used ||
        (s_fds[fd].kind != FD_KIND_FILE && !raw_umd_queued &&
         !(s_fds[fd].kind == FD_KIND_STD &&
           (op == IO_ASYNC_WRITE || op == IO_ASYNC_CLOSE))))
        return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    IoAsyncRequest *request = &s_fds[fd].async;
    if (request->state == IO_ASYNC_PENDING) return SCE_ERROR_KERNEL_ASYNC_BUSY;
    int32_t priority = request->priority;
    uint32_t callback_uid = request->callback_uid;
    uint32_t callback_arg = request->callback_arg;
    memset(request, 0, sizeof(*request));
    request->state = IO_ASYNC_PENDING;
    request->op = op;
    request->submitted_epoch = s_io_dispatch_epoch;
    request->sequence = ++s_io_async_sequence;
    request->priority = priority;
    request->callback_uid = callback_uid;
    request->callback_arg = callback_arg;
    request->a1 = a1;
    request->a2 = a2;
    request->a3 = a3;
    request->stack0 = stack0;
    request->stack1 = stack1;
    s_closed_async_state[fd] = IO_ASYNC_IDLE;
    return 0u;
}

static uint32_t h_IoOpenAsync(CpuState *s) {
    char path[512];
    uint32_t rc = io_guest_path(A0, path, sizeof(path));
    if (rc != 0u) return rc;
    if (io_raw_device_path(path) != IO_RAW_DEVICE_NONE)
        return hle_io_raw_device_boundary(IO_RAW_BOUNDARY_OPEN_ASYNC,
                                          "sceIoOpenAsync", path, UINT32_MAX);
    /* The host lookup runs now so the call can return the PSP-visible fd, but the
     * open itself stays a pending request on that fd, completed by the dispatch
     * tick like every other asynchronous request. A game registers the completion
     * callback with sceIoSetAsyncCallback on the returned fd, after this call;
     * completing here left nothing to notify, so a loader that waits for that
     * callback waited forever (#308). PPSSPP's sceIoOpenAsync likewise returns
     * the fd and completes the open on its async I/O thread, notifying the
     * callback then. */
    uint32_t fd = h_IoOpen(s);
    if (fd < sizeof(s_fds) / sizeof(s_fds[0]) && s_fds[fd].used &&
        io_async_submit(fd, IO_ASYNC_OPEN, 0u, 0u, 0u, 0u, 0u) != 0u) {
        /* A descriptor kind the request queue does not hold completes at once. */
        s_fds[fd].async.state = IO_ASYNC_COMPLETE;
        s_fds[fd].async.op = IO_ASYNC_NONE;
        s_fds[fd].async.result = (int64_t)(int32_t)fd;
        s_fds[fd].async_res = (int64_t)(int32_t)fd;
    }
    return fd;
}

static uint32_t h_IoReadAsync(CpuState *s) {
    uint32_t fd = A0;
    if (fd >= sizeof(s_fds) / sizeof(s_fds[0]))
        return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    if (!s_fds[fd].used)
        return s_fds[fd].async.state == IO_ASYNC_PENDING ?
               SCE_ERROR_KERNEL_ASYNC_BUSY : SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    uint32_t raw_error = hle_io_raw_async_fd_boundary(
        fd, IO_RAW_BOUNDARY_READ_ASYNC, "sceIoReadAsync");
    if (raw_error != 0u) return raw_error;
    if (s_fds[fd].kind == FD_KIND_STD) return 0x80010009u;
    if (hle_fd_is_raw_umd(fd)) {
        /* The I/O manager queues the request and its worker runs the device read, so the
         * device's refusals (a zero count, a destination that cannot hold every requested
         * sector, a read past the last sector) come back as the request's result, not from
         * the submission (uOFW src/kd/iofilemgr/iofilemgr.c do_read: async requests are
         * handed to the descriptor's async thread before df_read runs). The count is
         * sectors; the completion checks the whole destination span before any copy. */
        return io_async_submit(fd, IO_ASYNC_READ, A1, A2, 0u, 0u, 0u);
    }
    if (A2 != 0u && !sr_guest_span_writable(A1, A2)) return 0x80010016u;
    return io_async_submit(fd, IO_ASYNC_READ, A1, A2, 0u, 0u, 0u);
}

static uint32_t h_IoWriteAsync(CpuState *s) {
    uint32_t fd = A0;
    if (fd >= sizeof(s_fds) / sizeof(s_fds[0]))
        return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    if (!s_fds[fd].used)
        return s_fds[fd].async.state == IO_ASYNC_PENDING ?
               SCE_ERROR_KERNEL_ASYNC_BUSY : SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    uint32_t raw_error = hle_io_raw_fd_boundary(
        fd, IO_RAW_BOUNDARY_WRITE_ASYNC, "sceIoWriteAsync");
    if (raw_error != 0u) return raw_error;
    if (A2 != 0u && !sr_guest_span_readable(A1, A2)) return 0x80010016u;
    return io_async_submit(fd, IO_ASYNC_WRITE, A1, A2, 0u, 0u, 0u);
}

static uint32_t h_IoLseekAsync(CpuState *s) {
    uint32_t fd = A0;
    if (fd >= sizeof(s_fds) / sizeof(s_fds[0]) || !s_fds[fd].used)
        return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    if (s_fds[fd].async.state == IO_ASYNC_PENDING) return SCE_ERROR_KERNEL_ASYNC_BUSY;
    uint32_t raw_error = hle_io_raw_async_fd_boundary(
        fd, IO_RAW_BOUNDARY_LSEEK64_ASYNC, "sceIoLseekAsync");
    if (raw_error != 0u) return raw_error;
    return io_async_submit(fd, IO_ASYNC_LSEEK64, A2, A3, 0u, stack_arg(s, 0), 0u);
}

static uint32_t h_IoLseek32Async(CpuState *s) {
    return io_async_submit(A0, IO_ASYNC_LSEEK32, A1, A2, 0u, 0u, 0u);
}

static uint32_t h_IoIoctlAsync(CpuState *s) {
    uint32_t fd = A0, in = A2, inlen = A3;
    if (fd >= sizeof(s_fds) / sizeof(s_fds[0]) || !s_fds[fd].used)
        return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    if (s_fds[fd].async.state == IO_ASYNC_PENDING) return SCE_ERROR_KERNEL_ASYNC_BUSY;
    uint32_t raw_error = hle_io_raw_fd_boundary(
        fd, IO_RAW_BOUNDARY_IOCTL_ASYNC, "sceIoIoctlAsync");
    if (raw_error != 0u) return raw_error;
    uint32_t out = stack_arg(s, 0), outlen = stack_arg(s, 1);
    if (inlen != 0u && !sr_guest_span_readable(in, inlen)) return 0x80010016u;
    if (outlen != 0u && !sr_guest_span_writable(out, outlen)) return 0x80010016u;
    return io_async_submit(fd, IO_ASYNC_IOCTL, A1, in, inlen, out, outlen);
}

static uint32_t io_async_query(uint32_t fd, uint32_t result_addr, int wait,
                               IoRawDeviceBoundary raw_boundary, const char *api) {
    uint32_t raw_error = hle_io_raw_async_fd_boundary(fd, raw_boundary, api);
    if (raw_error != 0u) return raw_error;
    if (fd >= sizeof(s_fds) / sizeof(s_fds[0]))
        return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    if (result_addr != 0u && !sr_guest_span_writable(result_addr, 8u))
        return 0x80010016u;
    if (!s_fds[fd].used && s_closed_async_state[fd] == IO_ASYNC_IDLE)
        return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    IoAsyncState *state = (s_fds[fd].used ||
                           s_fds[fd].async.state != IO_ASYNC_IDLE) ?
                          &s_fds[fd].async.state : &s_closed_async_state[fd];
    if (*state == IO_ASYNC_IDLE) return SCE_ERROR_KERNEL_NO_ASYNC;
    if (*state == IO_ASYNC_PENDING) {
        if (!wait) return 1u;
        uint32_t rc = io_async_complete_fd(fd);
        if (rc != 0u) return rc;
        state = (s_fds[fd].used || s_fds[fd].async.state != IO_ASYNC_IDLE) ?
                &s_fds[fd].async.state : &s_closed_async_state[fd];
    }
    int64_t result = (s_fds[fd].used || s_fds[fd].async.state != IO_ASYNC_IDLE) ?
                     s_fds[fd].async.result : s_closed_res[fd];
    if (result_addr != 0u) {
        MEM_W32(result_addr, (uint32_t)result);
        MEM_W32(result_addr + 4u, (uint32_t)((uint64_t)result >> 32));
    }
    if (!s_fds[fd].used) {
        s_closed_async_state[fd] = IO_ASYNC_IDLE;
        s_closed_res[fd] = 0;
    }
    return 0u;
}

static uint32_t h_IoWaitAsync(CpuState *s) {
    if (hle_log_on())
        fprintf(stderr, "HLE: IoWaitAsync fd=0x%x (from 0x%x)\n", A0, sched_current_uid());
    return io_async_query(A0, A1, 1, IO_RAW_BOUNDARY_ASYNC_STATUS, "sceIoWaitAsync");
}

static uint32_t h_IoPollAsync(CpuState *s) {
    return io_async_query(A0, A1, 0, IO_RAW_BOUNDARY_ASYNC_STATUS, "sceIoPollAsync");
}

static uint32_t h_IoGetAsyncStat(CpuState *s) {
    return io_async_query(A0, A2, A1 == 0u, IO_RAW_BOUNDARY_ASYNC_STATUS,
                          "sceIoGetAsyncStat");
}

static uint32_t h_IoWaitAsyncCB(CpuState *s) {
    uint32_t rc = io_async_query(A0, A1, 1, IO_RAW_BOUNDARY_ASYNC_STATUS,
                                 "sceIoWaitAsyncCB");
    if (rc == 0u && sr_thread_has_pending_callbacks(sched_current_uid()))
        sr_thread_dispatch_callbacks();
    return rc;
}

static uint32_t h_IoChangeAsyncPriority(CpuState *s) {
    uint32_t fd = A0;
    if (fd >= sizeof(s_fds) / sizeof(s_fds[0]) || !s_fds[fd].used)
        return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    uint32_t raw_error = hle_io_raw_async_fd_boundary(
        fd, IO_RAW_BOUNDARY_ASYNC_PRIORITY, "sceIoChangeAsyncPriority");
    if (raw_error != 0u) return raw_error;
    s_fds[fd].async.priority = (int32_t)A1;
    return 0u;
}

static uint32_t h_IoSetAsyncCallback(CpuState *s) {
    uint32_t fd = A0, callback = A1;
    if (fd >= sizeof(s_fds) / sizeof(s_fds[0]) || !s_fds[fd].used)
        return SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR;
    uint32_t raw_error = hle_io_raw_async_fd_boundary(
        fd, IO_RAW_BOUNDARY_ASYNC_CALLBACK, "sceIoSetAsyncCallback");
    if (raw_error != 0u) return raw_error;
    if (callback != 0u && !sr_callback_is_valid(callback)) return 0x800201a1u;
    s_fds[fd].async.callback_uid = callback;
    s_fds[fd].async.callback_arg = A2;
    return 0u;
}

static uint32_t h_IoCloseAsync(CpuState *s) {
    return io_async_submit(A0, IO_ASYNC_CLOSE, 0u, 0u, 0u, 0u, 0u);
}

static uint32_t h_IoSync(CpuState *s) {
    char device[64];
    if (!guest_cstr(A0, device, sizeof(device))) return 0x80010016u;
    if (A1 != 0u) {
        fprintf(stderr,
                "SEMANTIC_BOUNDARY: IO_SYNC_ARGUMENT: nonzero sceIoSync unk "
                "values are not supported yet (0x80010086)\n");
        return 0x80010086u;
    }
    size_t device_len = strlen(device);
    if (!((device_len == 4u && sr_vfs_strnicmp(device, "ms0:", 4u) == 0) ||
          (device_len == 7u && sr_vfs_strnicmp(device, "fatms0:", 7u) == 0) ||
          (device_len == 7u && sr_vfs_strnicmp(device, "msfat0:", 7u) == 0))) {
        fprintf(stderr,
                "SEMANTIC_BOUNDARY: IO_SYNC_DEVICE: sceIoSync supports the Memory Stick "
                "device only; this device is not supported yet (0x80010086)\n");
        return 0x80010086u;
    }
    for (uint32_t fd = 3u; fd < sizeof(s_fds) / sizeof(s_fds[0]); fd++) {
        Fd *file = &s_fds[fd];
        if (file->host && file->async.state == IO_ASYNC_PENDING)
            (void)io_async_complete_fd(fd);
    }
    for (size_t fd = 3u; fd < sizeof(s_fds) / sizeof(s_fds[0]); fd++) {
        Fd *file = &s_fds[fd];
        if (!file->used || !file->host || !file->writable) continue;
        if (fflush(file->host) != 0) return 0x80010005u;
#ifdef _WIN32
        if (_commit(_fileno(file->host)) != 0) return 0x80010005u;
#else
        if (fsync(fileno(file->host)) != 0) return 0x80010005u;
#endif
    }
    return 0u;
}
/* Parent directory of a host path (last separator). Returns 0 if no parent. */
#ifdef _WIN32
static int ms0_parent_path(const char *host_path, char *out, size_t cap) {
    if (!host_path || !out || cap == 0) return 0;
    const char *slash = strrchr(host_path, '\\');
    const char *slash_fwd = strrchr(host_path, '/');
    if (slash_fwd && (!slash || slash_fwd > slash)) slash = slash_fwd;
    if (!slash) return 0;
    size_t n = (size_t)(slash - host_path);
    if (n == 0 || n + 1u > cap) return 0;
    memcpy(out, host_path, n);
    out[n] = '\0';
    return 1;
}
#endif /* _WIN32 */

/* Parent must exist, be a directory, and stay inside the canonical ms root.
 * Missing parent -> not-found; non-directory parent -> is-a-directory;
 * outside root -> illegal path. Residual TOCTOU: containment is a by-path
 * check immediately before the subsequent Create/Move/Delete. */
#ifdef _WIN32
static uint32_t ms0_parent_contained_check(const char *hp, const wchar_t *canonical) {
    char parent[512];
    if (!ms0_parent_path(hp, parent, sizeof(parent))) return 0x80010005u;
    wchar_t *wp = NULL;
    if (!sr_wide_path_alloc(parent, &wp)) return 0x80010005u;
    DWORD attrs = GetFileAttributesW(wp);
    free(wp);
    if (attrs == INVALID_FILE_ATTRIBUTES) return 0x80010002u;
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return 0x80010015u;
    if (!sr_vfs_dir_is_contained(parent, canonical)) return 0x80010016u;
    return 0;
}
#endif /* _WIN32 */

/* sceIoRename(oldname, newname). Both names resolve beneath the unified
 * Memory Stick root, never the disc. POSIX uses the descriptor-relative
 * renameat extension in vfs_contained.h; Windows keeps its verified-root
 * checks and MoveFileEx route below. Foreign devices and traversal fail closed
 * with EINVAL (0x80010016). */
#ifdef _WIN32
static uint32_t h_IoRename(CpuState *s) {
    char oldpath[256], newpath[256];
    if (io_guest_path(A0, oldpath, sizeof(oldpath)) != 0u ||
        io_guest_path(A1, newpath, sizeof(newpath)) != 0u)
        return 0x80010016u;
    if (sr_flash0_font_is_device_path(oldpath) || sr_flash0_font_is_device_path(newpath))
        return sr_flash0_font_refuse_write(sr_flash0_font_is_device_path(oldpath) ? oldpath : newpath,
                                           "rename");
    char *old_hp = host_path_alloc(oldpath);
    char *new_hp = host_path_alloc(newpath);
    if (!old_hp || !new_hp) {
        free(old_hp);
        free(new_hp);
        return 0x80010016u;
    }
    wchar_t canonical[MAX_PATH * 2];
    if (!sr_vfs_canonical_root(sr_ms0_root(), canonical,
                               sizeof(canonical) / sizeof(wchar_t))) {
        free(old_hp);
        free(new_hp);
        return 0x80010005u;
    }
    uint32_t rc = ms0_parent_contained_check(old_hp, canonical);
    if (rc == 0) rc = ms0_parent_contained_check(new_hp, canonical);
    if (rc != 0) {
        free(old_hp);
        free(new_hp);
        return rc;
    }
    wchar_t *w_new = NULL;
    if (sr_wide_path_alloc(new_hp, &w_new)) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(w_new, GetFileExInfoStandard, &fad)) {
            if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                free(w_new);
                free(old_hp);
                free(new_hp);
                return 0x80010015u;
            }
            int was_dir = 0;
            if (!sr_vfs_delete_contained_leaf(new_hp, canonical, &was_dir)) {
                free(w_new);
                free(old_hp);
                free(new_hp);
                return 0x80010005u;
            }
        }
        free(w_new);
    }
    wchar_t *w_old = NULL;
    w_new = NULL;
    if (!sr_wide_path_alloc(old_hp, &w_old) || !sr_wide_path_alloc(new_hp, &w_new)) {
        free(w_old);
        free(w_new);
        free(old_hp);
        free(new_hp);
        return 0x80010005u;
    }
    BOOL ok = MoveFileExW(w_old, w_new, MOVEFILE_COPY_ALLOWED);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    free(w_old);
    free(w_new);
    free(old_hp);
    free(new_hp);
    if (!ok) {
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            return 0x80010002u;
        return 0x80010005u;
    }
    return 0;
}
#endif /* _WIN32 */

/* sceIoMkdir(path): create the final component under the unified Memory Stick
 * root. Foreign/traversal -> EINVAL; missing parent -> ENOENT; already exists
 * -> EEXIST; CreateDirectoryW failure -> EIO. Intermediate components are not
 * created (PSP mkdir does not create parents). */
#ifdef _WIN32
static uint32_t h_IoMkdir(CpuState *s) {
    char path[256];
    if (io_guest_path(A0, path, sizeof(path)) != 0u)
        return 0x80010016u;
    if (sr_flash0_font_is_device_path(path)) return sr_flash0_font_refuse_write(path, "mkdir");
    char *hp = host_path_alloc(path);
    if (!hp) return 0x80010016u;
    wchar_t canonical[MAX_PATH * 2];
    if (!sr_vfs_canonical_root(sr_ms0_root(), canonical,
                               sizeof(canonical) / sizeof(wchar_t))) {
        free(hp);
        return 0x80010005u;
    }
    uint32_t prc = ms0_parent_contained_check(hp, canonical);
    if (prc != 0) {
        free(hp);
        return prc;
    }
    wchar_t *wh = NULL;
    if (!sr_wide_path_alloc(hp, &wh)) {
        free(hp);
        return 0x80010005u;
    }
    DWORD attrs = GetFileAttributesW(wh);
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        free(wh);
        free(hp);
        return 0x80010011u; /* EEXIST */
    }
    BOOL made = CreateDirectoryW(wh, NULL);
    DWORD err = made ? ERROR_SUCCESS : GetLastError();
    free(wh);
    free(hp);
    if (made) return 0;
    if (err == ERROR_ALREADY_EXISTS) return 0x80010011u;
    return 0x80010005u;
}
#endif /* _WIN32 */

#ifndef _WIN32
static uint32_t h_IoRename(CpuState *s) {
    char old_guest[256], new_guest[256];
    char old_rel[SR_CD_REL_MAX], new_rel[SR_CD_REL_MAX];
    if (io_guest_path(A0, old_guest, sizeof(old_guest)) != 0u ||
        io_guest_path(A1, new_guest, sizeof(new_guest)) != 0u)
        return ms0_posix_psp_error(SR_CD_INVALID_PATH);
    if (sr_flash0_font_is_device_path(old_guest) || sr_flash0_font_is_device_path(new_guest))
        return sr_flash0_font_refuse_write(
            sr_flash0_font_is_device_path(old_guest) ? old_guest : new_guest, "rename");
    sr_cd_status st = ms0_posix_guest_relpath(old_guest, old_rel, sizeof(old_rel));
    if (st == SR_CD_OK)
        st = ms0_posix_guest_relpath(new_guest, new_rel, sizeof(new_rel));
    if (st != SR_CD_OK) return ms0_posix_psp_error(st);

    sr_cd_root root;
    st = ms0_posix_root_open(&root);
    if (st == SR_CD_OK) {
        st = sr_cd_rename_file(&root, old_rel, new_rel);
        sr_cd_root_close(&root);
    }
    return ms0_posix_psp_error(st);
}

static uint32_t h_IoMkdir(CpuState *s) {
    char guest[256], rel[SR_CD_REL_MAX];
    if (io_guest_path(A0, guest, sizeof(guest)) != 0u)
        return ms0_posix_psp_error(SR_CD_INVALID_PATH);
    if (sr_flash0_font_is_device_path(guest)) return sr_flash0_font_refuse_write(guest, "mkdir");
    sr_cd_status st = ms0_posix_guest_relpath(guest, rel, sizeof(rel));
    if (st != SR_CD_OK) return ms0_posix_psp_error(st);

    sr_cd_root root;
    st = ms0_posix_root_open(&root);
    if (st == SR_CD_OK) {
        st = sr_cd_mkdir_leaf(&root, rel);
        sr_cd_root_close(&root);
    }
    return ms0_posix_psp_error(st);
}
#endif /* !_WIN32 */

/* sceIoRemove(path): delete one regular file under the unified Memory Stick
 * root through the contained-delete seam. Foreign/traversal -> EINVAL;
 * missing -> ENOENT; directory -> EISDIR; delete failure -> EIO. */
#ifdef _WIN32
static uint32_t h_IoRemove(CpuState *s) {
    char path[256];
    if (io_guest_path(A0, path, sizeof(path)) != 0u)
        return 0x80010016u;
    if (sr_flash0_font_is_device_path(path)) return sr_flash0_font_refuse_write(path, "remove");
    char *hp = host_path_alloc(path);
    if (!hp) return 0x80010016u;
    wchar_t canonical[MAX_PATH * 2];
    if (!sr_vfs_canonical_root(sr_ms0_root(), canonical,
                               sizeof(canonical) / sizeof(wchar_t))) {
        free(hp);
        return 0x80010005u;
    }
    wchar_t *wh = NULL;
    if (!sr_wide_path_alloc(hp, &wh)) {
        free(hp);
        return 0x80010005u;
    }
    DWORD attrs = GetFileAttributesW(wh);
    free(wh);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        free(hp);
        return 0x80010002u;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        free(hp);
        return 0x80010015u;
    }
    int was_dir = 0;
    int ok = sr_vfs_delete_contained_leaf(hp, canonical, &was_dir);
    free(hp);
    return ok ? 0 : 0x80010005u;
}
#endif /* _WIN32 */

#ifndef _WIN32
static uint32_t h_IoRemove(CpuState *s) {
    char guest[256], rel[SR_CD_REL_MAX];
    if (io_guest_path(A0, guest, sizeof(guest)) != 0u)
        return ms0_posix_psp_error(SR_CD_INVALID_PATH);
    if (sr_flash0_font_is_device_path(guest)) return sr_flash0_font_refuse_write(guest, "remove");
    sr_cd_status st = ms0_posix_guest_relpath(guest, rel, sizeof(rel));
    if (st != SR_CD_OK) return ms0_posix_psp_error(st);

    sr_cd_root root;
    st = ms0_posix_root_open(&root);
    if (st == SR_CD_OK) {
        st = sr_cd_delete_file(&root, rel);
        sr_cd_root_close(&root);
    }
    return ms0_posix_psp_error(st);
}
#endif /* !_WIN32 */

static uint32_t h_IoRmdir(CpuState *s) {
    char guest[512], rel[SR_CD_REL_MAX];
    uint32_t rc = io_guest_path(A0, guest, sizeof(guest));
    if (rc != 0u) return rc;
    if (sr_flash0_font_is_device_path(guest)) return sr_flash0_font_refuse_write(guest, "rmdir");
    sr_cd_status status = sr_cd_ms0_guest_relpath(guest, rel, sizeof(rel));
    if (status != SR_CD_OK) return sr_cd_psp_error(status);
    sr_cd_root root;
    status = sr_cd_root_open(sr_ms0_root(), &root);
    if (status == SR_CD_OK) {
        status = sr_cd_remove_dir_empty(&root, rel);
        sr_cd_root_close(&root);
    }
    return sr_cd_psp_error(status);
}

static uint32_t h_IoChdir(CpuState *s) {
    char path[512];
    uint32_t rc = io_guest_path(A0, path, sizeof(path));
    if (rc != 0u) return rc;
    uint32_t dirfd = h_io_dopen_path(path);
    if (!hle_fd_is_directory(dirfd))
        return dirfd;
    CpuState close_cpu;
    memset(&close_cpu, 0, sizeof(close_cpu));
    close_cpu.r[4] = dirfd;
    rc = h_IoDclose(&close_cpu);
    if (rc != 0u) return rc;
    IoCwd *cwd = io_cwd_find(sched_current_uid(), 1);
    if (!cwd) return 0x8001000cu;
    size_t len = strlen(path);
    if (len >= sizeof(cwd->path)) return 0x80010024u;
    memcpy(cwd->path, path, len + 1u);
    return 0u;
}

#define IO_CSTAT_MODE 0x00000001u
#define IO_MODE_TYPE_MASK 0x0000f000u
#define IO_MODE_REGULAR 0x00002000u
#define IO_MODE_DIRECTORY 0x00001000u

static uint32_t h_IoChstat(CpuState *s) {
    char guest[512], rel[SR_CD_REL_MAX];
    uint32_t rc = io_guest_path(A0, guest, sizeof(guest));
    if (rc != 0u) return rc;
    if (sr_flash0_font_is_device_path(guest))
        return sr_flash0_font_refuse_write(guest, "change attributes");
    uint32_t bits = A2;
    if ((bits & ~IO_CSTAT_MODE) != 0u) {
        fprintf(stderr,
                "SEMANTIC_BOUNDARY: IO_CHSTAT_NON_MODE_FIELDS: sceIoChstat fields "
                "other than mode are not supported yet (0x80010086)\n");
        return 0x80010086u;
    }
    if (bits == 0u) return 0u;
    if (!A1 || !sr_guest_span_readable(A1, 4u)) return 0x80010016u;
    uint32_t requested_mode = MEM_R32(A1);
    uint32_t type = requested_mode & IO_MODE_TYPE_MASK;
    if (type != IO_MODE_REGULAR && type != IO_MODE_DIRECTORY)
        return 0x80010016u;
    uint32_t lba, size;
    if (iso_lookup(guest, &lba, &size) == 0) return 0x8001001eu;
    sr_cd_status status = sr_cd_ms0_guest_relpath(guest, rel, sizeof(rel));
    if (status != SR_CD_OK) return sr_cd_psp_error(status);
    sr_cd_root root;
    status = sr_cd_root_open(sr_ms0_root(), &root);
    if (status == SR_CD_OK) {
        status = sr_cd_set_mode(&root, rel, requested_mode);
        sr_cd_root_close(&root);
    }
    return sr_cd_psp_error(status);
}

#ifdef SR_HLE_THREAD_SELFTEST
/* The focused native HLE harness exposes the small IoFileMgr slice under its
 * production handlers without widening the ThreadMan-only registry used by
 * that executable.  The wrappers and identity probe are test-build-only. */
void hle_register_iofilemgr_handlers(void);
uint32_t sr_hle_test_io_open(CpuState *s) { return h_IoOpen(s); }
uint32_t sr_hle_test_io_read(CpuState *s) { return h_IoRead(s); }
uint32_t sr_hle_test_io_write(CpuState *s) { return h_IoWrite(s); }
uint32_t sr_hle_test_io_lseek(CpuState *s) { return h_IoLseek(s); }
uint32_t sr_hle_test_io_lseek32(CpuState *s) { return h_IoLseek32(s); }
uint32_t sr_hle_test_io_dopen(CpuState *s) { return h_IoDopen(s); }
uint32_t sr_hle_test_io_dread(CpuState *s) { return h_IoDread(s); }
uint32_t sr_hle_test_io_dclose(CpuState *s) { return h_IoDclose(s); }
#ifdef _WIN32 /* the classifier maps FindFirstFileW's error codes and exists on Win32 only */
uint32_t sr_hle_test_vfs_initial_find_error(unsigned long error, int *found) {
    return vfs_overlay_initial_find_error(error, found);
}
#endif
uint32_t sr_hle_test_io_ioctl(CpuState *s) { return h_IoIoctl(s); }
void sr_hle_test_register_iofilemgr(void) { hle_register_iofilemgr_handlers(); }
uint32_t sr_hle_test_io_devctl_refusal_log_count(void) {
    return atomic_load_explicit(&s_hle_devctl_refusal_log_count,
                                memory_order_acquire);
}
uint32_t sr_hle_test_io_close(CpuState *s) { return h_IoClose(s); }
uint32_t sr_hle_test_io_open_async(CpuState *s) { return h_IoOpenAsync(s); }
uint32_t sr_hle_test_io_close_async(CpuState *s) { return h_IoCloseAsync(s); }
uint32_t sr_hle_test_io_write_async(CpuState *s) { return h_IoWriteAsync(s); }
uint32_t sr_hle_test_io_read_async(CpuState *s) { return h_IoReadAsync(s); }
uint32_t sr_hle_test_io_lseek_async(CpuState *s) { return h_IoLseekAsync(s); }
uint32_t sr_hle_test_io_lseek32_async(CpuState *s) { return h_IoLseek32Async(s); }
uint32_t sr_hle_test_io_ioctl_async(CpuState *s) { return h_IoIoctlAsync(s); }
uint32_t sr_hle_test_io_wait_async(CpuState *s) { return h_IoWaitAsync(s); }
uint32_t sr_hle_test_io_poll_async(CpuState *s) { return h_IoPollAsync(s); }
uint32_t sr_hle_test_io_get_async_stat(CpuState *s) { return h_IoGetAsyncStat(s); }
uint32_t sr_hle_test_io_chdir(CpuState *s) { return h_IoChdir(s); }
uint32_t sr_hle_test_io_rmdir(CpuState *s) { return h_IoRmdir(s); }
uint32_t sr_hle_test_io_chstat(CpuState *s) { return h_IoChstat(s); }
uint32_t sr_hle_test_io_sync(CpuState *s) { return h_IoSync(s); }
uint32_t sr_hle_test_io_change_async_priority(CpuState *s) {
    return h_IoChangeAsyncPriority(s);
}
uint32_t sr_hle_test_io_set_async_callback(CpuState *s) {
    return h_IoSetAsyncCallback(s);
}
uint32_t sr_hle_test_io_rename(CpuState *s) { return h_IoRename(s); }
uint32_t sr_hle_test_io_mkdir(CpuState *s) { return h_IoMkdir(s); }
uint32_t sr_hle_test_io_remove(CpuState *s) { return h_IoRemove(s); }
#endif
static int data_stat_write(uint32_t st, uint64_t size) {
    if (!st || size > UINT32_MAX) return 0;
    for (int i = 0; i < 0x58; i++) MEM_W8(st + (uint32_t)i, 0);
    MEM_W32(st + 0, 0x2000u | 0x0124u);
    MEM_W32(st + 4, 0x0004u | 0x0001u);
    MEM_W32(st + 8, (uint32_t)size);
    MEM_W32(st + 0x40, 0);
    return 1;
}

/* sceIoGetstat for flash0: (read-only font device). A served font reports its size as a
 * regular file; flash0:/font reports as a directory. */
static uint32_t h_io_getstat_flash0(const char *path, uint32_t st) {
    Flash0Sources sources;
    uint32_t font_size = 0;
    int font_is_dir = 0;
    flash0_font_sources(&sources);
    uint32_t rc = sr_flash0_font_stat(path, &sources, &font_size, &font_is_dir);
    if (rc != 0u) return rc;
    if (font_is_dir) {
        for (int i = 0; i < 0x58; i++) MEM_W8(st + (uint32_t)i, 0);
        MEM_W32(st + 0, 0x1000u | 0x0124u);
        MEM_W32(st + 4, 0x0010u);
        MEM_W32(st + 0x40, 0);
        return 0u;
    }
    return data_stat_write(st, font_size) ? 0u : 0x80010005u;
}

static uint32_t h_IoGetstat(CpuState *s) {
    /* a0=path, a1=SceIoStat*. SceIoStat layout: mode(+0), attr(+4), size(+8,64-bit),
     * ctime(+0x10), atime(+0x20), mtime(+0x30), st_private[6](+0x40). For UMD files PPSSPP
     * fills st_private[0] (+0x40) with the file's starting LBN; the game reads that to build
     * a raw "sce_lbn0x<LBN>" path. Omitting it made the game read garbage and fetch the wrong
     * sector (e.g. REGFILE.CDI at LBN 0x5f20 was read as 0x80). */
    char path[256];
    if (io_guest_path(A0, path, sizeof(path)) != 0u)
        return 0x80010016u;
    if (io_raw_device_path(path) != IO_RAW_DEVICE_NONE)
        return hle_io_raw_device_boundary(IO_RAW_BOUNDARY_GETSTAT,
                                          "sceIoGetstat", path, UINT32_MAX);
    if (sr_flash0_font_is_device_path(path)) return h_io_getstat_flash0(path, A1);
    uint32_t lba, size, st = A1;
    if (iso_lookup(path, &lba, &size) != 0) {
        /* Host-backed path under the unified Memory Stick root (ordinary ms0:
         * sceIoGetstat). Foreign devices fail closed here and fall through. */
        char *hp = host_path_alloc(path);
        if (hp) {
#ifdef _WIN32
            wchar_t *wh = NULL;
            if (sr_wide_path_alloc(hp, &wh)) {
                WIN32_FILE_ATTRIBUTE_DATA fad;
                if (GetFileAttributesExW(wh, GetFileExInfoStandard, &fad)) {
                    free(wh);
                    free(hp);
                    int is_dir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    uint64_t fsize = ((uint64_t)fad.nFileSizeHigh << 32) |
                                     (uint64_t)fad.nFileSizeLow;
                    if (getenv("SR_STATLOG"))
                        fprintf(stderr, "Getstat(%s) -> ms0 host size=0x%llx dir=%d\n",
                                path, (unsigned long long)fsize, is_dir);
                    for (int i = 0; i < 0x58; i++) MEM_W8(st + (uint32_t)i, 0);
                    MEM_W32(st + 0, is_dir ? (0x1000u | 0x0124u) : (0x2000u | 0x0124u));
                    MEM_W32(st + 4, is_dir ? 0x0010u : (0x0004u | 0x0001u));
                    if (!is_dir) {
                        MEM_W32(st + 8, (uint32_t)fsize);
                        MEM_W32(st + 12, (uint32_t)(fsize >> 32));
                    }
                    MEM_W32(st + 0x40, 0); /* st_private[0]: no LBN (host-backed) */
                    return 0;
                }
                free(wh);
            }
#else
            /* lstat, not stat: a symlink is this host's redirect and must be
             * reported as the link itself, exactly as the Windows leg reports
             * a reparse point rather than its target. */
            struct stat fad;
            if (lstat(hp, &fad) == 0) {
                free(hp);
                int is_dir = S_ISDIR(fad.st_mode);
                uint64_t fsize = (uint64_t)S_ISREG(fad.st_mode) ? (uint64_t)fad.st_size : 0u;
                if (getenv("SR_STATLOG"))
                    fprintf(stderr, "Getstat(%s) -> ms0 host size=0x%llx dir=%d\n",
                            path, (unsigned long long)fsize, is_dir);
                for (int i = 0; i < 0x58; i++) MEM_W8(st + (uint32_t)i, 0);
                MEM_W32(st + 0, is_dir ? (0x1000u | 0x0124u) : (0x2000u | 0x0124u));
                MEM_W32(st + 4, is_dir ? 0x0010u : (0x0004u | 0x0001u));
                if (!is_dir) {
                    MEM_W32(st + 8, (uint32_t)fsize);
                    MEM_W32(st + 12, (uint32_t)(fsize >> 32));
                }
                MEM_W32(st + 0x40, 0); /* st_private[0]: no LBN (host-backed) */
                return 0;
            }
#endif
            free(hp);
        }
        if (s_data_archive_mode && data_archive_guest_path_allowed(path)) {
            int wanted_variant = -2;
            char *key = data_normalize_guest_key(path, &wanted_variant);
            if (key) {
                FILE *loose = NULL;
                uint32_t loose_size = 0;
                int loose_result = data_loose_file_open(key, &loose, &loose_size);
                if (loose_result < 0) {
                    free(key);
                    return 0x80010005u;
                }
                if (loose_result > 0) {
                    if (loose) fclose(loose);
                    free(key);
                    return data_stat_write(st, loose_size) ? 0u : 0x80010005u;
                }
                free(key);
            }
        }
        /* Not on the ISO -- try the extracted-XB data-root (graphical/text data the game
         * expects to read from paths like "data/menu/text/<X>.to", which are packed inside
         * XB archives on the real ISO; dev workflow extracts them onto a host tree). */
        const SrAssetIndexEntry *entry = host_data_lookup(path);
        if (entry) {
            FILE *probe = sr_fopen_utf8(entry->host, L"rb");
            uint32_t actual_size = 0;
            if (!probe || !sr_stream_size_u32(probe, &actual_size)) {
                if (probe) fclose(probe);
                fprintf(stderr, "host_data: indexed asset stat failed for %s (errno=%d)\n",
                        path, errno);
                return 0x80010002;
            }
            fclose(probe);
            if (getenv("SR_STATLOG"))
                fprintf(stderr, "Getstat(%s) -> data-root %s size=0x%x\n", path,
                        entry->host, (unsigned)actual_size);
            for (int i = 0; i < 0x58; i++) MEM_W8(st + (uint32_t)i, 0);
            MEM_W32(st + 0,  0x2000 | 0x0124);   /* mode: regular file, r-x */
            MEM_W32(st + 4,  0x0004 | 0x0001);   /* attr: file */
            MEM_W32(st + 8,  actual_size); /* size low (SceOff is 64-bit at +8) */
            MEM_W32(st + 0x40, 0);                /* st_private[0]: no LBN (host-backed) */
            return 0;
        }
        if (s_data_archive_mode && data_archive_guest_path_allowed(path)) {
            int wanted_variant = -2;
            char *key = data_normalize_guest_key(path, &wanted_variant);
            if (key) {
                SrArchiveFile archive_file;
                if (sr_archive_vfs_lookup(&s_archive_vfs, key, wanted_variant,
                                          &archive_file)) {
                    free(key);
                    return data_stat_write(st, archive_file.size) ? 0u : 0x80010005u;
                }
                free(key);
            }
        }
        if (getenv("SR_STATLOG")) fprintf(stderr, "Getstat(%s) -> NOT FOUND\n", path);
        return 0x80010002;
    }
    if (getenv("SR_STATLOG")) fprintf(stderr, "Getstat(%s) -> lba=0x%x size=0x%x\n", path, lba, size);
    for (int i = 0; i < 0x58; i++) MEM_W8(st + (uint32_t)i, 0);
    MEM_W32(st + 0, 0x2000 | 0x0124);          /* mode: regular file, r-x */
    MEM_W32(st + 4, 0x0004 | 0x0001);          /* attr: file */
    MEM_W32(st + 8, size);                      /* size low (SceOff is 64-bit at +8) */
    MEM_W32(st + 0x40, iso_physical_lba(lba));  /* st_private[0]: physical UMD start LBN */
    return 0;
}

#ifdef SR_HLE_THREAD_SELFTEST
uint32_t sr_hle_test_io_getstat(CpuState *s) { return h_IoGetstat(s); }
#endif

static int s_iofilemgr_handlers_registered;

void hle_register_iofilemgr_handlers(void) {
    if (s_iofilemgr_handlers_registered) return;
    s_iofilemgr_handlers_registered = 1;
    sr_hle_register(0x109f50bc, "sceIoOpen", h_IoOpen);
    sr_hle_register(0x779103a0, "sceIoRename", h_IoRename);
    sr_hle_register(0x68963324, "sceIoLseek32", h_IoLseek32);
    sr_hle_register(0x27eb27b8, "sceIoLseek", h_IoLseek);
    sr_hle_register(0x63632449, "sceIoIoctl", h_IoIoctl);
    sr_hle_register(0x810c4bc3, "sceIoClose", h_IoClose);
    sr_hle_register(0xace946e8, "sceIoGetstat", h_IoGetstat);
    sr_hle_register(0xb29ddf9c, "sceIoDopen", h_IoDopen);
    sr_hle_register(0xe3eb004c, "sceIoDread", h_IoDread);
    sr_hle_register(0xeb092469, "sceIoDclose", h_IoDclose);
    sr_hle_register(0x89aa9906, "sceIoOpenAsync", h_IoOpenAsync);
    sr_hle_register(0xa0b5a7c2, "sceIoReadAsync", h_IoReadAsync);
    sr_hle_register(0x71b19e77, "sceIoLseekAsync", h_IoLseekAsync);
    sr_hle_register(0x3251ea56, "sceIoPollAsync", h_IoPollAsync);
    sr_hle_register(0xff5940b6, "sceIoCloseAsync", h_IoCloseAsync);
    sr_hle_register(0x0facab19, "sceIoWriteAsync", h_IoWriteAsync);
    sr_hle_register(0x1117c65f, "sceIoRmdir", h_IoRmdir);
    sr_hle_register(0x55f4717d, "sceIoChdir", h_IoChdir);
    sr_hle_register(0xb293727f, "sceIoChangeAsyncPriority", h_IoChangeAsyncPriority);
    sr_hle_register(0x1b385d8f, "sceIoLseek32Async", h_IoLseek32Async);
    sr_hle_register(0xab96437f, "sceIoSync", h_IoSync);
    sr_hle_register(0xb8a740f4, "sceIoChstat", h_IoChstat);
    sr_hle_register(0xe95a012b, "sceIoIoctlAsync", h_IoIoctlAsync);
    sr_hle_register(0xcb05f8d6, "sceIoGetAsyncStat", h_IoGetAsyncStat);
    sr_hle_register(0xa12a0514, "sceIoSetAsyncCallback", h_IoSetAsyncCallback);
    sr_hle_register(0x54f5fb11, "sceIoDevctl", h_IoDevctl);
    sr_hle_register(0x06a70004, "sceIoMkdir", h_IoMkdir);
    sr_hle_register(0xf27a9c51, "sceIoRemove", h_IoRemove);
}

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

/* From hle_register_wait_conformance_handlers(). */
void hle_register_wait_conformance_io(void) {
    sr_hle_register(0x6a638d83, "sceIoRead", h_IoRead);
    sr_hle_register(0x42ec03ac, "sceIoWrite", h_IoWrite);
    sr_hle_register(0xe23eec33, "sceIoWaitAsync", h_IoWaitAsync);
    sr_hle_register(0x35dbd746, "sceIoWaitAsyncCB", h_IoWaitAsyncCB);
}
