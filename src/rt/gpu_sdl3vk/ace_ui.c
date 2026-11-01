/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * ace_ui.c - Ace Mode's in-game UI pass (contract in ace_ui.h).
 */
/* Every Vulkan call goes through a loaded pointer: no prototype, no loader dependency. */
#define VK_NO_PROTOTYPES 1
#define NK_UI_WITH_VULKAN 1
#include "ace_ui.h"

#include "nk_ui_theme.h"
#include "backend/nk_ui_vk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ACE_UI_MAX_IMAGES 8u

struct AceUi {
    AceUiInit init;
    NkUi *ui;
    NkUiVk *vk;
    VkFormat format;                         /* the render pass's and the backend's */
    VkRenderPass pass;
    uint64_t gen;                            /* the swapchain the views below were made for */
    int have_gen;
    uint32_t image_count;
    VkExtent2D extent;
    VkImageView views[ACE_UI_MAX_IMAGES];
    VkFramebuffer fbs[ACE_UI_MAX_IMAGES];
    PFN_vkCreateRenderPass CreateRenderPass;
    PFN_vkDestroyRenderPass DestroyRenderPass;
    PFN_vkCreateImageView CreateImageView;
    PFN_vkDestroyImageView DestroyImageView;
    PFN_vkCreateFramebuffer CreateFramebuffer;
    PFN_vkDestroyFramebuffer DestroyFramebuffer;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
    PFN_vkCmdEndRenderPass CmdEndRenderPass;
    PFN_vkQueueWaitIdle QueueWaitIdle;
};

static void set_error(char *out, size_t size, const char *text) {
    if (out && size) snprintf(out, size, "%s", text);
}

/* ---- layout ------------------------------------------------------------------------------- */

static NkUiSizing sizing(NkUiSizeKind kind, float value) {
    NkUiSizing s;
    memset(&s, 0, sizeof(s));
    s.kind = kind;
    s.value = value;
    return s;
}

static NkUiLayout layout(NkUiLayoutDirection direction, NkUiSizing w, NkUiSizing h, float gap) {
    NkUiLayout l;
    memset(&l, 0, sizeof(l));
    l.direction = direction;
    l.width = w;
    l.height = h;
    l.gap = gap;
    return l;
}

/* 0xRRGGBBAA with its alpha scaled by `a` (0 to 1). */
static uint32_t faded(uint32_t rgba, float a) {
    uint32_t alpha = rgba & 0xFFu;
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    return (rgba & 0xFFFFFF00u) | (uint32_t)((float)alpha * a + 0.5f);
}

/* Every run here is one line. Clay sizes a run's box from its words measured one by one, which
 * can come out a pixel or two narrower than the run shaped whole, and the Vulkan backend clips a
 * run to its box; so each run is laid out with its spaces made no-break spaces, which keeps it
 * one slice, measured and drawn alike (as the launcher keeps a folder path on one line). */
static const char *one_line(NkUi *ui, const char *text) {
    char buf[1024];
    size_t used = 0;
    for (const char *p = text; *p && used + 3u < sizeof(buf); p++) {
        if (*p == ' ') {
            buf[used++] = (char)0xC2;
            buf[used++] = (char)0xA0;
        } else {
            buf[used++] = *p;
        }
    }
    buf[used] = '\0';
    return nk_ui_frame_text(ui, buf);
}

/* A chip: surface_high with a line_strong edge, or paper on it with a strong edge in high
 * contrast; the edge is part of the shape, never the only cue. */
static NkUiStyle chip_style(const NkUiTheme *theme, int high_contrast, float a) {
    NkUiStyle style;
    memset(&style, 0, sizeof(style));
    style.fill_rgba = faded(theme->surface_high, a);
    style.border_rgba = faded(high_contrast ? theme->paper : theme->line_strong, a);
    style.border_width = high_contrast ? 2.0f : NK_UI_BORDER_HAIRLINE;
    style.corner_radius = NK_UI_RADIUS_CHIP;
    return style;
}

/* UTF-16 units to UTF-8, a lone surrogate as U+FFFD; returns bytes written. */
static size_t utf8_of(const uint16_t *units, int count, char *out, size_t cap) {
    size_t used = 0;
    if (!cap) return 0;
    for (int i = 0; i < count && units[i]; i++) {
        uint32_t cp = units[i];
        if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1 < count &&
            units[i + 1] >= 0xDC00u && units[i + 1] <= 0xDFFFu) {
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (units[i + 1] - 0xDC00u);
            i++;
        } else if (cp >= 0xD800u && cp <= 0xDFFFu) {
            cp = 0xFFFDu;
        }
        char b[4];
        size_t n;
        if (cp < 0x80u) { b[0] = (char)cp; n = 1; }
        else if (cp < 0x800u) { b[0] = (char)(0xC0u | (cp >> 6)); b[1] = (char)(0x80u | (cp & 0x3Fu)); n = 2; }
        else if (cp < 0x10000u) {
            b[0] = (char)(0xE0u | (cp >> 12)); b[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            b[2] = (char)(0x80u | (cp & 0x3Fu)); n = 3;
        } else {
            b[0] = (char)(0xF0u | (cp >> 18)); b[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            b[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu)); b[3] = (char)(0x80u | (cp & 0x3Fu)); n = 4;
        }
        if (used + n + 1u > cap) break;
        memcpy(out + used, b, n);
        used += n;
    }
    out[used] = '\0';
    return used;
}

static const char *cell_label(const OskCell *c, char ch[2]) {
    switch (c->kind) {
    case OSK_CELL_CHAR: ch[0] = c->ch; ch[1] = '\0'; return ch;
    case OSK_CELL_SPACE: return "SPACE";
    case OSK_CELL_BKSP: return "BKSP";
    case OSK_CELL_CASE: return "CASE";
    case OSK_CELL_CANCEL: return "CANCEL";
    case OSK_CELL_OK: return "OK";
    default: return "";
    }
}

/* The keyboard: the guest's description, the text with its caret, the grid and the controls,
 * in one panel at the foot of the window. Input still reaches it the way it does today (the
 * pad and the keyboard through the session API); this only draws it. */
static void keyboard_layout(NkUi *ui, const NkUiTheme *theme, const OskOverlay *o,
                            float logical_w, float logical_h, int high_contrast) {
    float panel_w = logical_w - 2.0f * NK_UI_SPACE_4;
    if (panel_w > 720.0f) panel_w = 720.0f;
    float inner = panel_w - 2.0f * NK_UI_SPACE_4;
    float unit = inner / 10.0f;
    /* Seven rows, the text around them and the chips above fit the window: the keys shrink
     * before anything is cut off. Outside the rows the panel takes about 144 px (its padding,
     * the description, the field, the hint and their gaps) and the chips about 62. */
    float key_h = (logical_h - 2.0f * NK_UI_SPACE_4 - 62.0f - 144.0f) / (float)OSK_OVERLAY_ROWS;
    if (key_h > 34.0f) key_h = 34.0f;
    if (key_h < 14.0f) key_h = 14.0f;
    NkUiLayout panel = layout(NK_UI_LAYOUT_COLUMN, sizing(NK_UI_SIZE_FIXED, panel_w),
                              sizing(NK_UI_SIZE_FIT, 0.0f), NK_UI_SPACE_2);
    panel.padding_left = panel.padding_right = NK_UI_SPACE_4;
    panel.padding_top = panel.padding_bottom = NK_UI_SPACE_3;
    NkUiStyle panel_style;
    memset(&panel_style, 0, sizeof(panel_style));
    panel_style.fill_rgba = faded(theme->surface_raised, 0.94f);
    panel_style.border_rgba = high_contrast ? theme->paper : theme->line_strong;
    panel_style.border_width = high_contrast ? 2.0f : NK_UI_BORDER_HAIRLINE;
    panel_style.corner_radius = NK_UI_RADIUS_TILE;
    if (!nk_ui_begin_container(ui, ACE_UI_ID_KEYBOARD, &panel, &panel_style)) return;

    char text[OSK_OVERLAY_TEXT_MAX * 4 + 4];
    NkUiTextStyle desc_style = nk_ui_type(NK_UI_TYPE_BODY_S, theme->muted);
    desc_style.wrap = false;
    (void)utf8_of(o->desc, OSK_OVERLAY_DESC_MAX, text, sizeof(text));
    if (text[0])
        nk_ui_text(ui, ACE_UI_ID_KEYBOARD_DESC,
                   one_line(ui, nk_ui_text_clamp(ui, text, &desc_style, inner, 1)), &desc_style);

    NkUiLayout field = layout(NK_UI_LAYOUT_ROW, sizing(NK_UI_SIZE_GROW, 0.0f),
                              sizing(NK_UI_SIZE_FIT, 0.0f), 0.0f);
    field.padding_left = field.padding_right = NK_UI_SPACE_3;
    field.padding_top = field.padding_bottom = NK_UI_SPACE_2;
    NkUiStyle field_style;
    memset(&field_style, 0, sizeof(field_style));
    field_style.fill_rgba = theme->surface;
    field_style.border_rgba = high_contrast ? theme->paper : theme->line_strong;
    field_style.border_width = NK_UI_BORDER_HAIRLINE;
    field_style.corner_radius = NK_UI_RADIUS_CONTROL;
    if (nk_ui_begin_container(ui, ACE_UI_ID_KEYBOARD_FIELD, &field, &field_style)) {
        size_t n = utf8_of(o->text, o->len, text, sizeof(text) - 2u);
        text[n] = '_';                       /* the caret, where the next character goes */
        text[n + 1u] = '\0';
        NkUiTextStyle field_text = nk_ui_type(NK_UI_TYPE_DATA, theme->paper);
        field_text.wrap = false;
        /* The end of the text is what the person is typing: a long one keeps its end. */
        const char *shown = text;
        while (shown[0] && nk_ui_text_width(ui, shown, &field_text) > inner - 2.0f * NK_UI_SPACE_3) {
            shown++;
            while (((unsigned char)shown[0] & 0xC0u) == 0x80u) shown++;
        }
        nk_ui_text(ui, ACE_UI_ID_KEYBOARD_TEXT, one_line(ui, shown), &field_text);
        nk_ui_end_container(ui);
    }

    OskCell cells[OSK_OVERLAY_CELLS];
    int n = osk_overlay_cells(o, cells, OSK_OVERLAY_CELLS);
    NkUiLayout grid = layout(NK_UI_LAYOUT_COLUMN, sizing(NK_UI_SIZE_GROW, 0.0f),
                             sizing(NK_UI_SIZE_FIT, 0.0f), 0.0f);
    int grid_open = nk_ui_begin_container(ui, ACE_UI_ID_KEYBOARD_GRID, &grid, NULL);
    for (int row = 0; grid_open && row < OSK_OVERLAY_ROWS; row++) {
        NkUiLayout line = layout(NK_UI_LAYOUT_ROW, sizing(NK_UI_SIZE_GROW, 0.0f),
                                 sizing(NK_UI_SIZE_FIXED, key_h), 0.0f);
        if (!nk_ui_begin_container(ui, ACE_UI_ID_KEYBOARD_ROWS + (NkUiId)row, &line, NULL)) continue;
        for (int i = 0; i < n; i++) {
            const OskCell *c = &cells[i];
            if (c->row != row) continue;
            NkUiLayout key = layout(NK_UI_LAYOUT_ROW, sizing(NK_UI_SIZE_FIXED, unit * (float)c->span),
                                    sizing(NK_UI_SIZE_GROW, 0.0f), 0.0f);
            key.padding_left = key.padding_right = 2.0f;
            key.padding_top = key.padding_bottom = 2.0f;
            if (!nk_ui_begin_container(ui, ACE_UI_ID_KEYBOARD_KEYS + (NkUiId)(2 * i) + 0x1000u,
                                       &key, NULL)) {
                continue;
            }
            NkUiLayout cap = layout(NK_UI_LAYOUT_ROW, sizing(NK_UI_SIZE_GROW, 0.0f),
                                    sizing(NK_UI_SIZE_GROW, 0.0f), 0.0f);
            cap.align_x = NK_UI_ALIGN_CENTER;
            cap.align_y = NK_UI_ALIGN_CENTER;
            NkUiStyle cap_style;
            memset(&cap_style, 0, sizeof(cap_style));
            cap_style.corner_radius = NK_UI_RADIUS_CHIP;
            uint32_t label_rgba = theme->paper;
            if (c->highlight) {
                cap_style.fill_rgba = theme->accent_dark;
                cap_style.border_rgba = high_contrast ? theme->paper : theme->focus;
                cap_style.border_width = high_contrast ? 2.0f : NK_UI_BORDER_HAIRLINE;
                label_rgba = theme->ink;
            } else if (c->enabled) {
                cap_style.fill_rgba = theme->surface_high;
                cap_style.border_rgba = theme->line_strong;
                cap_style.border_width = NK_UI_BORDER_HAIRLINE;
            } else {
                cap_style.fill_rgba = theme->surface;
                label_rgba = theme->dim;
            }
            if (nk_ui_begin_container(ui, ACE_UI_ID_KEYBOARD_KEYS + (NkUiId)(2 * i), &cap, &cap_style)) {
                char ch[2];
                NkUiTextStyle label = nk_ui_type(c->kind == OSK_CELL_CHAR ? NK_UI_TYPE_DATA
                                                                         : NK_UI_TYPE_LABEL_S,
                                                 label_rgba);
                label.wrap = false;
                nk_ui_text(ui, ACE_UI_ID_KEYBOARD_KEYS + (NkUiId)(2 * i + 1), cell_label(c, ch), &label);
                nk_ui_end_container(ui);
            }
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    if (grid_open) nk_ui_end_container(ui);
    NkUiTextStyle hint = nk_ui_type(NK_UI_TYPE_CAPTION, theme->muted);
    hint.wrap = false;
    nk_ui_text(ui, ACE_UI_ID_KEYBOARD_HINT,
               one_line(ui, nk_ui_text_clamp(ui, "Cross types \xC2\xB7 Circle cancels \xC2\xB7 "
                                                 "Start is OK \xC2\xB7 L and R change case",
                                             &hint, inner, 1)),
               &hint);
    nk_ui_end_container(ui);
}

void ace_ui_viewport(const AceUiFrame *f, NkUiViewport *vp) {
    memset(vp, 0, sizeof(*vp));
    if (!f) return;
    vp->pixel_w = f->pixel_w;
    vp->pixel_h = f->pixel_h;
    vp->display_scale = f->display_scale > 0.0f ? f->display_scale : 1.0f;
    vp->text_size_pct = 100;
    vp->min_logical_w = ACE_UI_MIN_LOGICAL_W;
    vp->min_logical_h = ACE_UI_MIN_LOGICAL_H;
}

void ace_ui_layout(NkUi *ui, const AceUiFrame *f) {
    if (!ui || !f) return;
    const NkUiTheme *theme = nk_ui_theme(ui);
    NkUiViewport vp;
    ace_ui_viewport(f, &vp);
    NkUiScale scale = nk_ui_scale_for_viewport(&vp);
    NkUiLayout root = layout(NK_UI_LAYOUT_COLUMN, sizing(NK_UI_SIZE_FIXED, scale.logical_w),
                             sizing(NK_UI_SIZE_FIXED, scale.logical_h), 0.0f);
    /* The 16 px safe area, as the overlay keeps it. */
    root.padding_left = root.padding_right = root.padding_top = root.padding_bottom = NK_UI_SPACE_4;
    root.align_x = NK_UI_ALIGN_CENTER;
    if (!nk_ui_begin_container(ui, ACE_UI_ID_ROOT, &root, NULL)) return;

    int show_readout = f->readout && f->readout[0];
    int show_notice = f->notice && f->notice[0] && f->notice_alpha > 0.0f;
    NkUiLayout top = layout(NK_UI_LAYOUT_ROW, sizing(NK_UI_SIZE_GROW, 0.0f),
                            sizing(NK_UI_SIZE_FIT, 0.0f), NK_UI_SPACE_3);
    top.align_y = NK_UI_ALIGN_START;
    if ((show_readout || show_notice) && nk_ui_begin_container(ui, ACE_UI_ID_TOP, &top, NULL)) {
        float room = scale.logical_w - 2.0f * NK_UI_SPACE_4;
        if (show_readout) {
            NkUiLayout chip = layout(NK_UI_LAYOUT_ROW, sizing(NK_UI_SIZE_FIT, 0.0f),
                                     sizing(NK_UI_SIZE_FIT, 0.0f), 0.0f);
            chip.padding_left = chip.padding_right = NK_UI_SPACE_2 + 2.0f;
            chip.padding_top = chip.padding_bottom = NK_UI_SPACE_1 + 2.0f;
            NkUiStyle style = chip_style(theme, f->high_contrast, 1.0f);
            if (nk_ui_begin_container(ui, ACE_UI_ID_READOUT, &chip, &style)) {
                NkUiTextStyle text = nk_ui_type(NK_UI_TYPE_DATA,
                                                f->readout_warning ? theme->warning : theme->paper);
                text.wrap = false;
                nk_ui_text(ui, ACE_UI_ID_READOUT_TEXT,
                           one_line(ui, nk_ui_text_clamp(ui, f->readout, &text, room * 0.5f, 1)),
                           &text);
                nk_ui_end_container(ui);
            }
        }
        NkUiLayout spacer = layout(NK_UI_LAYOUT_ROW, sizing(NK_UI_SIZE_GROW, 0.0f),
                                   sizing(NK_UI_SIZE_FIXED, 1.0f), 0.0f);
        if (nk_ui_begin_container(ui, ACE_UI_ID_SPACER, &spacer, NULL)) nk_ui_end_container(ui);
        if (show_notice) {
            NkUiLayout chip = layout(NK_UI_LAYOUT_COLUMN, sizing(NK_UI_SIZE_FIT, 0.0f),
                                     sizing(NK_UI_SIZE_FIT, 0.0f), 2.0f);
            chip.padding_left = chip.padding_right = NK_UI_SPACE_3;
            chip.padding_top = chip.padding_bottom = NK_UI_SPACE_2;
            NkUiStyle style = chip_style(theme, f->high_contrast, f->notice_alpha);
            if (nk_ui_begin_container(ui, ACE_UI_ID_NOTICE, &chip, &style)) {
                NkUiTextStyle head = nk_ui_type(NK_UI_TYPE_LABEL_S,
                                                faded(theme->accent_dark, f->notice_alpha));
                NkUiTextStyle words = nk_ui_type(NK_UI_TYPE_CAPTION,
                                                 faded(theme->paper, f->notice_alpha));
                head.wrap = false;
                words.wrap = false;
                nk_ui_text(ui, ACE_UI_ID_NOTICE_HEAD, one_line(ui, "Ace Mode"), &head);
                nk_ui_text(ui, ACE_UI_ID_NOTICE_TEXT,
                           one_line(ui, nk_ui_text_clamp(ui, f->notice, &words, room * 0.5f, 1)),
                           &words);
                nk_ui_end_container(ui);
            }
        }
        nk_ui_end_container(ui);
    }
    if (f->keyboard && f->keyboard->status == OSK_OVERLAY_OPEN) {
        NkUiLayout fill = layout(NK_UI_LAYOUT_ROW, sizing(NK_UI_SIZE_GROW, 0.0f),
                                 sizing(NK_UI_SIZE_GROW, 0.0f), 0.0f);
        if (nk_ui_begin_container(ui, ACE_UI_ID_FILL, &fill, NULL)) nk_ui_end_container(ui);
        keyboard_layout(ui, theme, f->keyboard, scale.logical_w, scale.logical_h, f->high_contrast);
    }
    nk_ui_end_container(ui);
}

int ace_ui_frame_wants(const AceUiFrame *f) {
    if (!f || f->pixel_w <= 0 || f->pixel_h <= 0) return 0;
    return (f->readout && f->readout[0]) ||
           (f->notice && f->notice[0] && f->notice_alpha > 0.0f) ||
           (f->keyboard && f->keyboard->status == OSK_OVERLAY_OPEN);
}

/* ---- Vulkan ------------------------------------------------------------------------------- */

static void drop_targets(AceUi *ui) {
    for (uint32_t i = 0; i < ACE_UI_MAX_IMAGES; i++) {
        if (ui->fbs[i]) ui->DestroyFramebuffer(ui->init.device, ui->fbs[i], NULL);
        if (ui->views[i]) ui->DestroyImageView(ui->init.device, ui->views[i], NULL);
        ui->fbs[i] = VK_NULL_HANDLE;
        ui->views[i] = VK_NULL_HANDLE;
    }
    ui->image_count = 0;
    ui->have_gen = 0;
}

static void drop_pass(AceUi *ui) {
    drop_targets(ui);
    nk_ui_vk_destroy(ui->vk);
    ui->vk = NULL;
    if (ui->pass) ui->DestroyRenderPass(ui->init.device, ui->pass, NULL);
    ui->pass = VK_NULL_HANDLE;
    ui->format = VK_FORMAT_UNDEFINED;
}

/* One pass that loads the image the presenter blitted, draws over it and leaves it as it found
 * it, in TRANSFER_DST_OPTIMAL, for the presenter's own barrier to PRESENT_SRC. */
static int ensure_pass(AceUi *ui, VkFormat format) {
    if (ui->pass && ui->format == format) return 1;
    if (ui->pass) {
        (void)ui->QueueWaitIdle(ui->init.queue);
        drop_pass(ui);
    }
    VkAttachmentDescription color;
    memset(&color, 0, sizeof(color));
    color.format = format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    color.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub;
    memset(&sub, 0, sizeof(sub));
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency deps[2];
    memset(deps, 0, sizeof(deps));
    /* After the blit and the HUD copy into the image... */
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    /* ...and before the presenter's transfer-stage barrier to PRESENT_SRC. */
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    VkRenderPassCreateInfo rpci;
    memset(&rpci, 0, sizeof(rpci));
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &color;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 2;
    rpci.pDependencies = deps;
    if (ui->CreateRenderPass(ui->init.device, &rpci, NULL, &ui->pass) != VK_SUCCESS) {
        ui->pass = VK_NULL_HANDLE;
        return 0;
    }
    NkUiVkInit vi;
    memset(&vi, 0, sizeof(vi));
    vi.get_instance_proc_addr = ui->init.get_instance_proc_addr;
    vi.instance = ui->init.instance;
    vi.physical_device = ui->init.physical_device;
    vi.device = ui->init.device;
    vi.queue_family = ui->init.queue_family;
    vi.queue = ui->init.queue;
    vi.render_pass = ui->pass;
    vi.subpass = 0;
    vi.frames_in_flight = ui->init.frames_in_flight;
    vi.text = ui->init.text;
    ui->vk = nk_ui_vk_create(&vi);
    if (!ui->vk) {
        ui->DestroyRenderPass(ui->init.device, ui->pass, NULL);
        ui->pass = VK_NULL_HANDLE;
        return 0;
    }
    ui->format = format;
    return 1;
}

/* A view and a framebuffer per swapchain image. The presenter waits for every frame before it
 * remakes its swapchain, so the old ones are idle when the generation changes. */
static int ensure_targets(AceUi *ui, const AceUiTarget *t) {
    if (ui->have_gen && ui->gen == t->swapchain_gen && ui->image_count == t->image_count &&
        ui->extent.width == t->extent.width && ui->extent.height == t->extent.height) {
        return 1;
    }
    drop_targets(ui);
    if (t->image_count == 0 || t->image_count > ACE_UI_MAX_IMAGES) return 0;
    for (uint32_t i = 0; i < t->image_count; i++) {
        VkImageViewCreateInfo ivci;
        memset(&ivci, 0, sizeof(ivci));
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = t->images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = t->format;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;
        if (ui->CreateImageView(ui->init.device, &ivci, NULL, &ui->views[i]) != VK_SUCCESS) {
            ui->views[i] = VK_NULL_HANDLE;
            drop_targets(ui);
            return 0;
        }
        VkFramebufferCreateInfo fci;
        memset(&fci, 0, sizeof(fci));
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = ui->pass;
        fci.attachmentCount = 1;
        fci.pAttachments = &ui->views[i];
        fci.width = t->extent.width;
        fci.height = t->extent.height;
        fci.layers = 1;
        if (ui->CreateFramebuffer(ui->init.device, &fci, NULL, &ui->fbs[i]) != VK_SUCCESS) {
            ui->fbs[i] = VK_NULL_HANDLE;
            drop_targets(ui);
            return 0;
        }
    }
    ui->image_count = t->image_count;
    ui->extent = t->extent;
    ui->gen = t->swapchain_gen;
    ui->have_gen = 1;
    return 1;
}

AceUi *ace_ui_create(const AceUiInit *init, char *error, size_t error_size) {
    if (!init || !init->get_instance_proc_addr || !init->instance || !init->device ||
        !init->queue || !init->text || init->frames_in_flight == 0 ||
        init->frames_in_flight > NK_UI_VK_MAX_FRAMES) {
        set_error(error, error_size, "incomplete Vulkan handles");
        return NULL;
    }
    if (!nk_ui_vk_available()) {
        set_error(error, error_size, "this runtime was built without the nk_ui Vulkan backend");
        return NULL;
    }
    AceUi *ui = (AceUi *)calloc(1, sizeof(*ui));
    if (!ui) {
        set_error(error, error_size, "out of memory");
        return NULL;
    }
    ui->init = *init;
    PFN_vkGetDeviceProcAddr gdpa =
        (PFN_vkGetDeviceProcAddr)init->get_instance_proc_addr(init->instance, "vkGetDeviceProcAddr");
    if (gdpa) {
#define ACE_UI_LOAD(name) ui->name = (PFN_vk##name)gdpa(init->device, "vk" #name)
        ACE_UI_LOAD(CreateRenderPass);
        ACE_UI_LOAD(DestroyRenderPass);
        ACE_UI_LOAD(CreateImageView);
        ACE_UI_LOAD(DestroyImageView);
        ACE_UI_LOAD(CreateFramebuffer);
        ACE_UI_LOAD(DestroyFramebuffer);
        ACE_UI_LOAD(CmdBeginRenderPass);
        ACE_UI_LOAD(CmdEndRenderPass);
        ACE_UI_LOAD(QueueWaitIdle);
#undef ACE_UI_LOAD
    }
    if (!gdpa || !ui->CreateRenderPass || !ui->DestroyRenderPass || !ui->CreateImageView ||
        !ui->DestroyImageView || !ui->CreateFramebuffer || !ui->DestroyFramebuffer ||
        !ui->CmdBeginRenderPass || !ui->CmdEndRenderPass || !ui->QueueWaitIdle) {
        set_error(error, error_size, "a Vulkan function could not be loaded");
        free(ui);
        return NULL;
    }
    NkUiConfig config;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_OVERLAY;
    config.text = init->text;
    ui->ui = nk_ui_create(&config);
    if (!ui->ui) {
        set_error(error, error_size, "the UI layout core could not be created");
        free(ui);
        return NULL;
    }
    if (error && error_size) error[0] = '\0';
    return ui;
}

void ace_ui_destroy(AceUi *ui) {
    if (!ui) return;
    if (ui->pass || ui->image_count) (void)ui->QueueWaitIdle(ui->init.queue);
    drop_pass(ui);
    nk_ui_destroy(ui->ui);
    free(ui);
}

int ace_ui_record(AceUi *ui, const AceUiTarget *t, const AceUiFrame *f) {
    if (!ui || !t || !t->cmd || !f || !ace_ui_frame_wants(f)) return 0;
    if (t->image_index >= t->image_count || t->frame_slot >= ui->init.frames_in_flight) return 0;
    if (!ensure_pass(ui, t->format) || !ensure_targets(ui, t)) return 0;
    AceUiFrame frame = *f;
    frame.pixel_w = (int)t->extent.width;
    frame.pixel_h = (int)t->extent.height;
    NkUiViewport vp;
    ace_ui_viewport(&frame, &vp);
    NkUiScale scale = nk_ui_scale_for_viewport(&vp);
    NkUiPrefs prefs;
    memset(&prefs, 0, sizeof(prefs));
    prefs.reduce_motion = f->reduce_motion != 0;
    prefs.high_contrast_focus = f->high_contrast != 0;
    nk_ui_set_prefs(ui->ui, &prefs);
    nk_ui_set_viewport(ui->ui, &vp);
    nk_ui_vk_set_scale(ui->vk, scale.density);
    nk_ui_begin(ui->ui, f->now_s);
    ace_ui_layout(ui->ui, &frame);
    const NkUiDrawList *list = nk_ui_end(ui->ui);
    if (!list) return 0;
    struct VkCommandBuffer_T *cmd = (struct VkCommandBuffer_T *)t->cmd;
    if (!nk_ui_vk_prepare(ui->vk, list, cmd, t->frame_slot)) return 0;
    VkRenderPassBeginInfo rbi;
    memset(&rbi, 0, sizeof(rbi));
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = ui->pass;
    rbi.framebuffer = ui->fbs[t->image_index];
    rbi.renderArea.extent = t->extent;
    ui->CmdBeginRenderPass(t->cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    int drew = nk_ui_vk_render(ui->vk, list, cmd, t->extent.width, t->extent.height) ? 1 : 0;
    ui->CmdEndRenderPass(t->cmd);
    return drew;
}
