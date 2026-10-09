/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * osk_text_entry.c - a person's answer to one on-screen-keyboard field, collected without
 * stopping guest time (contract in osk_text_entry.h).
 *
 * On Windows the native input box (osk_win.c) runs its own modal loop until the person
 * presses OK or Cancel. It runs on a worker thread, one request at a time; the scheduler
 * thread only polls the request's state. The worker watches its own thread's window
 * creation (a thread-local CBT hook), so an abandoned request can close the box whenever
 * the abandon lands: before the box exists, while it is being created, or while it is
 * shown.
 *
 * Request states (s_state, changed only with interlocked operations):
 *   IDLE      no request; the next poll opens one and starts its worker
 *   OPEN      the worker is collecting the answer
 *   ANSWERED  the worker finished; the next poll hands the answer over and returns to IDLE
 *   ABANDONED the keyboard dropped the request; the worker discards its answer and returns
 *             the state to IDLE itself, so a new request never shares the box with an old one
 */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "osk_text_entry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OSK_TEXT_UNITS 256 /* the HLE keyboard's own field bound, terminator included */

/* SR_VIDEO=offscreen selects the headless host-memory presenter (gui.c): there is no
 * window, and no person to answer one. */
static int osk_text_entry_offscreen(void) {
    const char *video = getenv("SR_VIDEO");
    return video && strcmp(video, "offscreen") == 0;
}

static int osk_text_entry_unanswerable(void) {
    static int s_reported;
    if (!osk_text_entry_offscreen()) return 0;
    if (!s_reported) {
        s_reported = 1;
        fprintf(stderr, "osk: no person can answer the keyboard under the offscreen presenter; "
                        "it stays open while the title keeps running (an automated route "
                        "answers it with SR_OSK_SCRIPT or SR_OSK_TEXT)\n");
    }
    return 1;
}

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Copy at most cap - 1 units of src and a terminator. */
static void osk_text_copy(wchar_t *dst, const wchar_t *src, int cap) {
    int n = 0;
    if (src)
        while (n < cap - 1 && src[n]) {
            dst[n] = src[n];
            n++;
        }
    dst[n] = 0;
}

enum { ENTRY_IDLE = 0, ENTRY_OPEN = 1, ENTRY_ANSWERED = 2, ENTRY_ABANDONED = 3 };

static volatile LONG s_state = ENTRY_IDLE;
static PVOID volatile s_box;          /* the request's top-level window once it is created */
static volatile LONG s_close_sent;    /* the request's box has been told to close */
static HANDLE s_worker;               /* scheduler thread only */
/* The request, written by the scheduler thread before the worker starts. */
static wchar_t s_desc[OSK_TEXT_UNITS];
static wchar_t s_initial[OSK_TEXT_UNITS];
static int s_cap;
/* The answer, written by the worker before it publishes ANSWERED. */
static wchar_t s_text[OSK_TEXT_UNITS];
static int s_answer;

static LONG entry_state(void) {
    return InterlockedCompareExchange(&s_state, 0, 0);
}

/* Cancel the box as the person would, once per request. s_box keeps the box until the
 * worker returns (so the creation hook never records a second window), and every path that
 * closes it (abandon, the creation hook, an abandoned poll) races to send this once. */
static void entry_close_box(PVOID box) {
    if (box && InterlockedCompareExchange(&s_close_sent, 1, 0) == 0)
        PostMessageW((HWND)box, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
}

/* Thread-local CBT hook on the worker: records the box as it is created, and closes it at
 * once when the request was abandoned before the box existed. */
static LRESULT CALLBACK entry_watch_box(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        const CBT_CREATEWNDW *created = (const CBT_CREATEWNDW *)lp;
        if (created && created->lpcs && !(created->lpcs->style & WS_CHILD) &&
            InterlockedCompareExchangePointer(&s_box, (PVOID)wp, NULL) == NULL &&
            entry_state() == ENTRY_ABANDONED)
            entry_close_box((PVOID)wp);
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static DWORD WINAPI entry_worker(LPVOID unused) {
    wchar_t text[OSK_TEXT_UNITS];
    int answer = 0;
    (void)unused;
    HHOOK watch = SetWindowsHookExW(WH_CBT, entry_watch_box, NULL, GetCurrentThreadId());
    if (entry_state() == ENTRY_OPEN) {
        osk_text_copy(text, s_initial, s_cap);
        answer = sr_osk_input(s_desc, s_initial, text, s_cap) == 1;
    }
    if (watch) UnhookWindowsHookEx(watch);
    InterlockedExchangePointer(&s_box, NULL);
    if (answer) osk_text_copy(s_text, text, s_cap);
    s_answer = answer;
    if (InterlockedCompareExchange(&s_state, ENTRY_ANSWERED, ENTRY_OPEN) != ENTRY_OPEN)
        InterlockedExchange(&s_state, ENTRY_IDLE);   /* abandoned: nobody wants the answer */
    return 0;
}

/* Join a worker that has finished (or is returning) before its slot is reused. */
static void entry_reap_worker(void) {
    if (!s_worker) return;
    WaitForSingleObject(s_worker, INFINITE);
    CloseHandle(s_worker);
    s_worker = NULL;
}

int sr_osk_text_entry_poll(const wchar_t *desc, const wchar_t *initial, wchar_t *out, int cap) {
    if (!out || cap < 2) return 0;
    if (cap > OSK_TEXT_UNITS) cap = OSK_TEXT_UNITS;
    if (osk_text_entry_unanswerable()) return SR_OSK_TEXT_PENDING;
    switch (entry_state()) {
    case ENTRY_OPEN:
        return SR_OSK_TEXT_PENDING;
    case ENTRY_ABANDONED:
        entry_close_box(InterlockedCompareExchangePointer(&s_box, NULL, NULL));
        return SR_OSK_TEXT_PENDING;                  /* a new request waits for the old box */
    case ENTRY_ANSWERED: {
        int answer;
        entry_reap_worker();
        answer = s_answer;
        if (answer) osk_text_copy(out, s_text, cap < s_cap ? cap : s_cap);
        InterlockedExchange(&s_state, ENTRY_IDLE);
        return answer;
    }
    default:
        break;
    }
    entry_reap_worker();
    osk_text_copy(s_desc, desc, OSK_TEXT_UNITS);
    osk_text_copy(s_initial, initial, OSK_TEXT_UNITS);
    s_cap = cap;
    InterlockedExchange(&s_close_sent, 0);
    InterlockedExchange(&s_state, ENTRY_OPEN);
    s_worker = CreateThread(NULL, 0, entry_worker, NULL, 0, NULL);
    if (!s_worker) {
        InterlockedExchange(&s_state, ENTRY_IDLE);
        fprintf(stderr, "osk: the text-entry worker could not start (error %lu); the field is "
                        "answered cancelled\n", (unsigned long)GetLastError());
        return 0;
    }
    return SR_OSK_TEXT_PENDING;
}

void sr_osk_text_entry_abandon(void) {
    if (InterlockedCompareExchange(&s_state, ENTRY_ABANDONED, ENTRY_OPEN) == ENTRY_OPEN) {
        entry_close_box(InterlockedCompareExchangePointer(&s_box, NULL, NULL));
        return;
    }
    (void)InterlockedCompareExchange(&s_state, ENTRY_IDLE, ENTRY_ANSWERED); /* drop a late answer */
}

#else /* !_WIN32 */

/* No native input box exists on this host: sr_osk_input answers cancelled at once without
 * waiting, so asking it from the scheduler thread cannot stop guest time. */
int sr_osk_text_entry_poll(const wchar_t *desc, const wchar_t *initial, wchar_t *out, int cap) {
    if (!out || cap < 2) return 0;
    if (osk_text_entry_unanswerable()) return SR_OSK_TEXT_PENDING;
    return sr_osk_input(desc, initial, out, cap) == 1;
}

void sr_osk_text_entry_abandon(void) {
}

#endif /* _WIN32 */
