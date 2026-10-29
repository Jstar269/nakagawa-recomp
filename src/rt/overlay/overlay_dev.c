/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The overlay's developer pages (#314), offered from the pause menu in developer mode
   only. The VRAM viewer shows a window of guest VRAM (or the bound texture) that the host
   decoded while the game is paused: Source, Address, Stride and Format are focusable rows
   that left and right change, and every change asks the host to decode the new window.
   The Profile page shows the nakagawa-perf-v1 counters the host formatted, with one bar
   per closed telemetry interval. Both pages only read what the model gives them. */

#include "nk_overlay.h"
#include "nk_ui_widgets.h"

#include <stdio.h>
#include <string.h>

#define DEV_ID_EYEBROW 0x70006001u
#define DEV_ID_TITLE 0x70006002u
#define DEV_ID_BODY 0x70006003u
#define DEV_ID_PICTURE 0x70006004u
#define DEV_ID_IMAGE 0x70006005u
#define DEV_ID_EMPTY 0x70006006u
#define DEV_ID_ROWS 0x70006007u
#define DEV_ID_INFO 0x70006008u
#define DEV_ID_NOTE 0x70006009u
#define DEV_ID_FOOT 0x7000600Au
#define DEV_ID_PART 0x70006100u     /* + 0x10 per row, + k per part */

#define PROFILE_ID_EYEBROW 0x70007001u
#define PROFILE_ID_TITLE 0x70007002u
#define PROFILE_ID_TABLE 0x70007003u
#define PROFILE_ID_GRAPH 0x70007004u
#define PROFILE_ID_LEGEND 0x70007005u
#define PROFILE_ID_NOTE 0x70007006u
#define PROFILE_ID_EMPTY 0x70007007u
#define PROFILE_ID_ROW 0x70007100u    /* + 4 per row */
#define PROFILE_ID_BAR 0x70007200u    /* + 8 per bar, + part */

#define DEV_ROW_HEIGHT 44.0f
#define DEV_PICTURE_W 480.0f
#define DEV_PICTURE_H 272.0f
#define DEV_COLUMN_W 340.0f
#define PROFILE_GRAPH_H 48.0f
#define PROFILE_BAR_W 8.0f

static const uint32_t k_formats[] = {0u, 1u, 2u, 3u, NK_OVERLAY_VRAM_DEPTH16};
#define FORMAT_COUNT (sizeof(k_formats) / sizeof(k_formats[0]))

uint32_t nk_overlay_vram_bpp(uint32_t format) {
    return format == 3u ? 4u : 2u;
}

static bool viewer_format(uint32_t format) {
    for (size_t i = 0; i < FORMAT_COUNT; ++i) {
        if (k_formats[i] == format) return true;
    }
    return false;
}

void nk_overlay_vram_clamp(NkOverlayVramSurface *view) {
    const uint32_t end = NK_OVERLAY_VRAM_BASE + NK_OVERLAY_VRAM_SIZE;
    uint32_t phys;
    uint32_t bpp;
    uint32_t room;
    if (view == NULL) return;
    if (!viewer_format(view->format)) view->format = 0u;
    bpp = nk_overlay_vram_bpp(view->format);
    /* The cached and uncached views and the mirrors all fold onto the 2 MiB. */
    phys = view->addr & 0x1FFFFFFFu;
    if ((phys & 0xFF000000u) == NK_OVERLAY_VRAM_BASE) {
        phys = NK_OVERLAY_VRAM_BASE | (phys & (NK_OVERLAY_VRAM_SIZE - 1u));
    } else {
        phys = NK_OVERLAY_VRAM_BASE;
    }
    phys -= (phys - NK_OVERLAY_VRAM_BASE) % bpp;
    view->addr = phys;
    if (view->stride == 0u) view->stride = 1u;
    if (view->stride > NK_OVERLAY_VRAM_MAX_STRIDE) view->stride = NK_OVERLAY_VRAM_MAX_STRIDE;
    if (view->width == 0u) view->width = 1u;
    if (view->width > view->stride) view->width = view->stride;
    if (view->width > NK_OVERLAY_VRAM_MAX_SIDE) view->width = NK_OVERLAY_VRAM_MAX_SIDE;
    if (view->height == 0u) view->height = 1u;
    if (view->height > NK_OVERLAY_VRAM_MAX_SIDE) view->height = NK_OVERLAY_VRAM_MAX_SIDE;
    /* The last pixel is at ((height - 1) * stride + width - 1) * bpp from the address. */
    room = (end - view->addr) / bpp;   /* whole pixels left in VRAM, at least one */
    if (view->width > room) view->width = room;
    if ((uint64_t)(view->height - 1u) * view->stride + view->width > room) {
        view->height = (room - view->width) / view->stride + 1u;
    }
}

void nk_overlay_vram_step(NkOverlayVramSurface *view, int dir) {
    const uint32_t end = NK_OVERLAY_VRAM_BASE + NK_OVERLAY_VRAM_SIZE;
    uint64_t window;
    uint64_t addr;
    if (view == NULL || dir == 0) return;
    nk_overlay_vram_clamp(view);
    window = (uint64_t)view->stride * view->height * nk_overlay_vram_bpp(view->format);
    addr = view->addr;
    if (dir > 0) {
        /* Forward until the next window would start past the last whole one. */
        uint64_t last = window < NK_OVERLAY_VRAM_SIZE ? end - window : NK_OVERLAY_VRAM_BASE;
        addr = addr + window <= last ? addr + window : (addr < last ? last : addr);
    } else {
        addr = addr >= NK_OVERLAY_VRAM_BASE + window ? addr - window : NK_OVERLAY_VRAM_BASE;
    }
    view->addr = (uint32_t)addr;
    nk_overlay_vram_clamp(view);
}

const char *nk_overlay_vram_format_name(uint32_t format) {
    static const char *const names[] = {
        "5650", "5551", "4444", "8888", "CLUT4", "CLUT8", "CLUT16",
        "CLUT32", "DXT1", "DXT3", "DXT5", "DEPTH16"
    };
    return format < sizeof(names) / sizeof(names[0]) ? names[format] : "UNKNOWN";
}

static const char *source_name(int source) {
    switch (source) {
    case NK_OVERLAY_VRAM_DISPLAY: return "Display framebuffer";
    case NK_OVERLAY_VRAM_DRAW: return "Draw framebuffer";
    case NK_OVERLAY_VRAM_DEPTH: return "Depth buffer";
    case NK_OVERLAY_VRAM_TEXTURE: return "Bound texture";
    default: return "Custom window";
    }
}

/* Puts the window on a source the GE state names; a source it does not name leaves the
   window where it is. The texture keeps its own place and format: the host decodes it
   through the texture decoder, not as a VRAM window. */
static bool choose_source(NkOverlayVramView *view, const NkOverlayVram *vram, int source) {
    if (vram == NULL || source < 0 || source >= (int)NK_OVERLAY_VRAM_SOURCE_COUNT ||
        !vram->present[source]) {
        return false;
    }
    view->source = source;
    view->view = vram->sources[source];
    if (source != NK_OVERLAY_VRAM_TEXTURE) nk_overlay_vram_clamp(&view->view);
    return true;
}

/* The person moved the window off its source: from the texture, it becomes a VRAM window
   in a format the viewer reads. */
static void make_custom(NkOverlayVramView *view) {
    if (view->source == NK_OVERLAY_VRAM_TEXTURE && !viewer_format(view->view.format)) {
        view->view.format = 0u;
    }
    view->source = NK_OVERLAY_VRAM_CUSTOM;
    nk_overlay_vram_clamp(&view->view);
}

static NkUiId part(unsigned row, unsigned k) {
    return (NkUiId)(DEV_ID_PART + row * 0x10u + k);
}

/* One focusable row: the label, then the value between arrows. Left and right belong to the
   row; returns -1, 1 or 0 for the change asked this frame (Confirm steps forward). */
static int value_row(NkUi *ui, NkUiId id, unsigned row, const char *label, const char *value) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    bool focused = nk_ui_button_focused(ui, id);
    NkUiLayout layout = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_grow(),
                                          nk_overlay_fixed(DEV_ROW_HEIGHT), NK_UI_SPACE_3);
    NkUiLayout cell = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_fixed(72.0f),
                                        nk_overlay_fit(), 0.0f);
    NkUiStyle style;
    NkUiNavDir dir = NK_UI_DIR_NONE;
    char shown[64];
    int change = 0;
    memset(&style, 0, sizeof(style));
    style.corner_radius = NK_UI_RADIUS_CONTROL;
    style.fill_rgba = focused ? theme->surface_high : 0u;
    layout.align_y = NK_UI_ALIGN_CENTER;
    layout.padding_left = layout.padding_right = NK_UI_SPACE_3;
    nk_ui_register_button(ui, id, NK_UI_BUTTON_OWNS_HORIZONTAL);
    if (nk_ui_begin_container(ui, id, &layout, &style)) {
        if (focused) nk_ui_focus_ring(ui, id, NK_UI_RADIUS_CONTROL);
        if (nk_ui_begin_container(ui, part(row, 0u), &cell, NULL)) {
            nk_ui_text_role(ui, part(row, 1u), label, NK_UI_TYPE_BODY_S, theme->muted);
            nk_ui_end_container(ui);
        }
        if (focused) {
            (void)snprintf(shown, sizeof(shown), "\xE2\x80\xB9 %s \xE2\x80\xBA", value);
        } else {
            (void)snprintf(shown, sizeof(shown), "%s", value);
        }
        nk_ui_text_role(ui, part(row, 2u), nk_ui_frame_text(ui, shown), NK_UI_TYPE_DATA,
                        focused ? theme->accent_dark : theme->paper);
        nk_ui_end_container(ui);
    }
    if (focused && nk_ui_take_event(ui, NK_UI_EV_NAV, &dir)) {
        change = dir == NK_UI_DIR_RIGHT ? 1 : dir == NK_UI_DIR_LEFT ? -1 : 0;
    } else if (nk_ui_take_activation(ui, id)) {
        change = 1;
    }
    return change;
}

/* The next source the GE state names, `dir` along the list from `from` (wrapping); `from`
   when none other is named. */
static int next_source(const NkOverlayVram *vram, int from, int dir) {
    int count = (int)NK_OVERLAY_VRAM_SOURCE_COUNT;
    int at = from;
    for (int i = 0; i < count; ++i) {
        at = (at + dir + count) % count;
        if (vram != NULL && vram->present[at]) return at;
    }
    return from;
}

static size_t format_index(uint32_t format) {
    for (size_t i = 0; i < FORMAT_COUNT; ++i) {
        if (k_formats[i] == format) return i;
    }
    return 0u;
}

static void vram_picture(NkUi *ui, const NkOverlayVram *vram, float width) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiLayout box = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_fixed(width),
                                       nk_overlay_fixed(DEV_PICTURE_H * width / DEV_PICTURE_W),
                                       0.0f);
    NkUiStyle style;
    memset(&style, 0, sizeof(style));
    style.fill_rgba = theme->surface;
    style.border_rgba = theme->line_strong;
    style.border_width = NK_UI_BORDER_HAIRLINE;
    box.align_x = NK_UI_ALIGN_CENTER;
    box.align_y = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, DEV_ID_PICTURE, &box, &style)) return;
    if (vram != NULL && vram->image != NULL) {
        NkUiImageArt art;
        memset(&art, 0, sizeof(art));
        art.contain = vram->image;
        art.contain_scale = 1.0f;
        (void)nk_ui_image_art(ui, DEV_ID_IMAGE, nk_overlay_grow(), nk_overlay_grow(), &art);
    } else {
        nk_ui_text_role(ui, DEV_ID_EMPTY, "Nothing to show", NK_UI_TYPE_CAPTION, theme->dim);
    }
    nk_ui_end_container(ui);
}

NkOverlayAction nk_overlay_page_vram(NkUi *ui, NkOverlayState *state, const NkOverlayModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_MOVE, "Move"},
        {NK_UI_PROMPT_ACT_CONFIRM, "Change"},
        {NK_UI_PROMPT_ACT_BACK, "Back"}
    };
    const NkUiTheme *theme = nk_ui_theme(ui);
    const NkOverlayVram *vram = model->vram;
    NkOverlayVramView *view = &state->vram;
    NkOverlayAction result = nk_overlay_action(NK_OVERLAY_ACT_NONE, 0);
    bool moved = false;
    float inner;
    float picture;
    char text[96];
    int change;
    if (!view->chosen) {
        /* The display framebuffer first, or the first source the GE state names. */
        view->chosen = true;
        view->source = NK_OVERLAY_VRAM_CUSTOM;
        memset(&view->view, 0, sizeof(view->view));
        view->view.addr = NK_OVERLAY_VRAM_BASE;
        view->view.stride = 512u;
        view->view.width = 480u;
        view->view.height = 272u;
        if (!choose_source(view, vram, NK_OVERLAY_VRAM_DISPLAY)) {
            (void)choose_source(view, vram, next_source(vram, NK_OVERLAY_VRAM_DISPLAY, 1));
        }
        nk_overlay_vram_clamp(&view->view);
        moved = true;
    }
    inner = nk_overlay_page_begin(ui, model, 920.0f);
    if (inner <= 0.0f) return result;
    picture = inner - DEV_COLUMN_W - NK_UI_SPACE_6;
    if (picture > DEV_PICTURE_W) picture = DEV_PICTURE_W;
    if (picture < 240.0f) picture = 240.0f;
    nk_overlay_eyebrow(ui, DEV_ID_EYEBROW, NULL, "Game paused \xC2\xB7 Developer", theme->muted);
    nk_ui_text_role(ui, DEV_ID_TITLE, "VRAM viewer", NK_UI_TYPE_TITLE, theme->paper);
    {
        NkUiLayout body = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_grow(), nk_overlay_fit(),
                                            NK_UI_SPACE_6);
        if (nk_ui_begin_container(ui, DEV_ID_BODY, &body, NULL)) {
            NkUiLayout rows = nk_overlay_layout(NK_UI_LAYOUT_COLUMN, nk_overlay_grow(),
                                                nk_overlay_fit(), NK_UI_SPACE_1);
            vram_picture(ui, vram, picture);
            if (nk_ui_begin_container(ui, DEV_ID_ROWS, &rows, NULL)) {
                NkOverlayVramSurface *w = &view->view;
                change = value_row(ui, NK_OVERLAY_ID_VRAM_SOURCE, 0u, "Source",
                                   source_name(view->source));
                if (change != 0) {
                    /* From a moved window either way goes back to the first named source. */
                    int to = view->source == NK_OVERLAY_VRAM_CUSTOM
                        ? next_source(vram, (int)NK_OVERLAY_VRAM_SOURCE_COUNT - 1, 1)
                        : next_source(vram, view->source, change);
                    moved = choose_source(view, vram, to) || moved;
                }
                (void)snprintf(text, sizeof(text), "0x%08X", (unsigned)w->addr);
                change = value_row(ui, NK_OVERLAY_ID_VRAM_ADDRESS, 1u, "Address", text);
                if (change != 0) {
                    make_custom(view);
                    nk_overlay_vram_step(w, change);
                    moved = true;
                }
                (void)snprintf(text, sizeof(text), "%u px", (unsigned)w->stride);
                change = value_row(ui, NK_OVERLAY_ID_VRAM_STRIDE, 2u, "Stride", text);
                if (change != 0) {
                    uint32_t step = NK_OVERLAY_VRAM_STRIDE_STEP;
                    make_custom(view);
                    if (change > 0) {
                        w->stride = (w->stride / step + 1u) * step;
                    } else {
                        w->stride = w->stride > step ? ((w->stride - 1u) / step) * step : step;
                    }
                    if (w->stride > NK_OVERLAY_VRAM_MAX_STRIDE) w->stride = NK_OVERLAY_VRAM_MAX_STRIDE;
                    w->width = w->stride < NK_OVERLAY_VRAM_MAX_SIDE ? w->stride
                                                                    : NK_OVERLAY_VRAM_MAX_SIDE;
                    w->height = 272u;
                    nk_overlay_vram_clamp(w);
                    moved = true;
                }
                change = value_row(ui, NK_OVERLAY_ID_VRAM_FORMAT, 3u, "Format",
                                   nk_overlay_vram_format_name(w->format));
                if (change != 0) {
                    size_t at;
                    make_custom(view);
                    at = (format_index(w->format) + (change > 0 ? 1u : FORMAT_COUNT - 1u)) %
                         FORMAT_COUNT;
                    w->format = k_formats[at];
                    nk_overlay_vram_clamp(w);
                    moved = true;
                }
                (void)snprintf(text, sizeof(text), "%u \xC3\x97 %u px%s", (unsigned)w->width,
                               (unsigned)w->height,
                               view->source == NK_OVERLAY_VRAM_TEXTURE && vram != NULL &&
                                       vram->texture_swizzled ? ", swizzled" : "");
                nk_ui_text_role(ui, DEV_ID_INFO, nk_ui_frame_text(ui, text), NK_UI_TYPE_DATA,
                                theme->muted);
                if (vram != NULL && vram->note != NULL && vram->note[0] != '\0') {
                    NkUiTextStyle note = nk_ui_type(NK_UI_TYPE_CAPTION, theme->warning);
                    note.wrap = true;
                    nk_ui_text(ui, DEV_ID_NOTE, vram->note, &note);
                }
                nk_ui_end_container(ui);
            }
            nk_ui_end_container(ui);
        }
    }
    {
        NkUiTextStyle foot = nk_ui_type(NK_UI_TYPE_FOOTNOTE, theme->dim);
        foot.wrap = true;
        nk_ui_text(ui, DEV_ID_FOOT,
                   "Read-only. The paused game's memory as the guest would read it; nothing "
                   "is written to it.", &foot);
    }
    if (nk_ui_button(ui, NK_OVERLAY_ID_VRAM_BACK, "Back", 0u) ||
        nk_ui_take_event(ui, NK_UI_EV_BACK, NULL)) {
        nk_overlay_go(state, NK_OVERLAY_SCREEN_MENU, NK_OVERLAY_ID_VRAM);
    }
    nk_overlay_page_end(ui, "", prompts, sizeof(prompts) / sizeof(prompts[0]));
    if (moved) result = nk_overlay_action(NK_OVERLAY_ACT_VRAM_VIEW, 0);
    return result;
}

/* One bar: the interval's parts stacked from the bottom, CPU lowest. */
static void profile_bar(NkUi *ui, size_t index, const float *parts) {
    const NkUiTheme *theme = nk_ui_theme(ui);
    const uint32_t colours[NK_OVERLAY_PROFILE_PART_COUNT] = {
        theme->accent_dark, theme->warning, theme->info, theme->line_strong
    };
    NkUiId id = (NkUiId)(PROFILE_ID_BAR + (NkUiId)index * 8u);
    NkUiLayout bar = nk_overlay_layout(NK_UI_LAYOUT_COLUMN, nk_overlay_fixed(PROFILE_BAR_W),
                                       nk_overlay_fixed(PROFILE_GRAPH_H), 0.0f);
    bar.align_y = NK_UI_ALIGN_END;
    if (!nk_ui_begin_container(ui, id, &bar, NULL)) return;
    for (int p = NK_OVERLAY_PROFILE_PART_COUNT - 1; p >= 0; --p) {
        float share = parts[p] > 0.0f ? (parts[p] < 1.0f ? parts[p] : 1.0f) : 0.0f;
        float height = (float)(int)(share * PROFILE_GRAPH_H + 0.5f);
        NkUiLayout piece;
        NkUiStyle style;
        if (height < 1.0f) continue;
        piece = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_grow(), nk_overlay_fixed(height),
                                  0.0f);
        memset(&style, 0, sizeof(style));
        style.fill_rgba = colours[p];
        if (nk_ui_begin_container(ui, (NkUiId)(id + 1u + (NkUiId)p), &piece, &style)) {
            nk_ui_end_container(ui);
        }
    }
    nk_ui_end_container(ui);
}

NkOverlayAction nk_overlay_page_profile(NkUi *ui, NkOverlayState *state,
                                        const NkOverlayModel *model) {
    static const NkUiPromptItem prompts[] = {
        {NK_UI_PROMPT_ACT_CONFIRM, "Select"},
        {NK_UI_PROMPT_ACT_BACK, "Back"}
    };
    const NkUiTheme *theme = nk_ui_theme(ui);
    const NkOverlayProfile *profile = model->profile;
    NkOverlayAction result = nk_overlay_action(NK_OVERLAY_ACT_NONE, 0);
    size_t rows = profile != NULL ? profile->row_count : 0u;
    size_t bars = profile != NULL ? profile->bar_count : 0u;
    float inner;
    if (rows > NK_OVERLAY_PROFILE_ROWS) rows = NK_OVERLAY_PROFILE_ROWS;
    if (bars > NK_OVERLAY_PROFILE_BARS) bars = NK_OVERLAY_PROFILE_BARS;
    inner = nk_overlay_page_begin(ui, model, 760.0f);
    if (inner <= 0.0f) return result;
    nk_overlay_eyebrow(ui, PROFILE_ID_EYEBROW, NULL, "Game paused \xC2\xB7 Developer", theme->muted);
    nk_ui_text_role(ui, PROFILE_ID_TITLE, "Profile", NK_UI_TYPE_TITLE, theme->paper);
    if (rows == 0u) {
        nk_ui_text_role(ui, PROFILE_ID_EMPTY, "No interval has closed yet.", NK_UI_TYPE_BODY,
                        theme->muted);
    } else {
        NkUiLayout table = nk_overlay_layout(NK_UI_LAYOUT_COLUMN, nk_overlay_grow(),
                                             nk_overlay_fit(), 2.0f);
        if (nk_ui_begin_container(ui, PROFILE_ID_TABLE, &table, NULL)) {
            for (size_t i = 0; i < rows; ++i) {
                NkUiId id = (NkUiId)(PROFILE_ID_ROW + (NkUiId)i * 4u);
                NkUiLayout row = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_grow(),
                                                   nk_overlay_fixed(18.0f), NK_UI_SPACE_3);
                NkUiLayout cell = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_fixed(120.0f),
                                                    nk_overlay_fit(), 0.0f);
                const NkOverlayProfileRow *r = &profile->rows[i];
                row.align_y = NK_UI_ALIGN_CENTER;
                if (!nk_ui_begin_container(ui, id, &row, NULL)) continue;
                if (nk_ui_begin_container(ui, (NkUiId)(id + 1u), &cell, NULL)) {
                    nk_ui_text_role(ui, (NkUiId)(id + 2u), r->label != NULL ? r->label : "",
                                    NK_UI_TYPE_BODY_S, theme->muted);
                    nk_ui_end_container(ui);
                }
                nk_ui_text_role(ui, (NkUiId)(id + 3u), r->value != NULL ? r->value : "",
                                NK_UI_TYPE_DATA, theme->paper);
                nk_ui_end_container(ui);
            }
            nk_ui_end_container(ui);
        }
    }
    if (bars > 0u) {
        NkUiLayout graph = nk_overlay_layout(NK_UI_LAYOUT_ROW, nk_overlay_grow(),
                                             nk_overlay_fixed(PROFILE_GRAPH_H), 2.0f);
        size_t shown = bars;
        /* As many of the newest bars as the card holds. */
        size_t fit = (size_t)((inner + 2.0f) / (PROFILE_BAR_W + 2.0f));
        if (shown > fit) shown = fit;
        graph.align_x = NK_UI_ALIGN_END;
        graph.align_y = NK_UI_ALIGN_END;
        if (nk_ui_begin_container(ui, PROFILE_ID_GRAPH, &graph, NULL)) {
            for (size_t i = bars - shown; i < bars; ++i) profile_bar(ui, i, profile->bars[i]);
            nk_ui_end_container(ui);
        }
        nk_ui_text_role(ui, PROFILE_ID_LEGEND,
                        "One bar per second, newest right: cpu, ge wait, present, idle",
                        NK_UI_TYPE_CAPTION, theme->muted);
    }
    if (profile != NULL && profile->note != NULL && profile->note[0] != '\0') {
        NkUiTextStyle note = nk_ui_type(NK_UI_TYPE_FOOTNOTE, theme->dim);
        note.wrap = true;
        nk_ui_text(ui, PROFILE_ID_NOTE, profile->note, &note);
    }
    if (nk_ui_button(ui, NK_OVERLAY_ID_PROFILE_BACK, "Back", 0u) ||
        nk_ui_take_event(ui, NK_UI_EV_BACK, NULL)) {
        nk_overlay_go(state, NK_OVERLAY_SCREEN_MENU, NK_OVERLAY_ID_PROFILE);
    }
    nk_overlay_page_end(ui, "", prompts, sizeof(prompts) / sizeof(prompts[0]));
    return result;
}
