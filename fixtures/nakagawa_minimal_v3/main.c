/*
 * NAKAGAWA_MINIMAL_V3 - stock-hardware breadcrumb diagnostic fixture.
 *
 * Same constraints as v2: -nostdlib, no libc, no stdout/debug screen, no new
 * imports. Import table is exactly sceIoOpen / sceIoWrite / sceIoClose
 * (IoFileMgrForUser) + sceKernelExitGame (LoadExecForUser).
 *
 * Every step leaves a persistent marker file (existence = trace). The marker
 * helper opens with WRONLY|CREAT|TRUNC and immediately closes; it never
 * writes payload data, so marker content is always empty by design.
 *
 * Sequence:
 *   S0        start
 *   open result file -> S1_OPEN_OK | E1_OPEN_FAIL (then exit)
 *   write      -> S2_WRITE_OK | E2_WRITE_BAD  (return count compared, not formatted)
 *   close      -> S3_CLOSE_OK | E3_CLOSE_BAD  (return >= 0 compared)
 *   S4_PRE_EXIT
 *   sceKernelExitGame()
 *
 * The payload is a static constant carrying the computed 5050 result; the
 * uint32 loop still computes the sum and selects between the known-answer
 * payload and a BAD payload so the recompiler side can observe the arithmetic.
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>

PSP_MODULE_INFO("NAKAGAWA_MINIMAL_V3", 0, 1, 1);

static const char R_OK[] = "ms0:/NK_S0_START";
static const char O_OK[] = "ms0:/NK_S1_OPEN_OK";
static const char O_BAD[] = "ms0:/NK_E1_OPEN_FAIL";
static const char W_OK[] = "ms0:/NK_S2_WRITE_OK";
static const char W_BAD[] = "ms0:/NK_E2_WRITE_BAD";
static const char C_OK[] = "ms0:/NK_S3_CLOSE_OK";
static const char C_BAD[] = "ms0:/NK_E3_CLOSE_BAD";
static const char P_OK[] = "ms0:/NK_S4_PRE_EXIT";
static const char RESULT_PATH[] = "ms0:/NAKAGAWA_MINIMAL_RESULT.TXT";

static const char PAYLOAD_OK[] = "NAKAGAWA_MINIMAL SUM=5050\n";
static const char PAYLOAD_BAD[] = "NAKAGAWA_MINIMAL SUM=BAD\n";

static void mark(const char *path) {
    int fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) sceIoClose(fd);
}

int module_stop(int argc, char *argv[]) {
    return 0;
}

int module_start(int argc, char *argv[]) {
    unsigned int sum = 0;
    unsigned int i;
    for (i = 1; i <= 100; i++) {
        sum += i;
    }

    mark(R_OK);

    int fd = sceIoOpen(RESULT_PATH,
                       PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        mark(O_OK);
    } else {
        mark(O_BAD);
        sceKernelExitGame();
    }

    const char *payload = (sum == 5050u) ? PAYLOAD_OK : PAYLOAD_BAD;
    unsigned int expect = (sum == 5050u) ? sizeof(PAYLOAD_OK) - 1u
                                         : sizeof(PAYLOAD_BAD) - 1u;
    int wret = sceIoWrite(fd, payload, expect);
    if (wret == (int)expect) {
        mark(W_OK);
    } else {
        mark(W_BAD);
    }

    int cret = sceIoClose(fd);
    if (cret >= 0) {
        mark(C_OK);
    } else {
        mark(C_BAD);
    }

    mark(P_OK);
    sceKernelExitGame();
    return 0;
}
