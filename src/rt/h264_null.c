// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

#include "sr_h264.h"
#include "psmf_producer.h"
#include "perf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR_H264_MAX_INSTANCES 8
#define SR_H264_MAX_AU_METADATA (1u << 20)

typedef struct {
    uint64_t ps_offset;
    int64_t pts;
    int has_pts;
} SrH264AuMetadata;

typedef struct {
    int used;
    const SrH264Backend *backend;
    int backend_id;
    SrPsmfProducer *ps_demux;
    SrH264AuMetadata *au_metadata;
    uint32_t au_metadata_head, au_metadata_count, au_metadata_capacity;
    int ps_failed, ps_eos, ps_failure_reported;
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
static int null_submit_au(int id, const SrH264AccessUnit *au) {
    (void)id; (void)au;
    return -1;
}
static int null_frame(int id, int eos, const SrH264FrameTarget *target,
                      SrH264FrameInfo *info) {
    (void)id; (void)eos; (void)target; (void)info;
    return -1;
}

static const SrH264Backend s_null_backend = {
    "null",
    null_create,
    null_destroy,
    null_reset,
    null_submit_au,
    null_frame
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

static void report_ps_failure(SrH264Instance *instance) {
    if (!instance || instance->ps_failure_reported) return;
    instance->ps_failure_reported = 1;
    SrPsmfProducerStats stats;
    sr_psmf_producer_stats(instance->ps_demux, &stats);
    fprintf(stderr, "h264: program-stream demux rejected input at byte %llu: %s\n",
            (unsigned long long)stats.fail_offset,
            sr_psmf_producer_fail_reason(instance->ps_demux) ?
                sr_psmf_producer_fail_reason(instance->ps_demux) : "unknown parser failure");
}

static int au_metadata_reserve_one(SrH264Instance *instance) {
    if (instance->au_metadata_count >= SR_H264_MAX_AU_METADATA) return 0;
    if (instance->au_metadata_head &&
        instance->au_metadata_head + instance->au_metadata_count >=
            instance->au_metadata_capacity) {
        memmove(instance->au_metadata,
                instance->au_metadata + instance->au_metadata_head,
                (size_t)instance->au_metadata_count * sizeof(*instance->au_metadata));
        instance->au_metadata_head = 0;
    }
    if (instance->au_metadata_head + instance->au_metadata_count <
        instance->au_metadata_capacity) return 1;
    uint32_t capacity = instance->au_metadata_capacity ?
                        instance->au_metadata_capacity : 64u;
    if (capacity > SR_H264_MAX_AU_METADATA / 2u) capacity = SR_H264_MAX_AU_METADATA;
    else capacity *= 2u;
    if (capacity <= instance->au_metadata_capacity) return 0;
    SrH264AuMetadata *grown = (SrH264AuMetadata *)realloc(
        instance->au_metadata, (size_t)capacity * sizeof(*instance->au_metadata));
    if (!grown) return 0;
    instance->au_metadata = grown;
    instance->au_metadata_capacity = capacity;
    return 1;
}

static int pump_program_stream(SrH264Instance *instance) {
    if (!instance || !instance->ps_demux || instance->ps_failed) return -1;
    for (;;) {
        uint64_t cursor_before = sr_psmf_producer_stream_cursor(instance->ps_demux);
        int produced = sr_psmf_producer_pump(instance->ps_demux, 4096u);
        if (sr_psmf_producer_failed(instance->ps_demux)) {
            instance->ps_failed = 1;
            report_ps_failure(instance);
            return -1;
        }
        SrPsmfAu au;
        while (sr_psmf_producer_pop(instance->ps_demux, SR_PSMF_AU_VIDEO, &au)) {
            if (!au_metadata_reserve_one(instance) || !instance->backend ||
                !instance->backend->submit_au) {
                sr_psmf_au_release(&au);
                instance->ps_failed = 1;
                return -1;
            }
            SrH264AccessUnit backend_au;
            backend_au.data = au.data;
            backend_au.size = au.size;
            backend_au.pts = au.pts;
            backend_au.has_pts = au.has_pts != 0;
            if (instance->backend->submit_au(instance->backend_id, &backend_au) < 0) {
                sr_psmf_au_release(&au);
                instance->ps_failed = 1;
                return -1;
            }
            uint32_t at = instance->au_metadata_head + instance->au_metadata_count++;
            instance->au_metadata[at].ps_offset = au.ps_offset;
            instance->au_metadata[at].pts = au.pts;
            instance->au_metadata[at].has_pts = au.has_pts != 0;
            sr_psmf_au_release(&au);
        }
        uint64_t cursor_after = sr_psmf_producer_stream_cursor(instance->ps_demux);
        if (!produced && cursor_after == cursor_before) break;
    }
    if (sr_psmf_producer_failed(instance->ps_demux)) {
        instance->ps_failed = 1;
        report_ps_failure(instance);
        return -1;
    }
    return 0;
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
        SrPsmfProducer *demux = sr_psmf_producer_open_stream(0);
        if (!demux) {
            if (backend->destroy) backend->destroy(backend_id);
            report_unavailable();
            return -1;
        }
        s_instances[i].used = 1;
        s_instances[i].backend = backend;
        s_instances[i].backend_id = backend_id;
        s_instances[i].ps_demux = demux;
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
    sr_psmf_producer_close(instance->ps_demux);
    free(instance->au_metadata);
    memset(instance, 0, sizeof(*instance));
}

int sr_h264_reset(int id) {
    SrH264Instance *instance = instance_for(id);
    if (!instance || !instance->backend || !instance->backend->reset || !instance->ps_demux)
        return -1;
    int result = instance->backend->reset(instance->backend_id);
    if (result < 0) {
        report_unavailable();
        return result;
    }
    sr_psmf_producer_reset(instance->ps_demux);
    instance->au_metadata_head = 0;
    instance->au_metadata_count = 0;
    instance->ps_failed = instance->ps_eos = instance->ps_failure_reported = 0;
    return 0;
}

int sr_h264_feed(int id, const uint8_t *data, uint32_t len) {
    SrH264Instance *instance = instance_for(id);
    if (!instance || !instance->ps_demux || !data || !len ||
        instance->ps_failed || instance->ps_eos) return -1;
    if (sr_psmf_producer_feed_stream(instance->ps_demux, data, len, 0) < 0) {
        instance->ps_failed = 1;
        report_ps_failure(instance);
        return -1;
    }
    return pump_program_stream(instance);
}
int sr_h264_submit_au(int id, const uint8_t *au, uint32_t len) {
    return sr_h264_submit_au_timed(id, au, len, -1, 0);
}

int sr_h264_submit_au_timed(int id, const uint8_t *au, uint32_t len,
                            int64_t pts, int has_pts) {
    SrH264Instance *instance = instance_for(id);
    if (!instance || !instance->backend || !instance->backend->submit_au ||
        !au || !len || (has_pts != 0 && has_pts != 1)) return -1;
    SrH264AccessUnit access_unit;
    access_unit.data = au;
    access_unit.size = len;
    access_unit.pts = has_pts ? pts : -1;
    access_unit.has_pts = has_pts;
    return instance->backend->submit_au(instance->backend_id, &access_unit);
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
    if (!instance || !instance->ps_demux || instance->ps_failed) return -1;
    if (eos && !instance->ps_eos) {
        if (sr_psmf_producer_feed_stream(instance->ps_demux, NULL, 0, 1) < 0) {
            instance->ps_failed = 1;
            report_ps_failure(instance);
            return -1;
        }
        instance->ps_eos = 1;
        if (pump_program_stream(instance) < 0) return -1;
    }
    if (!instance->au_metadata_count) return 0;
    SrH264AuMetadata current = instance->au_metadata[instance->au_metadata_head++];
    instance->au_metadata_count--;
    if (!instance->au_metadata_count) instance->au_metadata_head = 0;
    if (psConsumed) {
        *psConsumed = instance->au_metadata_count ?
            instance->au_metadata[instance->au_metadata_head].ps_offset :
            sr_psmf_producer_stream_bytes(instance->ps_demux);
    }
    if (pts) *pts = current.has_pts ? current.pts : -1;
    return 1;
}

int64_t sr_h264_first_audio_pts(int id) {
    SrH264Instance *instance = instance_for(id);
    return instance && instance->ps_demux ?
           sr_psmf_producer_first_audio_pts(instance->ps_demux) : -1;
}
