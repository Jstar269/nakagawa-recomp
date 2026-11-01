/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* What the screens split out of scr_title.c share. Each screen is one file
   (scr_<screen>.c); what two or more of them use lives here and in
   scr_flow.c (the one-card frame, the files-a-disc-needs box) and
   scr_form.c (the setting rows the wizard and Settings share). The ids of
   the one-card flow pages are the FLOW screen's (nk_ui.h), so their macros
   are here; every other screen keeps its own in its own file. */

#ifndef NK_PLAYER_UI_SCR_SHARED_H
#define NK_PLAYER_UI_SCR_SHARED_H

#include "player_ui_nk.h"
#include "nk_ui_widgets.h"

#include <string.h>

static inline NkUiSizing sizing_grow(void) {
    NkUiSizing sizing = {NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    return sizing;
}

static inline NkUiSizing sizing_fit(void) {
    NkUiSizing sizing = {NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f};
    return sizing;
}

static inline NkUiSizing sizing_fixed(float value) {
    NkUiSizing sizing = {NK_UI_SIZE_FIXED, value, 0.0f, 0.0f};
    return sizing;
}

static inline NkUiLayout layout_make(NkUiLayoutDirection direction, NkUiSizing width,
                              NkUiSizing height, float gap) {
    NkUiLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.width = width;
    layout.height = height;
    layout.direction = direction;
    layout.gap = gap;
    return layout;
}

static inline PlayerUiAction action(PlayerUiActionKind kind, size_t title_index,
                             PlayerUiPrimaryAction primary) {
    PlayerUiAction result;
    result.kind = kind;
    result.primary = primary;
    result.title_index = title_index;
    return result;
}

static inline const char *safe_text(const char *text, const char *fallback) {
    return text != NULL && text[0] != '\0' ? text : fallback;
}

/* Whether a page's body between the top bar and the prompt bar scrolls. At the
   design text size every page fits the window, and the screen tests hold it to
   that; a larger text size can make one taller than the room, and then its
   body scrolls (with the focused control brought into view) instead of pushing
   the prompt bar off the window. */
static inline bool page_scrolls(const NkUi *ui) {
    return nk_ui_text_scale(ui) > 1.0f;
}

/* An outcome line's colour: only a warning is amber, and only a failure red. */
static inline uint32_t note_rgba(const NkUiTheme *theme, PlayerUiNoteKind kind) {
    switch (kind) {
    case PLAYER_UI_NOTE_DONE: return theme->ready;
    case PLAYER_UI_NOTE_WARNING: return theme->warning;
    case PLAYER_UI_NOTE_ERROR: return theme->error;
    case PLAYER_UI_NOTE_NEUTRAL:
    default: return theme->muted;
    }
}

#define FLOW_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_FLOW, (offset))
#define FLOW_ID_ROOT FLOW_ID(0x000001u)
#define FLOW_ID_TOP FLOW_ID(0x000010u)
#define FLOW_ID_BODY FLOW_ID(0x000020u)
#define FLOW_ID_CARD FLOW_ID(0x000021u)
#define FLOW_ID_EYEBROW FLOW_ID(0x000022u)
#define FLOW_ID_TITLE FLOW_ID(0x000023u)
#define FLOW_ID_LEAD FLOW_ID(0x000024u)
#define FLOW_ID_STAGES FLOW_ID(0x000030u)
#define FLOW_ID_STAGES_SPAN 0x10u
#define FLOW_ID_STAGE_LABELS FLOW_ID(0x000130u)
#define FLOW_ID_STAGE_LABELS_SPAN 0x10u
#define FLOW_ID_BAR FLOW_ID(0x000040u)
#define FLOW_ID_STATUS FLOW_ID(0x000041u)
#define FLOW_ID_STATUS_SPACER FLOW_ID(0x000141u)
#define FLOW_ID_MESSAGE FLOW_ID(0x000042u)
#define FLOW_ID_ELAPSED FLOW_ID(0x000043u)
#define FLOW_ID_NOTICE FLOW_ID(0x000044u)
#define FLOW_ID_LOG FLOW_ID(0x000050u)
#define FLOW_ID_LOG_SPAN 0x10u
#define FLOW_ID_ROWS FLOW_ID(0x000060u)
#define FLOW_ID_ROWS_SPAN 0x10u
#define FLOW_ID_ROWS_MORE FLOW_ID(0x000150u)       /* "and 3 more" under the rows */
#define FLOW_ID_ITEMS FLOW_ID(0x000200u)           /* Remove: + 0x10 * part + item */
#define FLOW_ID_ITEMS_SPAN 0x40u
#define FLOW_ID_TOTAL FLOW_ID(0x000070u)
#define FLOW_ID_ACTIONS FLOW_ID(0x000080u)
#define FLOW_ID_DETAILS FLOW_ID(0x000090u)
#define FLOW_ID_DETAILS_SPAN 0x8u
#define FLOW_ID_REPORT_TEXT FLOW_ID(0x000098u)
#define FLOW_ID_REPORT_ROW FLOW_ID(0x000099u)
#define FLOW_ID_PROMPTS FLOW_ID(0x0000A0u)
#define FLOW_ID_WAVES FLOW_ID(0x0000B0u)

#define FLOW_MARK_DIAMOND "\xE2\x97\x86"
#define FLOW_MARK_DOT "\xE2\x97\x8F"
#define FLOW_MARK_CROSS "\xC3\x97" /* multiplication sign: in every UI face */
#define FLOW_MARK_TRIANGLE "\xE2\x96\xB2"
#define FLOW_MARK_CHECK "\xE2\x9C\x93 "
#define FLOW_MIDDOT " \xC2\xB7 "

/* What one setting row shows: its button reads "name: value". */
typedef struct {
    const char *name;
    const char *value;
    const char *sentence;
} WizardRow;

/* How a group of setting rows shares its box: the button column they line up on,
   and whether each sentence sits under its button instead of beside it. */
typedef struct {
    float cell;
    bool stacked;
} WizardColumn;

/* The online and developer switches, in the wizard and in Settings. */
#define WIZARD_ONLINE_SENTENCE                                                    \
    "Check nakagawa.jaycast.net for news and let you send reports. Nothing is " \
    "sent without you."
#define WIZARD_DEVELOPER_SENTENCE \
    "Show a developer log on screen to help track down problems. Stays on this PC."

/* Frame and box helpers (scr_flow.c). */
float scr_flow_begin(NkUi *ui, float width, float height, float card_width);
void scr_flow_end(NkUi *ui, const char *hint, const NkUiPromptItem *prompts, size_t count);
void scr_flow_eyebrow(NkUi *ui, const char *marker, const char *text, uint32_t rgba);
NkUiStyle scr_flow_box_style(const NkUiTheme *theme, uint32_t fill);
void scr_flow_bytes(char *out, size_t cap, unsigned long long bytes);
bool scr_input_guide_present(const PlayerUiInputGuide *guide);
void scr_input_guide_box(NkUi *ui, NkUiId base, const PlayerUiInputGuide *guide, float width,
                         size_t max_rows);

/* Setting rows (scr_form.c). */
float scr_check_column_width(NkUi *ui, const PlayerUiCheckRow *checks, size_t count, bool status,
                             NkUiTypeRole role, const char *fallback, float design);
bool scr_wizard_box_begin(NkUi *ui, uint32_t edge);
WizardColumn scr_wizard_column(NkUi *ui, const WizardRow *rows, size_t count, float width);
bool scr_wizard_value(NkUi *ui, NkUiId id, NkUiId text_id, const char *name, const char *value,
                      const char *sentence, bool on, float width, const WizardColumn *column);
bool scr_wizard_value_state(NkUi *ui, NkUiId id, NkUiId text_id, const char *name,
                            const char *value, const char *sentence, bool on, bool disabled,
                            float width, const WizardColumn *column);
bool scr_wizard_switch(NkUi *ui, NkUiId id, NkUiId text_id, const char *name,
                       const char *sentence, bool on, float width, const WizardColumn *column);
WizardColumn scr_online_developer_column(NkUi *ui, bool online, bool developer, float width);

#endif /* NK_PLAYER_UI_SCR_SHARED_H */
