/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#if !defined(_WIN32) && !defined(_WIN64)

/* realpath() is XSI rather than base POSIX: glibc guards its declaration on
   __USE_MISC || __USE_XOPEN_EXTENDED, so _POSIX_C_SOURCE 200809L alone leaves it
   undeclared and the call below compiles to an implicit int -- which -Werror
   turns into a build failure on every POSIX host while the Win32 backend builds
   clean. _XOPEN_SOURCE 700 is the portable spelling and implies POSIX.1-2008;
   glibc's _DEFAULT_SOURCE would also expose it but does not carry to musl or
   the BSDs. */
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "nk_platform.h"
#include "nk_types.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

bool nk_platform_absolute_path(const char *path, char *out_path, size_t max_len) {
    if (!path || !*path || !out_path || max_len == 0) return false;

    char *resolved = realpath(path, NULL);
    if (!resolved) return false;

    size_t length = strlen(resolved);
    if (length >= max_len) {
        free(resolved);
        return false;
    }
    memcpy(out_path, resolved, length + 1);
    free(resolved);
    return true;
}

int nk_fseek64(FILE *f, int64_t offset, int whence) {
    return fseeko(f, (off_t)offset, whence);
}

int64_t nk_ftell64(FILE *f) {
    return (int64_t)ftello(f);
}

char nk_platform_path_separator(void) {
    return '/';
}

/* POSIX file names are byte strings and this project's paths are UTF-8, so the
 * narrow CRT calls already carry the exact name the wide Win32 path would
 * have produced. These exist so host code never has to know that. */
FILE *nk_fopen_utf8(const char *path, const char *mode) {
    if (!path || !mode) return NULL;
    return fopen(path, mode);
}

int nk_remove_utf8(const char *path) {
    if (!path || !*path) return -1;
    return unlink(path);
}

int nk_rename_utf8(const char *from, const char *to) {
    if (!from || !*from || !to || !*to) return -1;
    return rename(from, to);
}

bool nk_platform_file_exists(const char *path) {
    if (!path || !*path) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISREG(st.st_mode);
}

bool nk_platform_dir_exists(const char *path) {
    if (!path || !*path) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

bool nk_platform_path_missing(const char *path) {
    struct stat st;
    if (!path || !*path) return false;
    if (lstat(path, &st) == 0) return false;
    return errno == ENOENT || errno == ENOTDIR;
}

static bool tree_size_posix(const char *path, uint64_t *bytes) {
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT || errno == ENOTDIR;
    if (S_ISLNK(st.st_mode)) return true;
    if (!S_ISDIR(st.st_mode)) {
        if (S_ISREG(st.st_mode)) *bytes += (uint64_t)st.st_size;
        return true;
    }
    DIR *dir = opendir(path);
    if (!dir) return false;
    struct dirent *ent;
    bool ok = true;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[4096];
        int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(child) || !tree_size_posix(child, bytes)) {
            ok = false;
            break;
        }
    }
    closedir(dir);
    return ok;
}

static bool remove_tree_posix(const char *path) {
    /* unlink() removes a file or symlink as itself and fails on a directory. */
    if (unlink(path) == 0 || errno == ENOENT || errno == ENOTDIR) return true;
    if (errno != EISDIR && errno != EPERM) return false;
    /* O_NOFOLLOW refuses a symlink swapped in for the directory. */
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT;
    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        return false;
    }
    struct dirent *ent;
    bool ok = true;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[4096];
        int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(child) || !remove_tree_posix(child)) {
            ok = false;
            break;
        }
    }
    closedir(dir);
    if (!ok) return false;
    return rmdir(path) == 0 || errno == ENOENT;
}

/* Runs fn on path, or with skip_child set on each top-level child but that one. */
static bool tree_walk_top(const char *path, const char *skip_child,
                          bool (*fn)(const char *, void *), void *ctx, bool *kept) {
    if (!path || !*path) return false;
    if (kept) *kept = false;
    struct stat st;
    if (!skip_child || lstat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return fn(path, ctx);
    DIR *dir = opendir(path);
    if (!dir) return false;
    struct dirent *ent;
    bool ok = true;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (strcmp(ent->d_name, skip_child) == 0) {
            if (kept) *kept = true;
            continue;
        }
        char child[4096];
        int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(child) || !fn(child, ctx)) {
            ok = false;
            break;
        }
    }
    closedir(dir);
    return ok;
}

static bool tree_size_step(const char *path, void *ctx) {
    return tree_size_posix(path, (uint64_t *)ctx);
}

static bool remove_tree_step(const char *path, void *ctx) {
    (void)ctx;
    return remove_tree_posix(path);
}

bool nk_platform_tree_size(const char *path, const char *skip_child, uint64_t *out_bytes) {
    if (!out_bytes) return false;
    *out_bytes = 0;
    return tree_walk_top(path, skip_child, tree_size_step, out_bytes, NULL);
}

bool nk_platform_remove_tree(const char *path, const char *skip_child) {
    bool kept = false;
    if (!tree_walk_top(path, skip_child, remove_tree_step, NULL, &kept)) return false;
    if (!skip_child || kept) return true;
    return remove_tree_posix(path);
}

bool nk_platform_list_files(const char *dir, NkDirFileFn fn, void *ctx) {
    if (!dir || !*dir || !fn) return false;
    DIR *handle = opendir(dir);
    if (!handle) return false;
    struct dirent *item;
    while ((item = readdir(handle)) != NULL) {
        if (item->d_name[0] == '.') continue;
        char path[4096];
        int written = snprintf(path, sizeof(path), "%s/%s", dir, item->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) continue;
        struct stat st;
        if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (!fn(item->d_name, ctx)) break;
    }
    closedir(handle);
    return true;
}

int64_t nk_platform_get_file_size(const char *path) {
    if (!path) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

bool nk_platform_mkdir_p(const char *dir_path) {
    if (!dir_path || !*dir_path) return false;
    char tmp[1024];
    size_t len = strlen(dir_path);
    if (len >= sizeof(tmp)) return false;
    memcpy(tmp, dir_path, len + 1);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                /* Ignore error if directory already exists */
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        /* check exists */
    }
    return nk_platform_dir_exists(dir_path);
}

bool nk_platform_mkdir_p_private(const char *dir_path) {
    if (!dir_path || !*dir_path) return false;
    char tmp[1024];
    size_t len = strlen(dir_path);
    if (len >= sizeof(tmp)) return false;
    memcpy(tmp, dir_path, len + 1);

    /* Only newly-created components are made private. Existing ancestors can
       be shared application or system directories (for example /home or
       /tmp); changing their mode here would be an unrelated permission
       mutation. lstat() also keeps a symlink from satisfying the directory
       check and redirecting later writes outside the intended tree. */
    for (char *p = tmp + 1;; p++) {
        if (*p != '/' && *p != '\0') continue;
        char saved = *p;
        *p = '\0';
        if (tmp[0] != '\0') {
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
                *p = saved;
                return false;
            }
            struct stat st;
            if (lstat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) {
                *p = saved;
                return false;
            }
        }
        *p = saved;
        if (saved == '\0') break;
    }
    return true;
}

FILE *nk_platform_fopen_private(const char *path, const char *mode) {
    if (!path || !mode || mode[0] != 'w' || strchr(mode, '+') != NULL) return NULL;
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags, 0600);
    if (fd < 0) return NULL;
    FILE *file = fdopen(fd, mode);
    if (!file) close(fd);
    return file;
}

static char s_app_data_dir_override[NK_MAX_PATH];

bool nk_platform_set_app_data_dir_override(const char *path) {
    if (!path || !path[0]) {
        s_app_data_dir_override[0] = '\0';
        return true;
    }
    size_t length = strlen(path);
    if (length >= sizeof(s_app_data_dir_override)) return false;
    memcpy(s_app_data_dir_override, path, length + 1u);
    return true;
}

static bool resolve_posix_path(NkPathType type, char *out_path, size_t max_len) {
    if (!out_path || max_len == 0) return false;
    if (type == NK_PATH_DATA && s_app_data_dir_override[0]) {
        int written = snprintf(out_path, max_len, "%s", s_app_data_dir_override);
        return written > 0 && (size_t)written < max_len;
    }

#if defined(__APPLE__)
    const char *home = getenv("HOME");
    if (!home) return false;
    int written = 0;
    switch (type) {
        case NK_PATH_CONFIG:
            written = snprintf(out_path, max_len, "%s/Library/Application Support/NakagawaRecomp/config", home);
            break;
        case NK_PATH_DATA:
            written = snprintf(out_path, max_len, "%s/Library/Application Support/NakagawaRecomp/data", home);
            break;
        case NK_PATH_CACHE:
            written = snprintf(out_path, max_len, "%s/Library/Caches/NakagawaRecomp", home);
            break;
        case NK_PATH_LOGS:
            written = snprintf(out_path, max_len, "%s/Library/Logs/NakagawaRecomp", home);
            break;
        case NK_PATH_SAVES:
            written = snprintf(out_path, max_len, "%s/Library/Application Support/NakagawaRecomp/saves", home);
            break;
        default:
            written = snprintf(out_path, max_len, "%s/Library/Application Support/NakagawaRecomp", home);
            break;
    }
#else
    const char *home = getenv("HOME");
    const char *env_val = NULL;
    int written = 0;

    switch (type) {
        case NK_PATH_CONFIG:
            env_val = getenv("XDG_CONFIG_HOME");
            if (env_val && *env_val) {
                written = snprintf(out_path, max_len, "%s/nakagawa-recomp", env_val);
            } else if (home && *home) {
                written = snprintf(out_path, max_len, "%s/.config/nakagawa-recomp", home);
            }
            break;
        case NK_PATH_DATA:
            env_val = getenv("XDG_DATA_HOME");
            if (env_val && *env_val) {
                written = snprintf(out_path, max_len, "%s/nakagawa-recomp", env_val);
            } else if (home && *home) {
                written = snprintf(out_path, max_len, "%s/.local/share/nakagawa-recomp", home);
            }
            break;
        case NK_PATH_CACHE:
            env_val = getenv("XDG_CACHE_HOME");
            if (env_val && *env_val) {
                written = snprintf(out_path, max_len, "%s/nakagawa-recomp", env_val);
            } else if (home && *home) {
                written = snprintf(out_path, max_len, "%s/.cache/nakagawa-recomp", home);
            }
            break;
        case NK_PATH_LOGS:
            env_val = getenv("XDG_STATE_HOME");
            if (env_val && *env_val) {
                written = snprintf(out_path, max_len, "%s/nakagawa-recomp/logs", env_val);
            } else if (home && *home) {
                written = snprintf(out_path, max_len, "%s/.local/state/nakagawa-recomp/logs", home);
            }
            break;
        case NK_PATH_SAVES:
            env_val = getenv("XDG_DATA_HOME");
            if (env_val && *env_val) {
                written = snprintf(out_path, max_len, "%s/nakagawa-recomp/saves", env_val);
            } else if (home && *home) {
                written = snprintf(out_path, max_len, "%s/.local/share/nakagawa-recomp/saves", home);
            }
            break;
        default:
            return false;
    }
#endif

    return written > 0 && (size_t)written < max_len;
}

bool nk_platform_get_path(NkPathType type, char *out_path, size_t max_len) {
    if (!resolve_posix_path(type, out_path, max_len)) return false;
    (void)nk_platform_mkdir_p(out_path);
    return true;
}

bool nk_platform_resolve_app_data_dir(char *out_path, size_t max_len) {
    return resolve_posix_path(NK_PATH_DATA, out_path, max_len);
}

bool nk_platform_resolve_path(NkPathType type, char *out_path, size_t max_len) {
    return resolve_posix_path(type, out_path, max_len);
}

bool nk_platform_get_app_data_dir(char *out_path, size_t max_len) {
    return nk_platform_get_path(NK_PATH_DATA, out_path, max_len);
}

bool nk_platform_get_legacy_app_data_dir(char *out_path, size_t max_len) {
    (void)out_path;
    (void)max_len;
    return false;
}

bool nk_platform_spawn_process(
    const char *executable_path,
    const char * const *argv,
    const char * const *envp,
    const char *working_directory,
    const char *output_log_path,
    NkProcessHandle *out_process
) {
    if (!executable_path || !out_process) return false;
    memset(out_process, 0, sizeof(*out_process));

    int output_fd = -1;
    if (output_log_path && output_log_path[0]) {
        output_fd = open(output_log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (output_fd < 0) {
            out_process->spawn_error = errno;
            return false;
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        out_process->spawn_error = errno;
        if (output_fd >= 0) close(output_fd);
        return false;
    }

    if (pid == 0) {
        /* Child process: establish own process group to allow tree-wide signals */
        (void)setpgid(0, 0);

        if (output_fd >= 0) {
            if (dup2(output_fd, STDOUT_FILENO) < 0 ||
                dup2(output_fd, STDERR_FILENO) < 0) {
                _exit(127);
            }
            close(output_fd);
        }

        if (working_directory && *working_directory) {
            if (chdir(working_directory) != 0) {
                _exit(127);
            }
        }

        /* If custom envp provided, apply it. The entries are const (callers pass
         * string literals), so the name is copied out rather than split in place. */
        if (envp) {
            for (int i = 0; envp[i]; i++) {
                const char *eq = strchr(envp[i], '=');
                if (eq) {
                    char name[256];
                    size_t name_len = (size_t)(eq - envp[i]);
                    if (name_len == 0 || name_len >= sizeof(name)) _exit(127);
                    memcpy(name, envp[i], name_len);
                    name[name_len] = '\0';
                    if (setenv(name, eq + 1, 1) != 0) _exit(127);
                } else {
                    unsetenv(envp[i]);
                }
            }
        }

        char * const default_argv[] = { (char *)executable_path, NULL };
        char * const *exec_argv = argv ? (char * const *)argv : default_argv;

        execv(executable_path, exec_argv);
        _exit(127);
    }

    if (output_fd >= 0) close(output_fd);

    /* Parent process: close race by setting child's process group */
    (void)setpgid(pid, pid);

    out_process->native_handle = (void *)(intptr_t)pid;
    out_process->job_handle = NULL;
    out_process->process_id = (int)pid;
    out_process->is_active = true;
    return true;
}

bool nk_platform_path_exceeds_limit(const char *path, bool is_directory) {
    (void)is_directory;
    if (!path || !*path) return false;
#ifdef PATH_MAX
    return strlen(path) + 1u > (size_t)PATH_MAX;
#else
    return strlen(path) + 1u > 4096u;
#endif
}

/* A POSIX spawn has no security-product refusal the player can tell apart. */
NkSpawnBlock nk_platform_spawn_block(int os_error) {
    (void)os_error;
    return NK_SPAWN_BLOCK_NONE;
}

bool nk_platform_describe_os_error(int os_error, char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (os_error == 0) return false;
    const char *text = strerror(os_error);
    if (!text || !text[0]) return false;
    snprintf(out, out_size, "%s", text);
    return true;
}

bool nk_platform_is_process_running(NkProcessHandle *process) {
    if (!process || !process->is_active) return false;
    pid_t pid = (pid_t)(intptr_t)process->native_handle;
    int status = 0;
    pid_t res = waitpid(pid, &status, WNOHANG);
    if (res == 0) {
        return true;
    }
    /* This call just reaped the child, so it is the only chance to read the
       exit status: a second waitpid fails with ECHILD. Cache it so a following
       nk_platform_wait_process reports the real code instead of -1. */
    if (res == pid && WIFEXITED(status)) {
        process->cached_exit_code = WEXITSTATUS(status);
        process->has_cached_exit = true;
    }
    process->is_active = false;
    return false;
}

/* nk_platform.h reserves a negative timeout for an infinite wait; every
   nonnegative value is a real deadline, and the Win32 backend honours it via
   WaitForSingleObject. Discarding it here and always blocking meant a caller
   such as nk_launch_wait(..., 5000) hung forever on POSIX whenever the runtime
   stalled, with no way to recover -- the opposite of what the timeout is for.

   POSIX has no portable "wait for this child, but only for so long" primitive.
   sigtimedwait on SIGCHLD is close, but SIGCHLD belongs to the whole process
   and a host application may already handle it, so consuming it here would
   break code this backend does not own. Polling with a short bounded sleep is
   correct on every POSIX target and costs nothing on a wait already measured
   in seconds. */
static bool deadline_passed(const struct timespec *deadline) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return true;
    if (now.tv_sec != deadline->tv_sec) return now.tv_sec > deadline->tv_sec;
    return now.tv_nsec >= deadline->tv_nsec;
}

bool nk_platform_bring_process_window_to_front(const NkProcessHandle *process) {
    (void)process;
    return false;
}

int nk_platform_wait_process(NkProcessHandle *process, int timeout_ms) {
    if (!process) return -1;
    if (process->has_cached_exit) {
        return process->cached_exit_code;
    }
    pid_t pid = (pid_t)(intptr_t)process->native_handle;
    int status = 0;

    if (timeout_ms < 0) {
        pid_t res;
        do {
            res = waitpid(pid, &status, 0);
        } while (res < 0 && errno == EINTR);
        process->is_active = false;
        if (res == pid && WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        return -1;
    }

    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        /* No usable clock: refuse to convert a bounded wait into an unbounded
           one. Report failure rather than blocking indefinitely. */
        return -1;
    }
    deadline.tv_sec += (time_t)(timeout_ms / 1000);
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    for (;;) {
        pid_t res = waitpid(pid, &status, WNOHANG);
        if (res == pid) {
            process->is_active = false;
            if (WIFEXITED(status)) {
                return WEXITSTATUS(status);
            }
            return -1;
        }
        if (res < 0) {
            if (errno == EINTR) continue;
            process->is_active = false;
            return -1;
        }
        if (deadline_passed(&deadline)) {
            /* Timed out with the child still alive. is_active stays true and
               the handle stays valid, so the caller can wait again or
               terminate -- the same state WAIT_TIMEOUT leaves on Win32. */
            return -1;
        }
        struct timespec slice = { 0, 2L * 1000L * 1000L }; /* 2 ms */
        nanosleep(&slice, NULL);
    }
}

/* How long a child gets to honour SIGTERM before it is killed, and how long
   the kill itself is given to take effect. Both are short enough that the UI
   thread calling Stop does not visibly hang. */
#define NK_TERM_GRACE_MS 2000
#define NK_KILL_GRACE_MS 1000

void nk_platform_terminate_process(NkProcessHandle *process) {
    if (!process) return;
    pid_t pid = (pid_t)process->process_id;
    if (pid <= 0) {
        pid = (pid_t)(intptr_t)process->native_handle;
    }
    if (pid <= 0) return;
    if (!process->is_active && kill(-pid, 0) != 0) return;

    if (kill(-pid, SIGTERM) != 0 && errno != ESRCH) {
        /* Not ours to signal; there is nothing useful left to do. */
        process->is_active = false;
        return;
    }

    /* SIGTERM is a request, not an outcome. Marking the child inactive right
       here and then erasing the pid in close_process left a normally
       terminating runtime as a zombie until the player itself exited, and left
       a runtime that delays or ignores SIGTERM still running while the UI
       reported it stopped. Reap it, and escalate if it will not go.
       nk_platform_wait_process leaves is_active true on a timeout and clears
       it once the child is reaped, which is what distinguishes the two. */
    nk_platform_wait_process(process, NK_TERM_GRACE_MS);
    if (process->is_active || kill(-pid, 0) == 0) {
        kill(-pid, SIGKILL);
        if (process->is_active) {
            nk_platform_wait_process(process, NK_KILL_GRACE_MS);
        }
    }
    process->is_active = false;
}

void nk_platform_close_process(NkProcessHandle *process) {
    if (!process) return;
    process->native_handle = NULL;
    process->job_handle = NULL;
    process->process_id = 0;
    process->is_active = false;
    process->has_cached_exit = false;
    process->cached_exit_code = 0;
}

bool nk_platform_local_time_zone(int *standard_minutes, bool *daylight_now) {
    time_t now = time(NULL);
    time_t as_standard;
    struct tm local;
    struct tm utc;
    if (!standard_minutes || !daylight_now || now == (time_t)-1) return false;
    if (!localtime_r(&now, &local) || !gmtime_r(&now, &utc)) return false;
    /* The UTC clock read back as local standard time lands the standard offset before now. */
    utc.tm_isdst = 0;
    as_standard = mktime(&utc);
    if (as_standard == (time_t)-1) return false;
    *standard_minutes = (int)(difftime(now, as_standard) / 60.0);
    *daylight_now = local.tm_isdst > 0;
    return true;
}

#endif /* !_WIN32 */
