/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * osk_text_entry_selftest.c - selftest for the keyboard's non-blocking text entry
 * (osk_text_entry.c).
 *
 * The native input box is replaced by a fake sr_osk_input that behaves like it: on Windows it
 * blocks its caller until the test lets the "person" answer, and it can create a real (never
 * shown) top-level window that closes only when it is cancelled. Checked:
 *   - a poll opens a request and returns pending at once while the box is still open, and
 *     the box runs on another thread than the poller (the scheduler is never blocked);
 *   - repeated polls neither block nor open a second box;
 *   - a confirmed answer is handed over once (bounded by the caller's capacity), a cancelled
 *     one is reported cancelled with the caller's buffer untouched, and the next poll opens
 *     a new request;
 *   - abandoning a request closes its box whether the abandon lands before the box exists
 *     or while it is shown, a late answer is never handed over, and a new request waits
 *     until the abandoned box is gone;
 *   - under the offscreen presenter nobody can answer: the request stays pending and no box
 *     is ever opened;
 *   - on a host without a native box the field is answered cancelled at once.
 * No game data; no visible window. Exit 0 = every check passed.
 */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* setenv / unsetenv */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "osk_text_entry.h"
#include "osk_overlay.h"

static int s_failures;
static int s_checks;

static void check(int ok, const char *what) {
    s_checks++;
    if (!ok) {
        s_failures++;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* ---- the fake native box -------------------------------------------------------------- */

static HANDLE s_box_entered;      /* the fake box started */
static HANDLE s_person_go;        /* the test lets the person answer (or the window appear) */
static HANDLE s_window_ready;     /* the fake box's window exists */
static HANDLE s_box_left;         /* the fake box returned */
static volatile LONG s_box_calls;
static volatile LONG s_box_thread;
static int s_box_answer;          /* what the person presses: 1 OK, 0 Cancel */
static int s_box_with_window;     /* create a real top-level window and wait for its cancel */
static volatile LONG s_window_closed_by_cancel;
static const wchar_t *s_box_text = L"ACE";

static LRESULT CALLBACK fake_box_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL) {
        InterlockedExchange(&s_window_closed_by_cancel, 1);
        DestroyWindow(w);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static int fake_box_body(wchar_t *out, int cap) {
    WaitForSingleObject(s_person_go, INFINITE);
    if (s_box_with_window) {
        WNDCLASSW wc;
        MSG m;
        HWND w;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = fake_box_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"NkOskSelftestBox";
        RegisterClassW(&wc);
        w = CreateWindowExW(0, wc.lpszClassName, L"fake box", WS_POPUP, 0, 0, 10, 10, NULL, NULL,
                            wc.hInstance, NULL);
        if (!w) return 0;
        SetEvent(s_window_ready);
        while (GetMessageW(&m, NULL, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        return 0;
    }
    if (s_box_answer && out && cap > 1) {
        int n = 0;
        while (n < cap - 1 && s_box_text[n]) {
            out[n] = s_box_text[n];
            n++;
        }
        out[n] = 0;
    }
    return s_box_answer;
}

int sr_osk_input(const wchar_t *desc, const wchar_t *initial, wchar_t *out, int cap) {
    int answer;
    (void)desc;
    (void)initial;
    InterlockedIncrement(&s_box_calls);
    InterlockedExchange(&s_box_thread, (LONG)GetCurrentThreadId());
    SetEvent(s_box_entered);
    answer = fake_box_body(out, cap);
    SetEvent(s_box_left);
    return answer;
}

static int poll_once(wchar_t *out, int cap) {
    return sr_osk_text_entry_poll(L"Enter name.", L"", out, cap, 0);
}

/* Poll until the request stops being pending, at most ~5 s. */
static int poll_until_answered(wchar_t *out, int cap) {
    for (int i = 0; i < 5000; i++) {
        int r = poll_once(out, cap);
        if (r != SR_OSK_TEXT_PENDING) return r;
        Sleep(1);
    }
    return SR_OSK_TEXT_PENDING;
}

/* Poll until a new box has been opened (the previous one may still be leaving). */
static int poll_until_new_box(LONG calls_before, wchar_t *out, int cap) {
    int r = SR_OSK_TEXT_PENDING;
    for (int i = 0; i < 5000 && s_box_calls == calls_before; i++) {
        r = poll_once(out, cap);
        if (r != SR_OSK_TEXT_PENDING) return r;
        Sleep(1);
    }
    return r;
}

static int signalled(HANDLE event) {
    return WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
}

static void reset_box(int answer, int with_window) {
    ResetEvent(s_box_entered);
    ResetEvent(s_person_go);
    ResetEvent(s_window_ready);
    ResetEvent(s_box_left);
    s_box_answer = answer;
    s_box_with_window = with_window;
    InterlockedExchange(&s_window_closed_by_cancel, 0);
}

static void test_poll_never_waits_for_the_person(void) {
    wchar_t out[16];
    LONG before;
    DWORD t0;
    int r;
    reset_box(1, 0);
    before = s_box_calls;
    t0 = GetTickCount();
    r = poll_once(out, 16);
    check(r == SR_OSK_TEXT_PENDING, "the first poll opens the request and reports it pending");
    check(signalled(s_box_entered), "the native box is shown for the open request");
    check((DWORD)s_box_thread != GetCurrentThreadId(),
          "the native box runs on a worker, never on the polling (scheduler) thread");
    for (int i = 0; i < 50; i++)
        check(poll_once(out, 16) == SR_OSK_TEXT_PENDING,
              "while the person has not answered every poll reports pending");
    check(GetTickCount() - t0 < 2000u, "polling an unanswered request never waits for it");
    check(s_box_calls == before + 1, "repeated polls do not open a second box");

    wcscpy(out, L"zz");
    SetEvent(s_person_go);
    r = poll_until_answered(out, 16);
    check(r == 1, "a confirmed answer is handed over once the person presses OK");
    check(wcscmp(out, L"ACE") == 0, "the confirmed text reaches the caller's buffer");

    reset_box(1, 0);
    r = poll_once(out, 16);
    check(r == SR_OSK_TEXT_PENDING && signalled(s_box_entered),
          "after an answer the next poll opens a new request");
    SetEvent(s_person_go);
    (void)poll_until_answered(out, 16);
}

static void test_cancel_and_capacity(void) {
    wchar_t out[16];
    int r;
    reset_box(0, 0);
    wcscpy(out, L"old");
    (void)sr_osk_text_entry_poll(L"Enter name.", L"old", out, 16, 0);
    SetEvent(s_person_go);
    r = poll_until_answered(out, 16);
    check(r == 0, "a cancelled box is reported cancelled");
    check(wcscmp(out, L"old") == 0, "a cancelled answer leaves the caller's buffer untouched");

    reset_box(1, 0);
    s_box_text = L"ABCDEFGH";
    (void)poll_once(out, 4);
    SetEvent(s_person_go);
    r = poll_until_answered(out, 4);
    check(r == 1 && wcscmp(out, L"ABC") == 0,
          "a confirmed answer is cut to the field's capacity and terminated");
    s_box_text = L"ACE";
}

static void test_abandon_closes_the_box(void) {
    wchar_t out[16];
    LONG calls;
    int r;

    /* Abandoned while the box is shown: the cancel closes it and its answer is dropped. */
    reset_box(1, 1);
    check(poll_once(out, 16) == SR_OSK_TEXT_PENDING && signalled(s_box_entered),
          "a box opens for the request");
    SetEvent(s_person_go);
    check(signalled(s_window_ready), "the box's window is created");
    sr_osk_text_entry_abandon();
    check(signalled(s_box_left) && s_window_closed_by_cancel,
          "abandoning a request closes a box that is shown");

    calls = s_box_calls;
    reset_box(1, 0);
    r = poll_until_new_box(calls, out, 16);
    check(r == SR_OSK_TEXT_PENDING && s_box_calls == calls + 1,
          "after an abandon no answer is handed over, and the next poll opens a new request "
          "once the old box is gone");
    SetEvent(s_person_go);
    r = poll_until_answered(out, 16);
    check(r == 1 && wcscmp(out, L"ACE") == 0, "the new request is answered normally");

    /* Abandoned before the box exists: the box is closed as soon as it is created. */
    reset_box(1, 1);
    check(poll_once(out, 16) == SR_OSK_TEXT_PENDING && signalled(s_box_entered),
          "another box is about to open");
    sr_osk_text_entry_abandon();
    SetEvent(s_person_go);
    check(signalled(s_box_left) && s_window_closed_by_cancel,
          "abandoning a request before its box exists closes the box when it is created");

    calls = s_box_calls;
    reset_box(1, 0);
    r = poll_until_new_box(calls, out, 16);
    SetEvent(s_person_go);
    r = poll_until_answered(out, 16);
    check(r == 1 && s_box_calls == calls + 1, "the request after it gets its own box");
}

static void test_offscreen_presenter_has_no_person(void) {
    wchar_t out[16];
    LONG before = s_box_calls;
    set_env("SR_VIDEO", "offscreen");
    for (int i = 0; i < 20; i++)
        check(poll_once(out, 16) == SR_OSK_TEXT_PENDING,
              "under the offscreen presenter the request stays pending");
    sr_osk_text_entry_abandon();
    check(s_box_calls == before, "under the offscreen presenter no box is ever opened");
    set_env("SR_VIDEO", NULL);
}

/* A presenter that draws the keyboard answers the field in the window: no box is shown, the
 * request waits for the person, and the answer is handed over once. */
static void test_overlay_host_answers_in_the_window(void) {
    wchar_t out[16];
    LONG before = s_box_calls;
    set_env("SR_VIDEO", NULL);
    sr_osk_overlay_set_host(1);
    check(sr_osk_text_entry_poll(L"Enter name.", L"old", out, 16, 0) == SR_OSK_TEXT_PENDING,
          "with the overlay host the request opens in the window and waits");
    Sleep(20);
    check(sr_osk_text_entry_poll(L"Enter name.", L"old", out, 16, 0) == SR_OSK_TEXT_PENDING,
          "the request is still pending while the person has not answered");
    check(s_box_calls == before, "with the overlay host no native box is ever shown");
    sr_osk_overlay_key(OSK_KEY_CONFIRM);
    check(sr_osk_text_entry_poll(L"Enter name.", L"old", out, 16, 0) == 1 && wcscmp(out, L"old") == 0,
          "start answers the overlay request with the text so far");
    check(sr_osk_text_entry_poll(L"Enter name.", L"", out, 16, 0) == SR_OSK_TEXT_PENDING,
          "the next field opens a new overlay request");
    sr_osk_overlay_key(OSK_KEY_CANCEL);
    check(sr_osk_text_entry_poll(L"Enter name.", L"", out, 16, 0) == 0,
          "circle cancels the overlay request");
    check(sr_osk_text_entry_poll(L"Enter name.", L"", out, 16, 0) == SR_OSK_TEXT_PENDING,
          "a poll after the cancel opens the next request");
    sr_osk_text_entry_abandon();
    check(!sr_osk_overlay_active(), "abandon closes the overlay request");
    check(s_box_calls == before, "abandon with the overlay host never shows a box");
    sr_osk_overlay_set_host(0);
}

int main(void) {
    s_box_entered = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_person_go = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_window_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_box_left = CreateEventW(NULL, TRUE, FALSE, NULL);
    set_env("SR_VIDEO", NULL);
    test_poll_never_waits_for_the_person();
    test_cancel_and_capacity();
    test_abandon_closes_the_box();
    test_offscreen_presenter_has_no_person();
    test_overlay_host_answers_in_the_window();
    printf("osk_text_entry_selftest: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}

#else /* !_WIN32 */

static int s_box_calls;

/* This host has no native box: the real one answers cancelled at once. */
int sr_osk_input(const wchar_t *desc, const wchar_t *initial, wchar_t *out, int cap) {
    (void)desc;
    (void)initial;
    (void)out;
    (void)cap;
    s_box_calls++;
    return 0;
}

int main(void) {
    wchar_t out[16];
    set_env("SR_VIDEO", NULL);
    wcscpy(out, L"old");
    check(sr_osk_text_entry_poll(L"Enter name.", L"old", out, 16, 0) == 0,
          "without a native box the field is answered cancelled at once");
    check(s_box_calls == 1 && wcscmp(out, L"old") == 0,
          "the cancelled answer leaves the caller's buffer untouched");
    set_env("SR_VIDEO", "offscreen");
    check(sr_osk_text_entry_poll(L"Enter name.", L"", out, 16, 0) == SR_OSK_TEXT_PENDING,
          "under the offscreen presenter the request stays pending");
    check(s_box_calls == 1, "under the offscreen presenter the box is never asked");
    sr_osk_text_entry_abandon();
    set_env("SR_VIDEO", NULL);
    /* With a presenter that draws the keyboard the field waits in the window, and no box is
     * asked. */
    sr_osk_overlay_set_host(1);
    check(sr_osk_text_entry_poll(L"Enter name.", L"old", out, 16, 0) == SR_OSK_TEXT_PENDING,
          "with the overlay host the request waits in the window");
    check(s_box_calls == 1, "with the overlay host the box is never asked");
    sr_osk_overlay_key(OSK_KEY_CONFIRM);
    check(sr_osk_text_entry_poll(L"Enter name.", L"old", out, 16, 0) == 1 && wcscmp(out, L"old") == 0,
          "start answers the overlay request");
    sr_osk_text_entry_abandon();
    sr_osk_overlay_set_host(0);
    printf("osk_text_entry_selftest: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}

#endif /* _WIN32 */
