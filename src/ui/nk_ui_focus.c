/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_ui_focus.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Direction scoring: score = main + 2 * cross, lower wins.
   Horizontal moves: main is the center distance along x; cross is 0 when the rows
   overlap and the center distance along y otherwise.
   Vertical moves: main is the center distance along y; cross is 0.3 * |dx| when the
   columns overlap and |dx| otherwise. Ties go to reading order (top, then left,
   then declaration order). */
#define NK_UI_FOCUS_EPS 0.01f
#define NK_UI_FOCUS_TIE 0.0001f
#define NK_UI_FOCUS_COLUMN_WEIGHT 0.3f
#define NK_UI_FOCUS_CROSS_WEIGHT 2.0f

struct NkUiFocusGraph {
    NkUiFocusEntry *entries;
    size_t count;
    size_t capacity;
    NkUiFocusScroller *scrollers;
    size_t scroller_count;
    size_t scroller_capacity;
    NkUiId focused;
    NkUiId memory[NK_UI_FOCUS_MAX_SCOPES];
    bool has_scroll;
    NkUiFocusScrollRequest scroll;
};

static bool nk_ui_focus_rect_valid(NkUiRect r) {
    return isfinite(r.x) && isfinite(r.y) && isfinite(r.w) && isfinite(r.h) &&
           r.w >= 0.0f && r.h >= 0.0f && isfinite(r.x + r.w) && isfinite(r.y + r.h);
}

static float nk_ui_focus_cx(NkUiRect r) { return r.x + r.w * 0.5f; }
static float nk_ui_focus_cy(NkUiRect r) { return r.y + r.h * 0.5f; }

static bool nk_ui_focus_overlap(float a0, float a1, float b0, float b1) {
    return a0 < b1 && b0 < a1;
}

static bool nk_ui_focus_enabled(const NkUiFocusEntry *e) {
    return (e->flags & NK_UI_FOCUS_DISABLED) == 0u;
}

/* Index of the entry with this id, or -1. */
static long nk_ui_focus_find(const NkUiFocusEntry *entries, size_t count, NkUiId id) {
    size_t i;
    if (id == 0u) {
        return -1;
    }
    for (i = 0; i < count; ++i) {
        if (entries[i].id == id) {
            return (long)i;
        }
    }
    return -1;
}

/* Highest scope that holds an enabled entry, or -1. */
static int nk_ui_focus_top_scope(const NkUiFocusEntry *entries, size_t count) {
    int top = -1;
    size_t i;
    for (i = 0; i < count; ++i) {
        if (nk_ui_focus_enabled(&entries[i]) && (int)entries[i].scope > top) {
            top = (int)entries[i].scope;
        }
    }
    return top;
}

static long nk_ui_focus_pick(const NkUiFocusEntry *entries, size_t count,
                             size_t current, NkUiNavDir dir) {
    const NkUiFocusEntry *cur = &entries[current];
    float cx = nk_ui_focus_cx(cur->rect);
    float cy = nk_ui_focus_cy(cur->rect);
    bool horizontal = dir == NK_UI_DIR_LEFT || dir == NK_UI_DIR_RIGHT;
    long best = -1;
    float best_score = 0.0f;
    size_t i;
    if (dir != NK_UI_DIR_LEFT && dir != NK_UI_DIR_RIGHT &&
        dir != NK_UI_DIR_UP && dir != NK_UI_DIR_DOWN) {
        return -1;
    }
    if (horizontal && (cur->flags & NK_UI_FOCUS_OWNS_HORIZONTAL) != 0u) {
        return -1;
    }
    for (i = 0; i < count; ++i) {
        const NkUiFocusEntry *c = &entries[i];
        float dx;
        float dy;
        float main_axis;
        float cross;
        float score;
        bool in_half_plane;
        if (i == current || !nk_ui_focus_enabled(c) || c->scope != cur->scope) {
            continue;
        }
        dx = nk_ui_focus_cx(c->rect) - cx;
        dy = nk_ui_focus_cy(c->rect) - cy;
        switch (dir) {
        case NK_UI_DIR_RIGHT: in_half_plane = dx > NK_UI_FOCUS_EPS; break;
        case NK_UI_DIR_LEFT: in_half_plane = dx < -NK_UI_FOCUS_EPS; break;
        case NK_UI_DIR_DOWN: in_half_plane = dy > NK_UI_FOCUS_EPS; break;
        default: in_half_plane = dy < -NK_UI_FOCUS_EPS; break;
        }
        if (!in_half_plane) {
            continue;
        }
        if (horizontal) {
            bool same_row = nk_ui_focus_overlap(cur->rect.y, cur->rect.y + cur->rect.h,
                                                c->rect.y, c->rect.y + c->rect.h);
            if (!same_row && (cur->flags & NK_UI_FOCUS_ROW_MEMBER) != 0u) {
                continue;
            }
            main_axis = fabsf(dx);
            cross = same_row ? 0.0f : fabsf(dy);
        } else {
            bool same_col = nk_ui_focus_overlap(cur->rect.x, cur->rect.x + cur->rect.w,
                                                c->rect.x, c->rect.x + c->rect.w);
            main_axis = fabsf(dy);
            cross = same_col ? NK_UI_FOCUS_COLUMN_WEIGHT * fabsf(dx) : fabsf(dx);
        }
        score = main_axis + NK_UI_FOCUS_CROSS_WEIGHT * cross;
        if (best < 0 || score < best_score - NK_UI_FOCUS_TIE) {
            best = (long)i;
            best_score = score;
        } else if (score <= best_score + NK_UI_FOCUS_TIE) {
            const NkUiFocusEntry *b = &entries[best];
            float by = nk_ui_focus_cy(b->rect);
            float bx = nk_ui_focus_cx(b->rect);
            float cyy = nk_ui_focus_cy(c->rect);
            float cxx = nk_ui_focus_cx(c->rect);
            if (cyy < by - NK_UI_FOCUS_TIE ||
                (fabsf(cyy - by) <= NK_UI_FOCUS_TIE && cxx < bx - NK_UI_FOCUS_TIE)) {
                best = (long)i;
                best_score = score;
            }
        }
    }
    return best;
}

NkUiId nk_ui_focus_directional(const NkUiFocusEntry *entries, size_t count,
                               NkUiId current, NkUiNavDir direction) {
    long cur;
    long next;
    if (entries == NULL) {
        return current;
    }
    cur = nk_ui_focus_find(entries, count, current);
    if (cur < 0) {
        return current;
    }
    next = nk_ui_focus_pick(entries, count, (size_t)cur, direction);
    return next >= 0 ? entries[next].id : current;
}

NkUiFocusGraph *nk_ui_focus_graph_create(void) {
    return (NkUiFocusGraph *)calloc(1, sizeof(NkUiFocusGraph));
}

void nk_ui_focus_graph_destroy(NkUiFocusGraph *graph) {
    if (graph == NULL) {
        return;
    }
    free(graph->entries);
    free(graph->scrollers);
    free(graph);
}

void nk_ui_focus_begin_frame(NkUiFocusGraph *graph) {
    if (graph == NULL) {
        return;
    }
    graph->count = 0;
    graph->scroller_count = 0;
}

bool nk_ui_focus_register(NkUiFocusGraph *graph, const NkUiFocusEntry *entry) {
    if (graph == NULL || entry == NULL || entry->id == 0u ||
        entry->scope >= NK_UI_FOCUS_MAX_SCOPES || !nk_ui_focus_rect_valid(entry->rect) ||
        nk_ui_focus_find(graph->entries, graph->count, entry->id) >= 0) {
        return false;
    }
    if (graph->count == graph->capacity) {
        size_t capacity = graph->capacity != 0u ? graph->capacity * 2u : 32u;
        NkUiFocusEntry *grown;
        if (capacity < graph->capacity) {
            return false;
        }
        grown = (NkUiFocusEntry *)realloc(graph->entries, capacity * sizeof(*grown));
        if (grown == NULL) {
            return false;
        }
        graph->entries = grown;
        graph->capacity = capacity;
    }
    graph->entries[graph->count++] = *entry;
    return true;
}

bool nk_ui_focus_register_scroller(NkUiFocusGraph *graph, const NkUiFocusScroller *scroller) {
    size_t i;
    if (graph == NULL || scroller == NULL || scroller->id == 0u ||
        !nk_ui_focus_rect_valid(scroller->viewport) || !isfinite(scroller->scroll_x) ||
        !isfinite(scroller->scroll_y) || !isfinite(scroller->content_w) ||
        !isfinite(scroller->content_h)) {
        return false;
    }
    for (i = 0; i < graph->scroller_count; ++i) {
        if (graph->scrollers[i].id == scroller->id) {
            return false;
        }
    }
    if (graph->scroller_count == graph->scroller_capacity) {
        size_t capacity = graph->scroller_capacity != 0u ? graph->scroller_capacity * 2u : 8u;
        NkUiFocusScroller *grown = (NkUiFocusScroller *)realloc(
            graph->scrollers, capacity * sizeof(*grown));
        if (grown == NULL) {
            return false;
        }
        graph->scrollers = grown;
        graph->scroller_capacity = capacity;
    }
    graph->scrollers[graph->scroller_count++] = *scroller;
    return true;
}

/* Picks the focus for a scope with no valid focus: its remembered id, a DEFAULT
   entry, then its first enabled entry. Returns 0 when the scope is empty. */
static NkUiId nk_ui_focus_enter_scope(const NkUiFocusGraph *graph, int scope) {
    long found;
    size_t i;
    if (scope < 0) {
        return 0u;
    }
    found = nk_ui_focus_find(graph->entries, graph->count, graph->memory[scope]);
    if (found >= 0 && nk_ui_focus_enabled(&graph->entries[found]) &&
        (int)graph->entries[found].scope == scope) {
        return graph->entries[found].id;
    }
    for (i = 0; i < graph->count; ++i) {
        const NkUiFocusEntry *e = &graph->entries[i];
        if ((int)e->scope == scope && nk_ui_focus_enabled(e) &&
            (e->flags & NK_UI_FOCUS_DEFAULT) != 0u) {
            return e->id;
        }
    }
    for (i = 0; i < graph->count; ++i) {
        const NkUiFocusEntry *e = &graph->entries[i];
        if ((int)e->scope == scope && nk_ui_focus_enabled(e)) {
            return e->id;
        }
    }
    return 0u;
}

void nk_ui_focus_end_frame(NkUiFocusGraph *graph) {
    int top;
    long cur;
    int scope;
    if (graph == NULL) {
        return;
    }
    top = nk_ui_focus_top_scope(graph->entries, graph->count);
    /* A scope above the active one has closed, so it forgets where it was. */
    for (scope = top + 1; scope < NK_UI_FOCUS_MAX_SCOPES; ++scope) {
        graph->memory[scope] = 0u;
    }
    if (top < 0) {
        graph->focused = 0u;
        return;
    }
    cur = nk_ui_focus_find(graph->entries, graph->count, graph->focused);
    if (cur < 0 || !nk_ui_focus_enabled(&graph->entries[cur]) ||
        (int)graph->entries[cur].scope != top) {
        graph->focused = nk_ui_focus_enter_scope(graph, top);
    }
    graph->memory[top] = graph->focused;
}

NkUiId nk_ui_focus_current(const NkUiFocusGraph *graph) {
    return graph != NULL ? graph->focused : 0u;
}

int nk_ui_focus_active_scope(const NkUiFocusGraph *graph) {
    return graph != NULL ? nk_ui_focus_top_scope(graph->entries, graph->count) : -1;
}

NkUiId nk_ui_focus_remembered(const NkUiFocusGraph *graph, uint32_t scope) {
    if (graph == NULL || scope >= NK_UI_FOCUS_MAX_SCOPES) {
        return 0u;
    }
    return graph->memory[scope];
}

bool nk_ui_focus_scroll_into_view(const NkUiFocusGraph *graph, NkUiId id,
                                  NkUiFocusScrollRequest *request) {
    long idx;
    size_t i;
    const NkUiFocusEntry *e;
    const NkUiFocusScroller *s = NULL;
    float nx;
    float ny;
    float max_x;
    float max_y;
    if (graph == NULL || request == NULL) {
        return false;
    }
    idx = nk_ui_focus_find(graph->entries, graph->count, id);
    if (idx < 0 || graph->entries[idx].scroller == 0u) {
        return false;
    }
    e = &graph->entries[idx];
    for (i = 0; i < graph->scroller_count; ++i) {
        if (graph->scrollers[i].id == e->scroller) {
            s = &graph->scrollers[i];
            break;
        }
    }
    if (s == NULL) {
        return false;
    }
    nx = s->scroll_x;
    ny = s->scroll_y;
    if (e->rect.y < s->viewport.y || e->rect.h > s->viewport.h) {
        ny += e->rect.y - s->viewport.y;
    } else if (e->rect.y + e->rect.h > s->viewport.y + s->viewport.h) {
        ny += (e->rect.y + e->rect.h) - (s->viewport.y + s->viewport.h);
    }
    if (e->rect.x < s->viewport.x || e->rect.w > s->viewport.w) {
        nx += e->rect.x - s->viewport.x;
    } else if (e->rect.x + e->rect.w > s->viewport.x + s->viewport.w) {
        nx += (e->rect.x + e->rect.w) - (s->viewport.x + s->viewport.w);
    }
    max_x = s->content_w - s->viewport.w;
    max_y = s->content_h - s->viewport.h;
    if (max_x < 0.0f) max_x = 0.0f;
    if (max_y < 0.0f) max_y = 0.0f;
    if (nx > max_x) nx = max_x;
    if (ny > max_y) ny = max_y;
    if (nx < 0.0f) nx = 0.0f;
    if (ny < 0.0f) ny = 0.0f;
    if (nx == s->scroll_x && ny == s->scroll_y) {
        return false;
    }
    request->scroller = s->id;
    request->scroll_x = nx;
    request->scroll_y = ny;
    return true;
}

static void nk_ui_focus_commit(NkUiFocusGraph *graph, NkUiId id, int scope) {
    graph->focused = id;
    if (scope >= 0 && scope < NK_UI_FOCUS_MAX_SCOPES) {
        graph->memory[scope] = id;
    }
    graph->has_scroll = nk_ui_focus_scroll_into_view(graph, id, &graph->scroll);
}

bool nk_ui_focus_set(NkUiFocusGraph *graph, NkUiId id) {
    long idx;
    int top;
    if (graph == NULL) {
        return false;
    }
    idx = nk_ui_focus_find(graph->entries, graph->count, id);
    top = nk_ui_focus_top_scope(graph->entries, graph->count);
    if (idx < 0 || !nk_ui_focus_enabled(&graph->entries[idx]) ||
        (int)graph->entries[idx].scope != top) {
        return false;
    }
    nk_ui_focus_commit(graph, id, top);
    return true;
}

bool nk_ui_focus_owns_horizontal(const NkUiFocusGraph *graph) {
    long cur;
    if (graph == NULL) {
        return false;
    }
    cur = nk_ui_focus_find(graph->entries, graph->count, graph->focused);
    return cur >= 0 && nk_ui_focus_enabled(&graph->entries[cur]) &&
           (graph->entries[cur].flags & NK_UI_FOCUS_OWNS_HORIZONTAL) != 0u;
}

NkUiId nk_ui_focus_move(NkUiFocusGraph *graph, NkUiNavDir direction) {
    long cur;
    long next;
    int top;
    if (graph == NULL) {
        return 0u;
    }
    top = nk_ui_focus_top_scope(graph->entries, graph->count);
    cur = nk_ui_focus_find(graph->entries, graph->count, graph->focused);
    if (cur < 0 || (int)graph->entries[cur].scope != top ||
        !nk_ui_focus_enabled(&graph->entries[cur])) {
        return 0u;
    }
    next = nk_ui_focus_pick(graph->entries, graph->count, (size_t)cur, direction);
    if (next < 0) {
        return 0u;
    }
    nk_ui_focus_commit(graph, graph->entries[next].id, top);
    return graph->focused;
}

NkUiId nk_ui_focus_cycle(NkUiFocusGraph *graph, bool reverse) {
    long cur;
    int top;
    size_t i;
    size_t n;
    size_t start;
    if (graph == NULL || graph->count == 0u) {
        return 0u;
    }
    top = nk_ui_focus_top_scope(graph->entries, graph->count);
    if (top < 0) {
        return 0u;
    }
    n = graph->count;
    cur = nk_ui_focus_find(graph->entries, graph->count, graph->focused);
    /* With no usable focus, start just before the first (or after the last) entry so
       the first step lands on the first (or last) enabled one. */
    if (cur < 0) {
        start = reverse ? 0u : n - 1u;
    } else {
        start = (size_t)cur;
    }
    for (i = 1; i <= n; ++i) {
        size_t idx = reverse ? (start + n - (i % n)) % n : (start + i) % n;
        const NkUiFocusEntry *e = &graph->entries[idx];
        if (nk_ui_focus_enabled(e) && (int)e->scope == top) {
            if (cur >= 0 && idx == (size_t)cur) {
                return 0u; /* the only focusable entry: nothing moves */
            }
            nk_ui_focus_commit(graph, e->id, top);
            return e->id;
        }
    }
    return 0u;
}

bool nk_ui_focus_take_scroll(NkUiFocusGraph *graph, NkUiFocusScrollRequest *request) {
    if (graph == NULL || request == NULL || !graph->has_scroll) {
        return false;
    }
    *request = graph->scroll;
    graph->has_scroll = false;
    return true;
}

NkUiId nk_ui_focus_hit_test(const NkUiFocusGraph *graph, float x, float y) {
    int top;
    size_t i;
    if (graph == NULL || !isfinite(x) || !isfinite(y)) {
        return 0u;
    }
    top = nk_ui_focus_top_scope(graph->entries, graph->count);
    for (i = graph->count; i > 0u; --i) {
        const NkUiFocusEntry *e = &graph->entries[i - 1u];
        if ((int)e->scope == top && nk_ui_focus_enabled(e) &&
            x >= e->rect.x && x < e->rect.x + e->rect.w &&
            y >= e->rect.y && y < e->rect.y + e->rect.h) {
            return e->id;
        }
    }
    return 0u;
}

NkUiId nk_ui_focus_scroller_at(const NkUiFocusGraph *graph, float x, float y) {
    size_t i;
    if (graph == NULL || !isfinite(x) || !isfinite(y)) {
        return 0u;
    }
    for (i = graph->scroller_count; i > 0u; --i) {
        const NkUiFocusScroller *s = &graph->scrollers[i - 1u];
        if (x >= s->viewport.x && x < s->viewport.x + s->viewport.w &&
            y >= s->viewport.y && y < s->viewport.y + s->viewport.h) {
            return s->id;
        }
    }
    return 0u;
}
