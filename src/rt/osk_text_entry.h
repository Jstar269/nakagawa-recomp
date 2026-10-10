/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * osk_text_entry.h - a person's answer to one on-screen-keyboard field, collected without
 * stopping guest time.
 *
 * The PSP keyboard is a system overlay: while it is open the title keeps running, polling
 * sceUtilityOskGetStatus once per frame. Every guest thread here is a coroutine on the one
 * scheduler thread, so the keyboard must never wait on host UI. The HLE keyboard polls the
 * open request instead: each call either reports it pending or hands over the answer.
 *
 *   sr_osk_text_entry_poll(desc, initial, out, cap, input_type)
 *     With no request open, opens one for this field (desc and initial are copied) and
 *     returns SR_OSK_TEXT_PENDING without waiting for it. While it is open it returns
 *     SR_OSK_TEXT_PENDING (the arguments are not read again). Once the person answered it
 *     returns 1 (confirmed; `out` holds the text, at most cap - 1 units and a terminator) or
 *     0 (cancelled, or the host UI failed; `out` is untouched), and the next call opens a
 *     new request. The caller asks for one field at a time, in order.
 *
 *   sr_osk_text_entry_abandon()
 *     The keyboard no longer wants the open request (the title shut it down or started a
 *     new one): its host UI is closed and a late answer is discarded. A request opened
 *     afterwards waits until the abandoned one's host UI is gone.
 *
 *   input_type is SceUtilityOskData.inputtype: the Latin categories it allows (osk_overlay.h).
 *
 * Who can answer:
 *   - a presenter that can draw the keyboard (the SDL3/Vulkan window, sr_osk_overlay_host):
 *     a person, on the in-window keyboard (src/rt/osk_overlay.c, drawn by the presenter).
 *     Nothing waits: the request is polled once per frame like the rest of the keyboard.
 *   - Windows without such a presenter: a person, in the native input box (src/rt/osk_win.c),
 *     shown on a worker thread so guest time keeps running. This is the fallback where the
 *     overlay cannot draw.
 *   - the offscreen presenter (SR_VIDEO=offscreen): nobody. Headless bring-up has no person
 *     and opens no window, so the request stays pending and the keyboard stays open, as on a
 *     PSP nobody is typing at; an automated route answers it with SR_OSK_SCRIPT/SR_OSK_TEXT.
 *   - a host without a native input box and without the overlay: the field is answered
 *     cancelled at once, the runtime's existing no-dialog result.
 */
#ifndef SR_OSK_TEXT_ENTRY_H
#define SR_OSK_TEXT_ENTRY_H

#include <stdint.h>
#include <wchar.h>

#define SR_OSK_TEXT_PENDING (-1)

int sr_osk_text_entry_poll(const wchar_t *desc, const wchar_t *initial, wchar_t *out, int cap,
                           uint32_t input_type);
void sr_osk_text_entry_abandon(void);

/* The native input box (src/rt/osk_win.c): blocks the calling thread until the person
 * confirms (1, text in out) or cancels (0). On Windows only the text-entry worker may call
 * it; hosts without a native box answer cancelled at once, so they call it directly. */
int sr_osk_input(const wchar_t *desc, const wchar_t *initial, wchar_t *out, int cap);

#endif /* SR_OSK_TEXT_ENTRY_H */
