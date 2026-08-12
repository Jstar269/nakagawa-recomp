#include <pspkernel.h>
#include <pspdebug.h>
#include <stdio.h>
#include <string.h>

PSP_MODULE_INFO("PSPDEV_PHASE5", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

#define FIXTURE_BUILD_ID "NAKAGAWA-PSPDEV-PHASE5-v1"
#define EXPECTED_SUM 5050

int main(int argc, char *argv[]) {
    pspDebugScreenInit();
    pspDebugScreenPrintf("Nakagawa PSP Recompilation Oracle\n");
    pspDebugScreenPrintf("Build ID: %s\n\n", FIXTURE_BUILD_ID);

    int sum = 0;
    for (int i = 1; i <= 100; i++) {
        sum += i;
    }

    int pass = (sum == EXPECTED_SUM);
    pspDebugScreenPrintf("Result: sum=%d (Expected: %d) -> %s\n",
                         sum, EXPECTED_SUM, pass ? "PASS" : "FAIL");
    printf("PSPDEV Phase5 Fixture: sum=%d build_id=%s pass=%d\n", sum, FIXTURE_BUILD_ID, pass);

    sceIoMkdir("ms0:/NAKAGAWA_TEST", 0777);
    int fd = sceIoOpen("ms0:/NAKAGAWA_TEST/RESULT.TXT", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        char buf[256];
        int len = snprintf(buf, sizeof(buf), "BUILD_ID=%s\nSUM=%d\nPASS=%d\n", FIXTURE_BUILD_ID, sum, pass);
        sceIoWrite(fd, (const void *)buf, len);
        sceIoClose(fd);
        pspDebugScreenPrintf("Wrote result to ms0:/NAKAGAWA_TEST/RESULT.TXT\n");
    } else {
        pspDebugScreenPrintf("ms0:/NAKAGAWA_TEST/RESULT.TXT unavailable\n");
    }

    pspDebugScreenPrintf("\nExiting in 3 seconds...\n");
    sceKernelDelayThread(3000000);

    sceKernelExitGame();
    return 0;
}
