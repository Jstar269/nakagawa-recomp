// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// Resident-runner handshake v0: proves a long-running PSP module can be
// driven through host0:/ files with no per-experiment relaunch (and hence
// no ExitGame-teardown thread-table poisoning, the campaign's operating
// rule). Creates NO threads, so the boot stays fresh for later launches.
// Every record goes to stdout AND host0:/runner_log.txt, because stdout
// after the injecting pspsh session ends may be unobservable; the log file
// is the acceptance channel. Host acceptance: runner_ack.txt ends at ACK2
// and runner_log.txt holds 1 META + 4 TEST lines.

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("NAKAGAWA_RUNNER", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

#define CMD_PATH "host0:/runner_cmd.txt"
#define ACK_PATH "host0:/runner_ack.txt"
#define LOG_PATH "host0:/runner_log.txt"
#define ITERATIONS 3

static int emulator_present(void) {
    uint32_t flag = 0;
    if (sceIoDevctl("emulator:", 3, NULL, 0, &flag, sizeof(flag)) < 0) {
        return 0;
    }
    return flag == 1;
}

static void log_and_emit(int emulated, const char *text) {
    if (emulated) {
        sceIoDevctl("emulator:", 2, (void *)text, (int)strlen(text), NULL, 0);
    } else {
        printf("%s", text);
    }
    SceUID fd = sceIoOpen(LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND,
                          0777);
    if (fd >= 0) {
        sceIoWrite(fd, text, strlen(text));
        sceIoClose(fd);
    }
}

static int read_cmd(char *buf, size_t cap) {
    SceUID fd = sceIoOpen(CMD_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) {
        return (int)fd;
    }
    const int got = sceIoRead(fd, buf, cap - 1);
    sceIoClose(fd);
    if (got < 0) {
        return got;
    }
    buf[got] = '\0';
    return got;
}

static int write_ack(int iter) {
    char text[32];
    snprintf(text, sizeof(text), "ACK%d\n", iter);
    SceUID fd = sceIoOpen(ACK_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC,
                          0777);
    if (fd < 0) {
        return (int)fd;
    }
    const int wrote = sceIoWrite(fd, text, strlen(text));
    sceIoClose(fd);
    return wrote;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    const int emulated = emulator_present();
    setvbuf(stdout, NULL, _IONBF, 0);
    char line[256];
    snprintf(line, sizeof(line),
             "NAKAGAWA_PSP_META schema=1 source=%s model=unknown "
             "firmware=unknown "
             "binary_sha256=0000000000000000000000000000000000000000000000000000000000000000 "
             "source_commit=0000000000000000000000000000000000000000 "
             "fixture=nakagawa-runner-v0\n",
             emulated ? "ppsspp" : "psp");
    log_and_emit(emulated, line);
    for (int i = 0; i < ITERATIONS; i++) {
        char cmd[64];
        const int rc = read_cmd(cmd, sizeof(cmd));
        uint32_t acked = 0;
        if (rc > 0 && strncmp(cmd, "PING", 4) == 0) {
            acked = (write_ack(i) > 0) ? 1u : 0u;
        }
        snprintf(line, sizeof(line),
                 "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-RUNNER-001 "
                 "case_id=handshake status=%s result=0x%08x out0=0x%08x "
                 "out1=0x%08x\n",
                 (acked && rc > 0) ? "PASS" : "FAIL", (unsigned int)i,
                 (unsigned int)rc, (unsigned int)acked);
        log_and_emit(emulated, line);
        sceKernelDelayThread(1000 * 1000);
    }
    snprintf(line, sizeof(line),
             "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-RUNNER-001 case_id=done "
             "status=PASS result=0x%08x\n",
             (unsigned int)ITERATIONS);
    log_and_emit(emulated, line);
    return 0;
}
