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
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

bool nk_platform_get_path(NkPathType type, char *out_path, size_t max_len) {
    if (!out_path || max_len == 0) return false;

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

    if (written <= 0 || (size_t)written >= max_len) return false;
    nk_platform_mkdir_p(out_path);
    return true;
}

bool nk_platform_get_app_data_dir(char *out_path, size_t max_len) {
    return nk_platform_get_path(NK_PATH_DATA, out_path, max_len);
}

bool nk_platform_spawn_process(
    const char *executable_path,
    const char * const *argv,
    const char * const *envp,
    const char *working_directory,
    NkProcessHandle *out_process
) {
    if (!executable_path || !out_process) return false;
    memset(out_process, 0, sizeof(*out_process));

    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }

    if (pid == 0) {
        /* Child process */
        if (working_directory && *working_directory) {
            if (chdir(working_directory) != 0) {
                _exit(127);
            }
        }

        /* If custom envp provided, apply it */
        if (envp) {
            for (int i = 0; envp[i]; i++) {
                char *eq = strchr(envp[i], '=');
                if (eq) {
                    *eq = '\0';
                    setenv(envp[i], eq + 1, 1);
                    *eq = '=';
                }
            }
        }

        char * const default_argv[] = { (char *)executable_path, NULL };
        char * const *exec_argv = argv ? (char * const *)argv : default_argv;

        execv(executable_path, exec_argv);
        _exit(127);
    }

    out_process->native_handle = (void *)(intptr_t)pid;
    out_process->process_id = (int)pid;
    out_process->is_active = true;
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
    if (!process || !process->is_active) return;
    pid_t pid = (pid_t)(intptr_t)process->native_handle;

    if (kill(pid, SIGTERM) != 0 && errno != ESRCH) {
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
    if (process->is_active) {
        kill(pid, SIGKILL);
        nk_platform_wait_process(process, NK_KILL_GRACE_MS);
    }
    process->is_active = false;
}

void nk_platform_close_process(NkProcessHandle *process) {
    if (!process) return;
    process->native_handle = NULL;
    process->process_id = 0;
    process->is_active = false;
    process->has_cached_exit = false;
    process->cached_exit_code = 0;
}

#endif /* !_WIN32 */
