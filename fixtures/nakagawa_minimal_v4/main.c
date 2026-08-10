/*
 * NAKAGAWA_MINIMAL_V4 - stock-hardware lifecycle diagnostic.
 *
 * ONLY meaningful behavioral change vs v3: termination. v3 called
 * sceKernelExitGame() after the S4 marker and the stock PSP still hung
 * (black screen, no XMB return). v4 instead returns normally from
 * module_start (the PSPDEV-documented PRX-module pattern: a
 * PSP_NO_CREATE_MAIN_THREAD() module whose module_start() performs file I/O
 * and returns 0). The SDK's own crt0_prx.c module_start/_start returns 0 on
 * the normal path and 1 on the no-create-thread inline path; the return
 * value is the StartModule result, not a direct XMB signal.
 *
 * Same arithmetic, same markers through S4, same payload, same constraints:
 * -nostdlib, no libc/newlib, no display/debug screen, no stdout, no new
 * imports (sceKernelExitGame is REMOVED: import table goes 4 -> 3).
 *
 * Hardware acceptance question: does normal return from module_start allow
 * Sony OFW to return control cleanly, or does the PSP remain black/hung?
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>

PSP_MODULE_INFO("NAKAGAWA_MINIMAL_V4", 0, 1, 1);
PSP_NO_CREATE_MAIN_THREAD();

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
        return 1;
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
    return 0;
}
