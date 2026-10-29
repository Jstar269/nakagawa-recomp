// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Nakagawa Font Copy: a small PSP app that copies the console's own system fonts to its folder
 * on the Memory Stick, so Nakagawa Recomp on a PC can import them while the PSP is in its normal
 * USB storage mode (docs/SETUP.md, "System fonts"). It is the first piece of the PSP companion
 * app (#737) and needs a PSP that runs homebrew.
 *
 * The copy is fixtures/psp_oracle/font_files.h, the read-only copy the hardware oracle's
 * font-files case uses, with the settings in fontcopy_config.h: flash0:/font is only listed and
 * opened read-only, only .pgf files are copied, and every write goes to <ms0: or ef0:>/NAKAGAWA/
 * fonts. Each copy is checked with SHA-256 against a read-back and a second flash0 read. No key,
 * decryption, network or other write is involved; the fonts are plain files.
 *
 * Build: src/psp_companion/Makefile (PSPSDK). The host harness tests of the selection and the
 * destination are in tools/test_psp_oracle.py. */

#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font_files.h"
#include "fontcopy_config.h"

PSP_MODULE_INFO("NAKAGAWA_FONTCOPY", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

#define FONTCOPY_PROGRESS_ROW 12

/* What the copy reported, read from its records as they arrive. */
typedef struct FontCopyView {
    uint32_t done;
    uint32_t failed;
    uint32_t first_error;  /* the first failing copy's result; 0 when none */
    uint32_t list_error;   /* the font folder listing's result when it failed; 0 otherwise */
} FontCopyView;

static FontFiles s_files;
static FontCopyTarget s_target;
static FontCopyView s_view;
static const char *s_argv0; /* where the app was started from: decides ms0: or ef0: */

static int fontcopy_exit_callback(int arg1, int arg2, void *common) {
    (void)arg1;
    (void)arg2;
    (void)common;
    sceKernelExitGame();
    return 0;
}

static int fontcopy_callback_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    const int id = sceKernelCreateCallback("NakagawaFontCopyExit", fontcopy_exit_callback, NULL);
    if (id >= 0) sceKernelRegisterExitCallback(id);
    sceKernelSleepThreadCB();
    return 0;
}

/* The HOME button's "quit" leaves the app at any time, as every PSP app allows. */
static void fontcopy_start_exit_callback(void) {
    const SceUID thread = sceKernelCreateThread("NakagawaFontCopyCallbacks",
                                                fontcopy_callback_thread, 0x11, 0x1000, 0, NULL);
    if (thread >= 0) sceKernelStartThread(thread, 0, NULL);
}

/* The word after " <key>=" in a record line; 0 when the key is absent. */
static uint32_t fontcopy_field(const char *line, const char *key) {
    const char *at = strstr(line, key);
    return at != NULL ? (uint32_t)strtoul(at + strlen(key), NULL, 0) : 0u;
}

/* The file name after " detail=", up to the next space. */
static void fontcopy_detail(const char *line, char *out, size_t size) {
    const char *at = strstr(line, " detail=");
    size_t length = 0u;
    if (at != NULL) {
        at += strlen(" detail=");
        while (at[length] != '\0' && at[length] != ' ' && at[length] != '\n' &&
               length + 1u < size) {
            out[length] = at[length];
            length++;
        }
    }
    out[length] = '\0';
}

static void fontcopy_emit(void *ctx, const char *line, size_t length) {
    FontCopyView *view = (FontCopyView *)ctx;
    (void)length;
    if (strstr(line, " case_id=font-copy-") != NULL) {
        char name[FONTFILES_NAME_MAX + 1u];
        fontcopy_detail(line, name, sizeof(name));
        if (strstr(line, " status=PASS ") != NULL) {
            view->done++;
        } else {
            view->failed++;
            if (view->first_error == 0u) view->first_error = fontcopy_field(line, " result=");
        }
        pspDebugScreenSetXY(0, FONTCOPY_PROGRESS_ROW);
        pspDebugScreenPrintf("Copying font %lu of %lu: %-40s",
                             (unsigned long)(view->done + view->failed),
                             (unsigned long)s_files.candidates, name);
    } else if (strstr(line, " case_id=font-dir-open ") != NULL &&
               strstr(line, " status=FAIL ") != NULL) {
        view->list_error = fontcopy_field(line, " result=");
    } else if (strstr(line, " case_id=font-dest ") != NULL) {
        view->first_error = fontcopy_field(line, " result=");
    }
}

static void fontcopy_step(void *ctx, const char *step) {
    (void)ctx;
    (void)step;
}

/* Wait for one of the buttons in mask to be pressed; a button already held when the wait
 * starts counts only after it is released. */
static uint32_t fontcopy_wait(uint32_t mask) {
    SceCtrlData pad;
    uint32_t held = mask;
    for (;;) {
        sceCtrlReadBufferPositive(&pad, 1);
        const uint32_t now = pad.Buttons & mask;
        const uint32_t pressed = now & ~held;
        if (pressed != 0u) return pressed;
        held = now;
    }
}

static void fontcopy_title(void) {
    pspDebugScreenClear();
    pspDebugScreenSetXY(0, 1);
    pspDebugScreenPrintf(" Nakagawa Font Copy\n");
    pspDebugScreenPrintf(" ------------------\n\n");
}

static void fontcopy_intro(void) {
    fontcopy_title();
    pspDebugScreenPrintf(" This copies your PSP's own system fonts to the Memory Stick,\n");
    pspDebugScreenPrintf(" so Nakagawa Recomp on your PC can show your games' text in them.\n");
    pspDebugScreenPrintf(" It only reads the fonts and changes nothing else on your PSP.\n\n");
    pspDebugScreenPrintf(" The fonts go to %s\n\n", s_target.folder);
    pspDebugScreenPrintf(" Press X to copy the fonts, or O to leave.\n");
}

/* Run one copy and say how it went. Returns 1 when every font was copied and checked. */
static int fontcopy_run(void) {
    memset(&s_files, 0, sizeof(s_files));
    memset(&s_view, 0, sizeof(s_view));
    s_files.emit = fontcopy_emit;
    s_files.step = fontcopy_step;
    s_files.ctx = &s_view;
    fontcopy_configure(&s_files, &s_target, s_argv0);
    fontcopy_title();
    pspDebugScreenPrintf(" Reading your PSP's fonts...\n");
    fontfiles_run(&s_files);

    fontcopy_title();
    if (s_files.candidates > 0u && s_files.copied == s_files.candidates) {
        pspDebugScreenPrintf(" Done: all %lu fonts were copied and checked.\n",
                             (unsigned long)s_files.copied);
        pspDebugScreenPrintf(" They are in %s\n\n", s_target.folder);
        pspDebugScreenPrintf(" Next:\n");
        pspDebugScreenPrintf("  1. Leave this app (press X).\n");
        pspDebugScreenPrintf("  2. Connect your PSP to your PC with a USB cable.\n");
        pspDebugScreenPrintf("  3. On the PSP, open Settings > USB Connection.\n");
        pspDebugScreenPrintf("  4. In Nakagawa Recomp, choose \"Get fonts from my PSP\".\n\n");
        pspDebugScreenPrintf(" Press X to leave.\n");
        return 1;
    }
    if (s_files.candidates == 0u) {
        pspDebugScreenPrintf(" No font could be read from this PSP, so nothing was copied.\n");
        pspDebugScreenPrintf(" (Reading the font folder gave error 0x%08lX.)\n\n",
                             (unsigned long)(s_view.first_error != 0u ? s_view.first_error
                                                                      : s_view.list_error));
    } else if (s_files.copied == 0u) {
        pspDebugScreenPrintf(" The fonts could not be written to %s\n", s_target.folder);
        pspDebugScreenPrintf(" (error 0x%08lX). Check that the Memory Stick is in, is not\n",
                             (unsigned long)s_view.first_error);
        pspDebugScreenPrintf(" locked and has free space, then try again.\n\n");
    } else {
        pspDebugScreenPrintf(" %lu of %lu fonts were copied; %lu could not be copied\n",
                             (unsigned long)s_files.copied, (unsigned long)s_files.candidates,
                             (unsigned long)(s_files.candidates - s_files.copied));
        pspDebugScreenPrintf(" (first error 0x%08lX). Check the Memory Stick and try again.\n\n",
                             (unsigned long)s_view.first_error);
    }
    pspDebugScreenPrintf(" Press X to try again, or O to leave.\n");
    return 0;
}

int main(int argc, char *argv[]) {
    fontcopy_start_exit_callback();
    pspDebugScreenInit();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    s_argv0 = argc > 0 ? argv[0] : NULL;
    fontcopy_target(&s_target, s_argv0);

    fontcopy_intro();
    if ((fontcopy_wait(PSP_CTRL_CROSS | PSP_CTRL_CIRCLE) & PSP_CTRL_CIRCLE) == 0u) {
        for (;;) {
            if (fontcopy_run()) {
                (void)fontcopy_wait(PSP_CTRL_CROSS | PSP_CTRL_CIRCLE);
                break;
            }
            if (fontcopy_wait(PSP_CTRL_CROSS | PSP_CTRL_CIRCLE) & PSP_CTRL_CIRCLE) break;
        }
    }
    sceKernelExitGame();
    return 0;
}
