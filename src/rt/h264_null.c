// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

#include "sr_h264.h"
#include "perf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR_H264_MAX_INSTANCES 8

typedef struct {
    int used;
    const SrH264Backend *backend;
    int backend_id;
} SrH264Instance;

static SrH264Instance s_instances[SR_H264_MAX_INSTANCES];
static int s_selection = -1;
static int s_unavailable_reported;

#if defined(_WIN32)
extern const SrH264Backend sr_h264_mf_backend;
#endif

static int null_create(void) { return -1; }
static void null_destroy(int id) { (void)id; }
static int null_reset(int id) { (void)id; return -1; }
static int null_feed(int id, const uint8_t *data, uint32_t len) {
    (void)id; (void)data; (void)len;
    return -1;
}
static int null_submit_au(int id, const uint8_t *au, uint32_t len) {
    (void)id; (void)au; (void)len;
    return -1;
}
static int null_frame(int id, int eos, const SrH264FrameTarget *target,
                      SrH264FrameInfo *info) {
    (void)id; (void)eos; (void)target; (void)info;
    return -1;
}
static int null_au_take(int id, int eos, uint64_t *ps_consumed, int64_t *pts) {
    (void)id; (void)eos; (void)ps_consumed; (void)pts;
    return -1;
}
static int64_t null_first_audio_pts(int id) { (void)id; return -1; }

static const SrH264Backend s_null_backend = {
    "null",
    null_create,
    null_destroy,
    null_reset,
    null_feed,
    null_submit_au,
    null_frame,
    null_au_take,
    null_first_audio_pts
};

static const SrH264Backend *named_backend(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "null") == 0) return &s_null_backend;
#if defined(_WIN32)
    if (strcmp(name, "mf") == 0 || strcmp(name, "media-foundation") == 0 ||
        strcmp(name, "media_foundation") == 0)
        return &sr_h264_mf_backend;
#endif
    return NULL;
}

static const SrH264Backend *selected_backend(void) {
    if (s_selection >= 0) {
        return s_selection == 0 ? &s_null_backend :
#if defined(_WIN32)
               &sr_h264_mf_backend;
#else
               &s_null_backend;
#endif
    }
#if defined(_WIN32)
    if (!getenv("SR_NOH264")) s_selection = 1;
    else s_selection = 0;
#else
    s_selection = 0;
#endif
    return selected_backend();
}

int sr_h264_select_backend(const char *name) {
    if (!name || !*name || strcmp(name, "auto") == 0) {
        s_selection = -1;
        return 1;
    }
    const SrH264Backend *backend = named_backend(name);
    if (!backend) return 0;
    s_selection = backend == &s_null_backend ? 0 : 1;
    return 1;
}

const char *sr_h264_selected_backend(void) {
    const SrH264Backend *backend = selected_backend();
    return backend ? backend->name : "unavailable";
}

int sr_h264_backend_available(void) {
    const SrH264Backend *backend = selected_backend();
    return backend && backend != &s_null_backend;
}

const char *sr_h264_backend_unavailable_reason(void) {
    return SR_H264_UNAVAILABLE_BOUNDARY;
}

static void report_unavailable(void) {
    if (s_unavailable_reported) return;
    s_unavailable_reported = 1;
    fprintf(stderr, "%s\n", SR_H264_UNAVAILABLE_BOUNDARY);
}

static SrH264Instance *instance_for(int id) {
    if (id < 0 || id >= SR_H264_MAX_INSTANCES || !s_instances[id].used) return NULL;
    return &s_instances[id];
}

int sr_h264_create(void) {
    const SrH264Backend *backend = selected_backend();
    if (!backend || backend == &s_null_backend) {
        report_unavailable();
        return -1;
    }
    for (int i = 0; i < SR_H264_MAX_INSTANCES; i++) {
        if (s_instances[i].used) continue;
        int backend_id = backend->create();
        if (backend_id < 0) {
            report_unavailable();
            return -1;
        }
        s_instances[i].used = 1;
        s_instances[i].backend = backend;
        s_instances[i].backend_id = backend_id;
        return i;
    }
    report_unavailable();
    return -1;
}

void sr_h264_destroy(int id) {
    SrH264Instance *instance = instance_for(id);
    if (!instance) return;
    if (instance->backend && instance->backend->destroy)
        instance->backend->destroy(instance->backend_id);
    memset(instance, 0, sizeof(*instance));
}

int sr_h264_reset(int id) {
    SrH264Instance *instance = instance_for(id);
    if (!instance || !instance->backend || !instance->backend->reset) return -1;
    int result = instance->backend->reset(instance->backend_id);
    if (result < 0) report_unavailable();
    return result;
}

int sr_h264_feed(int id, const uint8_t *data, uint32_t len) {
    SrH264Instance *instance = instance_for(id);
    if (!instance || !instance->backend || !instance->backend->feed ||
        !data || !len) return -1;
    return instance->backend->feed(instance->backend_id, data, len);
}

int sr_h264_submit_au(int id, const uint8_t *au, uint32_t len) {
    SrH264Instance *instance = instance_for(id);
    if (!instance || !instance->backend || !instance->backend->submit_au ||
        !au || !len) return -1;
    return instance->backend->submit_au(instance->backend_id, au, len);
}

int sr_h264_frame_ex(int id, int eos, const SrH264FrameTarget *target,
                     SrH264FrameInfo *info) {
    SrH264Instance *instance = instance_for(id);
    if (!instance || !instance->backend || !instance->backend->frame || !target) return -1;
    if (info) {
        memset(info, 0, sizeof(*info));
        info->pts = -1;
    }
    uint64_t perf_started = sr_perf_now_ns();
    int result = instance->backend->frame(instance->backend_id, eos, target, info);
    if (perf_started) sr_perf_h264(perf_started, result);
    return result;
}

int sr_h264_drain(int id, const SrH264FrameTarget *target,
                  SrH264FrameInfo *info) {
    return sr_h264_frame_ex(id, 1, target, info);
}

#if !defined(_WIN32)
int sr_h264_frame(int id, int eos, uint32_t buffer, int frameWidth, int pixelMode) {
    SrH264FrameTarget target;
    memset(&target, 0, sizeof(target));
    target.kind = SR_H264_TARGET_GUEST;
    target.guest_buffer = buffer;
    target.frame_width = frameWidth;
    target.pixel_mode = pixelMode;
    return sr_h264_frame_ex(id, eos, &target, NULL);
}
#endif

int sr_h264_frame_host(int id, int eos, uint8_t *dst, int maxW, int strideBytes) {
    SrH264FrameTarget target;
    memset(&target, 0, sizeof(target));
    target.kind = SR_H264_TARGET_HOST;
    target.host_buffer = dst;
    target.host_width = maxW;
    target.host_stride = strideBytes;
    return sr_h264_frame_ex(id, eos, &target, NULL);
}

int sr_h264_au_take(int id, int eos, uint64_t *psConsumed, int64_t *pts) {
    SrH264Instance *instance = instance_for(id);
    if (!instance || !instance->backend || !instance->backend->au_take) return -1;
    return instance->backend->au_take(instance->backend_id, eos, psConsumed, pts);
}

int64_t sr_h264_first_audio_pts(int id) {
    SrH264Instance *instance = instance_for(id);
    if (!instance || !instance->backend || !instance->backend->first_audio_pts) return -1;
    return instance->backend->first_audio_pts(instance->backend_id);
}
