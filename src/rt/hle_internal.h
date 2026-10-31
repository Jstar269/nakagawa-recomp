// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* Private to the runtime HLE translation units (#307): the includes, types, macros and small
 * helpers they share, and the declarations of what one of them defines and another uses. Every
 * runtime HLE source includes it first. Extracted from hle.c on 2026-10-31. */

#ifndef SR_HLE_INTERNAL_H
#define SR_HLE_INTERNAL_H

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _CRT_RAND_S
#define _CRT_RAND_S      /* rand_s(): the console profile's random identity (#1382) */
#endif
#ifndef _MSC_EXTENSIONS
#define _MSC_EXTENSIONS  /* for _Exit on MSVC; harmless under MinGW */
#endif
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif
#include "vfs_contained.h"
#include "recomp.h"
#include "iso.h"
#include "pgf_api.h"
#include "pgd_api.h"
#include "nk_input_profile.h"
#include "nk_platform.h"   /* per-user data directory and UTF-8 file primitives (sceReg overlay) */
#define NK_JSON_NO_LEGACY_ALIASES
#include "nk_json.h"       /* sceReg overlay parser */
#include "evf.h"         /* pure sceKernelEventFlag pattern/mode semantics */
#include "scripted_input.h" /* scripted controller input delivered in guest time */
#include "asset_index.h" /* dynamic extracted-data index (issue #223) */
#include "archive_vfs.h" /* validated read-only XB provider (issue #298) */
#include "sdkver.h"      /* retained compiled-SDK-version state (issue #71) */
#include "vfs_path.h"    /* host-neutral VFS path join helper (issue #19) */
#include "atrac3p_bridge.h" /* HLE bridges for separate ATRAC3+ and AT3 decoding */
#include "atrac3p/atrac3p_api.h" /* ATRAC3P_MAX_CHANNELS, ATRAC3P_PADDING_SIZE */
#include "mp3_bridge.h"     /* minimp3 frame decoder behind sceMp3 */
#include "fbcap_policy.h"   /* frame-capture slot policy for the present path (issue #57) */
#include "fbcap.h"           /* presenter-neutral frame capture service (issue #57) */
#include "audio_resample.h"  /* deterministic SRC-to-host-rate conversion */
#include "gpu_sdl3vk/ge_gpu.h" /* explicit guest-VRAM snapshot boundary */
#include "title_config.h"  /* title-qualified compatibility addresses (issue #98) */
#include "nested_frames.h" /* per-owner/per-depth frames for nested guest calls */
#include "hle_power.h"
#include "stale_code.h"  /* TD-27 opt-in stale translated-code detector */
#include "vfpu_tables.h" /* sr_vfpu_sha256(): enhancement data-file digests */
#include "prx_loader.h"  /* clean-room PRX image loader (G3) */
#include "flight_recorder.h"
#include "psmf_producer.h" /* bounded project-authored PSMF/MPEG-PS AU producer */
#include "sr_h264.h"       /* AVC decode backend seam, shared with the sceMpeg core */
#include "ge_shared.h"      /* GE state snapshot for headless VRAM diagnostics */
#include "osk_text_entry.h" /* keyboard text collected without stopping guest time */
#include "osk_overlay.h"    /* the in-window keyboard the presenter draws and the pad drives */
#include "flash0_font.h"    /* read-only flash0: font device (FONT_PLAN section 5) */
#include "../core/nk_font.h" /* shared Latin index filenames and cache validation */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <errno.h>
#include <limits.h>
#include <math.h>      /* fabsf(), INFINITY for sceFontFindOptimumFont */
#ifdef _WIN32
#include <process.h>     /* _exit() */
#include <windows.h>     /* Sleep() */
#include <io.h>          /* _open_osfhandle/_fdopen for contained FILE* opens */
#include <fcntl.h>       /* _O_BINARY/_O_RDWR/_O_APPEND */
#else
#include <unistd.h>      /* _exit(), getcwd(), readlink() */
#include <time.h>        /* clock_gettime()/nanosleep() for the host seams below */
#include <dirent.h>      /* opendir()/readdir() for the host data census */
#include <sys/stat.h>    /* mkdir()/stat()/lstat() for the host path helpers */
#include <sys/statvfs.h> /* statvfs() for the memstick volume report */
#endif

/* ---- handler table ---- */

typedef struct {
    uint32_t nid;
    const char *name;
    HleFn fn;
    uint32_t unsupported_error;
    atomic_uint unimplemented_error;
    atomic_uint unsupported_state;
    unsigned char unverified;      /* 1 when docs/HLE_COVERAGE.md classes the NID unverified */
    atomic_uint unverified_noted;  /* set by the first call's log line */
} HleEntry;

/* Late imports are intentionally separate from recomp.c's address->native dispatch table.
 * Track A owns that table; this registry only publishes stable NID classifications/guest
 * targets. 0 means unresolved, UINT32_MAX-1 means a built-in HLE NID (call sr_syscall), and
 * every other result is a runtime-loaded guest export address (call dispatch on that target). */
/* The table grows with the exports of the resident modules: a title loads and unloads
 * modules over its lifetime, so the registry invents no count of its own. */
typedef struct { uint32_t nid, target; } LateImportEntry;

/* ---- argument / return helpers ---- */

#define A0 (s->r[4])
#define A1 (s->r[5])
#define A2 (s->r[6])
#define A3 (s->r[7])

/* A blocking call made with CPU interrupts disabled or thread dispatch disabled
 * returns this, per PSPAutotests tests/intr/waits.expected. The handlers below
 * check sched_wait_permitted() individually, each at the point its own oracle
 * cells put it -- never as a shared pre-handler gate, because the same oracle
 * shows the precedence is per-API: ILLEGAL_MODE (L72) and ILLEGAL_THID (L204)
 * both beat it, while sceKernelWaitSema's bad-object error (L54/L55) does not.
 * See docs/PSP_INTR_WAITS_MATRIX.md. */
#define SCE_KERNEL_ERROR_CAN_NOT_WAIT 0x800201a7u
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT  0x800201a8u
#ifndef SCE_KERNEL_ERROR_WAIT_DELETE
#define SCE_KERNEL_ERROR_WAIT_DELETE   0x800201b5u
#endif

/* Partition metadata is kernel-owned. sceKernelGetBlockHeadAddr returns the first
 * caller-usable byte, so keep bookkeeping host-side: retail newlib immediately
 * writes malloc chunk metadata at the start of its UserSbrk block. */
/* `high` marks a High block (sceKernelAllocMemoryBlock placement 1): it lives in the top-down
 * arena the thread stacks share (sched_partition_alloc_high) and goes back there when freed. */
/* `part5` marks a block of partition 5, the volatile region (volatile_part_alloc). */
typedef struct { uint32_t uid, addr, size, slot_size, prev, next; int high, part5; } Block;
#define HLE_MAX_PARTITION_BLOCKS 256

typedef struct { uint32_t uid, backing_uid; } HleMemoryBlock;
#define HLE_MAX_MEMORY_BLOCKS 256

/* Free list for partition memory reuse. Real PSP firmware (SysMem) keeps partition
 * blocks sorted by address and uses First-Fit from the bottom (PSP_SMEM_Low)
 * coalescing adjacent free blocks on release.
 * NOTE: First-fit low-address allocation matches the PSP PSP_SMEM_Low contract.
 * Granularity is 256 bytes (0x100), matching PSP partition alignment. Any internal
 * splitting choice or free-block capacity is project-authored and marked as an
 * unmeasured choice relative to physical hardware sysmem fragmentation structures. */
typedef struct {
    uint32_t addr;
    uint32_t size;
} FreeBlock;

#define HLE_MAX_FREE_BLOCKS 256
#define SREG_REG_SLOTS 512u
#define SREG_CAT_SLOTS 512u
#define SREG_MAX_NODES 1024u
#define SREG_NAME_BYTES 27u       /* REG_KEYNAME_SIZE: up to 26 name bytes and the NUL */
#define SREG_PATH_BYTES 256u      /* a category path or name read from the guest, and its NUL */
#define SREG_VALUE_MAX 4096u
#define SREG_ERR_NOT_FOUND 0x8008271du    /* measured: sceRegGetKeyInfo of an unknown key */
#define SREG_ERR_EXIST 0x80010011u        /* EEXIST: the name is already an entry of the category */
#define SREG_ERR_INVAL 0x80010016u        /* EINVAL: bad mode/type/size/name; buffer below the value */
#define SREG_ERR_NOSPC 0x8001001cu        /* ENOSPC: value larger than the key's space, or no free node */
#define SREG_TYPE_DIR 1u
#define SREG_TYPE_INT 2u
#define SREG_TYPE_STR 3u
#define SREG_TYPE_BIN 4u
#define SREG_NODE_DEFAULT 0x01u     /* part of the defaults: never persisted unchanged, never removed */
#define SREG_NODE_PLACEHOLDER 0x02u /* default value neither measured nor modeled (neutral) */
#define SREG_NODE_READONLY 0x08u    /* generated from the served fonts: no write, no overlay */

typedef struct {
    uint16_t id;          /* nonzero while live; key handles carry it */
    uint8_t type;         /* SREG_TYPE_DIR for a category, else the value type */
    uint8_t flags;        /* SREG_NODE_* */
    int parent;           /* parent category node, or -1 at the top level */
    int sysparam_id;      /* default INT keys: PSP_SYSTEMPARAM_ID_INT_*, else -1 */
    uint32_t seq;         /* creation order; sceRegGetKeys lists entries in this order */
    uint32_t size;        /* value bytes; 0 for a category */
    uint8_t *value;
    char name[SREG_NAME_BYTES];
} SregNode;

typedef struct {
    uint8_t open;
    uint8_t mode;         /* 1 = read, 2 = read/write */
} SregRegSlot;

typedef struct {
    uint8_t open;
    uint8_t mode;         /* 1 = read, 2 = read/write */
    int node;             /* the category node */
    uint16_t node_id;     /* its id when opened; a removed category leaves the handle stale */
} SregCatSlot;

/* ---- console profile: the WLAN MAC address and OpenPSID this installation reports (#1382) ----
 * Both are console-bound values on a PSP (the WLAN chip's address and the per-console ID in
 * idStorage), so none is ever read from a console, copied from one, or derived from the host's
 * hardware identifiers or a game. Each installation gets its own: generated once from the host's
 * random source the first time a game asks, and kept in the per-user registry overlay file's
 * "console" object, so it is the same on every run. A MAC has the two low bits of its first
 * octet clear (unicast and universally administered, as a console's own address is: games have
 * been seen to mishandle the multicast and locally administered bits, which PPSSPP clears for
 * the same reason); an OpenPSID is 16 bytes, never all zero. The user (or a test)
 * can set either in the overlay: {"console": {"wlan_mac_hex": "<12 hex>", "open_psid_hex":
 * "<32 hex>"}}; a malformed console object makes the overlay invalid, as any malformed record does.
 * The object is not part of the sceReg tree: a PSP's registry holds neither value. */
typedef struct {
    int have_identity;
    uint8_t mac[6];
    uint8_t psid[16];
    int wlan_switch_set;     /* the overlay set the switch */
    uint32_t wlan_switch;
} PspConsoleProfile;

#ifndef SCE_KERNEL_ERROR_ILLEGAL_ADDR
#define SCE_KERNEL_ERROR_ILLEGAL_ADDR 0x80000103u
#endif
#define HLE_KERNEL_ERROR_NOT_IMPLEMENTED 0x80020002u /* SCE_KERNEL_ERROR_NOTIMP */
#define HLE_KERNEL_ERROR_ILLEGAL_ADDR 0x800200d3u     /* SCE_KERNEL_ERROR_ILLEGAL_ADDR */
#define HLE_KERNEL_ERROR_UNKNOWN_UID 0x800200cbu    /* SCE_KERNEL_ERROR_UNKNOWN_UID */
#define HLE_KERNEL_ERROR_NO_MEMORY 0x80020190u      /* SCE_KERNEL_ERROR_NO_MEMORY */

/* Fixed Pool (FPL) — partition-backed fixed block pool with free list,
 * waiter queuing, and blocking allocation support (PR-G). */
#define FPL_MAX 16
#define FPL_MAX_WAITERS 64

typedef struct {
    uint32_t thread_uid;
} FplWaiter;

typedef struct {
    uint32_t base;
    uint32_t cur;
    uint32_t end;
    uint32_t bsize;
    int used;
    uint32_t block_uid;
    uint32_t attr;
    char name[32];
    uint32_t *free_blocks;
    int nfree;
    int nblocks;
    uint8_t *allocated;
    FplWaiter waiters[FPL_MAX_WAITERS];
    int nwaiters;
} FplPool;

/* Variable Pool (VPL): one SysMem partition block per pool, with a host-side first-fit
 * free list for the pool span, waiter queuing, and blocking allocation support (PR-G).
 * The pool's kernel header and exact PSP alignment are UNMEASURED; this implementation
 * exposes the requested span and uses 16-byte chunks, the smallest conservative
 * alignment for guest allocations. */
#define VPL_MAX 16
#define VPL_MAX_WAITERS 64
typedef struct { uint32_t addr, size; } VplSpan;
typedef struct { uint32_t addr, size; } VplAlloc;
typedef struct { uint32_t thread_uid; uint32_t size; } VplWaiter;
/* The free spans (sorted by address) and the live blocks are host arrays that grow on
 * demand. A PSP keeps a block's header inside the pool, so only the pool's own bytes bound
 * how many blocks it holds; every block here is at least VPL_ALIGN bytes, which bounds
 * both arrays by size / VPL_ALIGN + 1. A fixed table made the 129th live block of a pool
 * with free space read as exhausted, and a blocking AllocateVpl then never returned. */
typedef struct {
    uint32_t base, size, block_uid;
    uint32_t attr;
    int used;
    VplSpan *free;
    int nfree, free_cap;
    VplAlloc *allocs;
    int nallocs, allocs_cap;
    VplWaiter waiters[VPL_MAX_WAITERS];
    int nwaiters;
} VplPool;

/* User TLS pool: one fixed-size block is assigned to each guest thread that
 * requests its address. The public EABI creation call supplies block size and
 * block count in addition to the four argument registers. */
#define TLSPL_MAX 16

typedef struct {
    int used;
    uint32_t uid;
    char name[32];
    uint32_t attr;
    uint32_t block_uid;
    uint32_t base;
    uint32_t block_size;
    uint32_t stride;
    uint32_t num_blocks;
    uint32_t total_size;
    uint32_t free_blocks;
    uint32_t *thread_uids;
} TlsplPool;
#define SCE_KERNEL_ERROR_ILLEGAL_CONTEXT    0x80020064u

typedef enum {
    MODULE_STATE_UNUSED = 0,
    MODULE_STATE_LOADED,
    MODULE_STATE_STARTING,
    MODULE_STATE_STARTED,
    MODULE_STATE_STOPPING,
    MODULE_STATE_STOPPED,
    MODULE_STATE_UNLOADED
} LoadedModuleState;

typedef struct {
    uint32_t uid;
    char path[256];
    uint32_t module_start;
    uint32_t module_stop;
    uint32_t image_base;
    uint32_t image_end;
    int start_entry_ran;
    LoadedModuleState state;
    int in_use;
    /* A guest-placed module: laid out by the guest allocator when the game loaded it
     * and bound to its position-independent translation (`code`, NULL when the module
     * is optional and absent, so host HLE serves it). Its entry points run with its own
     * $gp, as the kernel starts a module, and `host_served` records that host HLE
     * implements one of its named exports. */
    int runtime_placed;
    int host_served;
    uint32_t gp;
    const SrModuleCode *code;
    /* The kernel's record of a guest-placed module, filled from its ELF and the loader's
     * relocated image when it was placed (sceKernelQueryModuleInfo). has_info is 0 for a
     * module placed any other way, which then has no such record. */
    int has_info;
    SrMainModuleRecord info;
} LoadedModule;

/* Live module records and resident images. Guest-placed modules are bound in the
 * dispatch registry, which holds SR_MODULE_CODE_MAX_BOUND images at once; the loader's
 * own tables are sized to match it. */
#define HLE_MAX_LOADED_MODULES SR_MODULE_CODE_MAX_BOUND

/* Resident images: LoadModuleByID repopulates every manifest module, and a second load must
 * not overwrite a running module's live data. `started` is set once the module's real
 * module_start has run; only then are its exports linked to importers (see
 * started_module_export). `alloc_uid` is the exact user-partition reservation the image
 * owns, retained so unload can release precisely that range and nothing else (#280).
 * A released slot is compacted out of the table, so repeated load/unload cycles cannot
 * exhaust the fixed image table. */
typedef struct {
    uint32_t base, end;
    uint32_t alloc_uid;
    int started;
    const SrModuleCode *code;   /* translation bound at base (guest-placed images only) */
} PrxImage;
#define SCE_ERROR_MODULE_BAD_ID             0x80111101u
#define SCE_ERROR_MODULE_ALREADY_LOADED     0x80111102u
enum { PSMF_TRACK_VIDEO = 0, PSMF_TRACK_AUDIO = 1, PSMF_TRACKS = 2 };
enum { PSMF_Q_INPUT = 0, PSMF_Q_AU = 1, PSMF_Q_DECODED = 2, PSMF_Q_STAGES = 3 };
typedef struct { uint8_t state, flags; uint16_t reserved; uint32_t guestAddr, bytes; int64_t pts, dts; void *hostData; } SrPsmfQueueSlot;
typedef struct { uint32_t head, tail, count; SrPsmfQueueSlot slot[PSMF_Q_DEPTH]; } SrPsmfQueue;
typedef struct {
    int used; uint32_t guest, buffer, bufferSize, priority, tempBuf, tempSize;
    uint32_t fileLba, fileSize, streamOffset, streamSize, readOffset;
    uint32_t status, playerVersion, videoStreams, audioStreams, videoWidth, videoHeight;
    /* Per-entry header fields, exactly as the PSMF stream table declares them: a video entry
     * carries width/height in 16-pixel units at bytes 12/13, an audio entry the ATRAC3+
     * channel count at byte 14 (what scePsmfGetAudioInfo reports as `channels`).  Start()
     * copies the selected stream's values out; nothing here is inferred at parse time. */
    uint8_t videoStreamDim[128][2], audioStreamChannels[128];
    uint32_t audioChannels, audioFrequency;
    uint32_t videoCodec, videoStreamNum, audioCodec, audioStreamNum, playMode, playSpeed;
    uint32_t pixelMode, loopStatus, warmup, breakRequested; int64_t currentPts, durationPts, presentationBase;
    SrPsmfProducer *producer;
    struct { uint32_t lba, size; } source;
    /* Decoder-owned output stage.  Nothing here fabricates a frame or a PCM block: video
     * comes from the H.264 backend writing the guest display buffer, audio from the
     * project's ATRAC3+ bridge decoding one compressed frame per guest block. */
    int h264;                                  /* sr_h264 instance, -1 when unavailable */
    int h264Unavailable;                       /* decoder creation already failed once */
    int64_t videoClock; int videoClockValid;   /* running presentation time per picture */
    int64_t auPts[PSMF_OUT_PTS_CAPACITY];     /* one entry per submitted picture */
    int auPtsHead, auPtsCount; uint64_t auPtsLost;
    int64_t displayPts;                        /* last displaypts handed to the guest */
    int displayPtsValid;                       /* a picture has actually reached the guest */
    int64_t audioDisplayPts;                   /* last audio-block pts handed to the guest */
    int audioDisplayPtsValid;
    Atrac3pBridge *atrac; int atracChannels, atracAlign;
    int16_t *audioPcm; int audioPcmSampleCount, audioPcmValid;
    int64_t audioPcmPts; int audioPcmPtsValid;
    int64_t audioClock; int audioClockValid;
    uint32_t videoFramesOut, audioBlocksOut, videoErrors, audioErrors;
    uint32_t audioUpmixBlocks, audioFormatRejects;
    uint32_t logTickCalls, logTickBlocks;
    int videoDrained;
    /* The demuxer refusal has already been named on stderr for this stream.  Latched
     * with the same lifetime as the producer's own failure state, so one rejected
     * element produces exactly one message and a reset stream can report the next one. */
    int rejectNamed;
    char path[512]; SrPsmfQueue q[PSMF_TRACKS][PSMF_Q_STAGES];
} SrPsmfPlayer;

/* ---- PSMF A/V offset where pictures are presented and audio is played (issue #279) -------
 *
 * The counter line above stops at the guest getters.  This block measures the two points a
 * viewer actually perceives: the guest hands a decoded picture or audio block to the host
 * (the getters here, sceMpegAvcDecode/Csc in mpeg.c), and later the guest presents a
 * framebuffer that covers that picture (sceDisplaySetFrameBuf, both the immediate flip and
 * the VBLANK-latched one) or submits that audio buffer to an sceAudioOutput* call.
 *
 * Every delivery is remembered as (physical byte range, 90 kHz pts, delivery time) in a
 * small bounded ring per stream.  A later present or submit whose byte range overlaps a
 * remembered delivery matches it, first time only:
 *   video  present time = guest time of the present
 *   audio  play time    = guest time of the submit + the host queue lead ahead of it
 * (sr_audio_queued() before the push, 44.1 kHz frames; no host queue (-1) is "not
 * measured" and is counted as such, it never reads as a lead of zero).
 * The per-stream presentation offset is play_time - pts (pts converted from 90 kHz to
 * microseconds), kept exactly in units of 1/9 microsecond because 1 tick = 100/9 us.  The A/V
 * offset is video offset - audio offset over a window of guest time, so a constant startup
 * delay common to both streams cancels while a stream running slow shows up as drift.
 *
 * A title that composites the movie draws the picture through the GE instead of presenting
 * its buffer.  While the measurement runs, each textured GE_PRIM reports its texture and its
 * render target (sr_ge_texture_sample_hook, ge.c); a draw whose level-0 texture overlaps a
 * waiting picture marks that picture composited into the draw's render target, and the first
 * present of a framebuffer overlapping that target is the picture's presentation time.  A
 * composited picture is not superseded by a later delivery into its buffer (it was already
 * drawn); one whose render target is never presented stays pending and is counted.
 *
 * Nothing here changes behaviour: no guest memory is written, no time advances, and the
 * only host call on a match is one sr_audio_queued() read.  A delivery that is never
 * presented/played (overwritten by a later delivery, pushed out of the ring, or still waiting
 * when the report is made) is counted, never hidden.
 *
 * Enabled by SR_MPEGAV (or SR_MPEGLOG).  Output: one PSMF_AV_PRESENT line per window and a
 * PSMF_AV_PRESENT_FINAL summary at scePsmfPlayerDelete and at process exit. */
#define PSMF_AV_PENDING 32u
enum { PSMF_AV_VIDEO = 0, PSMF_AV_AUDIO = 1 };

typedef struct {
    uint64_t seq;
    uint32_t lo, hi;            /* physical byte range [lo, hi) */
    int64_t pts;
    int64_t delivered_us;
    int pts_valid, live;
    int composited;             /* drawn by the GE into the render target [fb_lo, fb_hi) */
    uint32_t fb_lo, fb_hi;
    uint64_t content;           /* audio followed by its bytes (FNV-1a), 0 when not */
    uint32_t content_bytes;
} PsmfAvEntry;

typedef struct {
    PsmfAvEntry pend[PSMF_AV_PENDING];
    uint64_t delivered, matched, unmeasured, superseded, evicted, nospan;
    uint64_t boundary_calls, boundary_hits;
    uint64_t composited, composited_matched;   /* video only */
    uint64_t content_matched;                  /* audio only */
    uint64_t lat_sum_us;
    int64_t lat_max_us;
    int64_t win_sum_x9;
    uint64_t win_n;
} PsmfAvStream;

/* ModuleMgrForUser (h_module_uid removed; no current consumer -- sr_alloc_uid supplies module uids ). */

/* Forward decls for the callback infrastructure (defined at file scope below the VBLANK
 * sub-interrupt handler globals). h_CreateCallback and h_NotifyCallback reference them. */
typedef struct {
    uint32_t uid;
    uint32_t entry;
    uint32_t arg;
    uint32_t owner_thread_uid;
    int used;
    int pending;
    uint32_t notify_count;
    uint32_t notify_arg;
    char name[32];
} CallbackEntry;
#define SCE_KERNEL_MODULE_INIT_PRIORITY    32

/* Whether a Memory Stick open may create its file. sceIoOpen takes this from PSP_O_CREAT
 * (0x0200) and PSP_O_EXCL (0x0800) (#1380); the stdio mode then only says how the stream
 * reads, writes, truncates and appends. MS0_OPEN_BY_MODE keeps the stdio meaning (r opens
 * an existing file, w and a create one) for the runtime's own callers. */
typedef enum {
    MS0_OPEN_BY_MODE = 0,
    MS0_OPEN_EXISTING,   /* never create: a missing file fails */
    MS0_OPEN_NEW,        /* create a new file: an existing name fails */
} Ms0OpenExistence;

/* Unified Memory Stick root (issue #334): ordinary sceIo* and savedata share
 * sr_ms0_root/sr_ms0_resolve so one guest path maps to one host path. Foreign
 * devices (disc0:, ...) fail closed (NULL). Empty file guests are rejected;
 * empty directory guests resolve to the root itself. */
#ifdef _WIN32
#define SR_HOST_PATH_SEP '\\'
#else
#define SR_HOST_PATH_SEP '/'
#endif
typedef struct {
    int used;
    uint32_t codecType;
    uint32_t buf;                 /* guest ring buffer base */
    uint32_t size;                /* bytes of file data fed so far (first_.size) */
    uint32_t secondBuf;           /* sceAtracSetSecondBuffer guest address (0 = none) */
    uint32_t secondBufSize;       /* sceAtracSetSecondBuffer byte size */
    uint32_t bufferMaxSize;       /* ring capacity (SetData bufferSize) */
    uint32_t fileSize;            /* total track size in bytes (RIFF extent) */
    uint32_t dataByteOffset;      /* file offset where audio frames start */
    uint32_t bytesPerFrame;       /* frame size (RIFF fmt blockAlign) */
    uint32_t avgBytesPerSec;      /* RIFF fmt nAvgBytesPerSec (bitrate source) */
    uint16_t formatTag;           /* RIFF fmt wFormatTag */
    uint8_t at3Extradata[14];     /* validated AT3 WAVEFORMATEX extension */
    uint32_t at3FactSize;         /* RIFF fact chunk size (8 or 12) */
    uint32_t at3FactDelay;        /* fact word after the sample count */
    int at3FirstFramePending;     /* no frame decoded since SetData */
    int at3SkipApplied;           /* leading samples the first call dropped (guest position offset) */
    /* Streamed AT3: file frame 0, held from SetData to prime the decoder (#1096). */
    uint8_t at3PrimeFrame[1024];
    int at3PrimeValid;
    int endSample, posSample, loopNum;
    int loopStartSample, loopEndSample;   /* 'smpl' loop points; -1 when absent */
    uint32_t bufferPos;           /* ring offset of the next frame to consume */
    uint32_t bufferValidBytes;    /* bytes available for decode in the ring */
    uint32_t bufferHeaderSize;    /* physical header prefix; becomes zero after first wrap */
    int bufferState;              /* ATRAC_STATE_* */
    /* Host-side FIFO view of the same guest ring. The PSP stream may split one
     * encoded frame across the physical end; keeping the valid bytes in FIFO
     * order lets the decoder consume that lawful split without rereading the
     * RIFF prefix or assuming a fixed wrap base. */
    uint8_t *streamWindow;
    uint32_t streamWindowSize;
    uint32_t streamWindowRead;
    uint32_t streamWindowWrite;
    uint32_t streamWindowQueued;
    /* Separate decoder instances preserve the distinct AT3 and ATRAC3+
     * frame/config contracts. host_frame/host_pcm are per-slot staging. */
    Atrac3pBridge *dec;           /* owned decoder bridge, or NULL */
    Atrac3Bridge *dec_at3;        /* owned plain-AT3 bridge, or NULL */
    uint32_t dec_codec_type;
    uint8_t dec_at3_extradata[14];
    int channels;                 /* RIFF fmt channels (1..8) */
    int dec_channels;             /* bridge config (channels) at create time */
    int dec_align;                /* bridge config (bytesPerFrame) at create time */
    int decodeFailureReported;    /* one boundary diagnostic per stream failure */
    uint8_t *host_frame;          /* one frame copied out of the guest ring */
    int host_frame_cap;           /* allocated size of host_frame */
    int16_t *host_pcm;            /* decode output staging, channels*2048 */
} Atrac;
enum {
    UTILITY_SLOT_NONE = 0, UTILITY_SLOT_SAVEDATA, UTILITY_SLOT_MSG, UTILITY_SLOT_NETCONF,
    UTILITY_SLOT_GAMEDATA, UTILITY_SLOT_GAMESHARING, UTILITY_SLOT_OSK
};

/* Lightweight utility services used during setup. Each service keeps its own state machine,
 * and the one slot above decides which of them a call reaches, so polling netconf cannot
 * consume a message/gamedata transition. Init captures the common parameter block and
 * completes it successfully; Update advances RUNNING->FINISHED. Update during INIT returns
 * INVALID_STATUS and changes nothing: measured for the message dialog (PSPAutotests
 * tests/utility/dialog/status.expected) and the OSK (#363), unmeasured for netconf,
 * gamedata-install and game-sharing, which fail closed the same way. */
typedef struct UtilityDialog {
    uint32_t param;
    uint32_t result;
    uint32_t generation;
    int status;
    unsigned ticks;
    unsigned kind;
    unsigned work_started : 1;
    unsigned work_done : 1;
    unsigned aborted : 1;     /* sceUtilityMsgDialogAbort was accepted while visible */
} UtilityDialog;

/* ---- sceUtilityOsk: the on-screen keyboard, answered by a person through a native input
 * box (osk_win.c) or, for automation, from a scripted answer.
 *
 * The public contract (PSPSDK psputility_osk.h) owns the flow: InitStart creates the
 * keyboard, Update refreshes it, ShutdownStart removes it, and sceUtilityOskGetStatus
 * returns the common pspUtilityDialogState (psputility.h): NONE=0, INIT=1, VISIBLE=2,
 * QUIT=3, FINISHED=4. The SDK's own OSK sample drives it exactly so: INIT waits, VISIBLE
 * calls Update, QUIT calls ShutdownStart, FINISHED waits, NONE ends the loop, and the
 * header says to poll after ShutdownStart until NONE. QUIT is where the keyboard is
 * dismissed whether the person confirmed or cancelled; the per-field result
 * (SceUtilityOskResult: UNCHANGED=0, CANCELLED=1, CHANGED=2) tells the two apart.
 * SceUtilityOskState (INITING..FINISHED, 0..5) is a different enum: it describes the
 * parameter block's own state field, not the GetStatus return value.
 *
 * GetStatus reports the state it was in and advances at most one step, so every step is
 * observable however often the title polls: INIT -> VISIBLE, VISIBLE -> QUIT once every
 * field has its answer, QUIT stands until ShutdownStart, then FINISHED -> NONE.
 * sceUtilityOskUpdate keeps its named no-dialog compatibility result (#281), so the runtime
 * owns the progression.
 *
 * The keyboard never stops guest time. On the PSP it is a system overlay drawn while the
 * title keeps running: the title polls GetStatus and calls Update once per frame, keeps
 * drawing its own screen and waiting for vblank, and its other threads keep running. Every
 * guest thread here is a coroutine on the one scheduler thread, so a host text box waited on
 * from inside GetStatus would freeze all of them -- no vblank, no frame, no audio -- for as
 * long as the box is open, and forever under the offscreen presenter, where nobody can
 * answer it. The person's answer is therefore collected without waiting
 * (osk_text_entry.h): GetStatus keeps reporting VISIBLE while it is pending and writes the
 * field on the poll that finds it answered. The blocking native box itself is only ever
 * shown by the text-entry worker. */
enum { OSK_NONE = 0, OSK_INIT = 1, OSK_VISIBLE = 2, OSK_QUIT = 3, OSK_FINISHED = 4 };

/* Memory Stick insert/eject callback registrations made through sceIoDevctl
 * 0x02415821 on ms0:/fatms0:. The 32-entry bound, duplicate registrations each
 * taking an entry, unregister removing only the first match, and the 0x80010016
 * refusals follow PPSSPP's sceIo.cpp fatms0: model, applied here to ms0: as well
 * (PSP-IO-001, #311, has not measured them). Dropping a deleted callback's
 * registrations is a project model: hardware behaviour for a deleted registered
 * callback is unmeasured, and dropping it means it can never fire. */
enum { MEMSTICK_CALLBACK_MAX = 32 };

/* ---- VTimer (ThreadManForUser virtual timers) ----------------------------------------------
 * A VTimer is a stopwatch on the scheduler's guest microsecond timeline. It accumulates the
 * guest time that elapses while it runs and holds that total while it is stopped.
 *
 * Measured on the PSP-3000 oracle (2026-10-15, PSP-HLE-VTIMER-001, firmware 6.61, PR #1003
 * record at oracle/hle-measured-20261014 1d5df5c9): Create returns a valid UID; Start returns 0,
 * also when it restarts a stopped timer; Stop returns 1 while running and 0 when not started;
 * Delete of a running timer returns 0; Refer and Delete on the deleted UID return 0x800201BE and
 * Refer writes nothing. Refer returns 0 and fills the struct: active is 1 while running and 0
 * when created or stopped; base is non-zero while running and 0 when created or stopped; current
 * is 0 when created, non-zero while running, and holds the accumulated time while stopped (the
 * stopped Refer and both stopped GetVTimerTime reads return the same value); schedule, handler
 * and common read 0; a size word of 0x08 writes 8 bytes. StartVTimer returns 1 while the timer
 * already runs (docs/HARDWARE_ORACLE.md, callbacks/VTimer/alarms; not part of that entry).
 *
 * Measured on the PSP-3000 oracle (2026-10-15, firmware 6.6.1, build b5d6fc5b, PSP-HLE-VTIMER-002
 * and PSP-HLE-VTIMER-003; docs/HARDWARE_ORACLE.md measured index): Start, Stop and GetVTimerTime
 * on a deleted UID return 0x800201BE, and that GetVTimerTime leaves its output untouched; a
 * second Delete returns 0x800201BE; Delete of a stopped timer returns 0; 64 creates in a row
 * each return a UID, with no refusal. While running, GetVTimerBase and Refer return the same
 * base, a system-clock value; both read 0 once stopped. Consistent with the measurement, not
 * measured exactly:
 *   - The time unit is microseconds. A GetVTimerTime delta of 204,379 against a system-clock
 *     delta of 220,608 over intervals offset by the probe's host0 writes agrees within about 8%;
 *     that is not a precise rate measurement.
 *   - base = system time at the most recent Start, and time = system time - base while running
 *     from zero: base + time stayed below the next system-clock read at both reads. No system
 *     time was read at Start itself.
 *
 * Runtime safety choice, not a measured VTimer return: NULL mandatory out-pointers return
 * ILLEGAL_ADDR because the PSP-3000 hangs on ReferSystemStatus(NULL) (PSP-HLE-NULL-OUT-001),
 * so no game relies on the output.
 *
 * Other assumptions, not measured:
 *   - The limit above 64 timers and the no-slot CreateVTimer error are unmeasured.
 *   - A size word of 0 writes nothing. That is the status-write rule the sema, event-flag and
 *     mailbox calls share, not a VTimer-specific citation.
 *
 * The Wide getter and the two time setters are below their own header, after GetVTimerTime; the
 * handler family is below its own header. */
/* The PSP-3000 accepted 64 creates in a row without a refusal (PSP-HLE-VTIMER-003), so the table
 * holds 64 timers. What the console does past 64 is unmeasured. */
#define VTIMER_MAX 64
#define VTIMER_INFO_SIZE 0x48u        /* PSPSDK SceKernelVTimerInfo, 72 bytes */

typedef struct {
    int used;
    uint32_t uid;
    char name[32];
    int running;
    uint64_t accum_us;     /* elapsed guest time over the finished running intervals */
    uint64_t started_us;   /* guest time at the start of the open interval, when running */
    uint32_t handler;      /* guest handler, 0 when none */
    uint32_t common;       /* the handler's fourth argument */
    uint64_t schedule;     /* timer time of the next handler call */
} VTimer;

/* sceKernelReferVTimerStatus(uid, SceKernelVTimerInfo *). Field offsets follow the PSPSDK
 * struct: size(0), name[32](4), active(36), then base, current and schedule as 8-byte clocks,
 * handler and common. Measured (PSP-HLE-VTIMER-001, PR #1003 record): active, base, current,
 * and schedule/handler/common = 0, with the states given in the header above. base equals
 * GetVTimerBase while running and reads 0 once stopped (PSP-HLE-VTIMER-002); base = system time
 * at Start is consistent with that run, not measured exactly. current is the same elapsed value
 * GetVTimerTime returns, as a 64-bit low/high pair. The microsecond unit is consistent with the
 * measurement, not a precise rate measurement (see the header). */
typedef struct {
    uint32_t size;          /* 0x00: struct size (0x48) */
    char     name[32];      /* 0x04: create-time name */
    uint32_t active;        /* 0x24: 1 while running, 0 when created or stopped (measured) */
    uint32_t base[2];       /* 0x28: guest time at the last Start while running, else 0 */
    uint32_t current[2];    /* 0x30: elapsed guest time, low/high (measured shape) */
    uint32_t schedule[2];   /* 0x38: timer time of the next handler call (0 until one is set) */
    uint32_t handler;       /* 0x40: the handler, 0 when none */
    uint32_t common;        /* 0x44: the handler's common argument */
} SceKernelVTimerInfo;
typedef char vtimer_info_is_72_bytes[(sizeof(SceKernelVTimerInfo) == VTIMER_INFO_SIZE) ? 1 : -1];

enum {
    RTC_ILLEGAL_ADDR = 0x80000103u,
    /* PSPAutotests tests/rtc/convert.expected: the RTC conversion family reports
     * invalid dates AND invalid output pointers as 0x800001fe
     * (SCE_KERNEL_ERROR_INVALID_VALUE): "Min year: 800001fe", "Year overflow:
     * 800001fe", "Zeroed time: 0 (800001fe)", "NULL filetime: -1337 (800001fe)". */
    RTC_INVALID_VALUE = 0x800001feu,
    RTC_UNIX_EPOCH_TICK = 62135596800000000ull,
    RTC_FILETIME_EPOCH_TICK = 50491123200000000ull,
};

#define SCE_ERROR_KERNEL_TOO_MANY_OPEN_FILES 0x80020320u
#define SCE_ERROR_KERNEL_BAD_FILE_DESCRIPTOR 0x80020323u
#define SCE_ERROR_KERNEL_INVALID_ARGUMENT    0x80020324u
#define SCE_ERROR_KERNEL_ASYNC_BUSY          0x80020329u
#define SCE_ERROR_KERNEL_NO_ASYNC            0x8002032au

typedef enum {
    IO_ASYNC_IDLE = 0,
    IO_ASYNC_PENDING,
    IO_ASYNC_COMPLETE,
} IoAsyncState;

typedef enum {
    IO_ASYNC_NONE = 0,
    IO_ASYNC_READ,
    IO_ASYNC_WRITE,
    IO_ASYNC_LSEEK32,
    IO_ASYNC_LSEEK64,
    IO_ASYNC_IOCTL,
    IO_ASYNC_CLOSE,
    IO_ASYNC_OPEN,
} IoAsyncOp;

typedef struct {
    IoAsyncState state;
    IoAsyncOp op;
    int64_t result;
    uint64_t submitted_epoch;
    uint64_t sequence;
    int32_t priority;
    uint32_t a1, a2, a3, stack0, stack1;
    uint32_t callback_uid, callback_arg;
} IoAsyncRequest;

typedef enum {
    FD_KIND_UNUSED = 0,
    FD_KIND_STD = 1,
    FD_KIND_FILE = 2,
    FD_KIND_RAW_DEVICE = 3,
    FD_KIND_DIRECTORY = 4,
} FdKind;

typedef struct {
    int used;
    FdKind kind;                 /* descriptor identity is independent of its number */
    uint8_t std_stream;          /* 0=stdin, 1=stdout, 2=stderr for FD_KIND_STD */
    uint8_t raw_umd;             /* 1 = UMD block device: size and off count 2048-byte sectors */
    uint32_t lba, size, off;
    int64_t async_res;
    int writable;
    int flash0;                  /* 1 = served by the read-only flash0: font device */
    IoAsyncRequest async;
    FILE *host;
    SrPgd *pgd;
    SrArchiveVfs *archive_vfs;
    size_t archive_mount;
    size_t archive_entry;
    const uint8_t *enhancement_data; /* rewritten bytes of an enhancement data file, or NULL */
    char guest_path[256];        /* the path the guest opened it with ("" if too long) */
} Fd;

typedef struct {
    int used;
    int backend;                 /* 0 = ISO9660, 1 = merged host VFS namespace */
    uint32_t index;
    SrVfsDirList list;           /* backend 1: fully materialized at Dopen */
    char *path;
} DirFd;

typedef enum {
    IO_RAW_BOUNDARY_OPEN_FLAGS = 0,
    IO_RAW_BOUNDARY_OPEN_ASYNC,
    IO_RAW_BOUNDARY_READ,
    IO_RAW_BOUNDARY_WRITE,
    IO_RAW_BOUNDARY_LSEEK32,
    IO_RAW_BOUNDARY_LSEEK64,
    IO_RAW_BOUNDARY_IOCTL,
    IO_RAW_BOUNDARY_GETSTAT,
    IO_RAW_BOUNDARY_DEVCTL,
    IO_RAW_BOUNDARY_DOPEN,
    IO_RAW_BOUNDARY_READ_ASYNC,
    IO_RAW_BOUNDARY_WRITE_ASYNC,
    IO_RAW_BOUNDARY_LSEEK32_ASYNC,
    IO_RAW_BOUNDARY_LSEEK64_ASYNC,
    IO_RAW_BOUNDARY_IOCTL_ASYNC,
    IO_RAW_BOUNDARY_CLOSE_ASYNC,
    IO_RAW_BOUNDARY_ASYNC_STATUS,
    IO_RAW_BOUNDARY_ASYNC_PRIORITY,
    IO_RAW_BOUNDARY_ASYNC_CALLBACK,
    IO_RAW_BOUNDARY_DREAD,
    IO_RAW_BOUNDARY_DCLOSE,
    IO_RAW_BOUNDARY_READ_PAST_END,
} IoRawDeviceBoundary;

typedef struct {
    uint32_t thread_uid;
    char path[512];
    int used;
} IoCwd;
#define IO_UMD_POSITION_REFUSED 0x8001b003u
#define SR_DATA_STATE_READY         2

/* Devices whose reads the archive VFS may serve: the disc (disc0:, umd:) and the
 * development host devices (host0: ... host9:), which the extracted-tree route also
 * serves through host_data_lookup. Memory-stick paths (ms0:, fatms0:) are excluded, so
 * save data never resolves to a disc archive member. */
typedef enum {
    IO_RAW_DEVICE_NONE = 0,
    IO_RAW_DEVICE_UMD0,
    IO_RAW_DEVICE_UMD1,
    IO_RAW_DEVICE_UMD_ALIAS,
    IO_RAW_DEVICE_MSSTOR0P1,
} IoRawDevicePath;
/* `ts` is stamped when the sample is latched, not when it is read. SceCtrlData.TimeStamp
 * contains the low 32 bits of the guest microsecond system clock at latch time
 * (corroborated by uOFW sceKernelGetSystemTimeLow, PSPAutotests ctrl/vblank, and emulator consensus). */
/* `input_id` is the scripted-input delivery id current when the sample was latched (0 when
 * nothing scripted was), so a guest read can report which scripted state it was handed. */
typedef struct { uint32_t btn; uint32_t ts; uint8_t lx, ly; uint32_t input_id; } CtrlSample;


/* ---- state-qualified acceptance routes (issue #64) --------------------------------
 *
 * A pad script written as "press CROSS at vblank 8600" is a bet that the guest is on the
 * screen its author saw when they recorded it. Boot and transition durations vary between
 * otherwise identical replays, so that bet loses: seven replays of one script from one
 * restored save baseline reached two different menu depths, and the two divergent runs
 * spent their whole budget in Story Mode instead of the intended Exhibition match. Both
 * still reported a complete run, because "reached vblank N" was the only thing anything
 * checked. Elapsed vblanks are not state.
 *
 * A route program replaces the bet with a measurement. Steps run in sequence, and a step
 * that names a screen does not complete until that screen is observed:
 *
 *   SIGGRID <cols> <rows>        signature grid (default 12x8); must precede CHECKPOINT
 *   SAMPLE_EVERY <vblanks>       observation cadence (default 20)
 *   TOLERANCE <n>                default per-checkpoint match tolerance (default 12)
 *   CHECKPOINT <NAME> [tol=<n>] <hex>
 *                                a screen signature; repeat NAME for alternates
 *   WAIT <NAME> <timeout>        block until NAME is observed; fail loudly on timeout
 *   EXPECT <NAME>                assert NAME is on screen right now; fail loudly if not
 *   PRESS <hexmask> <width>      hold mask for width vblanks
 *   DELAY <n>                    advance n vblanks (input cadence within one screen)
 *   PRESS_UNTIL_NID <import|0xNID> <hexmask> <width> <period> <timeout>
 *                                repeat the press (held width, released for the rest of period)
 *                                until the guest calls that import; fail on timeout
 *   END                          route complete
 *   WIDTHS VBLANKS|READS         the unit the widths of PRESS, DELAY, PRESS_UNTIL, PRESS_WHILE
 *                                and PRESS_UNTIL_NID count in (default VBLANKS); before every step
 *   READS | VBLANKS <step>       the unit of that one PRESS, DELAY, PRESS_UNTIL, PRESS_WHILE or
 *                                PRESS_UNTIL_NID
 *
 * A width in READS is a number of guest controller reads that observe the press, not of
 * vblanks: the press is held until the guest has read it that many times, however many
 * vblanks a starved host covered meanwhile (scripted_input.h). The timeouts of WAIT, EXPECT,
 * WAIT_NID and PRESS_UNTIL / PRESS_WHILE stay in vblanks, and so does every legacy row.
 *
 * "Observed" means a coarse signature of the presented framebuffer: the frame is divided
 * into cols x rows cells and each cell contributes its mean R, G and B. A screen matches
 * when the mean absolute difference against a recorded signature is within tolerance.
 * This is a private-title acceptance signal, not a PSP oracle: it proves which screen the
 * emulated guest reached, nothing about hardware. It is used because the alternatives are
 * worse -- guest menu-state addresses would be exactly the title-address patching this
 * project forbids, and no existing runtime event distinguishes a menu from its submenu.
 *
 * Failure is loud and terminal: ROUTE_FAIL on stderr and exit 86, so a run that reached
 * the wrong screen can never be archived as a successful route. SR_ROUTE_NO_EXIT keeps
 * the process alive for the executable regression tests (presence-based, like the other
 * legacy SR_* switches); SR_ROUTE_LEARN prints every sampled signature so a new checkpoint
 * can be recorded from a route the author has visually identified.
 *
 * Signatures are derived from retail frames and therefore belong in the private route
 * file beside the rest of the run inputs; nothing here writes one into the repository.
 *
 * A file with no keyword lines keeps the original "frame hexmask width" behaviour exactly.
 */
#define ROUTE_MAX_CELLS  192          /* 16x12 */
#define ROUTE_SIG_MAX    (ROUTE_MAX_CELLS * 3)
typedef struct {
    uint32_t addr;
    int32_t stride;
    int32_t fmt;
} DisplayFrameState;

typedef struct {
    uint32_t uid;
    uint32_t nid;
    uint32_t call_pc;
    uint32_t ra;
    uint32_t frame;
    const char *name;
    uint64_t seq;
} GeEnqueueTraceHle;

typedef struct {
    uint32_t uid;
    uint32_t callback_uid;
    uint32_t entry;
    uint32_t frame;
    uint64_t seq;
} GeEnqueueTraceCallback;

#define SR_VRAMDUMP_MAX_VBLANKS 8

/* sceGe_user: the GE list queue and its callbacks. eDRAM at 0x04000000. */
typedef struct {
    int used;
    uint32_t signal_func, signal_arg;
    uint32_t finish_func, finish_arg;
} GeCallback;

#define GE_LIST_MAX 64

/* The GE driver's display lists (#1374). PSP-GE-SYNC-001 cells cited here and below are
 * captured on a PSP-3000 (two runs on 2026-10-31, not yet acceptance-eligible: see
 * tools/psp_oracle/manifest.json). The structure follows the driver as uofw
 * reconstructs it (github.com/uofw/uofw src/kd/ge/ge.c, MIT): 64 list slots, a queue of
 * active lists that the GE runs one at a time in enqueue order, and a FIFO of free slots
 * that a list joins when it completes or is dequeued, and that an enqueue takes from the
 * front. A queue id names a slot, so an id stays in range after its list is gone and then
 * reads whatever the slot holds, as on a PSP.
 *   GE_LIST_FREE       no list. A peek names no list (0x80000100): PSP-GE-CONTROL-001 and
 *                      PSP-GE-SYNC-001 ge-list-sync-cancelled-poll.
 *   GE_LIST_STALLED    the active head, waiting at its stall address.
 *   GE_LIST_COMPLETED  ran to its END. Its slot is free for reuse already, and a
 *                      sceGeDrawSync(0) turns it FREE (uofw sceGeDrawSync).
 *   GE_LIST_PAUSED     the head, paused by sceGeBreak(0).
 *   GE_LIST_DRAWING    reached its END; the opt-in timing model has not completed it yet.
 *   GE_LIST_QUEUED     enqueued behind the head; it has not started.
 * The blocking sceGeListSync waits on a completion bit per slot, as the driver waits on an
 * event flag bit per list: an enqueue clears the slot's bit, the list's completion or its
 * dequeue sets it, and a sceGeBreak(1) reset leaves it alone, so a list the reset removed
 * keeps its waiter parked (PSP-GE-SYNC-001 ge-list-sync-cancelled-wait: no return) while a
 * completed list answers at once even after its slot was freed (ge-list-sync-stale-wait).
 * The bits start set: the driver creates its event flags with every bit set. */
enum {
    GE_LIST_FREE = 0,
    GE_LIST_STALLED = 1,
    GE_LIST_COMPLETED = 2,
    GE_LIST_PAUSED = 3,
    GE_LIST_DRAWING = 5, /* reached its END; the opt-in timing model has not completed it yet */
    GE_LIST_QUEUED = 6,
};

/* Blocking sceGeListSync/sceGeDrawSync park the caller on this object and
 * re-read the list table each time it is woken (#1374). Every transition that can
 * end such a wait wakes it: a list completing or being dequeued, and a sceGeBreak(1)
 * reset. */
#define GE_SYNC_WAIT_OBJ 0x47455359u /* "GESY" */

typedef struct {
    uint32_t uid;
    uint32_t start_pc;
    uint32_t current_pc;
    uint32_t stall_addr;
    uint32_t cbid;
    uint32_t cbarg;
    int status; // GE_LIST_*
    int executed; // the GE ran this list to its END; a later stall address must not re-run it
    int busy; // the list has started; only sceGeDrawSync(0) or a reset clears it (uofw isBusy)
    uint64_t order; // enqueue order: the active queue runs the oldest first
    uint32_t finish_id; // the 16-bit argument of the FINISH that ended it: the callback's id
    uint64_t done_us; // GE_LIST_DRAWING: modelled completion time (guest us)
    uint64_t finish_seq; // GE_LIST_DRAWING: completion order between equal times
} GeListInfo;

/* ---- message pipes -----------------------------------------------------------------
 *
 * HST uses the nonblocking message-pipe API as a byte FIFO for its worker queues.  Keep the
 * payload in host-owned kernel memory (as the PSP kernel does), and copy only at the send/receive
 * boundary.  waitMode 0 requests a complete transfer; nonzero mode permits the largest transfer
 * currently possible and reports that byte count through resultSize.
 *
 * PSP-resource model (issue #178): the PSP kernel allocates a pipe buffer from kernel memory,
 * which is a small partition (single-digit MBs on retail).  A request larger than
 * MSG_PIPE_MAX_CAPACITY is outside the modeled range and is rejected BEFORE any host
 * allocation, UID hand-out, or slot reservation -- a malformed guest can therefore never make
 * the host allocate gigabytes.  The ceiling is a project-level hard cap for this loader, not
 * a claim about the exact retail error code for oversized requests: ILLEGAL_SIZE is the
 * deterministic in-model rejection (NO_MEMORY would also be defensible for an allocation the
 * model cannot satisfy; pick one and keep it documented). */

#define MSG_PIPE_MAX 32
/* 1 MiB ceiling: far above the title's observed FIFO usage and comfortably within
 * a small kernel partition, while keeping the 32-slot worst case at 32 MiB of
 * host memory.  Rejected creates return SCE_KERNEL_ERROR_ILLEGAL_SIZE. */
#define MSG_PIPE_MAX_CAPACITY 0x100000u
#ifndef SCE_KERNEL_ERROR_ILLEGAL_ADDR
#define SCE_KERNEL_ERROR_ILLEGAL_ADDR  0x80000103u
#endif
typedef struct {
    SrWaitHandle invocation;
    int is_send;
    uint32_t size, wait_mode, buffer, resultp, progress, toptr;
    uint64_t deadline;
} MsgPipeWaiter;
typedef struct {
    int used;
    uint32_t uid, attr, capacity, read_pos, write_pos, count;
    uint32_t send_calls, receive_calls;
    SrWaitHandle reservation;
    uint32_t reserved;
    uint8_t *data;
    MsgPipeWaiter *waiters;
    int nwaiters, wait_cap;
    char name[32];
} MsgPipe;

/* ---- mailboxes (sceKernel*Mbx), campaign psp-hw-20260917 + PSPSDK ABI ---- */

#define MBX_MAX 32
#define MBX_MAX_WAITERS 64
#define MBX_MAX_MSGS 128

typedef struct {
    int used;
    uint32_t uid;
    uint32_t attr;
    char name[32];
    uint32_t msgs[MBX_MAX_MSGS];   /* guest SceKernelMsgPacket addresses, delivery order */
    int nmsgs;
    uint32_t waiters[MBX_MAX_WAITERS];
    int nwaiters;
} Mbx;

/* ---- semaphores and event flags, backed by the scheduler's block/wake-on-object ---- */

/* Semaphores, event flags and LwMutexes share this table, so every slot carries
 * the kind of object it holds and every lookup names the kind it accepts
 * (issue #1389). A UID of another kind is the calling API's own unknown-ID
 * error, as for an unknown or deleted UID: UNKNOWN_SEMID, UNKNOWN_EVFID or
 * LWMUTEX_NOT_FOUND. Before this, SetEventFlag on a semaphore UID ORed bits
 * into the semaphore's slot and DeleteEventFlag freed the semaphore. The kind also
 * lets sceKernelGetThreadmanIdList list one class at a time. */
typedef struct {
    int used; uint32_t uid; int count, maxc; uint32_t pattern;
    uint32_t attr, init_pattern;   /* event flags: create-time values for Refer */
    int initc;                     /* semaphores: create-time count for CancelSema(-1); LwMutex initialCount */
    uint32_t workarea;             /* LwMutex workarea address */
    char name[32];                 /* object name */
    int kind;                      /* SYNC_KIND_*: the object class sharing this table */
} Sync;
enum { SYNC_KIND_SEMA = 1, SYNC_KIND_EVENT_FLAG, SYNC_KIND_LWMUTEX };
#ifndef SCE_KERNEL_ERROR_ILLEGAL_ADDR
#define SCE_KERNEL_ERROR_ILLEGAL_ADDR               0x80000103u
#endif

typedef struct {
    uint32_t thread_uid;
    SrWaitHandle invocation;
    int      requested_count;
} MutexWaiter;

typedef struct Mutex {
    uint32_t     uid;
    char         name[32];
    uint32_t     attr;
    int          init_count;
    int          current_count;
    uint32_t     lock_thread;
    int          num_wait_threads;
    uint32_t     cancel_seq;
    MutexWaiter *waiters;
    int          wait_count;
    int          wait_cap;
} Mutex;

/* Event-flag waiter records (issue #1389). A thread blocked in
 * sceKernelWaitEventFlag[CB] holds one record naming the flag and its outBits
 * address, so DeleteEventFlag can write each waiter's outBits when it ends the
 * wait. A record is used only while its thread is still blocked on that flag
 * (evf_waiter_live): a thread terminated in the wait leaves a dead record that
 * is never written through and is reclaimed. A thread blocks on one object at a
 * time, so live records never outnumber the scheduler's 128 threads. */
#define EVF_WAITER_MAX 128
typedef struct { uint32_t evf, thread, outp, bits, mode, toptr; uint64_t order, deadline; int active; } EvfWaiter;

/* Defined in one runtime HLE source and used from another (#307). They had internal linkage
 * in hle.c; hidden visibility keeps them out of a shared library's exports. */
#pragma GCC visibility push(hidden)
void sr_host_sleep_ms(uint32_t ms);
uint64_t sr_host_monotonic_ms(void);
uint64_t sr_host_filetime_ticks(void);
void hle_register_unsupported_summary(void);
extern LateImportEntry *s_late_imports;
extern unsigned s_late_import_n, s_late_import_cap;
int hle_log_on(void);
int hle_syslog_on(void);
int hle_inlog_on(void);
int hle_wakelog_on(void);
int ge_log_on(void);
void hle_lock(void);
void hle_unlock(void);
void sr_hle_register_unsupported(uint32_t nid, const char *name, uint32_t error);
HleEntry *hle_find(uint32_t nid);
extern uint32_t s_uid;
extern uint32_t s_heap;
extern uint32_t sr_last_alloc_addr;
void user_partition_init(void);
extern Block s_blocks[HLE_MAX_PARTITION_BLOCKS];
uint32_t user_partition_top(void);
extern int s_nblocks;
extern HleMemoryBlock s_memory_blocks[HLE_MAX_MEMORY_BLOCKS];
extern FreeBlock s_free_blocks[HLE_MAX_FREE_BLOCKS];
extern int s_nfree_blocks;
uint32_t alloc_block(uint32_t size);
int partition_high_modelled(void);
uint32_t alloc_block_high(uint32_t size);
uint32_t block_addr(uint32_t uid);
extern uint32_t g_sdk_version;
uint32_t sreg_systemparam_int(uint32_t id);
extern SregNode s_sreg_nodes[SREG_MAX_NODES];
extern SregRegSlot s_sreg_reg[SREG_REG_SLOTS];
extern SregCatSlot s_sreg_cat[SREG_CAT_SLOTS];
extern int s_sreg_dirty;
void sreg_free_node(int i);
int sreg_child(int parent, const char *name);
int sreg_add(int parent, const char *name, uint32_t type, uint32_t size, uint8_t flags);
int sreg_walk(const char *path, int create, uint8_t flags, uint32_t *err, int *created);
int sreg_node_path(int node, char *out, size_t cap);
uint32_t sreg_ordered(int parent, int *out);
int sreg_find_path(const char *path);
void flash0_font_sources(Flash0Sources *out);
void sreg_build_font_registry(void);
int sr_utf8_env_alloc(const wchar_t *name, char **value_out, int *present_out);
char *sreg_overlay_path(void);
int sreg_read_file(const char *path, char **out, size_t *len_out, char *why, size_t why_len);
int sreg_hex_nibble(char c);
uint32_t sreg_type_from_name(const char *name);
const char *sreg_type_name(uint32_t type);
extern PspConsoleProfile s_console;
uint32_t sreg_persist(const char *caller);
void sreg_ensure_loaded(void);
void sreg_note_placeholder_read(int node, const char *caller);
extern uint32_t s_ctrl_sampling_mode;
extern uint32_t s_ctrl_sampling_cycle;
__attribute__((format(printf, 2, 3))) uint32_t hle_unmodelled_stop(const char *import_name, const char *fmt, ...);
int guest_cstr(uint32_t addr, char *out, int max);
uint32_t free_block(uint32_t uid);
extern FplPool s_fpls[FPL_MAX];
FplPool *fpl_lookup(uint32_t uid);
void fpl_remove_waiter(FplPool *p, uint32_t thread_uid);
extern VplPool s_vpls[VPL_MAX];
void vpl_release(VplPool *p);
void vpl_remove_waiter(VplPool *p, uint32_t thread_uid);
VplPool *vpl_lookup(uint32_t uid);
extern TlsplPool s_tlspls[TLSPL_MAX];
void tlspl_release_thread(TlsplPool *p, uint32_t thread_uid);
uint32_t h_WaitThreadEnd_impl(CpuState *s, int is_cb);
uint32_t ge_call_guest_rv(CpuState *s, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2);
uint32_t ge_call_guest_gp_rv(CpuState *s, uint32_t fn, uint32_t gp, uint32_t a0, uint32_t a1, uint32_t a2);
extern LoadedModule s_loaded_modules[HLE_MAX_LOADED_MODULES];
extern int s_nloaded_modules;
extern uint32_t s_deferred_self_stop_uid;
extern uint32_t s_deferred_self_stop_thread_uid;
extern SrLoadExecRequest s_loadexec_request;
extern int s_loadexec_pending;
extern uint32_t s_loadexec_boot_uid;
LoadedModule *find_loaded_module(uint32_t uid);
LoadedModule *alloc_loaded_module_slot(void);
void retire_loaded_module(LoadedModule *mod);
extern uint32_t s_last_prx_entry;
extern uint32_t s_last_prx_stop;
extern uint32_t s_last_prx_base;
unsigned register_prx_exports(const char *host_path, uint32_t base);
extern uint32_t s_prx_stage_base, s_prx_stage_len, s_prx_stage_cap;
extern int s_prx_staging;
extern PrxImage s_prx_images[HLE_MAX_LOADED_MODULES];
extern unsigned s_prx_image_count;
extern atomic_int s_prx_started_count;
void mark_module_started(uint32_t entry);
void unload_prx_image(uint32_t base);
int prx_image_bounds(uint32_t base, uint32_t *start_out, uint32_t *end_out);
extern uint32_t s_psmfplayer_lib_version;
int load_prx_image(const char *host_path, uint32_t base, const char *name);
int loadexec_module_file_matches_translation(const char *file);
uint32_t load_module_by_guest_path(const char *path, uint32_t option);
uint32_t h_ok(CpuState *s);
void hle_register_icache_handlers(void);
void hle_register_dcache_handlers(void);
extern SrPsmfPlayer s_psmf_players[4];
extern unsigned long s_psmf_calls , s_psmf_getvideo , s_psmf_getaudio;
#ifdef SR_MPEG_MEDIA_SELFTEST
extern int s_psmf_av_test_clock_set, s_psmf_av_test_queue_set;
#endif
#ifdef SR_MPEG_MEDIA_SELFTEST
extern int s_psmf_av_test_queue;
#endif
int64_t psmf_av_now_us(void);
int psmf_av_queue_frames(int ch);
void psmf_av_window_tick(int64_t now_us);
void psmf_av_final(const char *reason);
void psmf_av_exit_report(void);
PsmfAvEntry *psmf_av_delivered(int stream, uint32_t first, uint32_t bytes, int64_t pts, int pts_valid);
uint64_t psmf_av_content(uint32_t addr, uint32_t bytes);
uint32_t psmf_av_texture_bytes(uint32_t bufw, uint32_t h, uint32_t fmt);
void psmf_av_match(PsmfAvStream *st, PsmfAvEntry *e, int64_t play_us, int play_ok);
void psmf_av_video_presented(uint32_t addr, int32_t stride, int32_t fmt);
void psmf_av_audio_by_content(PsmfAvStream *st, int ch, uint32_t buf, uint32_t bytes, uint32_t bytes_per_frame, uint32_t rate_hz);
void psmf_av_audio_submit(int ch, uint32_t buf, uint32_t bytes, uint32_t bytes_per_frame, uint32_t rate_hz);
void psmf_player_release(SrPsmfPlayer *p);
FILE *sr_fopen_utf8(const char *path, const wchar_t *mode);
extern uint32_t s_main_module_uid;
extern const SrMainModuleRecord *s_main_module_record;
int another_record_references_image(const LoadedModule *mod);
int loadexec_ms0_path(const char *path);
int loadexec_manifest_file_safe(const char *file);
extern uint32_t s_exit_cb_uid;
extern int s_exit_request_pending;
extern uint64_t s_exit_request_deadline_us;
int sr_utf8_to_wide_alloc(const char *src, wchar_t **out);
int sr_wide_to_utf8_alloc(const wchar_t *src, char **out);
int sr_wide_env_alloc(const wchar_t *name, wchar_t **value_out, int *present_out);
int sr_wide_join_alloc(const wchar_t *left, const wchar_t *right, wchar_t **out);
int sr_wide_configured_root_wide_alloc(const wchar_t *value, wchar_t **out);
int sr_wide_path_alloc(const char *utf8_path, wchar_t **out);
int sr_wide_module_dir_alloc(wchar_t **out);
int sr_wide_parent_alloc(const wchar_t *path, wchar_t **out);
int sr_wide_basename_alloc(const wchar_t *path, wchar_t **out);
int sr_wide_module_font_root(wchar_t **out);
int sr_stream_size_u32(FILE *stream, uint32_t *size_out);
int sr_ensure_directory_utf8(const char *path);
#ifdef _WIN32
FILE *ms0_fopen_existence_utf8(const char *host_path, const wchar_t *mode, Ms0OpenExistence existence);
#endif
FILE *ms0_fopen_utf8(const char *host_path, const wchar_t *mode);
#ifndef _WIN32
int sr_wide_stricmp(const wchar_t *left, const wchar_t *right);
#endif
#ifndef _WIN32
int sr_narrow_mode_alloc(const wchar_t *mode, char **out);
#endif
#ifndef _WIN32
sr_cd_status ms0_posix_guest_relpath(const char *guest, char *rel, size_t cap);
#endif
#ifndef _WIN32
uint32_t ms0_posix_psp_error(sr_cd_status status);
#endif
#ifndef _WIN32
sr_cd_status ms0_posix_root_open(sr_cd_root *root);
#endif
#ifndef _WIN32
FILE *ms0_fopen_guest_utf8(const char *guest_path, const wchar_t *mode, Ms0OpenExistence existence, sr_cd_status *status_out);
#endif
char *sr_utf8_join_alloc(const char *left, const char *right, char separator);
char *host_path_alloc(const char *guest);
extern atomic_ulong s_ms0_legacy_import_count;
int ms0_try_legacy_flat_import(const char *guest_path, const char *host_path);
void flash0_copy_root(char *dst, size_t capacity, const char *src);
void flash0_user_data_root(char *dst, size_t capacity);
void flash0_refuse_font_dir(const char *reason);
extern Atrac s_atrac[8];
void atrac_stream_window_free(Atrac *a);
int audio_stat_on(void);
extern unsigned long g_sas_calls, g_sas_calls_add, g_sas_pre_nonzero, g_sas_post_nonzero;
extern unsigned long g_sas_erased;
extern int g_sas_pre_peak, g_sas_post_peak;
extern unsigned long g_atrac_frames, g_atrac_frames_nonzero;
extern int g_atrac_peak;
extern unsigned long g_sas_no_voice, g_sas_no_voice_overwrite;
extern uint32_t g_sas_bufs[4];
extern int g_sas_nbufs;
extern uint32_t g_atrac_out_bufs[4];
extern int g_atrac_out_nbufs;
void atrac_release_decoder(Atrac *a);
extern int s_atrac_caps_configured;
extern uint32_t s_atrac_slot_codec[8];
void dialog_lock(void);
void dialog_unlock(void);
extern int s_utility_slot;
uint64_t rtc_now_tick(void);
int64_t psp_profile_local_offset_minutes(int32_t *zone_out, uint32_t *daylight_out);
extern int s_dlg_status , s_dlg_tick;
extern uint32_t s_dlg_param , s_dlg_param_size , s_dlg_result , s_dlg_generation;
extern unsigned s_dlg_work_started, s_dlg_work_done;
extern UtilityDialog s_msg_dialog;
extern UtilityDialog s_net_dialog;
extern UtilityDialog s_gamedata_dialog;
extern UtilityDialog s_sharing_dialog;
void utility_dialogs_reset_locked(void);
extern int s_osk_status;
extern uint32_t s_osk_param;
extern int s_osk_field;
void osk_abandon_request(void);
void hle_register_utility_dialog_handlers(void);
int console_profile_identity(void);
extern int s_volatile_locked;
void hle_register_umd_callback_handlers(void);
extern size_t s_callbacks_cap;
extern uint32_t s_memstick_callbacks[MEMSTICK_CALLBACK_MAX];
extern uint32_t s_memstick_callback_count;
int memstick_callback_remove_first(uint32_t uid);
int sr_thread_dispatch_callback_uid(uint32_t thread_uid, CpuState *cpu, uint32_t selected_uid);
int hle_dispatch_guest_scoped(CpuState *s, uint32_t fn, const char *owner, uint32_t *fatal_target);
extern uint32_t s_vcount_fwd;
extern VTimer s_vtimers[VTIMER_MAX];
VTimer *vtimer_lookup(uint32_t uid);
extern uint32_t s_vtimer_in_handler;
uint32_t io_guest_path(uint32_t guest_addr, char *out, size_t capacity);
extern Fd s_fds[64];
extern DirFd s_dirfds[64];
extern int64_t s_closed_res[64];
extern IoAsyncState s_closed_async_state[64];
extern uint64_t s_io_dispatch_epoch;
extern uint64_t s_io_async_sequence;
extern IoCwd s_io_cwds[64];
IoCwd *io_cwd_find(uint32_t thread_uid, int create);
void hle_fd_init(void);
int hle_fd_is_file(uint32_t fd);
int hle_fd_is_raw_device(uint32_t fd);
uint32_t hle_io_raw_device_boundary(IoRawDeviceBoundary operation, const char *api, const char *path, uint32_t fd);
uint32_t hle_io_raw_fd_boundary(uint32_t fd, IoRawDeviceBoundary operation, const char *api);
int hle_fd_is_raw_umd(uint32_t fd);
int hle_fd_allocate(void);
extern SrAssetIndex s_data_index;
extern SrArchiveVfs s_archive_vfs;
extern char *s_data_root_utf8;
extern int s_data_archive_mode;
extern atomic_int s_data_state;
char *data_loose_path_alloc(const char *root_utf8, const char *key);
int data_loose_file_open(const char *key, FILE **file_out, uint32_t *size_out);
char *data_normalize_guest_key(const char *guest_path, int *wanted_variant);
const SrAssetIndexEntry *host_data_lookup(const char *guest_path);
int io_path_equals_ci(const char *path, const char *expected);
int io_path_is_disc_device(const char *path);
IoRawDevicePath io_raw_device_path(const char *path);
int data_archive_guest_path_allowed(const char *guest_path);
uint32_t h_io_open_path(const char *path, uint32_t flags, int forced_slot);
int io_host_seek(FILE *host, uint32_t offset);
uint32_t h_IoWrite(CpuState *s);
uint32_t h_IoRead(CpuState *s);
uint32_t io_seek_destination(const Fd *f, int64_t offset, uint32_t whence, const char *api, uint32_t *position);
uint32_t h_IoLseek32(CpuState *s);
uint32_t h_io_ioctl_args(uint32_t fd, uint32_t cmd, uint32_t in, uint32_t inlen, uint32_t out, uint32_t outlen);
int io_memstick_capacity(uint32_t info[5]);
uint32_t io_async_complete_fd(uint32_t fd);
void io_async_dispatch_tick(void);
void hle_register_iofilemgr_handlers(void);
extern uint32_t g_audio_bufs[9][4];
extern int g_audio_nbufs[9];
void hle_audio_guest_reset(void);
void hle_register_regular_audio_handlers(void);
void hle_register_output2_audio_handlers(void);
void sr_dump_calls(void);
extern CtrlSample s_ctrl_live;
#ifdef SR_HLE_THREAD_SELFTEST
extern int s_ctrl_test_stick;
#endif
#ifdef SR_HLE_THREAD_SELFTEST
extern uint8_t s_ctrl_test_lx , s_ctrl_test_ly;
#endif
void ctrl_push(const CtrlSample *smp, uint32_t ts);
void ctrl_cycle_catch_up(void);
extern int s_route_cols , s_route_rows;
extern int s_route_learn;
extern SrInputPlayer s_input;
extern int s_route_watch_nid;
void route_note_nid(uint32_t nid);
int route_sig_bytes(void);
void hle_exit_audio_stats(void);
void hle_exit_close_audio_dump(void);
extern DisplayFrameState s_display_active;
extern DisplayFrameState s_display_latched;
extern int s_display_latched_pending;
extern int s_display_configured;
extern uint32_t s_framebuf , s_vcount;
extern uint32_t s_last_flip_vcount;
extern unsigned long s_watchdog_fires;
extern uint32_t s_picture_probe_vcount;
extern SrPictureWatch s_picture_watch;
extern uint64_t s_ge_trace_seq;
uint32_t ge_enqueue_trace_call_pc(const CpuState *s);
int ge_enqueue_trace_enabled(void);
GeEnqueueTraceHle *ge_enqueue_trace_hle_slot(uint32_t uid);
GeEnqueueTraceCallback *ge_enqueue_trace_callback_slot(uint32_t uid);
int route_sample(uint8_t *out);
void route_learn_emit(uint32_t v, const uint8_t *sig);
void route_tick(uint32_t v);
extern uint32_t s_vramdump_vblanks[SR_VRAMDUMP_MAX_VBLANKS];
extern unsigned s_vramdump_count;
extern unsigned s_vramdump_done;
void vramdump_init_once(void);
void vramdump_note_vblank(uint32_t vcount);
void hle_state_sample_vblank(uint32_t vcount);
void hle_state_sample_close(void);
void hle_enhancement_speed_vblank(uint32_t vcount);
extern GeCallback s_ge_cb[16];
void ge_finish_callback(CpuState *s, uint32_t cbid, uint32_t finish_id, uint32_t user_arg);
void ge_sync_wake(void);
extern GeListInfo s_ge_lists[GE_LIST_MAX];
extern uint64_t s_ge_list_done_bits;
extern uint8_t s_ge_free_slots[GE_LIST_MAX];
extern unsigned s_ge_free_head, s_ge_free_count;
extern uint64_t s_ge_list_order;
void ge_free_rebuild(void);
void ge_free_push(int slot);
uint64_t ge_slot_bit(int slot);
void ge_list_completed(int slot);
extern uint64_t s_ge_busy_until_ns;
extern unsigned s_ge_timing_drawing;
int ge_timing_on(void);
void hle_sas_guest_reset(void);
extern MsgPipe s_msg_pipes[MSG_PIPE_MAX];
MsgPipe *msg_pipe_find(uint32_t uid);
void hle_register_msgpipe_handlers(void);
extern Mbx s_mbx[MBX_MAX];
Mbx *mbx_find(uint32_t uid);
void mbx_remove_waiter(Mbx *m, uint32_t thid);
void hle_register_mbx_handlers(void);
extern Sync s_sync[128];
extern Mutex **s_mutexes;
extern size_t s_mutex_len;
extern size_t s_mutex_cap;
Mutex *mutex_find(uint32_t uid);
int mutex_grant_next(Mutex *m);
void mutex_reset_all(void);
extern EvfWaiter s_evf_waiters[EVF_WAITER_MAX];
void hle_register_thread_exit_handlers(void);
void hle_register_display_setframebuf_handler(void);
void hle_register_display_handlers(void);
void hle_register_ge_handlers(void);
void hle_register_mp3_handlers(void);
void hle_register_atrac_handlers(void);
void hle_register_sas_handlers(void);
void hle_register_utility_module_handlers(void);
void hle_register_exit_game_handler(void);
void hle_register_devkit_version_handler(void);
void hle_register_ctrl_sampling_handlers(void);
void hle_register_gpi_gpo_handlers(void);
void hle_register_mpeg_shared_handlers(void);
void hle_register_mpeg_avc_decode_handler(void);
void hle_register_mpeg_ycbcr_handlers(void);
void hle_register_impose_handlers(void);
void hle_register_font_handlers(void);
void hle_register_psmf_player_handlers(void);
void hle_register_power_coexistence_handlers(void);
void loadexec_reset_user_partition(void);
void hle_register_bulk_memory_cache_dma(void);
void hle_register_bulk_memory_kernel_utils(void);
void hle_register_cancel_release_kernel_sync(void);
void hle_register_cancel_release_kernel_thread(void);
void hle_register_console_profile_network(void);
void hle_register_console_profile_system(void);
void hle_register_kernel_import_sweep_kernel_thread_1(void);
void hle_register_kernel_import_sweep_kernel_time_1(void);
void hle_register_kernel_import_sweep_kernel_thread_2(void);
void hle_register_kernel_import_sweep_kernel_time_2(void);
void hle_register_kernel_import_sweep_kernel_memory(void);
void hle_register_kernel_import_sweep_ge(void);
void hle_register_kernel_import_sweep_kernel_module(void);
void hle_register_kernel_import_sweep_display(void);
void hle_register_kernel_module_npdrm(void);
void hle_register_module_lifecycle_handlers(void);
void hle_register_launch_sweep_blocker_rtc(void);
void hle_register_launch_sweep_blocker_kernel_thread(void);
void hle_register_launch_sweep_blocker_font(void);
void hle_register_partition_savedata_kernel_memory(void);
void hle_register_partition_savedata_utility(void);
void hle_register_selftest_oracle_kernel_thread(void);
void hle_register_selftest_oracle_kernel_module(void);
void hle_register_selftest_oracle_kernel_sync(void);
void hle_register_sysreg_utility(void);
void hle_register_sysreg_system(void);
void hle_register_time_kernel_interrupt(void);
void hle_register_time_kernel_thread(void);
void hle_register_time_kernel_time_1(void);
void hle_register_time_rtc(void);
void hle_register_time_kernel_time_2(void);
void hle_register_unregistered_import_kernel_time_1(void);
void hle_register_unregistered_import_kernel_thread_1(void);
void hle_register_unregistered_import_power(void);
void hle_register_unregistered_import_kernel_thread_2(void);
void hle_register_unregistered_import_kernel_time_2(void);
void hle_register_unregistered_import_display(void);
void hle_register_unregistered_import_utility(void);
void hle_register_unregistered_import_network_1(void);
void hle_register_unregistered_import_rtc(void);
void hle_register_unregistered_import_network_2(void);
void hle_register_unregistered_import_audio_codec(void);
void hle_register_wait_conformance_kernel_thread(void);
void hle_register_wait_conformance_display(void);
void hle_register_wait_conformance_kernel_sync_1(void);
void hle_register_wait_conformance_kernel_memory_1(void);
void hle_register_wait_conformance_kernel_sync_2(void);
void hle_register_wait_conformance_kernel_memory_2(void);
void hle_register_wait_conformance_umd(void);
void hle_register_wait_conformance_input(void);
void hle_register_wait_conformance_io(void);
void hle_register_wait_conformance_kernel_interrupt(void);
void hle_register_init_kernel_utils_1(void);
void hle_register_init_kernel_memory_1(void);
void hle_register_init_kernel_thread_1(void);
void hle_register_init_mpeg(void);
void hle_register_init_umd(void);
void hle_register_init_kernel_thread_2(void);
void hle_register_init_kernel_memory_2(void);
void hle_register_init_kernel_utils_2(void);
#pragma GCC visibility pop

/* Defined in one runtime HLE source and used from another; external in hle.c already. */
uint32_t sr_alloc_block_at(uint32_t addr, uint32_t size, const char *name);
extern CallbackEntry *s_callbacks;
extern size_t s_callbacks_len;
int sr_callback_find_in_table(uint32_t uid);
void hle_register_osk_handlers(void);
int sr_callback_table_unregister(uint32_t uid);
void sr_ctrl_sample(void);


/* Success-result exceptions named in the end-of-run summary. sceKernelVolatileMemUnlock is absent:
 * it returns the PSP-measured states now. sceUtilityOskUpdate stays listed for VISIBLE's measured 0
 * (#363 comment 6331070692, attended); after-use now returns the measured 0x80110001. */
static inline const char *hle_success_exception_issue(uint32_t nid) {
    switch (nid) {
    case 0xb3b5d042u: return "#363, #286"; /* sceAtracGetOutputChannel */
    case 0x4b85c861u: return "#363";       /* sceUtilityOskUpdate: measured VISIBLE success */
    default: return NULL;
    }
}

/* The other diagnostic switches on the per-call path follow the same rule (#1296). An
 * uncached getenv takes the C runtime's environment lock and compares the name against
 * every variable, and sr_syscall() plus the CpuSuspendIntr/CpuResumeIntr pair a guest
 * allocator brackets each allocation with read up to four of them per call. */
static inline int hle_env_switch(int *cached, const char *name) {
    if (*cached < 0) *cached = getenv(name) != NULL;
    return *cached;
}
/* Argument 5+idx of an HLE call. PSP userland is MIPS EABI: arguments 1..8 arrive in
 * a0-a3,t0-t3 (r4..r11) and only 9+ go to the stack (sp+0) -- same as PPSSPP's PARAM(n).
 * This used to read the o32 stack home slots (sp+16+), which EABI callers never write:
 * every >4-argument HLE call received stack garbage (found via sceMpegRingbufferConstruct
 * getting its fill callback from there -- the movie player jumped into the ring buffer). */
static inline uint32_t stack_arg(CpuState *s, int idx) {
    return idx < 4 ? s->r[8 + idx] : MEM_R32(s->r[29] + (uint32_t)(idx - 4) * 4);
}

static inline void sreg_put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t sreg_get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A name this model can store and persist: 1..26 bytes of printable ASCII without '/'. */
static inline int sreg_name_ok(const char *name) {
    size_t n = strlen(name);
    if (n == 0u || n >= SREG_NAME_BYTES) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20u || c > 0x7eu || c == '/') return 0;
    }
    return 1;
}

static inline int console_identity_ok(const uint8_t mac[6], const uint8_t psid[16]) {
    int psid_set = 0;
    for (int i = 0; i < 16; i++) psid_set |= psid[i] != 0u;
    return (mac[0] & 0x03u) == 0u && psid_set;   /* see PspConsoleProfile */
}

static inline int sreg_sync_file(FILE *f) {
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

static inline int64_t psmf_av_x9_to_us(int64_t x9) { return (x9 >= 0 ? x9 + 4 : x9 - 4) / 9; }

static inline void psmf_av_fmt(char *out, size_t cap, int ok, int64_t x9) {
    if (ok) snprintf(out, cap, "%lld", (long long)psmf_av_x9_to_us(x9));
    else snprintf(out, cap, "na");
}

/* Physical byte range of a guest span; refused when empty or past the 512 MiB window. */
static inline int psmf_av_range(uint32_t first, uint32_t bytes, uint32_t *lo, uint32_t *hi) {
    uint64_t l = SR_PHYS(first), h = l + (uint64_t)bytes;
    if (bytes == 0u || h > 0x20000000ull) return 0;
    *lo = (uint32_t)l;
    *hi = (uint32_t)h;
    return 1;
}

/* All guest clock APIs observe the scheduler's single monotonic microsecond
 * timeline.  They never advance it merely by being queried. */
static inline uint64_t now_usec(void) { return sched_vtime_us(); }

/* Elapsed guest time at `now`: the finished intervals plus the open one, if running. */
static inline uint64_t vtimer_elapsed_at(const VTimer *t, uint64_t now) {
    if (!t->running) return t->accum_us;
    return t->accum_us + (now >= t->started_us ? now - t->started_us : 0u);
}

static inline int rtc_write_u64(uint32_t addr, uint64_t value) {
    if (!addr || !sr_guest_span_writable(addr, 8u)) return 0;
    MEM_W32(addr, (uint32_t)value);
    MEM_W32(addr + 4u, (uint32_t)(value >> 32));
    return 1;
}

/* Standard descriptors are real reserved entries in the guest namespace.  A
 * closed standard descriptor keeps its kind (so ordinary allocation can never
 * take the slot), while a closed ordinary file becomes fully unused and is
 * eligible for reuse. */
static inline void hle_fd_release(Fd *f, int preserve_std_kind) {
    if (!f) return;
    FdKind kind = f->kind;
    uint8_t std_stream = f->std_stream;
    if (f->pgd) sr_pgd_free(f->pgd);
    if (f->host) fclose(f->host);
    memset(f, 0, sizeof(*f));
    if (preserve_std_kind && kind == FD_KIND_STD) {
        f->kind = FD_KIND_STD;
        f->std_stream = std_stream;
    }
}

static inline void hle_dirfd_clear(DirFd *d) {
    if (!d) return;
    sr_vfs_dirlist_destroy(&d->list);
    free(d->path);
    memset(d, 0, sizeof(*d));
}

static inline uint32_t hle_io_raw_umd_seek(Fd *f, int64_t offset, uint32_t whence,
                                    uint32_t *position) {
    if (whence >= 3u) return SCE_ERROR_KERNEL_INVALID_ARGUMENT;
    int64_t base = whence == 1u ? (int64_t)f->off :
                   (whence == 2u ? (int64_t)f->size - 1 : 0);
    if (offset > INT64_MAX - base) return IO_UMD_POSITION_REFUSED;
    int64_t target = base + offset;
    if (target < 0 || target >= (int64_t)f->size) return IO_UMD_POSITION_REFUSED;
    f->off = (uint32_t)target;
    *position = f->off;
    return 0u;
}

#endif
