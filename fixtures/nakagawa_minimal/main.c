/*
 * NAKAGAWA_MINIMAL - minimal stock user-mode PSP fixture for the Phase-5
 * audit of the Nakagawa recompiler.
 *
 * Built -nostdlib: no newlib CRT (no __libcglue_init thread/stdio startup,
 * no libc printf/snprintf/strlen). The only imports are the direct PSP
 * user/kernel API stubs below, so the PRX import table carries exactly the
 * fixture's own calls (sceKernelStdout, sceIoWrite, sceIoOpen, sceIoClose,
 * sceKernelExitGame).
 *
 * Evidence channels (all dedicated runtime handlers, no fake success):
 *  1. The uint32 loop result is converted to decimal with a hand-rolled
 *     conversion and written to the std stdout descriptor, which the runtime
 *     mirrors to the host console log (SCETYPEWRITE line).
 *  2. The same constant string is written through sceIoOpen/sceIoWrite/
 *     sceIoClose to a flat ms0:/ path (sceIoMkdir has no runtime handler, so
 *     no directory creation), and the host-side file is verified byte-for-
 *     byte. The first open lands on the runtime's implicit std slot, so the
 *     file is written through the second descriptor.
 * Clean exit via sceKernelExitGame(0).
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>

PSP_MODULE_INFO("NAKAGAWA_MINIMAL", 0, 1, 1);

int sceKernelStdout(void);

static const char HEAD[] = "NAKAGAWA_MINIMAL SUM=";
static const char TAIL[] = "\n";

static int decimal(unsigned int v, char *out, int cap) {
    char tmp[12];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v != 0) {
            tmp[n++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    int j = 0;
    for (int i = n - 1; i >= 0; i--) {
        if (j < cap) out[j++] = tmp[i];
    }
    return j;
}

static int run(void) {
    unsigned int sum = 0;
    unsigned int i;
    for (i = 1; i <= 100; i++) {
        sum += i;
    }

    char digits[12];
    int nd = decimal(sum, digits, sizeof(digits));
    int out = sceKernelStdout();
    sceIoWrite(out, HEAD, sizeof(HEAD) - 1);
    sceIoWrite(out, digits, nd);
    sceIoWrite(out, TAIL, sizeof(TAIL) - 1);

    int fd = sceIoOpen("ms0:/NAKAGAWA_MINIMAL_RESULT.TXT",
                       PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    int fd2 = sceIoOpen("ms0:/NAKAGAWA_MINIMAL_RESULT.TXT",
                        PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd2 >= 0) {
        sceIoWrite(fd2, HEAD, sizeof(HEAD) - 1);
        sceIoWrite(fd2, digits, nd);
        sceIoWrite(fd2, TAIL, sizeof(TAIL) - 1);
        sceIoClose(fd2);
    }
    if (fd >= 0) sceIoClose(fd);
    return 0;
}

int module_stop(int argc, char *argv[]) {
    return 0;
}

int module_start(int argc, char *argv[]) {
    run();
    sceKernelExitGame();
    return 0;
}
