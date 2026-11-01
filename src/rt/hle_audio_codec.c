// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* Audio decoders: sceAtrac, sceAudiocodec and sceMp3. Extracted from hle.c on 2026-10-31 (#307). */

#include "hle_internal.h"

/* sceAtrac3plus: control-flow model with the real streaming contract. The game feeds a track in
 * chunks: SetData installs the first chunk (buffer + size), then the audio thread polls
 * sceAtracGetStreamDataInfo for the ring write pointer/free bytes, reads the next file chunk into
 * it, and calls sceAtracAddStreamData. DecodeData consumes one codec-specific frame (2048 samples
 * for ATRAC3+, 1024 for plain AT3; bytesPerFrame bytes) per call and only advances while data is
 * available. GetRemainFrame and
 * GetStreamDataInfo report the honest ring state so the feed loop can advance. The earlier model
 * prevented the guest from streaming most of the track: it claimed ALLDATA_IS_ON_MEMORY with zero
 * writable bytes, so the guest starved at ~8% of the track. The streaming contract is now modeled
 * sufficiently for the guest to feed the complete ATRAC stream (the title BGM feeds end-to-end and
 * reaches DecodeData repeatedly). ATRAC3+ and plain AT3 (0x1001) use separate FFmpeg n4.4 decoder
 * paths through src/rt/atrac3p_bridge.h. Ported semantics from PPSSPP
 * Core/HLE/sceAtrac.cpp + AtracCtx.cpp (stream bookkeeping), minus the media engine. */
#define ATRAC_SAMPLES_PER_FRAME 2048
#define ATRAC3_SAMPLES_PER_FRAME 1024
#define ATRAC_CODEC_AT3PLUS 0x1000u
#define ATRAC_CODEC_AT3 0x1001u
/* Measured on a PSP-3000 (#1096): the first sceAtracDecodeData of an AT3 stream
 * whose fact delay is 500 returns this code with 0 samples. */
#define ATRAC_ERROR_API_FAIL 0x80630002u
#define ATRAC_ERROR_NO_ATRACID 0x80630003u
#define ATRAC_ERROR_INVALID_CODECTYPE 0x80630004u
#define ATRAC_ERROR_BAD_ATRACID 0x80630005u
#define ATRAC_ERROR_UNKNOWN_FORMAT 0x80630006u
#define ATRAC_ERROR_ALLDATA_LOADED 0x80630009u
#define ATRAC_ERROR_NO_DATA 0x80630010u
#define ATRAC_ERROR_SIZE_TOO_SMALL 0x80630011u
#define ATRAC_ERROR_ADD_DATA_IS_TOO_BIG 0x80630018u
#define ATRAC_ERROR_ALLDATA_DECODED 0x80630022u
/* Measured on a PSP-3000 (#1096): sceAtracDecodeData of a streamed AT3 track
 * after its last buffered frame, with file data still to stream. */
#define ATRAC_ERROR_BUFFER_IS_EMPTY 0x80630023u
#define ATRAC_ERROR_DECODE_DATA_AFTER_END 0x80630024u
#define ATRAC_PSP_ALLDATA_IS_ON_MEMORY 0xFFFFFFFFu
#define ATRAC_PSP_NONLOOP_STREAM_IS_ON_MEMORY 0xFFFFFFFEu
#define ATRAC_PSP_LOOP_STREAM_IS_ON_MEMORY 0xFFFFFFFDu
#define ATRAC_STATE_ALL_DATA_LOADED 2
#define ATRAC_STATE_HALFWAY_BUFFER 3
#define ATRAC_STATE_STREAMED_NO_LOOP 4
#define ATRAC_STATE_STREAMED_LOOP_END 5
#define ATRAC_STATE_STREAMED_LOOP_TRAILER 6
#define ATRAC_RIFF_SCAN_MAX 0x00100000u
Atrac s_atrac[8];
static int atrac_streamed_state(const Atrac *a);
static uint32_t atrac_stream_buffer_end(const Atrac *a);
static uint32_t atrac_remaining_frames(const Atrac *a);
static int atrac_reset_decoder(Atrac *a);
static int atrac_samples_per_frame(const Atrac *a) {
    return a->codecType == ATRAC_CODEC_AT3 ? ATRAC3_SAMPLES_PER_FRAME
                                           : ATRAC_SAMPLES_PER_FRAME;
}

void atrac_stream_window_free(Atrac *a) {
    free(a->streamWindow);
    a->streamWindow = NULL;
    a->streamWindowSize = 0;
    a->streamWindowRead = 0;
    a->streamWindowWrite = 0;
    a->streamWindowQueued = 0;
}
static int atrac_stream_window_init(Atrac *a) {
    atrac_stream_window_free(a);
    if (!atrac_streamed_state(a) || a->bufferMaxSize == 0u)
        return 1;
    a->streamWindow = (uint8_t *)malloc(a->bufferMaxSize);
    if (!a->streamWindow) return 0;
    a->streamWindowSize = a->bufferMaxSize;
    a->streamWindowRead = 0;
    a->streamWindowWrite = 0;
    a->streamWindowQueued = 0;
    for (uint32_t i = 0; i < a->bufferValidBytes; i++) {
        uint32_t phys = a->bufferMaxSize ?
            (a->bufferPos + i) % a->bufferMaxSize : 0u;
        a->streamWindow[a->streamWindowWrite] =
            (uint8_t)MEM_R8(a->buf + phys);
        a->streamWindowWrite++;
        if (a->streamWindowWrite == a->streamWindowSize)
            a->streamWindowWrite = 0;
        a->streamWindowQueued++;
    }
    return 1;
}
static int atrac_stream_window_append(Atrac *a, uint32_t physical,
                                      uint32_t bytes) {
    if (!bytes) return 1;
    if (!a->streamWindow || !a->streamWindowSize ||
        bytes > a->streamWindowSize - a->streamWindowQueued)
        return 0;
    for (uint32_t i = 0; i < bytes; i++) {
        uint32_t phys = a->bufferMaxSize ?
            (physical + i) % a->bufferMaxSize : physical + i;
        a->streamWindow[a->streamWindowWrite] =
            (uint8_t)MEM_R8(a->buf + phys);
        a->streamWindowWrite++;
        if (a->streamWindowWrite == a->streamWindowSize)
            a->streamWindowWrite = 0;
    }
    a->streamWindowQueued += bytes;
    return 1;
}
static int atrac_stream_window_peek(const Atrac *a, uint8_t *out,
                                    uint32_t bytes) {
    if (!bytes || !a->streamWindow ||
        bytes > a->streamWindowQueued)
        return 0;
    for (uint32_t i = 0; i < bytes; i++) {
        uint32_t p = a->streamWindowRead + i;
        if (p >= a->streamWindowSize) p -= a->streamWindowSize;
        out[i] = a->streamWindow[p];
    }
    return 1;
}
static void atrac_stream_window_consume(Atrac *a, uint32_t bytes) {
    if (!bytes || !a->streamWindow) return;
    if (bytes > a->streamWindowQueued) bytes = a->streamWindowQueued;
    a->streamWindowRead += bytes;
    a->streamWindowRead %= a->streamWindowSize;
    a->streamWindowQueued -= bytes;
}
#define ATRAC_ERROR_BAD_CODEC_PARAMS 0x80630008u
#define ATRAC3_SETDATA_DELAY_REFUSED 1855u   /* lowest refused fact delay (PSP-AT3-002) */
static int atrac_parse_track(Atrac *a, uint32_t buf, uint32_t size, uint32_t *refusal) {
    /* Scan the RIFF for "fmt " (frame size / blockAlign), "fact" (total samples),
     * "data" (frame payload offset) and "smpl" (loop points). The input is
     * guest-controlled: validate the complete supplied span first, then do all
     * chunk arithmetic in checked offsets so a hostile size cannot wrap the
     * cursor or make a malformed buffer look valid. Everything is parsed into
     * locals and committed only on success so a rejected buffer leaves the
     * previously configured context untouched. */
    if (size < 44u || !sr_guest_span_readable(buf, size)) return 0;
    if (MEM_R32(buf) != 0x46464952u /* 'RIFF' */ ||
        MEM_R32(buf + 8u) != 0x45564157u /* 'WAVE' */) return 0;

    uint32_t riff_end;
    uint32_t riff_size = MEM_R32(buf + 4u);
    if (!sr_size_add_ok(8u, riff_size, &riff_end) || riff_end < 44u)
        return 0;
    /* sceAtracSetData may receive only the currently filled prefix of a streamed
     * track.  The RIFF extent describes the eventual file, not necessarily this
     * call's buffer, so scan only the checked bytes that are available now. */
    uint32_t scan_end = size < riff_end ? size : riff_end;
    if (scan_end > ATRAC_RIFF_SCAN_MAX) scan_end = ATRAC_RIFF_SCAN_MAX;

    uint32_t fileSize = riff_end;
    uint32_t dataByteOffset = 0;
    uint32_t bytesPerFrame = 0;
    uint32_t avgBytesPerSec = 0;
    int channels = 0;
    uint16_t formatTag = 0;
    uint8_t at3Extradata[14] = {0};
    int hasFmt = 0;
    uint32_t at3FactSize = 0, at3FactDelay = 0;
    int endSample = 0;
    int loopStartSample = -1, loopEndSample = -1;
    for (uint32_t off = 12u; off <= scan_end - 8u;) {
        uint32_t p;
        uint32_t next;
        uint32_t chunk_bytes;
        if (!sr_size_add_ok(buf, off, &p)) return 0;
        uint32_t id = MEM_R32(p), sz = MEM_R32(p + 4u);
        /* Bytes of this chunk's payload actually inside the validated span
         * (off+8 <= scan_end <= size by the loop condition). Chunks whose
         * payload we read must fit fully, or the buffer is rejected; a chunk
         * header may still be followed by data beyond the fed prefix, which
         * the 'data' handling below tolerates. */
        uint32_t avail = scan_end - (off + 8u);
        if (id == 0x74636166u /* 'fact' */) {
            if (sz < 4u || sz > avail) return 0;
            if (a->codecType == ATRAC_CODEC_AT3) {
                if (sz != 8u && sz != 12u) return 0;
                at3FactSize = sz;
                at3FactDelay = MEM_R32(p + 12u);
            }
            uint32_t samples = MEM_R32(p + 8u);
            if (samples == 0u || samples > 0x7fffffffu) return 0;
            endSample = (int)samples;
        } else if (id == 0x20746d66u /* 'fmt ' */) {
            if (sz < 16u || sz > avail) return 0;
            if (hasFmt && a->codecType == ATRAC_CODEC_AT3) return 0;
            hasFmt = 1;
            formatTag = MEM_R16(p + 8u);
            uint32_t ch = MEM_R16(p + 10u);      /* channels */
            uint32_t align = MEM_R16(p + 20u);   /* blockAlign = bytes per frame */
            uint32_t rate = MEM_R32(p + 16u);    /* nAvgBytesPerSec = declared byte rate */
            if (ch < 1u || ch > 8u) return 0;
            if (align < 1u || align > 0x10000u) return 0;
            if (a->codecType == ATRAC_CODEC_AT3) {
                if (sz < 18u || MEM_R16(p + 24u) != sizeof(at3Extradata) ||
                    sz < 18u + sizeof(at3Extradata))
                    return 0;
                for (uint32_t i = 0; i < sizeof(at3Extradata); i++)
                    at3Extradata[i] = (uint8_t)MEM_R8(p + 26u + i);
                if (!atrac3_config_valid(formatTag, (int)ch, (int)align,
                                         at3Extradata, (int)sizeof(at3Extradata)))
                    return 0;
            }
            channels = (int)ch;
            bytesPerFrame = align;
            avgBytesPerSec = rate;
        } else if (id == 0x61746164u /* 'data' */) {
            if (!sr_size_add_ok(off, 8u, &dataByteOffset)) return 0;
        } else if (id == 0x6c706d73u /* 'smpl' */) {
            /* payload: manufacturer, product, samplePeriod, MIDIUnityNote,
             * MIDIPitchFraction, SMPTEFormat, SMPTEOffset, numSampleLoops(28),
             * samplerData(32), then 24-byte loop records at 36+. */
            if (sz > avail) return 0;
            if (sz >= 36u + 24u) {
                uint32_t numLoops = MEM_R32(p + 36u);
                if (numLoops >= 1u) {
                    uint32_t start = MEM_R32(p + 52u), end = MEM_R32(p + 56u);
                    if (start < 0x80000000u && end <= 0x7fffffffu &&
                        (start != 0u || end != 0u)) {
                        loopStartSample = (int)start;
                        loopEndSample = (int)end;
                    }
                }
            }
        }
        if (!sr_size_add_ok(sz, sz & 1u, &chunk_bytes) ||
            !sr_size_add_ok(off, 8u, &next) ||
            !sr_size_add_ok(next, chunk_bytes, &next))
            return 0;
        /* A chunk payload may legitimately extend past the currently fed
         * prefix of a streamed track (the 'data' chunk covers the whole audio).
         * Only the next chunk header must be inside the fed bytes; stop the
         * scan when the payload runs past them. */
        if (next > scan_end) break;
        off = next;
    }
    if (endSample <= 0) return 0;
    if (a->codecType == ATRAC_CODEC_AT3 && !hasFmt) return 0;
    /* MEASURED on a PSP-3000 (firmware 6.61; #1096, PSP-AT3-002 at3-delay-sweep, 2026-10-31,
     * over a whole 52964-byte track and an 8 KiB prefix of it): an AT3 stream whose 8-byte fact
     * chunk carries a delay of 1855 or more (1855 .. 3072 measured) is refused by sceAtracSetData
     * with 0x80630008, and the DecodeData that follows returns 0x80630010 because the context
     * holds no data; 1797 and every measured delay below it are accepted.  1798 .. 1854 is not
     * measured and is accepted here.  Only sceAtracSetData was measured; the other Set* calls
     * share this track analysis and are UNMEASURED. */
    if (a->codecType == ATRAC_CODEC_AT3 && at3FactSize == 8u && at3FactDelay >= ATRAC3_SETDATA_DELAY_REFUSED) {
        if (refusal) *refusal = ATRAC_ERROR_BAD_CODEC_PARAMS;
        return 0;
    }
    a->fileSize = fileSize;
    a->dataByteOffset = dataByteOffset;
    a->bytesPerFrame = bytesPerFrame;
    a->avgBytesPerSec = avgBytesPerSec;
    a->channels = channels;
    a->formatTag = formatTag;
    memcpy(a->at3Extradata, at3Extradata, sizeof(a->at3Extradata));
    a->at3FactSize = at3FactSize;
    a->at3FactDelay = at3FactDelay;
    a->endSample = endSample;
    a->loopStartSample = loopStartSample;
    a->loopEndSample = loopEndSample;
    return 1;
}
static int atrac_streamed_state(const Atrac *a) {
    return a->bufferState == ATRAC_STATE_STREAMED_NO_LOOP ||
           a->bufferState == ATRAC_STATE_STREAMED_LOOP_END ||
           a->bufferState == ATRAC_STATE_STREAMED_LOOP_TRAILER;
}
static void atrac_update_buffer_state(Atrac *a) {
    if (a->bufferMaxSize >= a->fileSize) {
        a->bufferState = a->size < a->fileSize ? ATRAC_STATE_HALFWAY_BUFFER
                                               : ATRAC_STATE_ALL_DATA_LOADED;
    } else if (a->loopEndSample <= 0) {
        a->bufferState = ATRAC_STATE_STREAMED_NO_LOOP;
    } else if ((uint32_t)a->loopEndSample >= (uint32_t)a->endSample) {
        a->bufferState = ATRAC_STATE_STREAMED_LOOP_END;
    } else {
        a->bufferState = ATRAC_STATE_STREAMED_LOOP_TRAILER;
    }
}
static uint32_t atrac_stream_buffer_end(const Atrac *a) {
    /* Ring end: frame-aligned capacity after the optional RIFF header. */
    if (a->bufferMaxSize <= a->bufferHeaderSize || a->bytesPerFrame == 0u)
        return a->bufferMaxSize;
    uint32_t frames = (a->bufferMaxSize - a->bufferHeaderSize) /
                      a->bytesPerFrame;
    return frames * a->bytesPerFrame + a->bufferHeaderSize;
}
static uint32_t atrac_stream_buffer_base(const Atrac *a) {
    return a->bufferHeaderSize;
}
static uint32_t atrac_file_offset_by_sample(const Atrac *a, int sample) {
    if (sample < 0) sample = 0;
    /* Frame*bytesPerFrame can exceed 32 bits for hostile RIFF metadata
     * (huge 'fact' sample count combined with a large blockAlign); clamp
     * so callers see an offset beyond any real file instead of a wrap. */
    uint64_t frame = (uint32_t)sample / (uint32_t)atrac_samples_per_frame(a);
    uint64_t byteOff = (uint64_t)a->dataByteOffset + frame * a->bytesPerFrame;
    return byteOff > 0xffffffffu ? 0xffffffffu : (uint32_t)byteOff;
}
static void atrac_calculate_stream_info(const Atrac *a, uint32_t *outOffset,
                                        uint32_t *outWritable, uint32_t *outReadOffset) {
    uint32_t readOffset = a->size;   /* file offset of the end of fed data */
    uint32_t offset = 0, writable = 0;
    if (a->bufferState == ATRAC_STATE_ALL_DATA_LOADED) {
        readOffset = 0; offset = 0; writable = 0;
    } else if (a->bufferState == ATRAC_STATE_HALFWAY_BUFFER) {
        offset = readOffset;
        writable = a->fileSize - readOffset;
    } else {
        uint32_t bufferEnd = atrac_stream_buffer_end(a);
        uint32_t validExtended = a->bufferPos + a->bufferValidBytes;
        if (validExtended < bufferEnd) {
            offset = validExtended;
            writable = bufferEnd - validExtended;
        } else {
            /* After the first lap the header prefix is gone and the physical
             * ring starts at zero, exactly as on the PSP/PPSSPP model. */
            uint32_t startUsed = validExtended - bufferEnd;
            offset = startUsed;
            writable = a->bufferPos > startUsed ? a->bufferPos - startUsed : 0;
        }
        if (readOffset >= a->fileSize) {
            if (a->bufferState == ATRAC_STATE_STREAMED_NO_LOOP) {
                /* Complete: nothing more to read. */
                readOffset = 0; offset = 0; writable = 0;
            } else {
                /* Loop from end: re-read from just before the loop point. */
                int loopOff = a->loopStartSample - atrac_samples_per_frame(a) * 2;
                readOffset = atrac_file_offset_by_sample(a, loopOff);
            }
        }
        {
            uint32_t endCheck;
            if (!sr_size_add_ok(readOffset, writable, &endCheck) ||
                endCheck > a->fileSize)
                writable = a->fileSize - readOffset;
        }
        if (offset + writable > a->bufferMaxSize) {
            offset = 0;
            writable = a->bufferMaxSize;
        }
    }
    if (outOffset) *outOffset = offset;
    if (outWritable) *outWritable = writable;
    if (outReadOffset) *outReadOffset = readOffset;
}
static void atrac_consume_frame(Atrac *a) {
    if (atrac_streamed_state(a))
        atrac_stream_window_consume(a, a->bytesPerFrame);
    a->bufferPos += a->bytesPerFrame;
    if (atrac_streamed_state(a)) {
        if (a->bufferValidBytes > a->bytesPerFrame)
            a->bufferValidBytes -= a->bytesPerFrame;
        else
            a->bufferValidBytes = 0;
    }
    uint32_t end = atrac_stream_buffer_end(a);
    if (a->bufferPos >= end) {
        a->bufferPos -= end;
        /* The data offset is a one-time header prefix. Once the read cursor
         * reaches the frame-aligned end, subsequent laps use the whole
         * physical buffer. */
        a->bufferHeaderSize = 0;
    }
}
static uint32_t atrac_remaining_frames(const Atrac *a) {
    if (a->bufferState == ATRAC_STATE_ALL_DATA_LOADED)
        return ATRAC_PSP_ALLDATA_IS_ON_MEMORY;
    uint32_t currentFileOffset =
        atrac_file_offset_by_sample(a, a->posSample - atrac_samples_per_frame(a));
    if (a->size >= a->fileSize) {
        if (a->bufferState == ATRAC_STATE_STREAMED_NO_LOOP)
            return ATRAC_PSP_NONLOOP_STREAM_IS_ON_MEMORY;
        if (a->bufferState == ATRAC_STATE_STREAMED_LOOP_TRAILER &&
            a->posSample > a->loopEndSample)
            return ATRAC_PSP_NONLOOP_STREAM_IS_ON_MEMORY;
        if (atrac_streamed_state(a) && a->loopNum == 0)
            return ATRAC_PSP_LOOP_STREAM_IS_ON_MEMORY;
    }
    if (atrac_streamed_state(a))
        return a->bufferValidBytes / a->bytesPerFrame;
    int remainingBytes = (int)(a->size - currentFileOffset);
    if (remainingBytes < 0) remainingBytes = 0;
    return (uint32_t)remainingBytes / a->bytesPerFrame;
}
/* Configure `a` from the first `size` bytes of a track held in a guest buffer
 * of `bufSize` bytes (the ring or the whole-file buffer). sceAtracSetData passes
 * size == bufSize; the halfway calls pass the filled prefix separately. */
static int atrac_set_track(Atrac *a, uint32_t buf, uint32_t size, uint32_t bufSize,
                           uint32_t *refusal) {
    /* Transactional: a rejected buffer leaves the previously configured
     * context untouched. */
    uint32_t oldBuf = a->buf;
    uint32_t oldSize = a->size;
    int oldPosSample = a->posSample;
    int oldLoopNum = a->loopNum;
    uint32_t oldBufferPos = a->bufferPos;
    uint32_t oldBufferValidBytes = a->bufferValidBytes;
    uint32_t oldBufferHeaderSize = a->bufferHeaderSize;
    uint8_t *oldStreamWindow = a->streamWindow;
    uint32_t oldStreamWindowSize = a->streamWindowSize;
    uint32_t oldStreamWindowRead = a->streamWindowRead;
    uint32_t oldStreamWindowWrite = a->streamWindowWrite;
    uint32_t oldStreamWindowQueued = a->streamWindowQueued;
    a->buf = buf;
    a->size = size;
    a->posSample = 0;
    a->loopNum = 0;
    a->bufferPos = 0;
    a->bufferValidBytes = 0;
    a->bufferHeaderSize = 0;
    if (!atrac_parse_track(a, buf, size, refusal)) {
        a->buf = oldBuf;
        a->size = oldSize;
        a->posSample = oldPosSample;
        a->loopNum = oldLoopNum;
        a->bufferPos = oldBufferPos;
        a->bufferValidBytes = oldBufferValidBytes;
        a->bufferHeaderSize = oldBufferHeaderSize;
        a->streamWindow = oldStreamWindow;
        a->streamWindowSize = oldStreamWindowSize;
        a->streamWindowRead = oldStreamWindowRead;
        a->streamWindowWrite = oldStreamWindowWrite;
        a->streamWindowQueued = oldStreamWindowQueued;
        return 0;
    }
    (void)atrac_reset_decoder(a);
    a->decodeFailureReported = 0;
    a->at3FirstFramePending = 1;
    a->at3SkipApplied = 0;
    if (oldStreamWindow) free(oldStreamWindow);
    a->streamWindow = NULL;
    a->streamWindowSize = 0;
    a->streamWindowRead = 0;
    a->streamWindowWrite = 0;
    a->streamWindowQueued = 0;
    a->bufferMaxSize = bufSize;
    if (a->size > a->fileSize) a->size = a->fileSize;
    if (a->bytesPerFrame == 0u || a->dataByteOffset == 0u) {
        /* No frame-size/ring contract in the fed prefix (e.g. a fact-only
         * envelope or a first buffer that has not reached the fmt/data chunks
         * yet): the ring cannot be sized, so treat the track as linear
         * all-data and let a later SetData with more of the file re-enter the
         * streaming path. */
        a->bufferState = ATRAC_STATE_ALL_DATA_LOADED;
        a->bufferPos = 0;
        a->bufferValidBytes = 0;
        a->bufferHeaderSize = 0;
        return 1;
    }
    atrac_update_buffer_state(a);
    a->at3PrimeValid = 0;
    if (atrac_streamed_state(a)) {
        a->bufferHeaderSize = a->dataByteOffset;
        a->bufferPos = a->dataByteOffset + a->bytesPerFrame;
        a->bufferValidBytes =
            a->size > a->bufferPos ? a->size - a->bufferPos : 0;
        (void)atrac_stream_window_init(a);
        /* MEASURED on a PSP-3000 (6.61, #1096, probe at3-decode, full PCM of the first call):
         * a streamed AT3 track's output starts at file frame 1, as bufferPos does here, but
         * the decoder first decodes frame 0 and discards it. The first call's samples are
         * frame 1 decoded with frame 0's overlap history (all 1,910 values within 8 steps of
         * that; the same frame decoded with empty history matches none). Frame 0 is kept here
         * because the ring may be refilled before the first decode. */
        if (a->codecType == ATRAC_CODEC_AT3 && a->bytesPerFrame <= sizeof(a->at3PrimeFrame) &&
            a->size >= a->dataByteOffset + a->bytesPerFrame &&
            sr_guest_span_readable(a->buf + a->dataByteOffset, a->bytesPerFrame)) {
            for (uint32_t i = 0u; i < a->bytesPerFrame; i++)
                a->at3PrimeFrame[i] = (uint8_t)MEM_R8(a->buf + a->dataByteOffset + i);
            a->at3PrimeValid = 1;
        }
    }
    return 1;
}
static int atrac_log_on(void);   /* defined below; used by decoder lifecycle */
static void atrac_report_decode_failure(Atrac *a) {
    if (a->decodeFailureReported) return;
    a->decodeFailureReported = 1;
    fprintf(stderr, "ATRAC_DECODE_REFUSED: ATRAC_ERROR_UNKNOWN_FORMAT: "
            "%s decoder did not produce PCM; decoding of this stream stopped.\n",
            a->codecType == ATRAC_CODEC_AT3 ? "ATRAC3" : "ATRAC3+");
}
unsigned long g_atrac_frames, g_atrac_frames_nonzero;
int g_atrac_peak;
uint32_t g_atrac_out_bufs[4];
int      g_atrac_out_nbufs;
static void audio_note_atrac_out(uint32_t buf) {
    if (!buf) return;
    for (int i = 0; i < g_atrac_out_nbufs; i++) if (g_atrac_out_bufs[i] == buf) return;
    if (g_atrac_out_nbufs < 4) g_atrac_out_bufs[g_atrac_out_nbufs++] = buf;
}

/* Decoder lifecycle. Plain ATRAC3 (0x1001) and ATRAC3+ (0x1000) have separate
 * FFmpeg n4.4 instances and separate format/frame contracts. */
void atrac_release_decoder(Atrac *a) {
    atrac3p_bridge_destroy(a->dec);
    a->dec = NULL;
    atrac3_bridge_destroy(a->dec_at3);
    a->dec_at3 = NULL;
    a->dec_codec_type = 0;
    a->dec_channels = 0;
    a->dec_align = 0;
    memset(a->dec_at3_extradata, 0, sizeof(a->dec_at3_extradata));
    free(a->host_frame);
    a->host_frame = NULL;
    a->host_frame_cap = 0;
    free(a->host_pcm);
    a->host_pcm = NULL;
}
static int atrac_reset_decoder(Atrac *a) {
    int ret = 0;
    if (a->dec)
        ret = atrac3p_bridge_reset(a->dec);
    else if (a->dec_at3)
        ret = atrac3_bridge_reset(a->dec_at3);
    if (ret >= 0)
        a->decodeFailureReported = 0;
    return ret;
}
static void atrac_ensure_decoder(Atrac *a) {
    if (a->dec || a->dec_at3) {
        /* A reused slot (h_AtracSetData on an existing id) may carry a decoder
         * created for a previous track config; recreate when the RIFF changed. */
        int same = a->dec_codec_type == a->codecType &&
                   a->dec_channels == a->channels &&
                   a->dec_align == (int)a->bytesPerFrame;
        if (same && a->codecType == ATRAC_CODEC_AT3)
            same = memcmp(a->dec_at3_extradata, a->at3Extradata,
                          sizeof(a->at3Extradata)) == 0;
        if (same)
            return;
        atrac_release_decoder(a);
    }
    if (a->bytesPerFrame == 0u) return;
    int ret;
    if (a->codecType == ATRAC_CODEC_AT3PLUS) {
        if (a->channels > ATRAC3P_MAX_CHANNELS) return;
        ret = atrac3p_bridge_create(a->channels, (int)a->bytesPerFrame, &a->dec);
    } else if (a->codecType == ATRAC_CODEC_AT3) {
        ret = atrac3_bridge_create(a->channels, (int)a->bytesPerFrame,
                                   a->at3Extradata, &a->dec_at3);
    } else {
        return;
    }
    if (ret < 0) {
        a->dec = NULL;
        a->dec_at3 = NULL;
        if (atrac_log_on())
            fprintf(stderr, "ATRAC_DECODER: create failed codec=0x%04x ch=%d align=%u ret=%d\n",
                    a->codecType, a->channels, a->bytesPerFrame, ret);
        return;
    }
    a->dec_codec_type = a->codecType;
    a->dec_channels = a->channels;
    a->dec_align = (int)a->bytesPerFrame;
    if (a->codecType == ATRAC_CODEC_AT3)
        memcpy(a->dec_at3_extradata, a->at3Extradata,
               sizeof(a->dec_at3_extradata));
    a->host_frame_cap = (int)a->bytesPerFrame;
    a->host_frame = malloc((size_t)a->bytesPerFrame + ATRAC3P_PADDING_SIZE);
    a->host_pcm = malloc((size_t)a->channels * ATRAC_SAMPLES_PER_FRAME * sizeof(int16_t));
    if (!a->host_frame || !a->host_pcm) {
        atrac_release_decoder(a);
        return;
    }
    if (atrac_log_on())
        fprintf(stderr, "ATRAC_DECODER: id=%d codec=0x%04x ch=%d align=%u ready\n",
                (int)(a - s_atrac), a->codecType, a->channels, a->bytesPerFrame);
}

static int atrac_log_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("SR_ATRACLOG") != NULL;
    return on;
}
/* Per-codec context capacity (#1379). sceAtracReinit(at3Count, at3plusCount)
 * reserves each context slot for one codec: ATRAC3+ slots first, then ATRAC3,
 * as ids 0, 1, ... in that order. sceAtracGetAtracID and every *AndGetID call
 * take a free slot reserved for the codec they need, and report
 * ATRAC_ERROR_NO_ATRACID once those are used up. Until the first successful
 * Reinit every one of the eight slots serves either codec: the capacity a
 * PSP's ATRAC library starts with is UNMEASURED here, so that earlier,
 * permissive behaviour is kept rather than replaced with an invented split. */
int s_atrac_caps_configured;
uint32_t s_atrac_slot_codec[8];   /* 0 = reserved for no codec */
static int atrac_alloc_slot(uint32_t codec) {
    for (int i = 0; i < 8; i++) {
        if (s_atrac[i].used) continue;
        if (s_atrac_caps_configured && s_atrac_slot_codec[i] != codec) continue;
        return i;
    }
    return -1;
}
static uint32_t h_AtracGetAtracID(CpuState *s) {
    uint32_t codecType = A0;
    if (codecType != ATRAC_CODEC_AT3PLUS && codecType != ATRAC_CODEC_AT3)
        return ATRAC_ERROR_INVALID_CODECTYPE;
    {
        int i = atrac_alloc_slot(codecType);
        if (i < 0) return ATRAC_ERROR_NO_ATRACID;
        memset(&s_atrac[i], 0, sizeof(s_atrac[i]));
        s_atrac[i].used = 1;
        s_atrac[i].codecType = codecType;
        if (atrac_log_on())
            fprintf(stderr, "ATRAC_GETID: id=%d codec=0x%08x\n", i, codecType);
        return (uint32_t)i;
    }
}
/* The sceAtracSet*Data and sceAtracSet*HalfwayBuffer family (PSPSDK
 * pspatrac3.h). A data call hands over a buffer whose bufSize bytes all hold
 * the track's start; a halfway call hands over a bufSize-byte buffer of which
 * only the first readSize bytes are filled, and the guest adds the rest
 * through sceAtracGetStreamDataInfo and sceAtracAddStreamData. The calls that
 * take an ID configure that context; the *AndGetID calls allocate one for the
 * codec the RIFF fmt chunk names. The MOut calls ask for mono output, which
 * the HLE models for mono tracks only. The validation order and the two
 * refusal codes below are upstream-derived and unmeasured: they follow the
 * public PPSSPP reference (GPL-2.0-or-later, Core/HLE/sceAtrac.cpp), not a
 * PSP measurement, and stay so labelled until a probe pins them (#1096). */
#define ATRAC_ERROR_INCORRECT_READ_SIZE 0x80630013u
#define ATRAC_ERROR_NOT_MONO 0x80630019u
/* Codec named by the fmt chunk of the RIFF prefix: tag 0x0270 is ATRAC3 and
 * WAVE_FORMAT_EXTENSIBLE (0xFFFE) is ATRAC3+. 0 for any other tag, or when the
 * prefix has no fmt chunk. */
static uint32_t atrac_riff_codec(uint32_t buf, uint32_t size) {
    if (size < 44u || !sr_guest_span_readable(buf, size) ||
        MEM_R32(buf) != 0x46464952u /* 'RIFF' */ ||
        MEM_R32(buf + 8u) != 0x45564157u /* 'WAVE' */)
        return 0u;
    uint32_t end = size < ATRAC_RIFF_SCAN_MAX ? size : ATRAC_RIFF_SCAN_MAX;
    for (uint32_t off = 12u; off <= end - 10u;) {
        uint32_t sz = MEM_R32(buf + off + 4u);
        if (MEM_R32(buf + off) == 0x20746d66u /* 'fmt ' */) {
            uint32_t tag = MEM_R16(buf + off + 8u);
            return tag == 0x0270u ? ATRAC_CODEC_AT3
                 : tag == 0xFFFEu ? ATRAC_CODEC_AT3PLUS : 0u;
        }
        uint64_t next = (uint64_t)off + 8u + sz + (sz & 1u);
        if (next > end) break;
        off = (uint32_t)next;
    }
    return 0u;
}
/* Channel count of the RIFF prefix as the codec `codec` parses it, without
 * touching any context; -1 when the prefix does not parse. */
static int atrac_riff_channels(uint32_t codec, uint32_t buf, uint32_t size,
                               uint32_t *refusal) {
    Atrac probe;
    memset(&probe, 0, sizeof(probe));
    probe.codecType = codec;
    return atrac_parse_track(&probe, buf, size, refusal) ? probe.channels : -1;
}
static void atrac_log_setdata(const Atrac *a, const char *call) {
    if (!atrac_log_on()) return;
    fprintf(stderr, "ATRAC_SETDATA: %s id=%d buf=0x%08x size=%u bufSize=%u fileSize=%u frame=%u dataOff=%u endSample=%d state=%d\n",
            call, (int)(a - s_atrac), a->buf, a->size, a->bufferMaxSize, a->fileSize,
            a->bytesPerFrame, a->dataByteOffset, a->endSample, a->bufferState);
}
/* Shared body of the *AndGetID calls. */
static uint32_t atrac_set_new(uint32_t buf, uint32_t readSize, uint32_t bufSize,
                              int mono, const char *call) {
    if (readSize > bufSize) return ATRAC_ERROR_INCORRECT_READ_SIZE;
    if (bufSize > readSize && !sr_guest_span_readable(buf, bufSize))
        return ATRAC_ERROR_SIZE_TOO_SMALL;
    uint32_t codec = atrac_riff_codec(buf, readSize);
    uint32_t refusal = 0;
    int channels = codec ? atrac_riff_channels(codec, buf, readSize, &refusal) : -1;
    if (channels < 0) return refusal ? refusal : ATRAC_ERROR_UNKNOWN_FORMAT;
    if (mono && channels != 1) return ATRAC_ERROR_NOT_MONO;
    int id = atrac_alloc_slot(codec);
    if (id < 0) return ATRAC_ERROR_NO_ATRACID;
    Atrac *a = &s_atrac[id];
    memset(a, 0, sizeof(*a));
    a->used = 1; a->codecType = codec;
    if (!atrac_set_track(a, buf, readSize, bufSize, &refusal)) {
        /* The ID was never handed out: do not leave a half-configured slot
         * behind that later calls could mistake for a valid context. */
        a->used = 0;
        return refusal ? refusal : ATRAC_ERROR_UNKNOWN_FORMAT;
    }
    atrac_ensure_decoder(a);
    if (a->codecType == ATRAC_CODEC_AT3 && !a->dec_at3) {
        atrac_release_decoder(a);
        atrac_stream_window_free(a);
        a->used = 0;
        return ATRAC_ERROR_UNKNOWN_FORMAT;
    }
    atrac_log_setdata(a, call);
    return (uint32_t)id;
}
static Atrac *atrac_of(uint32_t id) { return id < 8 && s_atrac[id].used ? &s_atrac[id] : 0; }
/* Shared body of the calls that configure an existing ID. A MOut call on a
 * track that is not mono returns NOT_MONO; the public reference keeps the
 * data in that case, but its stereo-to-mono output is not modelled here, so
 * the HLE refuses before touching the context. */
static uint32_t atrac_set_existing(uint32_t id, uint32_t buf, uint32_t readSize,
                                   uint32_t bufSize, int mono, const char *call) {
    Atrac *a = atrac_of(id);
    if (!a) return ATRAC_ERROR_BAD_ATRACID;
    if (readSize > bufSize) return ATRAC_ERROR_INCORRECT_READ_SIZE;
    if (!buf || readSize < 44u || !sr_guest_span_readable(buf, bufSize))
        return ATRAC_ERROR_SIZE_TOO_SMALL;
    uint32_t refusal = 0;
    if (mono) {
        int channels = atrac_riff_channels(a->codecType, buf, readSize, &refusal);
        if (refusal) return refusal;
        if (channels != 1) return ATRAC_ERROR_NOT_MONO;
    }
    /* A refusal leaves the context as it was (rollback in atrac_set_track); a
     * context that held no data keeps holding none. What the PSP leaves in a
     * context that already held data is UNMEASURED. */
    if (!atrac_set_track(a, buf, readSize, bufSize, &refusal))
        return refusal ? refusal : ATRAC_ERROR_UNKNOWN_FORMAT;
    atrac_ensure_decoder(a);
    if (a->codecType == ATRAC_CODEC_AT3 && !a->dec_at3)
        return ATRAC_ERROR_UNKNOWN_FORMAT;
    atrac_log_setdata(a, call);
    return 0;
}
/* sceAtracSetDataAndGetID(buf, bufSize) */
static uint32_t h_AtracSetDataAndGetID(CpuState *s) {
    return atrac_set_new(A0, A1, A1, 0, "SetDataAndGetID");
}
/* sceAtracSetHalfwayBufferAndGetID(buf, readSize, bufSize) */
static uint32_t h_AtracSetHalfwayBufferAndGetID(CpuState *s) {
    return atrac_set_new(A0, A1, A2, 0, "SetHalfwayBufferAndGetID");
}
/* sceAtracSetMOutDataAndGetID(buf, bufSize) */
static uint32_t h_AtracSetMOutDataAndGetID(CpuState *s) {
    return atrac_set_new(A0, A1, A1, 1, "SetMOutDataAndGetID");
}
/* sceAtracSetMOutHalfwayBufferAndGetID(buf, readSize, bufSize) */
static uint32_t h_AtracSetMOutHalfwayBufferAndGetID(CpuState *s) {
    return atrac_set_new(A0, A1, A2, 1, "SetMOutHalfwayBufferAndGetID");
}
/* sceAtracSetData(id, buf, bufSize) */
static uint32_t h_AtracSetData(CpuState *s) {
    return atrac_set_existing(A0, A1, A2, A2, 0, "SetData");
}
/* sceAtracSetHalfwayBuffer(id, buf, readSize, bufSize) */
static uint32_t h_AtracSetHalfwayBuffer(CpuState *s) {
    return atrac_set_existing(A0, A1, A2, A3, 0, "SetHalfwayBuffer");
}
/* sceAtracSetMOutData(id, buf, bufSize) */
static uint32_t h_AtracSetMOutData(CpuState *s) {
    return atrac_set_existing(A0, A1, A2, A2, 1, "SetMOutData");
}
/* sceAtracSetMOutHalfwayBuffer(id, buf, readSize, bufSize) */
static uint32_t h_AtracSetMOutHalfwayBuffer(CpuState *s) {
    return atrac_set_existing(A0, A1, A2, A3, 1, "SetMOutHalfwayBuffer");
}
/* sceAtracSetAA3DataAndGetID(buf, bufSize, fileSize, metadataSizeAddr) and
 * sceAtracSetAA3HalfwayBufferAndGetID(buf, readSize, bufSize, fileSize) take
 * an OMA/AA3 file: an ID3-style tag whose magic is "ea3", then an "EA3" header
 * whose codec byte and parameters name the codec, frame size and channels.
 * The header checks and their error codes below follow the public PPSSPP
 * reference (GPL-2.0-or-later, Core/Util/AtracTrack.cpp AnalyzeAA3Track);
 * none of them is measured on a PSP (UNMEASURED). This runtime parses RIFF
 * only: the AA3 frame layout, the codec parameters (the decoder extradata
 * comes from the RIFF fmt chunk today) and the metadata-size output are not
 * modelled, so a well-formed AA3 header fails closed with
 * ATRAC_ERROR_UNKNOWN_FORMAT and an ATRAC_AA3_UNSUPPORTED stderr line instead
 * of a made-up context. No ID is allocated and metadataSizeAddr is not
 * written. Until the AA3 parser exists, a title that streams AA3 stays
 * silent and visibly so. */
#define ATRAC_ERROR_AA3_INVALID_DATA 0x80631003u
#define ATRAC_ERROR_AA3_SIZE_TOO_SMALL 0x80631004u
static uint32_t atrac_aa3_refuse(uint32_t buf, uint32_t readSize, uint32_t bufSize,
                                 uint32_t fileSize, const char *call) {
    if (readSize > bufSize) return ATRAC_ERROR_INCORRECT_READ_SIZE;
    if (readSize < 10u || !sr_guest_span_readable(buf, readSize))
        return ATRAC_ERROR_AA3_SIZE_TOO_SMALL;
    if (MEM_R8(buf) != 'e' || MEM_R8(buf + 1u) != 'a' || MEM_R8(buf + 2u) != '3')
        return ATRAC_ERROR_AA3_INVALID_DATA;
    uint32_t tag = (uint32_t)MEM_R8(buf + 9u) | ((uint32_t)MEM_R8(buf + 8u) << 7) |
                   ((uint32_t)MEM_R8(buf + 7u) << 14) | ((uint32_t)MEM_R8(buf + 6u) << 21);
    if ((uint64_t)readSize < (uint64_t)tag + 46u)
        return ATRAC_ERROR_AA3_SIZE_TOO_SMALL;
    uint32_t ea3 = buf + 10u + tag;
    if (MEM_R8(ea3) != 'E' || MEM_R8(ea3 + 1u) != 'A' || MEM_R8(ea3 + 2u) != '3')
        return ATRAC_ERROR_AA3_INVALID_DATA;
    uint32_t codec = MEM_R8(ea3 + 32u);
    if (codec != 0u && codec != 1u)
        return ATRAC_ERROR_AA3_INVALID_DATA;
    fprintf(stderr,
            "ATRAC_AA3_UNSUPPORTED: %s buf=0x%08x readSize=%u bufSize=%u fileSize=%u "
            "codec byte %u: the OMA/AA3 container is not modelled; refused with "
            "ATRAC_ERROR_UNKNOWN_FORMAT (#1379).\n",
            call, buf, readSize, bufSize, fileSize, codec);
    return ATRAC_ERROR_UNKNOWN_FORMAT;
}
static uint32_t h_AtracSetAA3DataAndGetID(CpuState *s) {
    return atrac_aa3_refuse(A0, A1, A1, A2, "SetAA3DataAndGetID");
}
static uint32_t h_AtracSetAA3HalfwayBufferAndGetID(CpuState *s) {
    return atrac_aa3_refuse(A0, A1, A2, A3, "SetAA3HalfwayBufferAndGetID");
}
static uint32_t h_AtracReleaseAtracID(CpuState *s) {
    Atrac *a = atrac_of(A0);
    if (!a) return ATRAC_ERROR_BAD_ATRACID;
    atrac_release_decoder(a);
    atrac_stream_window_free(a);
    a->used = 0;
    return 0;
}
#ifdef SR_HLE_THREAD_SELFTEST
/* Streamed-ring geometry probe for the #32 wrap regression. No PSP API
 * exposes the ring's internal read cursor, and the defect is precisely that
 * the cursor leaves the frame grid after a wrap -- so the regression needs to
 * see the cursor to assert the invariant. Test builds only; reads state, never
 * changes it. */
int sr_hle_test_atrac_ring(uint32_t id, uint32_t *pos, uint32_t *base,
                           uint32_t *end, uint32_t *frame, uint32_t *valid) {
    Atrac *a = atrac_of(id);
    if (!a) return 0;
    if (pos) *pos = a->bufferPos;
    if (base) *base = atrac_stream_buffer_base(a);
    if (end) *end = atrac_stream_buffer_end(a);
    if (frame) *frame = a->bytesPerFrame;
    if (valid) *valid = a->bufferValidBytes;
    return 1;
}
/* Retained second-buffer probe for the TD-24 batch 4 regression. Test builds
 * only; reads state, never changes it. */
int sr_hle_test_atrac_second_buffer(uint32_t id, uint32_t *addr, uint32_t *size) {
    Atrac *a = atrac_of(id);
    if (!a) return 0;
    if (addr) *addr = a->secondBuf;
    if (size) *size = a->secondBufSize;
    return 1;
}
#endif
/* Decode the next frame through its codec-specific bridge into guest `out`,
 * dropping its first `skip` samples. *samples_out is the decoder frame length;
 * (frame - skip) samples are written. Returns
 *   1  frame decoded and written (channels*(samplesPerFrame-skip) interleaved s16),
 *   0  no frame available right now (streamed ring empty or frame not yet fed),
 *  -1  permanent failure (no decoder, or the frame failed to decode).
 * A -1 is a visible gap: the caller returns an error code instead of
 * fabricating PCM (no fake audio). */
static unsigned s_atrac_prime_decodes;   /* frame-0 priming decodes (#1096) */
/* How long sceAtracDecodeData keeps its caller waiting after a call that returned samples.
 * MEASURED on a PSP-3000 (6.61, probe at3-decode, two boots, 48 calls): a plain AT3 track,
 * 2 channels, 304-byte frames, 1024 samples a call, takes 1,152-1,203 us (median 1,177) and a
 * lower-priority peer runs for about 1 ms of it, as during a 1 ms sceKernelDelayThread; the
 * buffer-empty call (0x80630023) returns in 7 us. An ATRAC3+ track, 2 channels, 744-byte frames,
 * 2048 samples a call, takes 2,964-3,113 us (median 3,055 over 8 calls, one boot) with the peer
 * running for about 3 ms of it; its buffer-empty call returns in 8 us. Every other codec and
 * frame size is UNMEASURED and keeps returning at once, as before. */
static uint32_t s_atrac_last_block_us;
static uint32_t atrac_decode_block_us_for(uint32_t codec, int channels, uint32_t bytes_per_frame) {
    if (codec == ATRAC_CODEC_AT3 && channels == 2 && bytes_per_frame == 304u) return 1177u;
    if (codec == ATRAC_CODEC_AT3PLUS && channels == 2 && bytes_per_frame == 744u) return 3055u;
    return 0u;
}
static uint32_t atrac_measured_decode_block_us(const Atrac *a) {
    return atrac_decode_block_us_for(a->codecType, a->channels, a->bytesPerFrame);
}
#ifdef SR_HLE_THREAD_SELFTEST
uint32_t sr_hle_test_atrac_last_block_us(void) { return s_atrac_last_block_us; }
uint32_t sr_hle_test_atrac_block_us_for(uint32_t codec, int channels, uint32_t bytes_per_frame) {
    return atrac_decode_block_us_for(codec, channels, bytes_per_frame);
}
unsigned sr_hle_test_atrac_prime_decodes(int reset) {
    unsigned n = s_atrac_prime_decodes;
    if (reset) s_atrac_prime_decodes = 0u;
    return n;
}
#endif
static int atrac_decode_frame(Atrac *a, uint32_t out, int skip, int *samples_out) {
    uint32_t src;
    int streamed = atrac_streamed_state(a);
    if (samples_out) *samples_out = 0;
    if (skip < 0 || skip >= atrac_samples_per_frame(a)) return -1;
    if (a->bytesPerFrame == 0u) return 0;   /* ring not sized yet (streaming refill) */
    if (streamed) {
        if (a->bufferValidBytes < a->bytesPerFrame) return 0;
        if (!a->streamWindow || a->streamWindowQueued < a->bytesPerFrame)
            return -1;
        src = a->buf + a->bufferPos;
    } else {
        /* ALL_DATA_LOADED / HALFWAY_BUFFER: linear buffer, frame by posSample. */
        uint32_t off = atrac_file_offset_by_sample(a, a->posSample);
        if (a->size < a->bytesPerFrame || off > a->size - a->bytesPerFrame)
            return 0;   /* fed bytes do not reach this frame yet */
        src = a->buf + off;
    }
    if ((!a->dec && !a->dec_at3) || !a->host_frame || !a->host_pcm) return -1;
    if (a->bytesPerFrame > (uint32_t)a->host_frame_cap) return -1;
    if (streamed) {
        if (!atrac_stream_window_peek(a, a->host_frame, a->bytesPerFrame))
            return -1;
    } else {
        if (!sr_guest_span_readable(src, a->bytesPerFrame)) return -1;
        for (uint32_t i = 0u; i < a->bytesPerFrame; i++)
            a->host_frame[i] = (uint8_t)MEM_R8(src + i);
    }
    int n = 0;
    int ret = 0;
    if (a->codecType == ATRAC_CODEC_AT3 && a->at3FirstFramePending && a->at3PrimeValid) {
        /* The measured first-frame priming above: frame 0 through the decoder, output discarded. */
        a->at3PrimeValid = 0;
        s_atrac_prime_decodes++;
        ret = atrac3_bridge_decode(a->dec_at3, a->at3PrimeFrame,
                                   (int)a->bytesPerFrame, a->host_pcm, &n);
    }
    if (ret < 0)
        ;   /* the priming decode failed: reported below like any frame */
    else if (a->codecType == ATRAC_CODEC_AT3)
        ret = atrac3_bridge_decode(a->dec_at3, a->host_frame,
                                   (int)a->bytesPerFrame, a->host_pcm, &n);
    else
        ret = atrac3p_bridge_decode(a->dec, a->host_frame,
                                    (int)a->bytesPerFrame, a->host_pcm, &n);
    if (ret < 0 || n != atrac_samples_per_frame(a)) {
        /* Keep the offset with the codec error: a streamed-ring frame can be
         * pointed at incorrectly and a frame decoder genuinely cannot handle
         * may produce the same error; only the offset distinguishes them (#32). */
        if (atrac_log_on())
            fprintf(stderr, "ATRAC_DECODE: frame decode failed ret=%d n=%d src=0x%08x pos=%u base=%u end=%u frame=%u\n",
                    ret, n, src, a->bufferPos, atrac_stream_buffer_base(a),
                    atrac_stream_buffer_end(a), a->bytesPerFrame);
        atrac_report_decode_failure(a);
        return -1;
    }
    /* Whole-span preflight before the bulk guest write (AGENTS.md guest-memory
     * hardening): the complete channels*kept*2 writable span must be valid, and
     * a rejected output address must not leave partial PCM behind. */
    int kept = n - skip;
    const int16_t *pcm = a->host_pcm + skip * a->channels;
    if (out && !sr_guest_span_writable(out, (uint32_t)kept * (uint32_t)a->channels * 2u))
        return -1;
    /* host_pcm is already interleaved channels*n s16 (PR-A contract). */
    if (out)
        for (int i = 0; i < kept * a->channels; i++)
            MEM_W16(out + (uint32_t)i * 2u, (uint16_t)pcm[i]);
    if (audio_stat_on()) {
        int pk = 0;
        for (int i = 0; i < kept * a->channels; i++) {
            int v = pcm[i];
            if (v < 0) v = -v;
            if (v > pk) pk = v;
        }
        g_atrac_frames++;
        if (pk) g_atrac_frames_nonzero++;
        if (pk > g_atrac_peak) g_atrac_peak = pk;
        audio_note_atrac_out(out);
    }
    if (samples_out) *samples_out = n;
    return 1;
}

/* First sceAtracDecodeData of a plain AT3 track against the delay word d of an 8-byte RIFF fact
 * chunk.  MEASURED on a PSP-3000 (firmware 6.61; #1096, PSP-AT3-002 at3-delay-sweep, 2026-10-31,
 * 36 delays on one 2-channel 304-byte-block stream, then 21 more over the whole track):
 *   d == 0 or 955 <= d <= 1797: the first call returns 1024 - ((d + 69) mod 1024) samples (69 is
 *     the decoder's own delay): the leading (d + 69) mod 1024 samples of the first decoded frame are
 *     dropped (delays 955 and 956 differ by exactly one leading stereo sample), and every later call
 *     returns 1024 samples, identical whatever d is;
 *   1 <= d <= 954: sceAtracSetData accepts the track, but every DecodeData returns 0x80630002 with
 *     0 samples and consumes nothing;
 *   d >= 1855: refused by sceAtracSetData (atrac_parse_track).
 * 1798 .. 1854 is not measured: the same length rule is applied and ATRAC3_NOT_MEASURED is reported
 * once.  A 12-byte fact chunk is not measured either and keeps the complete decoder frame. */
#define ATRAC3_FIRST_FRAME_DECODER_DELAY 69u
#define ATRAC3_FIRST_FRAME_MIN_DELAY 955u        /* smallest nonzero delay the first decode accepts */
#define ATRAC3_FIRST_FRAME_MEASURED_MAX 1797u    /* largest delay measured on the accepted side */
/* The leading samples the first call drops, or -1 when the first call is refused. */
static int atrac3_first_frame_skip(uint32_t delay) {
    if (delay != 0u && delay < ATRAC3_FIRST_FRAME_MIN_DELAY) return -1;
    return (int)((delay + ATRAC3_FIRST_FRAME_DECODER_DELAY) % (uint32_t)ATRAC3_SAMPLES_PER_FRAME);
}
/* Returns the first-call error for a refused delay (0 otherwise) and sets
 * *skip to the leading samples the first frame drops. */
static uint32_t atrac3_first_frame_rule(const Atrac *a, int *skip) {
    static int reported;
    *skip = 0;
    if (a->codecType != ATRAC_CODEC_AT3 || !a->at3FirstFramePending)
        return 0;
    if (a->at3FactSize == 8u) {
        int dropped = atrac3_first_frame_skip(a->at3FactDelay);
        if (dropped < 0) return ATRAC_ERROR_API_FAIL;
        if (a->at3FactDelay > ATRAC3_FIRST_FRAME_MEASURED_MAX && !reported) {
            fprintf(stderr,
                    "ATRAC3_NOT_MEASURED: first-frame length for fact delay %u is not measured; "
                    "the measured length rule is applied (#1096).\n", a->at3FactDelay);
            reported = 1;
        }
        *skip = dropped;
        return 0;
    }
    if (!reported) {
        fprintf(stderr,
                "ATRAC3_NOT_MEASURED: first-frame length for fact size %u delay %u is not measured; "
                "the complete decoder frame is kept (#1096).\n",
                a->at3FactSize, a->at3FactDelay);
        reported = 1;
    }
    return 0;
}

static uint32_t h_AtracDecodeData(CpuState *s) {
    /* a0=id, a1=outSamples, a2=*decodedSamples, a3=*finishFlag, t0=*remainFrames. */
    s_atrac_last_block_us = 0u;
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    /* MEASURED on a PSP-3000 (#1096, `at3-decode`): the DecodeData after a
     * refused sceAtracSetData (fact delay 2048) returns 0x80630010. The
     * context holds no data, so a context that never received a track reports
     * the same. The output words were not recorded and are left untouched. The
     * measurement is plain AT3; ATRAC3+ follows the same context state here,
     * UNMEASURED. */
    if (a->bufferState == 0) return ATRAC_ERROR_NO_DATA;
    uint32_t out = A1, decAddr = A2, finAddr = A3, remAddr = stack_arg(s, 0);
    int n = 0, finished = 0;
    if (a->codecType == ATRAC_CODEC_AT3 &&
        a->posSample >= a->endSample && a->loopNum == 0) {
        if (decAddr) MEM_W32(decAddr, 0u);
        if (finAddr) MEM_W32(finAddr, 0u);
        if (remAddr) MEM_W32(remAddr, atrac_remaining_frames(a));
        return ATRAC_ERROR_DECODE_DATA_AFTER_END;
    }
    int skip = 0;
    uint32_t first_err = atrac3_first_frame_rule(a, &skip);
    if (first_err) {
        /* Measured: 0 samples. The other outputs are not recorded, and a
         * repeated call (which refuses again here, nothing being consumed)
         * was not measured. */
        if (decAddr) MEM_W32(decAddr, 0u);
        return first_err;
    }
    int decoded_samples = 0;
    int st = atrac_decode_frame(a, out, skip, &decoded_samples);
    if (st < 0) {
        /* Honest gap: this track cannot produce PCM in this build (unsupported
         * codec, decoder allocation failure, or a frame that failed to decode).
         * Report the failure rather than fabricate silence. */
        if (decAddr) MEM_W32(decAddr, 0u);
        if (finAddr) MEM_W32(finAddr, 1u);
        if (remAddr) MEM_W32(remAddr, 0u);
        atrac_report_decode_failure(a);
        return ATRAC_ERROR_UNKNOWN_FORMAT;
    }
    if (st == 0 && a->codecType == ATRAC_CODEC_AT3 &&
        atrac_streamed_state(a) && a->size < a->fileSize) {
        /* MEASURED on a PSP-3000 (#1096; fixtures/psp_oracle/README.md
         * `at3-decode`): once the buffered frames of a streamed AT3 track are
         * used up and file data remains to be added, DecodeData returns
         * 0x80630023 with 0 samples and end flag 0, and leaves the remaining-frames
         * output as the caller set it (the probe pre-set -1 and read -1 back).
         * Nothing is consumed, so sceAtracAddStreamData resumes decoding. */
        if (decAddr) MEM_W32(decAddr, 0u);
        if (finAddr) MEM_W32(finAddr, 0u);
        return ATRAC_ERROR_BUFFER_IS_EMPTY;
    }
    if (st > 0) {
        /* The guest receives the kept samples. The stream position still
         * advances by the whole decoder frame, as before this trim existed:
         * the PSP's position after a shortened first call is NOT_MEASURED
         * (#1096), so the frame-offset bookkeeping is left unchanged. */
        n = decoded_samples - skip;
        if (a->at3FirstFramePending) a->at3SkipApplied = skip;
        a->at3FirstFramePending = 0;
        if (atrac_streamed_state(a)) atrac_consume_frame(a);
        a->posSample += decoded_samples;
        if (atrac_streamed_state(a) && a->loopEndSample > 0 && a->posSample > a->loopEndSample) {
            a->posSample = a->loopStartSample >= 0 ? a->loopStartSample : 0;
            if (a->loopNum > 0) a->loopNum--;
            (void)atrac_reset_decoder(a);   /* loop rewind: no cross-frame leak */
        } else if (a->posSample >= a->endSample) {
            if (a->loopNum == 0) finished = 1;
            else {
                a->posSample = a->loopStartSample >= 0 ? a->loopStartSample : 0;
                if (a->loopNum > 0) a->loopNum--;
                (void)atrac_reset_decoder(a);
            }
        }
    }
    if (decAddr) MEM_W32(decAddr, (uint32_t)n);
    if (finAddr) MEM_W32(finAddr, (uint32_t)finished);
    if (remAddr) MEM_W32(remAddr, atrac_remaining_frames(a));
    if (atrac_log_on()) {
        static uint32_t call;
        call++;
        if (call <= 16u || (call & (call - 1u)) == 0u)
            fprintf(stderr, "ATRAC_DECODE: id=%u call=%u posSample=%d endSample=%d decoded=%d finished=%d state=%d avail=%u out=0x%08x out0=%04x out1=%04x out2=%04x out3=%04x\n",
                    A0, call, a->posSample, a->endSample, n, finished, a->bufferState, a->bufferValidBytes, out,
                    out ? MEM_R16(out + 0u) : 0, out ? MEM_R16(out + 2u) : 0,
                    out ? MEM_R16(out + 4u) : 0, out ? MEM_R16(out + 6u) : 0);
    }
    /* The decode takes time on the PSP and its caller gives up the CPU meanwhile, so threads of
     * the same priority run during it (#1296: HST's audio-output thread starved behind its
     * ATRAC decode threads when this returned at once). */
    const uint32_t block_us = st > 0 ? atrac_measured_decode_block_us(a) : 0u;
    s_atrac_last_block_us = block_us;
    if (block_us) sched_delay_current(block_us);
    return 0;
}
/* sceAtracGetNextSample(id, *outN): how many samples the NEXT DecodeData call
 * will produce. Previously registered to the generic success handler, which
 * leaves the caller's output variable untouched -- so the game read whatever
 * was already there and got a truthful-looking answer by accident or not at
 * all. This is the most frequently called stub on the boot route (4049 calls).
 *
 * No new state is needed: h_AtracDecodeData advances posSample by the
 * codec-specific samples-per-frame value only when a frame is consumed and stops at
 * endSample, so the honest answer is exactly what that path will do next.
 * Reporting anything else here would contradict the decoder's own bookkeeping. */
/* sceAtracGetMaxSample(id, *outMax): codec-specific samples per decode frame.
 * Plain AT3 is 1024 samples/channel; ATRAC3+ is 2048. */
static uint32_t h_AtracGetMaxSample(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    if (!A1 || !sr_guest_span_writable(A1, 4u)) return 0x80000103u;   /* ILLEGAL_ADDR */
    MEM_W32(A1, (uint32_t)atrac_samples_per_frame(a));
    return 0;
}
/* sceAtracGetChannel(id, *outChannels): active audio channel count (1 = mono,
 * 2 = stereo). Public behaviour reference: the PSPSDK ATRAC3+ interface and
 * PPSSPP Core/HLE/sceAtrac.cpp. The honest answer is the parsed fmt header's
 * channel count retained on the context (a->channels); a fact-only prefix
 * that never carried a fmt chunk honestly reports 0. Errors reuse the
 * neighbouring Atrac codes. */
static uint32_t h_AtracGetChannel(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    if (!A1 || !sr_guest_span_writable(A1, 4u)) return 0x80000103u;   /* ILLEGAL_ADDR */
    MEM_W32(A1, (uint32_t)(a->channels < 0 ? 0 : a->channels));
    return 0;
}
/* sceAtracGetBitrate(id, *outBitrate): stream bitrate in kbps. Public
 * behaviour reference: the PSPSDK ATRAC3+ interface and PPSSPP
 * Core/HLE/sceAtrac.cpp. The container's own declared rate is the fmt
 * nAvgBytesPerSec field the parser already retains (a->avgBytesPerSec), so
 * the honest answer is that rate converted to kilobits per second; no other
 * bitrate source exists in the track. Whether firmware derives the value
 * from this field or from blockAlign and the sample rate is UNMEASURED here.
 * Errors reuse the neighbouring Atrac codes. */
static uint32_t h_AtracGetBitrate(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    if (!A1 || !sr_guest_span_writable(A1, 4u)) return 0x80000103u;   /* ILLEGAL_ADDR */
    MEM_W32(A1, (uint32_t)((uint64_t)a->avgBytesPerSec * 8u / 1000u));
    return 0;
}
/* sceAtracGetInternalErrorInfo(id, *outErr): sticky internal decoder error.
 * Public behaviour reference: the PSPSDK ATRAC3+ interface and PPSSPP
 * Core/HLE/sceAtrac.cpp. This runtime retains no latent error word: every
 * failure surfaces synchronously as the handling call's return code
 * (DecodeData returns a codec error instead of fabricating PCM), so a
 * reachable context by construction has no outstanding internal error and
 * the honest report is 0. If a sticky error is ever retained it must be
 * stored on the context and read here. Errors reuse the neighbouring Atrac
 * codes. */
static uint32_t h_AtracGetInternalErrorInfo(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    if (!A1 || !sr_guest_span_writable(A1, 4u)) return 0x80000103u;   /* ILLEGAL_ADDR */
    MEM_W32(A1, 0u);
    return 0;
}
/* sceAtracIsSecondBufferNeeded(id): 1 while a streamed track still has unfed
 * file bytes, else 0. Public behaviour reference: the PSPSDK ATRAC3+
 * interface and PPSSPP Core/HLE/sceAtrac.cpp. The second buffer exists only
 * for streamed (ring) playback, so ALL_DATA/HALFWAY contexts never need one;
 * within streamed mode the honest "needs more" signal is the decoder's own
 * size < fileSize bookkeeping -- the same condition AddStreamData enforces --
 * so a stream-feeding loop that waits for nonzero cannot hang while data
 * remains. The exact firmware window (loop-refill timing, ring-full gating)
 * is UNMEASURED here. Bad IDs reuse the neighbouring Atrac code. */
static uint32_t h_AtracIsSecondBufferNeeded(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    return (atrac_streamed_state(a) && a->size < a->fileSize) ? 1u : 0u;
}
/* sceAtracSetSecondBuffer(id, secondBuffer, secondBufferSize): retain the
 * overflow/second buffer for a streamed track. Public behaviour reference:
 * the PSPSDK ATRAC3+ interface and PPSSPP Core/HLE/sceAtrac.cpp, where the
 * Set call stores the buffer reference the second-buffer query path reports.
 * The stored address is never dereferenced by this runtime (loop-refill from
 * the second buffer is UNMEASURED here), so retention is the complete honest
 * behaviour: a nonzero buffer must still be a readable guest span (fail
 * closed like h_AtracSetData), while (0, 0) clears the reference. Whether
 * firmware gates this on streamed state, and the exact error for a null
 * buffer with a nonzero size, are UNMEASURED: the inconsistent pair is
 * refused with the neighbouring SIZE_TOO_SMALL code. Errors reuse the
 * neighbouring Atrac codes. */
static uint32_t h_AtracSetSecondBuffer(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    uint32_t buf = A1, size = A2;
    if (!buf) {
        if (size != 0u) return ATRAC_ERROR_SIZE_TOO_SMALL;
        a->secondBuf = 0u;
        a->secondBufSize = 0u;
        return 0;
    }
    if (!sr_guest_span_readable(buf, size)) return ATRAC_ERROR_SIZE_TOO_SMALL;
    a->secondBuf = buf;
    a->secondBufSize = size;
    return 0;
}
static uint32_t h_AtracGetNextSample(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    if (!A1 || !sr_guest_span_writable(A1, 4u)) return 0x80000103u;   /* ILLEGAL_ADDR */
    /* MEASURED (#1096, PSP-AT3-002): before the first decode of a plain AT3 track with an 8-byte
     * fact chunk, the next call's length is 1024 - ((delay + 69) mod 1024), for every delay
     * sceAtracSetData accepts, including those whose decode is then refused. */
    if (a->codecType == ATRAC_CODEC_AT3 && a->at3FirstFramePending && a->at3FactSize == 8u) {
        MEM_W32(A1, (uint32_t)(ATRAC3_SAMPLES_PER_FRAME -
                    (int)((a->at3FactDelay + ATRAC3_FIRST_FRAME_DECODER_DELAY) %
                          (uint32_t)ATRAC3_SAMPLES_PER_FRAME)));
        return 0;
    }
    int remain = a->endSample - a->posSample;
    if (remain < 0) remain = 0;
    if (remain > atrac_samples_per_frame(a)) remain = atrac_samples_per_frame(a);
    MEM_W32(A1, (uint32_t)remain);
    return 0;
}
static uint32_t h_AtracGetRemainFrame(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    if (!A1 || !sr_guest_span_writable(A1, 4u)) return 0x80000103u;
    MEM_W32(A1, atrac_remaining_frames(a));
    return 0;
}
static uint32_t h_AtracGetStreamDataInfo(CpuState *s) {
    /* a1=*writePointer (buf + ring offset), a2=*writableBytes, a3=*readOffset (file). */
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    uint32_t offset = 0, writable = 0, readOffset = 0;
    atrac_calculate_stream_info(a, &offset, &writable, &readOffset);
    if (A1 && sr_guest_span_writable(A1, 4u))
        MEM_W32(A1, a->buf + offset);
    if (A2 && sr_guest_span_writable(A2, 4u))
        MEM_W32(A2, writable);
    if (A3 && sr_guest_span_writable(A3, 4u))
        MEM_W32(A3, readOffset);
    return 0;
}
static uint32_t h_AtracAddStreamData(CpuState *s) {
    /* a1=size (new bytes available), a2=*writableBytes (updated by the call). */
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    uint32_t n = A1, writeOffset = 0, maxWritable = 0, readOffset = 0;
    atrac_calculate_stream_info(a, &writeOffset, &maxWritable, &readOffset);
    if (a->bufferState == ATRAC_STATE_ALL_DATA_LOADED)
        return ATRAC_ERROR_ALLDATA_LOADED;
    if (n > maxWritable) return ATRAC_ERROR_ADD_DATA_IS_TOO_BIG;
    uint32_t newSize;
    if (!sr_size_add_ok(readOffset, n, &newSize) || newSize > a->fileSize)
        return ATRAC_ERROR_ADD_DATA_IS_TOO_BIG;
    uint32_t newValid = a->bufferValidBytes;
    if (atrac_streamed_state(a) &&
        (!sr_size_add_ok(a->bufferValidBytes, n, &newValid) ||
         newValid > a->bufferMaxSize))
        return ATRAC_ERROR_ADD_DATA_IS_TOO_BIG;
    /* Keep a logical FIFO view of the guest ring.  The physical write span can
     * begin at the tail of the old header-prefixed lap and continue at offset
     * zero (or, after the first lap, at any wrapped offset).  The decoder must
     * see those bytes in file order, not as one host-contiguous guest span. */
    if (atrac_streamed_state(a) &&
        !atrac_stream_window_append(a, writeOffset, n))
        return ATRAC_ERROR_ADD_DATA_IS_TOO_BIG;
    a->size = newSize;
    if (atrac_streamed_state(a)) a->bufferValidBytes = newValid;
    atrac_update_buffer_state(a);
    if (atrac_log_on())
        fprintf(stderr, "ATRAC_STREAM_DATA: id=%u n=%u size=%u writable=%u state=%d avail=%u pos=%u\n",
                A0, n, a->size, maxWritable, a->bufferState, a->bufferValidBytes, a->bufferPos);
    if (A2 && sr_guest_span_writable(A2, 4u)) {
        atrac_calculate_stream_info(a, NULL, &maxWritable, NULL);
        MEM_W32(A2, maxWritable);
    }
    return 0;
}
/* MEASURED (#1096, PSP-AT3-002): after a shortened first AT3 call the position is the samples
 * the guest has received (955 after a 955-sample call, then 1979, 3003, ...), so the dropped
 * leading samples are taken off the decoder position. */
static uint32_t h_AtracGetNextDecodePosition(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    if (a->posSample >= a->endSample) return ATRAC_ERROR_ALLDATA_DECODED;
    int pos = a->posSample - a->at3SkipApplied;
    if (A1) MEM_W32(A1, (uint32_t)(pos > 0 ? pos : 0));
    return 0;
}
static uint32_t h_AtracGetSoundSample(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    if (A1) MEM_W32(A1, (uint32_t)a->endSample);          /* end sample */
    if (A2) MEM_W32(A2, a->loopStartSample >= 0 ? (uint32_t)a->loopStartSample : 0xFFFFFFFFu);
    if (A3) MEM_W32(A3, a->loopEndSample >= 0 ? (uint32_t)a->loopEndSample : 0xFFFFFFFFu);
    return 0;
}
static uint32_t h_AtracGetLoopStatus(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    if (A1) MEM_W32(A1, (uint32_t)a->loopNum);
    if (A2) MEM_W32(A2, a->loopEndSample > 0 && a->bufferState != ATRAC_STATE_STREAMED_LOOP_TRAILER ? 1u : 0u);
    return 0;
}
static uint32_t h_AtracSetLoopNum(CpuState *s) { Atrac *a = atrac_of(A0); if (a) a->loopNum = (int)A1; return 0; }
/* sceAtracGetBufferInfoForResetting and sceAtracResetPlayPosition share one
 * description of the refill that a reset to `sample` needs: one span of the
 * 32-byte PSPSDK pspatrac3.h PspBufferInfo (write address, bytes the guest
 * may write, bytes it must write, file offset of the first byte). */
typedef struct {
    uint32_t writePos, writable, minWrite, filePos;
} AtracResetSpan;
/* Error codes of the reset pair. They follow the public PPSSPP reference
 * (Core/HLE/ErrorCodes.h) and are not measured on a PSP here (#1096). */
#define ATRAC_ERROR_SECOND_BUFFER_NEEDED 0x80630012u
#define ATRAC_ERROR_BAD_SAMPLE 0x80630015u
#define ATRAC_ERROR_BAD_FIRST_RESET_SIZE 0x80630016u
#define ATRAC_ERROR_BAD_SECOND_RESET_SIZE 0x80630017u
/* The refill for a reset to `sample`, or the error both reset calls return.
 * A streamed track is refilled from the ring start with the frame before the
 * one that holds the sample, so the refill begins with a state-loading frame;
 * the guest may write the frame-aligned ring capacity (at most the rest of
 * the file) and must write two frames. A halfway buffer continues after its
 * fed prefix up to the sample's frame. The reset never needs the second
 * buffer. */
static uint32_t atrac_reset_buffer_info(const Atrac *a, uint32_t sample,
                                        AtracResetSpan *first,
                                        AtracResetSpan *second) {
    if (a->bufferState != ATRAC_STATE_ALL_DATA_LOADED &&
        a->bufferState != ATRAC_STATE_HALFWAY_BUFFER && !atrac_streamed_state(a))
        return ATRAC_ERROR_NO_DATA;
    if (a->bufferState == ATRAC_STATE_STREAMED_LOOP_TRAILER && !a->secondBuf)
        return ATRAC_ERROR_SECOND_BUFFER_NEEDED;
    if (sample > (uint32_t)a->endSample)
        return ATRAC_ERROR_BAD_SAMPLE;
    uint32_t spf = (uint32_t)atrac_samples_per_frame(a);
    first->writePos = a->buf;
    first->writable = 0u;
    first->minWrite = 0u;
    first->filePos = 0u;
    if (a->bufferState == ATRAC_STATE_HALFWAY_BUFFER) {
        uint64_t needed = (uint64_t)a->dataByteOffset +
                          (uint64_t)(sample / spf) * a->bytesPerFrame;
        uint64_t missing = needed > a->size ? needed - a->size : 0u;
        first->writePos = a->buf + a->size;
        first->writable = a->fileSize > a->size ? a->fileSize - a->size : 0u;
        first->minWrite = missing > 0xffffffffu ? 0xffffffffu : (uint32_t)missing;
        first->filePos = a->size;
    } else if (atrac_streamed_state(a)) {
        uint64_t off = a->dataByteOffset;
        if (sample >= spf)
            off += (uint64_t)((sample - spf) / spf) * a->bytesPerFrame;
        uint32_t fileOff = off > 0xffffffffu ? 0xffffffffu : (uint32_t)off;
        uint32_t ringBytes = a->bufferMaxSize / a->bytesPerFrame * a->bytesPerFrame;
        uint32_t rest = a->fileSize > fileOff ? a->fileSize - fileOff : 0u;
        first->writable = rest < ringBytes ? rest : ringBytes;
        first->minWrite = a->bytesPerFrame * 2u;
        first->filePos = fileOff;
    }
    second->writePos = a->secondBuf ? a->secondBuf : a->buf;
    second->writable = 0u;
    second->minWrite = 0u;
    second->filePos = 0u;
    return 0;
}
/* sceAtracResetPlayPosition(id, sample, writtenFirstBuf, writtenSecondBuf).
 * PSPSDK pspatrac3.h contract: the guest first asks
 * sceAtracGetBufferInfoForResetting where to write, writes the file bytes from
 * the reported file position to the reported write position, and then passes
 * the byte counts it wrote here. The library continues from that refill, so a
 * streamed context drops its old ring contents: the refill sits at the start
 * of the buffer (no RIFF prefix), the file read position continues after it,
 * and the decode cursor skips the refill's state-loading first frame exactly
 * as sceAtracSetData does for sample 0. Keeping the old ring (#1291) made the
 * next frames mix the stale ring with the guest's refill, which the decoder
 * rejects. A halfway buffer gains the bytes written after its fed prefix.
 * Counts outside what sceAtracGetBufferInfoForResetting offered are refused
 * and leave the context unchanged. Whether the PSP decodes the state-loading
 * frame to prime the decoder is not measured; like sceAtracSetData, the HLE
 * skips it. */
static uint32_t h_AtracResetPlayPosition(CpuState *s) {
    Atrac *a = atrac_of(A0);
    if (!a) return ATRAC_ERROR_BAD_ATRACID;
    uint32_t sample = A1, wroteFirst = A2, wroteSecond = A3;
    AtracResetSpan first, second;
    uint32_t err = atrac_reset_buffer_info(a, sample, &first, &second);
    if (err) return err;
    if (wroteFirst < first.minWrite || wroteFirst > first.writable)
        return ATRAC_ERROR_BAD_FIRST_RESET_SIZE;
    if (wroteSecond < second.minWrite || wroteSecond > second.writable)
        return ATRAC_ERROR_BAD_SECOND_RESET_SIZE;
    if (atrac_streamed_state(a)) {
        /* first.writable bounds the refill by the ring and the file end. */
        a->size = first.filePos + wroteFirst;
        a->bufferHeaderSize = 0;
        a->bufferPos = a->bytesPerFrame;
        a->bufferValidBytes = wroteFirst - a->bytesPerFrame;
        (void)atrac_stream_window_init(a);
        atrac_update_buffer_state(a);
    } else if (a->bufferState == ATRAC_STATE_HALFWAY_BUFFER) {
        a->size += wroteFirst;   /* first.writable is the rest of the file */
        atrac_update_buffer_state(a);
    }
    a->posSample = (int)sample;
    (void)atrac_reset_decoder(a);
    if (atrac_log_on())
        fprintf(stderr, "ATRAC_RESET: id=%u sample=%u wrote=%u size=%u pos=%u avail=%u state=%d\n",
                A0, sample, wroteFirst, a->size, a->bufferPos, a->bufferValidBytes, a->bufferState);
    return 0;
}

/* sceAtracGetSecondBufferInfo(id, *puiPosition, *puiDataByte): second-buffer
 * streaming position and required size. Public behaviour reference: PSPSDK
 * pspatrac3.h and PPSSPP Core/HLE/sceAtrac.cpp (AtracCtx::GetSecondBufferInfo).
 * Only streamed contexts with a loop trailer need a second buffer; otherwise
 * zero is reported with ATRAC_ERROR_SECOND_BUFFER_NOT_NEEDED (0x80630022).
 * When needed, reports the file offset corresponding to the loop end sample
 * and the remaining track bytes to the end of the file. */
static uint32_t h_AtracGetSecondBufferInfo(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    if (!A1 || !sr_guest_span_writable(A1, 4u) || !A2 || !sr_guest_span_writable(A2, 4u))
        return 0x80000103u; /* ILLEGAL_ADDR */
    if (a->bufferState != ATRAC_STATE_STREAMED_LOOP_TRAILER) {
        MEM_W32(A1, 0u);
        MEM_W32(A2, 0u);
        return 0x80630022u; /* SECOND_BUFFER_NOT_NEEDED */
    }
    uint32_t fileOffset = (a->loopEndSample > 0 && a->bytesPerFrame > 0)
        ? (a->dataByteOffset + (uint32_t)a->loopEndSample / (uint32_t)atrac_samples_per_frame(a) * a->bytesPerFrame)
        : 0u;
    uint32_t desiredSize = (a->fileSize > fileOffset) ? (a->fileSize - fileOffset) : 0u;
    MEM_W32(A1, fileOffset);
    MEM_W32(A2, desiredSize);
    return 0;
}

/* sceAtracGetBufferInfoForResetting / sceAtracGetBufferInfoForReseting:
 * obtains the buffer information required to seek to a sample position before
 * sceAtracResetPlayPosition. Public behaviour reference: PSPSDK pspatrac3.h
 * (PspBufferInfo, 32 bytes) and PPSSPP Core/HLE/sceAtrac.cpp.
 * Fills the 8-word PspBufferInfo describing the first and second buffer spans;
 * h_AtracResetPlayPosition accepts exactly the refill reported here. */
static uint32_t h_AtracGetBufferInfoForResetting(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return ATRAC_ERROR_BAD_ATRACID;
    if (!A2 || !sr_guest_span_writable(A2, 32u)) return 0x80000103u; /* ILLEGAL_ADDR */
    AtracResetSpan first, second;
    uint32_t err = atrac_reset_buffer_info(a, A1, &first, &second);
    if (err) return err;
    const AtracResetSpan *spans[2] = { &first, &second };
    for (uint32_t i = 0; i < 2u; i++) {
        MEM_W32(A2 + i * 16u + 0u, spans[i]->writePos);
        MEM_W32(A2 + i * 16u + 4u, spans[i]->writable);
        MEM_W32(A2 + i * 16u + 8u, spans[i]->minWrite);
        MEM_W32(A2 + i * 16u + 12u, spans[i]->filePos);
    }
    return 0;
}

/* sceAtracReinit(at3Count, at3plusCount): reserve context slots per codec.
 * Refuses with SCE_KERNEL_ERROR_BUSY (0x80000021) while any context is in use.
 * Counts are signed ints; a count of zero or below reserves nothing for that
 * codec ((0, 0) reserves nothing at all, so every sceAtracGetAtracID then
 * reports ATRAC_ERROR_NO_ATRACID). Slot cost and budget follow the public
 * PPSSPP reference (GPL-2.0-or-later, Core/HLE/sceAtrac.cpp sceAtracReinit),
 * UNMEASURED on a PSP: an ATRAC3+ context costs two units, an ATRAC3 context
 * one, against a budget of six (four for the oldest ATRAC library, whose SDK
 * version this runtime does not track). Requests are granted in order, ATRAC3+
 * first; those that do not fit are dropped and the call returns
 * SCE_KERNEL_ERROR_NO_MEMORY (0x80020190) after granting the ones that fit. */
#define ATRAC_REINIT_BUDGET 6
static uint32_t s_atrac_reinit_count = 0u;
static uint32_t h_AtracReinit(CpuState *s) {
    int at3 = (int)A0, at3plus = (int)A1;
    for (int i = 0; i < 8; i++) {
        if (s_atrac[i].used) return 0x80000021u; /* SCE_KERNEL_ERROR_BUSY */
    }
    memset(s_atrac_slot_codec, 0, sizeof(s_atrac_slot_codec));
    int next = 0, space = ATRAC_REINIT_BUDGET;
    for (int i = 0; i < at3plus && space >= 0; i++) {
        space -= 2;
        if (space >= 0) s_atrac_slot_codec[next++] = ATRAC_CODEC_AT3PLUS;
    }
    for (int i = 0; i < at3 && space >= 0; i++) {
        space -= 1;
        if (space >= 0) s_atrac_slot_codec[next++] = ATRAC_CODEC_AT3;
    }
    s_atrac_caps_configured = 1;
    s_atrac_reinit_count++;
    return space >= 0 ? 0u : HLE_KERNEL_ERROR_NO_MEMORY;
}

/* sceAtracReleaseResources(): release system resources allocated for ATRAC decoding. */
static uint32_t s_atrac_release_count = 0u;
static uint32_t h_AtracReleaseResources(CpuState *s) {
    (void)s;
    s_atrac_release_count++;
    return 0;
}

#ifdef SR_HLE_THREAD_SELFTEST
uint32_t sr_hle_test_atrac_reinit_count(void) { return s_atrac_reinit_count; }
void sr_hle_test_atrac_reinit_reset(void) { s_atrac_reinit_count = 0u; }
/* Back to the default pool (no sceAtracReinit yet): eight slots, either codec. */
void sr_hle_test_atrac_caps_reset(void) {
    s_atrac_caps_configured = 0;
    memset(s_atrac_slot_codec, 0, sizeof(s_atrac_slot_codec));
}
uint32_t sr_hle_test_atrac_release_count(void) { return s_atrac_release_count; }
void sr_hle_test_atrac_release_reset(void) { s_atrac_release_count = 0u; }
#endif

/* ---- sceMp3 ---------------------------------------------------------------------------------
 * The PSP MP3 library (libmp3.prx, loaded with sceUtilityLoadAvModule(PSP_AV_MODULE_MP3)). A game
 * reserves a handle over a guest stream buffer and a guest PCM buffer, feeds the file through
 * sceMp3GetInfoToAddStreamData/sceMp3NotifyAddStreamData, calls sceMp3Init once the first frame is
 * in, and then each sceMp3Decode returns one frame of interleaved stereo s16 in the next half of
 * its PCM buffer.
 *
 * derived-translated: the handle lifecycle, per-call error codes and their precedence, the stream
 * buffer geometry (a 0x5c0-byte work area, then two halves the game refills in turn), the end and
 * loop rules, the header parse and the frame-count/seek arithmetic are translated from PPSSPP
 * Core/HLE/sceMp3.cpp and Core/HW/SimpleAudioDec.cpp (AuCtx) at hrydgard/ppsspp
 * 0028f30da6784570264a41ef6cce1c3963692ea7 (GPL-2.0-or-later, Copyright (c) 2012- PPSSPP
 * Project), whose comments cite the pspautotests audio/mp3 hardware tests for those rules.
 * Modified for this runtime: frames are decoded by the vendored minimp3 through src/rt/mp3_bridge.c
 * instead of FFmpeg; guest spans are validated when the handle is reserved; a frame still
 * incomplete in the buffer is kept for the next call (PPSSPP drops the buffer) unless the whole
 * file has been added; seeks reset the decoder history and use 64-bit offset arithmetic; after a
 * seek by frame the two halves restart at the seek position (PPSSPP keeps them tied to the file
 * start, so the next refill it asks for runs past the end of the stream buffer); the
 * non-44.1 kHz refusal follows the libmp3 SDK-version threshold PPSSPP documents instead of its
 * deliberate leniency. Nothing here is PSP-measured by this project.
 *
 * sceMp3LowLevelInit/sceMp3LowLevelDecode and the module entry points are refused by name (see
 * hle_register_mp3_handlers): their contracts are not established. */
#define MP3_MAX_HANDLES 2u
#define MP3_AU_BUF_MIN_SIZE 8192
#define MP3_PCM_BUF_MIN_SIZE 9216
#define MP3_STREAM_WORKAREA 0x5c0u     /* stream bytes start this far into the stream buffer */
#define MP3_HEADER_SEARCH 1440u        /* sceMp3Init looks this far past the work area for a sync */
#define MP3_INIT_MIN_BYTES_SDK6 156    /* SDK 6+: bytes that must be added before sceMp3Init */
#define SCE_MP3_ERROR_INVALID_HANDLE 0x80671001u
#define SCE_MP3_ERROR_BAD_ADDR 0x80671002u
#define SCE_MP3_ERROR_BAD_SIZE 0x80671003u
#define SCE_MP3_ERROR_UNRESERVED_HANDLE 0x80671102u
#define SCE_MP3_ERROR_NOT_YET_INIT_HANDLE 0x80671103u
#define SCE_MP3_ERROR_NO_RESOURCE_AVAIL 0x80671201u
#define SCE_MP3_ERROR_BAD_SAMPLE_RATE 0x80671302u
#define SCE_MP3_ERROR_BAD_RESET_FRAME 0x80671501u
#define SCE_MP3_AVCODEC_ERROR_INVALID_DATA 0x807f00fdu
#define SCE_MP3_KERNEL_ERROR_INVALID_POINTER 0x80000103u
#define SCE_MP3_KERNEL_ERROR_INVALID_VALUE 0x800001feu

typedef struct Mp3Ctx {
    int used;
    uint64_t startPos, endPos;     /* file offsets from the reserve argument */
    uint32_t auBuf, auBufSize;     /* guest stream buffer (0 for an argument-less handle) */
    uint32_t pcmBuf, pcmBufSize;   /* guest PCM buffer, used in two halves */
    int64_t readPos;               /* file offset of the next byte the game adds */
    int64_t ringBase;              /* file offset that lands at the start of the first half */
    int64_t available;             /* bytes added but not yet consumed by the decoder */
    uint8_t *src;                  /* host copy of the added, unconsumed bytes */
    uint32_t srcLen, srcCap;
    int version;                   /* MPEG version bits once sceMp3Init succeeded, else -1 */
    int channels, bitRate, samplingRate;
    uint32_t maxOutputSample;
    int frameNum;
    uint32_t sumDecodedSamples;
    int loopNum;                   /* -1: loop forever (the default) */
    int nextOutputHalf;
    Mp3Bridge *dec;
} Mp3Ctx;
static Mp3Ctx s_mp3[MP3_MAX_HANDLES];
static int s_mp3_resource_inited;

static Mp3Ctx *mp3_ctx(uint32_t handle) {
    return handle < MP3_MAX_HANDLES && s_mp3[handle].used ? &s_mp3[handle] : NULL;
}
/* An out-of-range handle is always INVALID_HANDLE; an unused in-range one gets the call's code. */
static uint32_t mp3_missing(uint32_t handle, uint32_t code) {
    return handle >= MP3_MAX_HANDLES ? SCE_MP3_ERROR_INVALID_HANDLE : code;
}
static void mp3_free(Mp3Ctx *c) {
    mp3_bridge_destroy(c->dec);
    free(c->src);
    memset(c, 0, sizeof(*c));
}
static void mp3_drop_source(Mp3Ctx *c, uint32_t n) {
    if (n > c->srcLen) n = c->srcLen;
    if (n) memmove(c->src, c->src + n, c->srcLen - n);
    c->srcLen -= n;
    c->available -= n;
}
static int32_t mp3_stream_half(const Mp3Ctx *c) {
    return ((int32_t)c->auBufSize - (int32_t)MP3_STREAM_WORKAREA) / 2;
}
/* The game may write ahead to the end of the half after the one the decoder is reading, so a
 * half opens up only once the decoder has consumed past its end. Not clamped to the buffer size:
 * PPSSPP reports the hardware asking for 6721 bytes of an 8192-byte buffer after a notify of -1. */
static int32_t mp3_bytes_needed(const Mp3Ctx *c) {
    if (c->readPos >= (int64_t)c->endPos) return 0;
    int32_t half = mp3_stream_half(c);
    if (half <= 0) return 0;
    int64_t written = c->readPos - c->ringBase;
    int64_t consumed = written - c->available;
    int64_t halves_done = consumed / half - ((consumed % half < 0) ? 1 : 0);
    int64_t need = (halves_done + 2) * half - written;
    return need > 0 ? (int32_t)need : 0;
}
/* Where the next added bytes go, past the work area: it walks the two halves and wraps. */
static int32_t mp3_write_offset(const Mp3Ctx *c) {
    int64_t size = (int64_t)mp3_stream_half(c) * 2;
    if (size <= 0) return 0;
    int64_t pos = (c->readPos - c->ringBase) % size;
    return (int32_t)(pos < 0 ? pos + size : pos);
}
/* Header fields as libmp3 reports them: kbps (0 for free format, -1 when invalid) and Hz. */
static int mp3_header_bitrate(int version, int layer, int index) {
    static const int16_t v1[3][16] = {
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, -1},     /* Layer III */
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, -1},    /* Layer II */
        {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, -1}, /* Layer I */
    };
    static const int16_t v2[2][16] = {
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, -1},         /* Layer II/III */
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, -1},    /* Layer I */
    };
    if (layer == 0) return -1;
    if (version == 3) return v1[layer - 1][index];
    if (version == 2 || version == 0) return v2[layer == 3][index];
    return -1;
}
static int mp3_header_sample_rate(int version, int index) {
    static const int32_t rates[4][4] = {
        {11025, 12000, 8000, -1}, {-1, -1, -1, -1}, {22050, 24000, 16000, -1}, {44100, 48000, 32000, -1},
    };
    return rates[version & 3][index & 3];
}

static uint32_t h_Mp3InitResource(CpuState *s) {
    (void)s;
    if (s_mp3_resource_inited) return 0;
    s_mp3_resource_inited = 1;
    sched_delay_current(200);
    return 0;
}

static uint32_t h_Mp3TermResource(CpuState *s) {
    (void)s;
    if (!s_mp3_resource_inited) return 0;
    for (uint32_t i = 0; i < MP3_MAX_HANDLES; i++)
        if (s_mp3[i].used) mp3_free(&s_mp3[i]);
    s_mp3_resource_inited = 0;
    sched_delay_current(100);
    return 0;
}

/* sceMp3ReserveMp3Handle(SceMp3InitArg *arg): u64 stream start, u64 stream end, stream buffer
 * and size, PCM buffer and size (32 bytes). A NULL arg reserves a handle with no buffers, which
 * every stream call then reports as UNRESERVED. */
static uint32_t h_Mp3ReserveMp3Handle(CpuState *s) {
    uint32_t arg = A0;
    if (!s_mp3_resource_inited) return SCE_MP3_ERROR_NO_RESOURCE_AVAIL;
    uint32_t handle = MP3_MAX_HANDLES;
    for (uint32_t i = 0; i < MP3_MAX_HANDLES; i++)
        if (!s_mp3[i].used) { handle = i; break; }
    if (handle == MP3_MAX_HANDLES) return SCE_MP3_ERROR_NO_RESOURCE_AVAIL;
    /* The PSP crashes on a bad pointer; this is PPSSPP's error for it. */
    if (arg && !sr_guest_span_readable(arg, 32u)) return SCE_MP3_KERNEL_ERROR_INVALID_POINTER;

    Mp3Ctx c;
    memset(&c, 0, sizeof(c));
    if (arg) {
        c.startPos = (uint64_t)MEM_R32(arg) | ((uint64_t)MEM_R32(arg + 4u) << 32);
        c.endPos = (uint64_t)MEM_R32(arg + 8u) | ((uint64_t)MEM_R32(arg + 12u) << 32);
        c.auBuf = MEM_R32(arg + 16u);
        c.auBufSize = MEM_R32(arg + 20u);
        c.pcmBuf = MEM_R32(arg + 24u);
        c.pcmBufSize = MEM_R32(arg + 28u);
        if (c.startPos >= c.endPos) return SCE_MP3_ERROR_BAD_SIZE;
        if (!c.auBuf || !c.pcmBuf) return SCE_MP3_ERROR_BAD_ADDR;
        if ((int32_t)c.auBufSize < MP3_AU_BUF_MIN_SIZE || (int32_t)c.pcmBufSize < MP3_PCM_BUF_MIN_SIZE)
            return SCE_MP3_ERROR_BAD_SIZE;
        /* Runtime safety choice (unmeasured on hardware): both buffers must be mapped guest
         * memory before any host access is formed over them. */
        if (!sr_guest_span_writable(c.auBuf, c.auBufSize) ||
            !sr_guest_span_writable(c.pcmBuf, c.pcmBufSize))
            return SCE_MP3_ERROR_BAD_ADDR;
        /* Following the needed-bytes protocol keeps at most two halves unconsumed. */
        c.srcCap = c.auBufSize;
        c.src = (uint8_t *)malloc(c.srcCap);
    }
    if ((arg && !c.src) || mp3_bridge_create(&c.dec) != 0) {
        fprintf(stderr, "MP3: host allocation failed reserving handle %u\n", handle);
        free(c.src);
        return SCE_MP3_ERROR_NO_RESOURCE_AVAIL;
    }
    c.used = 1;
    c.readPos = (int64_t)c.startPos;
    c.ringBase = c.readPos;
    c.version = -1;
    c.samplingRate = -1;
    c.loopNum = -1;
    s_mp3[handle] = c;
    return handle;
}

static uint32_t h_Mp3ReleaseMp3Handle(CpuState *s) {
    uint32_t handle = A0;
    Mp3Ctx *c = mp3_ctx(handle);
    if (c) { mp3_free(c); return 0; }
    if (handle >= MP3_MAX_HANDLES) return SCE_MP3_ERROR_INVALID_HANDLE;
    return 0; /* releasing an unused handle number is not an error */
}

/* sceMp3Init(handle): parse the first frame header found within MP3_HEADER_SEARCH bytes of the
 * stream area (skipping, say, an ID3 tag) and fix the stream's format. */
static uint32_t h_Mp3Init(CpuState *s) {
    uint32_t handle = A0;
    Mp3Ctx *c = mp3_ctx(handle);
    if (!c) return mp3_missing(handle, SCE_MP3_ERROR_UNRESERVED_HANDLE);
    if (!c->auBuf) return SCE_MP3_ERROR_UNRESERVED_HANDLE;
    uint32_t area = c->auBuf + MP3_STREAM_WORKAREA;
    uint32_t hdr = 0;
    int found = 0;
    /* Inside the stream buffer validated at reserve (at least 8192 bytes). */
    for (uint32_t off = 0; off < MP3_HEADER_SEARCH; off++) {
        if (MEM_R8(area + off) == 0xffu && (MEM_R8(area + off + 1u) & 0xc0u) == 0xc0u) {
            hdr = 0xff000000u | ((uint32_t)MEM_R8(area + off + 1u) << 16) |
                  ((uint32_t)MEM_R8(area + off + 2u) << 8) | MEM_R8(area + off + 3u);
            found = 1;
            break;
        }
    }
    sched_delay_current(500);
    if (!found) return SCE_MP3_AVCODEC_ERROR_INVALID_DATA;
    int layer = (int)(hdr >> 17) & 3;
    int version = (int)(hdr >> 19) & 3;
    int bitrate = mp3_header_bitrate(version, layer, (int)(hdr >> 12) & 0xf);
    int rate = mp3_header_sample_rate(version, (int)(hdr >> 10) & 3);
    int channels = ((hdr >> 6) & 3) == 3 ? 1 : 2;
    if (bitrate <= 0 || rate < 0) return SCE_MP3_AVCODEC_ERROR_INVALID_DATA;
    uint32_t sdk = sr_sdkver_get(&g_sdk_version);
    if (sdk >= 0x06000000u && c->readPos < MP3_INIT_MIN_BYTES_SDK6)
        return SCE_MP3_KERNEL_ERROR_INVALID_VALUE;
    /* libmp3 accepts a rate other than 44.1 kHz only from a game compiled with SDK 3.09.05 or
     * later (it compares against 0x030904FF); 48 and 32 kHz otherwise come back as BAD_SAMPLE_RATE. */
    if (rate != 44100 && sdk <= 0x030904ffu) return SCE_MP3_ERROR_BAD_SAMPLE_RATE;
    c->samplingRate = rate;
    c->channels = channels;
    c->bitRate = bitrate;
    c->maxOutputSample = layer == 3 ? 384u : (layer == 2 || version == 3) ? 1152u : 576u;
    /* Frame count from the nominal frame size; like the PSP, padding slots are ignored and the
     * upper halves of the 64-bit positions are not used. */
    uint64_t bytes_per_second = (uint64_t)(c->maxOutputSample / 8u) * (uint64_t)bitrate * 1000u;
    uint64_t total = (c->endPos & 0xffffffffu) - (c->startPos & 0xffffffffu);
    c->frameNum = (int)((total * (uint64_t)rate) / bytes_per_second);
    c->version = version;
    return 0;
}

/* Decode the next frame into the next PCM half. Returns the PCM bytes reported (0 at the end of
 * the stream). */
static uint32_t mp3_decode_half(Mp3Ctx *c, uint32_t pcmAddr) {
    uint32_t half = c->pcmBufSize / 2u;
    uint32_t out = c->pcmBuf + (uint32_t)c->nextOutputHalf * half;
    if (pcmAddr) MEM_W32(pcmAddr, out);

    int16_t pcm[MP3_BRIDGE_MAX_PCM_VALUES];
    uint32_t pcm_bytes = 0;
    int all_added = c->readPos >= (int64_t)c->endPos;
    int end = c->readPos - c->available >= (int64_t)c->endPos;
    while (!end && c->srcLen) {
        int consumed = 0, samples = 0;
        int r = mp3_bridge_decode(c->dec, c->src, (int)c->srcLen, pcm, &consumed, &samples, NULL);
        mp3_drop_source(c, (uint32_t)consumed);
        if (r == MP3_BRIDGE_OK) {
            c->sumDecodedSamples += (uint32_t)samples;
            pcm_bytes = (uint32_t)samples * 4u;
            break;
        }
        if (r == MP3_BRIDGE_BAD_FRAME && consumed > 0) continue;
        /* No complete frame left. Once the whole file has been added nothing can complete it,
         * so the remainder is not audio and the stream is over. */
        if (all_added) mp3_drop_source(c, c->srcLen);
        break;
    }
    /* The hardware rewinds in the same call that decodes the last frame. */
    end = c->readPos - c->available >= (int64_t)c->endPos;
    if (end && c->loopNum != 0) {
        c->sumDecodedSamples = 0;
        c->readPos = (int64_t)c->startPos;
        c->ringBase = c->readPos;
        if (c->loopNum > 0) c->loopNum--;
    }
    /* Inside the PCM buffer validated at reserve; a half holds a whole 1152-sample frame. */
    for (uint32_t i = 0; i < pcm_bytes / 2u; i++) MEM_W16(out + i * 2u, (uint16_t)pcm[i]);
    for (uint32_t i = pcm_bytes; i < half; i++) MEM_W8(out + i, 0u);
    c->nextOutputHalf ^= 1;
    if (pcm_bytes == 0 && !end) {
        /* Underrun: no frame is buffered. Like PPSSPP, the half is silence and counts as
         * decoded; what the PSP returns here is not measured. */
        static unsigned underruns;
        if (++underruns <= 8u) fprintf(stderr, "MP3: decode underrun, returned a half of silence\n");
        return half;
    }
    return pcm_bytes;
}

/* sceMp3Decode(handle, s16 **pcm): *pcm receives the address of the half just written. */
static uint32_t h_Mp3Decode(CpuState *s) {
    uint32_t handle = A0, pcmAddr = A1;
    Mp3Ctx *c = mp3_ctx(handle);
    if (!c) return mp3_missing(handle, SCE_MP3_ERROR_NOT_YET_INIT_HANDLE);
    if (c->version < 0 || !c->auBuf) return SCE_MP3_ERROR_NOT_YET_INIT_HANDLE;
    /* The PSP faults on a bad out-pointer; refuse before decoding instead (unmeasured). */
    if (pcmAddr && !sr_guest_span_writable(pcmAddr, 4u)) return SCE_MP3_ERROR_BAD_ADDR;
    uint32_t bytes = mp3_decode_half(c, pcmAddr);
    if (bytes) sched_delay_current(2400);
    return bytes;
}

static uint32_t h_Mp3CheckStreamDataNeeded(CpuState *s) {
    uint32_t handle = A0;
    Mp3Ctx *c = mp3_ctx(handle);
    if (!c) return mp3_missing(handle, SCE_MP3_ERROR_UNRESERVED_HANDLE);
    if (!c->auBuf) return SCE_MP3_ERROR_UNRESERVED_HANDLE;
    return mp3_bytes_needed(c) > 0 ? 1u : 0u;
}

/* sceMp3GetInfoToAddStreamData(handle, u8 **dst, int *towrite, int *srcpos). */
static uint32_t h_Mp3GetInfoToAddStreamData(CpuState *s) {
    uint32_t handle = A0, dstPtr = A1, sizePtr = A2, posPtr = A3;
    Mp3Ctx *c = mp3_ctx(handle);
    if (!c) return mp3_missing(handle, SCE_MP3_ERROR_UNRESERVED_HANDLE);
    if (!c->auBuf) return SCE_MP3_ERROR_UNRESERVED_HANDLE;
    int32_t need = mp3_bytes_needed(c);
    uint32_t dst = need ? c->auBuf + MP3_STREAM_WORKAREA + (uint32_t)mp3_write_offset(c) : 0u;
    uint32_t pos = need ? (uint32_t)c->readPos : 0u;
    /* Each out-pointer is optional: an unmapped one is skipped, as in PPSSPP. */
    if (dstPtr && sr_guest_span_writable(dstPtr, 4u)) MEM_W32(dstPtr, dst);
    if (sizePtr && sr_guest_span_writable(sizePtr, 4u)) MEM_W32(sizePtr, (uint32_t)need);
    if (posPtr && sr_guest_span_writable(posPtr, 4u)) MEM_W32(posPtr, pos);
    return 0;
}

/* sceMp3NotifyAddStreamData(handle, int size): the game wrote size bytes where
 * sceMp3GetInfoToAddStreamData pointed it. */
static uint32_t h_Mp3NotifyAddStreamData(CpuState *s) {
    uint32_t handle = A0;
    int32_t size = (int32_t)A1;
    Mp3Ctx *c = mp3_ctx(handle);
    if (!c) return mp3_missing(handle, SCE_MP3_ERROR_UNRESERVED_HANDLE);
    if (!c->auBuf) return SCE_MP3_ERROR_UNRESERVED_HANDLE;
    int32_t write_offset = mp3_write_offset(c);
    c->readPos += size;
    c->available += size;
    /* The positions follow the game's size even when it is negative or overruns the buffer;
     * only bytes that really sit in the stream area are taken. */
    if (size > 0 && (int64_t)MP3_STREAM_WORKAREA + write_offset + size <= (int64_t)c->auBufSize) {
        if (c->srcLen + (uint32_t)size > c->srcCap) {
            fprintf(stderr, "MP3: handle %u added %d bytes past two unconsumed halves; not taken\n",
                    handle, size);
        } else {
            uint32_t from = c->auBuf + MP3_STREAM_WORKAREA + (uint32_t)write_offset;
            for (int32_t i = 0; i < size; i++)
                c->src[c->srcLen + (uint32_t)i] = (uint8_t)MEM_R8(from + (uint32_t)i);
            c->srcLen += (uint32_t)size;
        }
    }
    return 0;
}

static uint32_t h_Mp3ResetPlayPosition(CpuState *s) {
    uint32_t handle = A0;
    Mp3Ctx *c = mp3_ctx(handle);
    if (!c) return mp3_missing(handle, SCE_MP3_ERROR_NOT_YET_INIT_HANDLE);
    if (c->version < 0 || !c->auBuf) return SCE_MP3_ERROR_NOT_YET_INIT_HANDLE;
    mp3_drop_source(c, c->srcLen);
    c->readPos = (int64_t)c->startPos;
    c->ringBase = c->readPos;
    c->available = 0;
    c->sumDecodedSamples = 0;
    mp3_bridge_reset(c->dec);
    return 0;
}

/* sceMp3ResetPlayPositionByFrame(handle, frame): seek to the nominal byte offset of frame. PPSSPP
 * reports the PSP landing one byte early for any frame but 0; the sync search covers it. */
static uint32_t h_Mp3ResetPlayPositionByFrame(CpuState *s) {
    uint32_t handle = A0, frame = A1;
    Mp3Ctx *c = mp3_ctx(handle);
    if (!c) return mp3_missing(handle, SCE_MP3_ERROR_NOT_YET_INIT_HANDLE);
    if (c->version < 0 || !c->auBuf) return SCE_MP3_ERROR_NOT_YET_INIT_HANDLE;
    if (frame >= (uint32_t)c->frameNum) return SCE_MP3_ERROR_BAD_RESET_FRAME;
    uint64_t bytes_per_second = (uint64_t)(c->maxOutputSample / 8u) * (uint64_t)c->bitRate * 1000u;
    mp3_drop_source(c, c->srcLen);
    c->readPos = (int64_t)c->startPos +
                 (int64_t)(((uint64_t)frame * bytes_per_second) / (uint64_t)c->samplingRate);
    if (frame) c->readPos -= 1;
    c->ringBase = c->readPos;
    c->available = 0;
    c->sumDecodedSamples = frame * c->maxOutputSample;
    mp3_bridge_reset(c->dec);
    return 0;
}

/* Stream counters: an unused handle is UNRESERVED. */
static Mp3Ctx *mp3_stream_ctx(uint32_t handle, uint32_t *err) {
    Mp3Ctx *c = mp3_ctx(handle);
    if (!c) { *err = mp3_missing(handle, SCE_MP3_ERROR_UNRESERVED_HANDLE); return NULL; }
    if (!c->auBuf) { *err = SCE_MP3_ERROR_UNRESERVED_HANDLE; return NULL; }
    return c;
}
static uint32_t h_Mp3GetSumDecodedSample(CpuState *s) {
    uint32_t err = 0;
    Mp3Ctx *c = mp3_stream_ctx(A0, &err);
    return c ? c->sumDecodedSamples : err;
}
static uint32_t h_Mp3GetLoopNum(CpuState *s) {
    uint32_t err = 0;
    Mp3Ctx *c = mp3_stream_ctx(A0, &err);
    return c ? (uint32_t)c->loopNum : err;
}
static uint32_t h_Mp3SetLoopNum(CpuState *s) {
    uint32_t err = 0;
    Mp3Ctx *c = mp3_stream_ctx(A0, &err);
    if (!c) return err;
    c->loopNum = (int32_t)A1 < 0 ? -1 : (int32_t)A1;
    return 0;
}

/* Format getters: an unused handle, or one before sceMp3Init, is NOT_YET_INIT; an argument-less
 * handle has no format and reads 0. */
static uint32_t mp3_format_value(uint32_t handle, int which) {
    Mp3Ctx *c = mp3_ctx(handle);
    if (!c) return mp3_missing(handle, SCE_MP3_ERROR_NOT_YET_INIT_HANDLE);
    if (c->version < 0) return SCE_MP3_ERROR_NOT_YET_INIT_HANDLE;
    if (!c->auBuf) return 0;
    switch (which) {
    case 0: return c->maxOutputSample;
    case 1: return (uint32_t)c->channels;
    case 2: return (uint32_t)c->bitRate;
    default: return (uint32_t)c->samplingRate;
    }
}
static uint32_t h_Mp3GetMaxOutputSample(CpuState *s) { return mp3_format_value(A0, 0); }
static uint32_t h_Mp3GetMp3ChannelNum(CpuState *s) { return mp3_format_value(A0, 1); }
static uint32_t h_Mp3GetBitRate(CpuState *s) { return mp3_format_value(A0, 2); }
static uint32_t h_Mp3GetSamplingRate(CpuState *s) { return mp3_format_value(A0, 3); }

static uint32_t h_Mp3GetFrameNum(CpuState *s) {
    uint32_t handle = A0;
    Mp3Ctx *c = mp3_ctx(handle);
    if (!c) return mp3_missing(handle, SCE_MP3_ERROR_NOT_YET_INIT_HANDLE);
    if (c->version < 0 || !c->auBuf) return SCE_MP3_ERROR_NOT_YET_INIT_HANDLE;
    return (uint32_t)c->frameNum;
}

/* Before sceMp3Init this reports UNRESERVED rather than NOT_YET_INIT; PPSSPP keeps that code
 * from the hardware tests while noting it looks wrong. */
static uint32_t h_Mp3GetMPEGVersion(CpuState *s) {
    uint32_t handle = A0;
    Mp3Ctx *c = mp3_ctx(handle);
    if (!c) return mp3_missing(handle, SCE_MP3_ERROR_UNRESERVED_HANDLE);
    if (c->version < 0) return SCE_MP3_ERROR_UNRESERVED_HANDLE;
    if (!c->auBuf) return 0;
    return (uint32_t)c->version;
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Test builds only: put the library back to its boot state between cases. */
void sr_hle_test_mp3_reset(void) {
    for (uint32_t i = 0; i < MP3_MAX_HANDLES; i++)
        if (s_mp3[i].used) mp3_free(&s_mp3[i]);
    s_mp3_resource_inited = 0;
}
#endif

void hle_register_mp3_handlers(void) {
    /* sceMp3: the stream/handle lifecycle above. */
    sr_hle_register(0x35750070u, "sceMp3InitResource", h_Mp3InitResource);
    sr_hle_register(0x3c2fa058u, "sceMp3TermResource", h_Mp3TermResource);
    sr_hle_register(0x07ec321au, "sceMp3ReserveMp3Handle", h_Mp3ReserveMp3Handle);
    sr_hle_register(0xf5478233u, "sceMp3ReleaseMp3Handle", h_Mp3ReleaseMp3Handle);
    sr_hle_register(0x44e07129u, "sceMp3Init", h_Mp3Init);
    sr_hle_register(0xd021c0fbu, "sceMp3Decode", h_Mp3Decode);
    sr_hle_register(0xd0a56296u, "sceMp3CheckStreamDataNeeded", h_Mp3CheckStreamDataNeeded);
    sr_hle_register(0xa703fe0fu, "sceMp3GetInfoToAddStreamData", h_Mp3GetInfoToAddStreamData);
    sr_hle_register(0x0db149f4u, "sceMp3NotifyAddStreamData", h_Mp3NotifyAddStreamData);
    sr_hle_register(0x2a368661u, "sceMp3ResetPlayPosition", h_Mp3ResetPlayPosition);
    sr_hle_register(0x0840e808u, "sceMp3ResetPlayPositionByFrame", h_Mp3ResetPlayPositionByFrame);
    sr_hle_register(0x354d27eau, "sceMp3GetSumDecodedSample", h_Mp3GetSumDecodedSample);
    sr_hle_register(0xd8f54a51u, "sceMp3GetLoopNum", h_Mp3GetLoopNum);
    sr_hle_register(0x3cef484fu, "sceMp3SetLoopNum", h_Mp3SetLoopNum);
    sr_hle_register(0x87c263d1u, "sceMp3GetMaxOutputSample", h_Mp3GetMaxOutputSample);
    sr_hle_register(0x7f696782u, "sceMp3GetMp3ChannelNum", h_Mp3GetMp3ChannelNum);
    sr_hle_register(0x87677e40u, "sceMp3GetBitRate", h_Mp3GetBitRate);
    sr_hle_register(0x8f450998u, "sceMp3GetSamplingRate", h_Mp3GetSamplingRate);
    sr_hle_register(0x3548aec8u, "sceMp3GetFrameNum", h_Mp3GetFrameNum);
    sr_hle_register(0xae6d2027u, "sceMp3GetMPEGVersion", h_Mp3GetMPEGVersion);
    /* Not modelled: the low-level pair works on caller-chosen handle numbers and an
     * unestablished input/output contract (PPSSPP's version guesses a 4096-byte window), and
     * the module entry points are not called by games. Refused by name with the non-kernel
     * library code (SCE_KERNEL_ERROR_ERRNO_FUNCTION_NOT_SUPPORTED) rather than faked. */
    sr_hle_register_unsupported(0x1b839b83u, "sceMp3LowLevelInit", 0x80010086u);
    sr_hle_register_unsupported(0xe3ee2c81u, "sceMp3LowLevelDecode", 0x80010086u);
    sr_hle_register_unsupported(0x8ab81558u, "sceMp3StartEntry", 0x80010086u);
    sr_hle_register_unsupported(0x732b042au, "sceMp3EndEntry", 0x80010086u);
}

void hle_register_atrac_handlers(void) {
    /* sceAtrac3plus (control flow only; silence output). */
    sr_hle_register(0x7a20e7af, "sceAtracSetDataAndGetID", h_AtracSetDataAndGetID);
    sr_hle_register(0x61eb33f5, "sceAtracReleaseAtracID", h_AtracReleaseAtracID);
    sr_hle_register(0x6a8c3cd5, "sceAtracDecodeData", h_AtracDecodeData);
    sr_hle_register(0x9ae849a7, "sceAtracGetRemainFrame", h_AtracGetRemainFrame);
    sr_hle_register(0x5d268707, "sceAtracGetStreamDataInfo", h_AtracGetStreamDataInfo);
    sr_hle_register(0x7db31251, "sceAtracAddStreamData", h_AtracAddStreamData);
    sr_hle_register(0xe23e3a35, "sceAtracGetNextDecodePosition", h_AtracGetNextDecodePosition);
    sr_hle_register(0xa2bba8be, "sceAtracGetSoundSample", h_AtracGetSoundSample);
    sr_hle_register(0xfaa4f89b, "sceAtracGetLoopStatus", h_AtracGetLoopStatus);
    sr_hle_register(0x868120b5, "sceAtracSetLoopNum", h_AtracSetLoopNum);
    sr_hle_register(0x644e5607, "sceAtracResetPlayPosition", h_AtracResetPlayPosition);
    /* Additional sceAtrac handlers (TD-24 batch 4) */
    sr_hle_register(0x132f1eca, "sceAtracReinit", h_AtracReinit);
    sr_hle_register(0x0fae370e, "sceAtracSetHalfwayBufferAndGetID", h_AtracSetHalfwayBufferAndGetID);
    sr_hle_register(0x3f6e26b5, "sceAtracSetHalfwayBuffer", h_AtracSetHalfwayBuffer);
    sr_hle_register(0x0e2a73ab, "sceAtracSetData", h_AtracSetData);
    sr_hle_register(0x780f88d1, "sceAtracGetAtracID", h_AtracGetAtracID);
    sr_hle_register(0x2dd3e298, "sceAtracGetBufferInfoForResetting", h_AtracGetBufferInfoForResetting);
    sr_hle_register(0xca3ca3d2, "sceAtracGetBufferInfoForReseting", h_AtracGetBufferInfoForResetting);
    /* 0x31668bba was a single-nibble transcription error for 0x31668baa (the
     * canonical NID in src/rt/nid_names.h, reproducible as sha1(name)[0:4]).
     * No guest import can carry the typo, so this registration was previously
     * unreachable and a real sceAtracGetChannel call was an unhandled-NID miss.
     * TD-24 batch 3 models it with h_AtracGetChannel (parsed fmt channels);
     * sceAtracGetOutputChannel keeps its h_ok acceptance below (#286 tracks
     * the remaining output-mapping gap). */
    sr_hle_register(0x31668baa, "sceAtracGetChannel", h_AtracGetChannel);
    sr_hle_register(0x36faabfb, "sceAtracGetNextSample", h_AtracGetNextSample);
    sr_hle_register(0xa554a158, "sceAtracGetBitrate", h_AtracGetBitrate);
    sr_hle_register(0xb3b5d042, "sceAtracGetOutputChannel", h_ok);
    sr_hle_register(0xd6a5f2f7, "sceAtracGetMaxSample", h_AtracGetMaxSample);
    sr_hle_register(0x5622b7c1, "sceAtracSetAA3DataAndGetID", h_AtracSetAA3DataAndGetID);
    sr_hle_register(0x5dd66588, "sceAtracSetAA3HalfwayBufferAndGetID", h_AtracSetAA3HalfwayBufferAndGetID);
    sr_hle_register(0x472e3825, "sceAtracSetMOutDataAndGetID", h_AtracSetMOutDataAndGetID);
    sr_hle_register(0x5cf9d852, "sceAtracSetMOutHalfwayBuffer", h_AtracSetMOutHalfwayBuffer);
    sr_hle_register(0x9cd7de03, "sceAtracSetMOutHalfwayBufferAndGetID", h_AtracSetMOutHalfwayBufferAndGetID);
    sr_hle_register(0xf6837a1a, "sceAtracSetMOutData", h_AtracSetMOutData);
    sr_hle_register(0x83bf7afd, "sceAtracSetSecondBuffer", h_AtracSetSecondBuffer);
    sr_hle_register(0x83e85ea0, "sceAtracGetSecondBufferInfo", h_AtracGetSecondBufferInfo);
    sr_hle_register(0xd5c28cc0, "sceAtracReleaseResources", h_AtracReleaseResources);
    sr_hle_register_unsupported(0xd1f59fdb, "sceAtracStartEntry", 0x80630004u); /* ATRAC invalid codec type */
    sr_hle_register(0xeca32a99, "sceAtracIsSecondBufferNeeded", h_AtracIsSecondBufferNeeded);
    sr_hle_register(0xe88f759b, "sceAtracGetInternalErrorInfo", h_AtracGetInternalErrorInfo);
    sr_hle_register_unsupported(0x231fc6b7, "_sceAtracGetContextAddress", 0x80630003u); /* ATRAC no ATRAC ID */
    sr_hle_register_unsupported(0x1575d64b, "sceAtracLowLevelInitDecoder", 0x80630004u); /* ATRAC invalid codec type */
    sr_hle_register_unsupported(0x0c116e1b, "sceAtracLowLevelDecode", 0x80630004u); /* ATRAC invalid codec type */
}

/* Registration runs of the shared helpers in hle.c (#307). Each holds one run of this file's
 * registrations, in their order, and is called from the helper where the run stood. */

/* From hle_register_unregistered_import_batch(). */
void hle_register_unregistered_import_audio_codec(void) {
    sr_hle_register_unsupported(0x29681260u, "sceAudiocodecReleaseEDRAM", 0x80010086u);
    sr_hle_register_unsupported(0x3a20a200u, "sceAudiocodecGetEDRAM", 0x80010086u);
    sr_hle_register_unsupported(0x5b37eb1du, "sceAudiocodecInit", 0x80010086u);
    sr_hle_register_unsupported(0x70a703f8u, "sceAudiocodecDecode", 0x80010086u);
}
