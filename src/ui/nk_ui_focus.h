/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_UI_FOCUS_H
#define NK_UI_FOCUS_H

/* Focus graph for the nk_ui core.

   Pure C over rectangles: no Clay, no SDL, no allocation after
   nk_ui_focus_graph_create except when the entry lists grow. Each frame the
   screen registers its focusable elements (rect = last layout, in logical UI
   pixels), then asks the graph to move. The graph keeps the focused id and one
   remembered id per scope between frames. */

#include "nk_ui.h"

/* Entry flags. */
enum {
    NK_UI_FOCUS_DISABLED = 1u << 0,   /* registered (keeps its rect) but never focusable */
    NK_UI_FOCUS_ROW_MEMBER = 1u << 1, /* left/right stay in this entry's row: a move
                                         never jumps to another row, it just fails at the end */
    NK_UI_FOCUS_DEFAULT = 1u << 2,    /* preferred first focus when its scope activates
                                         with no remembered id */
    NK_UI_FOCUS_NO_DEFAULT = 1u << 3  /* never taken as a scope's first focus (a destructive
                                         action): reached only by moving to it */
};

/* Scopes stack from the bottom: root, overlay, drawer, dialog. The highest scope that
   has an enabled entry this frame is the active scope and traps focus: directional
   moves, Tab and pointer hits never reach a lower scope. */
enum {
    NK_UI_SCOPE_ROOT = 0,
    NK_UI_SCOPE_OVERLAY = 1,
    NK_UI_SCOPE_DRAWER = 2,
    NK_UI_SCOPE_DIALOG = 3,
    NK_UI_FOCUS_MAX_SCOPES = 8
};

typedef struct {
    NkUiId id;          /* nonzero, unique within a frame */
    NkUiRect rect;      /* as laid out, including the current scroll offset */
    uint32_t flags;     /* NK_UI_FOCUS_* */
    uint32_t scope;     /* NK_UI_SCOPE_*; entries at or above NK_UI_FOCUS_MAX_SCOPES are rejected */
    NkUiId scroller;    /* 0, or the id of the registered scroll container that clips this entry */
} NkUiFocusEntry;

/* A scroll container. viewport is its visible rect; scroll_y and content_h describe
   the current vertical scroll state (scroll_y is how far the content has moved up,
   0 at the top). Horizontal scrolling uses scroll_x and content_w. */
typedef struct {
    NkUiId id;
    NkUiRect viewport;
    float scroll_x;
    float scroll_y;
    float content_w;
    float content_h;
} NkUiFocusScroller;

/* A request, produced by a focus move, to set a container's scroll offsets so the
   newly focused entry is fully visible. The host applies it to its scroll state. */
typedef struct {
    NkUiId scroller;
    float scroll_x;
    float scroll_y;
} NkUiFocusScrollRequest;

typedef struct NkUiFocusGraph NkUiFocusGraph;

NkUiFocusGraph *nk_ui_focus_graph_create(void);
void nk_ui_focus_graph_destroy(NkUiFocusGraph *graph);

/* Clears the registered entries and scrollers. Focus and scope memory persist. */
void nk_ui_focus_begin_frame(NkUiFocusGraph *graph);
/* Registers one entry. Declaration order is the Tab order. Returns false for a NULL
   argument, a zero or duplicate id, a non-finite or negative-size rect, a scope out
   of range, or out of memory. */
bool nk_ui_focus_register(NkUiFocusGraph *graph, const NkUiFocusEntry *entry);
bool nk_ui_focus_register_scroller(NkUiFocusGraph *graph, const NkUiFocusScroller *scroller);
/* Resolves the active scope and repairs focus: remembers the focus of the scope that
   was active, enters the new one (its remembered id, else a DEFAULT entry, else its
   first enabled entry), and drops focus that no longer exists or became disabled. */
void nk_ui_focus_end_frame(NkUiFocusGraph *graph);

NkUiId nk_ui_focus_current(const NkUiFocusGraph *graph);
/* The active scope of the last completed frame, or -1 when nothing is focusable. */
int nk_ui_focus_active_scope(const NkUiFocusGraph *graph);
/* The id remembered for a scope (0 if none). */
NkUiId nk_ui_focus_remembered(const NkUiFocusGraph *graph, uint32_t scope);

/* Focuses id when it is registered, enabled and in the active scope. Does not scroll. */
bool nk_ui_focus_set(NkUiFocusGraph *graph, NkUiId id);
/* Directional move within the active scope. Returns the newly focused id, or 0 when
   no move happened (nothing in that direction, or nothing focused). */
NkUiId nk_ui_focus_move(NkUiFocusGraph *graph, NkUiNavDir direction);
/* Tab / Shift+Tab: next or previous enabled entry of the active scope in
   declaration order, wrapping. With nothing focused it picks the first (or last). */
NkUiId nk_ui_focus_cycle(NkUiFocusGraph *graph, bool reverse);

/* The scroll request produced by the last successful move, cycle or set-with-scroll.
   Returns true once per request and clears it. */
bool nk_ui_focus_take_scroll(NkUiFocusGraph *graph, NkUiFocusScrollRequest *request);
/* Computes the scroll request that brings id fully into its container's viewport
   (minimal movement). Returns false when id has no scroller or is already visible. */
bool nk_ui_focus_scroll_into_view(const NkUiFocusGraph *graph, NkUiId id,
                                  NkUiFocusScrollRequest *request);

/* Pointer queries against the active scope. Hover never changes focus. */
NkUiId nk_ui_focus_hit_test(const NkUiFocusGraph *graph, float x, float y);
/* The scroll container under the point (the wheel target), or 0. */
NkUiId nk_ui_focus_scroller_at(const NkUiFocusGraph *graph, float x, float y);

/* Stateless best-candidate pick over a plain entry list, for tests and one-off use:
   the best enabled entry from the scope of `current` in `direction`, or `current`
   when there is none. A NULL list or unknown `current` returns `current`. */
NkUiId nk_ui_focus_directional(const NkUiFocusEntry *entries, size_t count,
                               NkUiId current, NkUiNavDir direction);

#endif /* NK_UI_FOCUS_H */
